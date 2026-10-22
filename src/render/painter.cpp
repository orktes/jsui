// Skia painter: CSS paint order for react-native-web styled boxes.
//
// Per node (every View is its own stacking context, as in react-native-web
// where Views are `position: relative; z-index: 0`):
//   transform (about transformOrigin) -> opacity / filter / blend layer ->
//   outer box shadows -> background color -> background gradients ->
//   inset shadows -> borders -> content (image, text) -> children sorted by
//   zIndex (clipped by overflow) -> outline.
// Geometry follows Chrome: shadow sigma = blur / 2, SkRRect corner radii with
// CSS clamping, borders as the area between the outer and inner rrect.
#include "render/painter.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <vector>

#include "core/document.h"
#include "core/scroll.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "include/core/SkRect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "render/image_cache.h"
#include "render/image_node.h"
#include "text/font_manager.h"
#include "text/text_layout.h"

namespace jsui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

SkColor4f sk(const Color& c) { return {c.r, c.g, c.b, c.a}; }

// ------------------------------------------------------------- geometry

struct Box {
  float w = 0, h = 0;
  float border[4] = {0, 0, 0, 0};   // top right bottom left (resolved by Yoga, RTL-aware)
  float padding[4] = {0, 0, 0, 0};
  SkRRect outer;  // border box
  SkRRect inner;  // padding box
  SkRect content;
  bool rounded = false;
};

// `w`, `h`: the (pixel-snapped) border-box size.
void computeBox(const Node* n, float w, float h, Box* b) {
  YGNodeRef y = n->yoga();
  b->w = w;
  b->h = h;
  const YGEdge edges[4] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
  for (int i = 0; i < 4; i++) {
    b->border[i] = YGNodeLayoutGetBorder(y, edges[i]);
    b->padding[i] = YGNodeLayoutGetPadding(y, edges[i]);
  }
  const ComputedStyle& s = n->style();
  SkRect r = SkRect::MakeWH(b->w, b->h);
  SkVector radii[4];
  b->rounded = false;
  for (int i = 0; i < 4; i++) {
    const Length& l = s.borderRadius[i];
    float rx = l.percent ? l.value * b->w / 100.f : l.value;
    float ry = l.percent ? l.value * b->h / 100.f : l.value;
    radii[i] = {std::max(0.f, rx), std::max(0.f, ry)};
    if (rx > 0 && ry > 0) b->rounded = true;
  }
  // setRectRadii scales all radii down together when they overlap (CSS rule).
  if (b->rounded) b->outer.setRectRadii(r, radii);
  else b->outer.setRect(r);
  SkRect ir = SkRect::MakeLTRB(b->border[3], b->border[0], b->w - b->border[1], b->h - b->border[2]);
  if (ir.width() < 0) ir.fRight = ir.fLeft = (ir.fLeft + ir.fRight) / 2;
  if (ir.height() < 0) ir.fBottom = ir.fTop = (ir.fTop + ir.fBottom) / 2;
  if (b->rounded) {
    SkVector in[4];
    const float bl = b->border[3], bt = b->border[0], br = b->border[1], bb = b->border[2];
    const float hx[4] = {bl, br, br, bl}, vy[4] = {bt, bt, bb, bb};
    for (int i = 0; i < 4; i++) {
      SkVector o = b->outer.radii(SkRRect::Corner(i));
      in[i] = {std::max(0.f, o.fX - hx[i]), std::max(0.f, o.fY - vy[i])};
    }
    b->inner.setRectRadii(ir, in);
  } else {
    b->inner.setRect(ir);
  }
  b->content = SkRect::MakeLTRB(ir.fLeft + b->padding[3], ir.fTop + b->padding[0], ir.fRight - b->padding[1],
                                ir.fBottom - b->padding[2]);
}

// CSS spread adjustment of a corner radius (css-backgrounds-3 "shadow shape").
float spreadRadius(float r, float spread) {
  if (r <= 0) return 0;
  if (spread >= 0) {
    if (r >= spread) return r + spread;
    float ratio = r / spread;
    return r + spread * (1 + (ratio - 1) * (ratio - 1) * (ratio - 1));
  }
  return std::max(0.f, r + spread);
}

// `rr` grown (spread > 0) or shrunk by `spread` on every side.
SkRRect outsetRRect(const SkRRect& rr, float spread) {
  SkRect r = rr.rect().makeOutset(spread, spread);
  if (r.width() < 0 || r.height() < 0) return SkRRect::MakeEmpty();
  if (rr.isRect()) return SkRRect::MakeRect(r);
  SkVector radii[4];
  for (int i = 0; i < 4; i++) {
    SkVector o = rr.radii(SkRRect::Corner(i));
    radii[i] = {spreadRadius(o.fX, spread), spreadRadius(o.fY, spread)};
  }
  SkRRect out;
  out.setRectRadii(r, radii);
  return out;
}

void clipRRect(SkCanvas* c, const SkRRect& rr, SkClipOp op = SkClipOp::kIntersect) {
  if (rr.isRect()) c->clipRect(rr.rect(), op, true);
  else c->clipRRect(rr, op, true);
}

void drawRRect(SkCanvas* c, const SkRRect& rr, const SkPaint& p) {
  if (rr.isRect()) c->drawRect(rr.rect(), p);
  else c->drawRRect(rr, p);
}

// ------------------------------------------------------------- shadows

sk_sp<SkMaskFilter> blurFilter(float blur) {
  return blur > 0 ? SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2) : nullptr;  // CSS: sigma = blur / 2
}

struct RootRecord;
sk_sp<SkMaskFilter> cachedBlur(RootRecord* root, float blur);

// Blurred shadow masks, rendered once per distinct shape and reused while it
// stays the same: blurring is the most expensive thing a game UI paints
// (cards, panels and buttons all carry shadows). Keys are in device pixels.
struct ShadowKey {
  float w, h, sigma, fx, fy;  // device size, device sigma, sub-pixel phase
  float radii[8];
  float hole[4];              // border box cut out of the shadow (relative to the shape), or zeros
  float holeRadii[8];
  float color[4];             // pre-coloured: drawn as a sprite, the fastest CPU blit
  bool operator==(const ShadowKey& o) const { return std::memcmp(this, &o, sizeof(ShadowKey)) == 0; }
};
struct ShadowKeyHash {
  size_t operator()(const ShadowKey& k) const {
    const auto* p = reinterpret_cast<const uint32_t*>(&k);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(ShadowKey) / 4; i++) h = (h ^ p[i]) * 1099511628211ull;
    return size_t(h);
  }
};
struct ShadowMask {
  sk_sp<SkImage> image;  // premultiplied N32
  int margin = 0;
  uint32_t lastUsed = 0;
};
struct ShadowCache {
  std::unordered_map<ShadowKey, ShadowMask, ShadowKeyHash> masks;
  size_t bytes = 0;
  uint32_t frame = 0;
  static constexpr size_t kMaxBytes = 24u << 20;
};
ShadowCache& shadowCache(RootRecord* root);

// Draws `shape` (local coords) blurred by CSS `blur` with `paint`'s color
// from the cache, minus `hole` (the border box, when the background does not
// hide the shadow; baking it into the mask avoids an anti-aliased clip, which
// is slow on the CPU). False when the transform is not axis-aligned (the
// caller blurs directly then).
bool drawCachedShadow(SkCanvas* c, const SkRRect& shape, const SkRRect* hole, const SkRRect& box, float blur,
                      const SkPaint& paint, ShadowCache& cache) {
  const SkMatrix& m = c->getTotalMatrix();
  if (!m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0) return false;
  SkRRect dev;
  if (!shape.transform(m, &dev)) return false;
  const SkRect& r = dev.rect();
  float ox = std::floor(r.left()), oy = std::floor(r.top());
  ShadowKey key{};
  key.w = r.width();
  key.h = r.height();
  key.sigma = blur / 2 * std::max(m.getScaleX(), m.getScaleY());
  key.fx = r.left() - ox;
  key.fy = r.top() - oy;
  for (int i = 0; i < 4; i++) {
    SkVector v = dev.radii(SkRRect::Corner(i));
    key.radii[i * 2] = v.fX;
    key.radii[i * 2 + 1] = v.fY;
  }
  SkRRect devHole;
  if (hole) {
    if (!hole->transform(m, &devHole)) return false;
    key.hole[0] = devHole.rect().left() - r.left();
    key.hole[1] = devHole.rect().top() - r.top();
    key.hole[2] = devHole.rect().width();
    key.hole[3] = devHole.rect().height();
    for (int i = 0; i < 4; i++) {
      SkVector v = devHole.radii(SkRRect::Corner(i));
      key.holeRadii[i * 2] = v.fX;
      key.holeRadii[i * 2 + 1] = v.fY;
    }
  }
  SkColor4f col = paint.getColor4f();
  key.color[0] = col.fR;
  key.color[1] = col.fG;
  key.color[2] = col.fB;
  key.color[3] = col.fA;
  auto it = cache.masks.find(key);
  if (it == cache.masks.end()) {
    int margin = int(std::ceil(key.sigma * 3)) + 2;
    int iw = int(std::ceil(key.fx + key.w)) + margin * 2, ih = int(std::ceil(key.fy + key.h)) + margin * 2;
    if (iw <= 0 || ih <= 0 || size_t(iw) * size_t(ih) > (4u << 20)) return false;  // huge: blur directly
    sk_sp<SkSurface> surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    if (!surf) return false;
    surf->getCanvas()->clear(SK_ColorTRANSPARENT);
    SkPaint mp;
    mp.setAntiAlias(true);
    mp.setColor4f(col);
    mp.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, key.sigma));
    SkRRect local = dev;
    local.offset(margin - ox, margin - oy);
    surf->getCanvas()->drawRRect(local, mp);
    if (hole) {
      SkPaint hp;
      hp.setAntiAlias(true);
      hp.setBlendMode(SkBlendMode::kDstOut);
      SkRRect h = devHole;
      h.offset(margin - ox, margin - oy);
      surf->getCanvas()->drawRRect(h, hp);
    }
    ShadowMask sm;
    sm.image = surf->makeImageSnapshot();
    sm.margin = margin;
    cache.bytes += size_t(iw) * ih * 4;
    if (cache.bytes > ShadowCache::kMaxBytes) {
      // Drop masks unused for a while (shapes that animated away).
      for (auto e = cache.masks.begin(); e != cache.masks.end();) {
        if (cache.frame - e->second.lastUsed > 2) {
          cache.bytes -= size_t(e->second.image->width()) * e->second.image->height() * 4;
          e = cache.masks.erase(e);
        } else {
          ++e;
        }
      }
    }
    it = cache.masks.emplace(key, std::move(sm)).first;
  }
  it->second.lastUsed = cache.frame;
  SkPaint p;  // colour is baked in; plain src-over sprite
  const SkImage* img = it->second.image.get();
  float x0 = ox - float(it->second.margin), y0 = oy - float(it->second.margin);
  SkIRect all = SkIRect::MakeXYWH(int(x0), int(y0), img->width(), img->height());
  // The box interior (inset by its corner radii) is either cut out of the
  // mask or covered by an opaque background: draw only the ring around it.
  SkRRect devBox;
  SkIRect skip = SkIRect::MakeEmpty();
  if (box.transform(m, &devBox)) {
    float rx = 0, ry = 0;
    for (int i = 0; i < 4; i++) {
      SkVector v = devBox.radii(SkRRect::Corner(i));
      rx = std::max(rx, v.fX);
      ry = std::max(ry, v.fY);
    }
    SkRect in = devBox.rect().makeInset(rx + 1, ry + 1);
    if (!in.isEmpty()) {
      skip = SkIRect::MakeLTRB(int(std::ceil(in.left())), int(std::ceil(in.top())), int(std::floor(in.right())),
                               int(std::floor(in.bottom())));
      if (!skip.intersect(all)) skip.setEmpty();
    }
  }
  c->save();
  c->setMatrix(SkMatrix::I());
  // Whole-image draws at integer offsets take Skia's sprite blitter; the
  // ring parts are pixel-aligned clips of the same draw.
  auto part = [&](const SkIRect& d) {
    if (d.isEmpty()) return;
    c->save();
    c->clipIRect(d);
    c->drawImage(img, x0, y0, SkSamplingOptions(), &p);
    c->restore();
  };
  if (skip.isEmpty() || skip.width() < 16 || skip.height() < 16) {
    part(all);
  } else {
    part(SkIRect::MakeLTRB(all.left(), all.top(), all.right(), skip.top()));
    part(SkIRect::MakeLTRB(all.left(), skip.bottom(), all.right(), all.bottom()));
    part(SkIRect::MakeLTRB(all.left(), skip.top(), skip.left(), skip.bottom()));
    part(SkIRect::MakeLTRB(skip.right(), skip.top(), all.right(), skip.bottom()));
  }
  c->restore();
  return true;
}

void paintOuterShadows(SkCanvas* c, const Box& b, const ComputedStyle& s, RootRecord* root) {
  bool opaqueBg = s.backgroundColor.a >= 1 && s.backgroundImages.empty();
  // The first shadow is on top, so paint in reverse.
  for (auto it = s.boxShadows.rbegin(); it != s.boxShadows.rend(); ++it) {
    const Shadow& sh = *it;
    if (sh.inset || sh.color.transparent()) continue;
    SkRRect shape = outsetRRect(b.outer, sh.spread);
    if (shape.isEmpty()) continue;
    shape.offset(sh.offsetX, sh.offsetY);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f(sk(sh.color));
    p.setMaskFilter(cachedBlur(root, sh.blur));
    // Shadows are cut out of the border box (visible through translucent
    // backgrounds otherwise). An opaque background hides it anyway.
    if (sh.blur > 0 && drawCachedShadow(c, shape, opaqueBg ? nullptr : &b.outer, b.outer, sh.blur, p, shadowCache(root)))
      continue;
    c->save();
    if (!opaqueBg) {
      SkRect bounds = shape.rect().makeOutset(sh.blur * 1.5f + 1, sh.blur * 1.5f + 1);
      c->clipRect(bounds);  // keeps the anti-aliased clip mask small
      clipRRect(c, b.outer, SkClipOp::kDifference);
    }
    drawRRect(c, shape, p);
    c->restore();
  }
}

void paintInsetShadows(SkCanvas* c, const Box& b, const ComputedStyle& s, RootRecord* root) {
  bool any = false;
  for (auto& sh : s.boxShadows) any |= sh.inset && !sh.color.transparent();
  if (!any || b.inner.isEmpty()) return;
  for (auto it = s.boxShadows.rbegin(); it != s.boxShadows.rend(); ++it) {
    const Shadow& sh = *it;
    if (!sh.inset || sh.color.transparent()) continue;
    SkRRect hole = outsetRRect(b.inner, -sh.spread);
    hole.offset(sh.offsetX, sh.offsetY);
    float pad = sh.blur * 1.5f + std::fabs(sh.offsetX) + std::fabs(sh.offsetY) + std::fabs(sh.spread) + 2;
    SkRRect frame = SkRRect::MakeRect(b.inner.rect().makeOutset(pad, pad));
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f(sk(sh.color));
    p.setMaskFilter(cachedBlur(root, sh.blur));
    c->save();
    clipRRect(c, b.inner);
    if (hole.isEmpty()) c->drawRRect(frame, p);
    else c->drawDRRect(frame, hole, p);
    c->restore();
  }
}

// ----------------------------------------------------------- gradients

// Resolves missing / out of order stop positions (CSS "color stop fixup").
void fixupStops(const Gradient& g, float lineLength, std::vector<SkColor4f>* colors, std::vector<float>* pos) {
  size_t n = g.stops.size();
  colors->resize(n);
  pos->assign(n, NAN);
  for (size_t i = 0; i < n; i++) {
    (*colors)[i] = sk(g.stops[i].color);
    if (g.stops[i].position) {
      float p = *g.stops[i].position;
      if (g.stops[i].px) p = lineLength > 0 ? p / lineLength : 0;
      (*pos)[i] = p;
    }
  }
  if (std::isnan((*pos)[0])) (*pos)[0] = 0;
  if (std::isnan((*pos)[n - 1])) (*pos)[n - 1] = std::max(1.f, (*pos)[0]);
  float maxSoFar = (*pos)[0];
  for (size_t i = 1; i < n; i++) {
    if (!std::isnan((*pos)[i])) {
      (*pos)[i] = std::max((*pos)[i], maxSoFar);
      maxSoFar = (*pos)[i];
    }
  }
  for (size_t i = 1; i < n;) {
    if (!std::isnan((*pos)[i])) {
      i++;
      continue;
    }
    size_t j = i;
    while (std::isnan((*pos)[j])) j++;
    float a = (*pos)[i - 1], bpos = (*pos)[j];
    for (size_t k = i; k < j; k++) (*pos)[k] = a + (bpos - a) * float(k - i + 1) / float(j - i + 1);
    i = j;
  }
}

sk_sp<SkShader> makeGradient(const Gradient& g, const SkRect& box) {
  if (g.stops.size() < 2 || box.isEmpty()) return nullptr;
  std::vector<SkColor4f> colors;
  std::vector<float> pos;
  SkGradientShader::Interpolation interp;
  interp.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;  // CSS interpolates premultiplied
  float w = box.width(), h = box.height();
  if (g.kind == Gradient::Kind::Linear) {
    float deg = g.angle;
    if (g.corner) {
      // "to <corner>": the gradient line is perpendicular to the diagonal
      // joining the two neighbouring corners.
      float a = std::atan2(h, w) * 180.f / kPi;
      deg = g.corner == 1 ? a : g.corner == 2 ? 180 - a : g.corner == 3 ? 180 + a : 360 - a;
    }
    float rad = deg * kPi / 180.f;
    float sx = std::sin(rad), sy = -std::cos(rad);
    float len = std::fabs(w * std::sin(rad)) + std::fabs(h * std::cos(rad));
    fixupStops(g, len, &colors, &pos);
    float lo = pos.front(), hi = pos.back();
    SkTileMode tile = SkTileMode::kClamp;
    if (g.repeating) {
      if (hi - lo <= 1e-4f) return SkShaders::Color(colors.back(), nullptr);
      tile = SkTileMode::kRepeat;
    } else {
      lo = std::min(lo, 0.f);
      hi = std::max(hi, 1.f);
    }
    for (auto& p : pos) p = (p - lo) / (hi - lo);
    SkPoint c = {box.centerX(), box.centerY()};
    SkPoint start = {c.fX - sx * len / 2, c.fY - sy * len / 2};
    SkPoint pts[2] = {{start.fX + sx * len * lo, start.fY + sy * len * lo},
                      {start.fX + sx * len * hi, start.fY + sy * len * hi}};
    if (len <= 0) return SkShaders::Color(colors.back(), nullptr);
    return SkGradientShader::MakeLinear(pts, colors.data(), nullptr, pos.data(), int(colors.size()), tile, interp,
                                        nullptr);
  }
  // Radial
  float cx = box.left() + g.centerX.resolve(w), cy = box.top() + g.centerY.resolve(h);
  float dl = std::fabs(cx - box.left()), dr = std::fabs(box.right() - cx);
  float dt = std::fabs(cy - box.top()), db = std::fabs(box.bottom() - cy);
  float rx, ry;
  if (g.explicitSize) {
    rx = g.sizeX.resolve(w);
    ry = g.circle ? rx : g.sizeY.resolve(h);
  } else {
    float sideX = 0, sideY = 0;
    bool corner = false;
    switch (g.extent) {
      case Gradient::Extent::ClosestSide: sideX = std::min(dl, dr), sideY = std::min(dt, db); break;
      case Gradient::Extent::FarthestSide: sideX = std::max(dl, dr), sideY = std::max(dt, db); break;
      case Gradient::Extent::ClosestCorner:
        sideX = std::min(dl, dr), sideY = std::min(dt, db), corner = true;
        break;
      case Gradient::Extent::FarthestCorner:
        sideX = std::max(dl, dr), sideY = std::max(dt, db), corner = true;
        break;
    }
    if (g.circle) {
      rx = ry = corner ? std::sqrt(sideX * sideX + sideY * sideY)
                       : (g.extent == Gradient::Extent::ClosestSide ? std::min(sideX, sideY) : std::max(sideX, sideY));
    } else {
      rx = corner ? sideX * std::sqrt(2.f) : sideX;
      ry = corner ? sideY * std::sqrt(2.f) : sideY;
    }
  }
  rx = std::max(rx, 1e-3f);
  ry = std::max(ry, 1e-3f);
  fixupStops(g, rx, &colors, &pos);
  float lo = std::max(0.f, pos.front()), hi = pos.back();
  SkTileMode tile = SkTileMode::kClamp;
  if (g.repeating) {
    if (hi - lo <= 1e-4f) return SkShaders::Color(colors.back(), nullptr);
    tile = SkTileMode::kRepeat;
  } else {
    lo = 0;
    hi = std::max(hi, 1.f);
  }
  for (auto& p : pos) p = std::clamp((p - lo) / (hi - lo), 0.f, 1.f);
  SkMatrix m = SkMatrix::Scale(1, ry / rx);
  m.postTranslate(cx, cy);
  if (lo > 0) {
    return SkGradientShader::MakeTwoPointConical({0, 0}, rx * lo, {0, 0}, rx * hi, colors.data(), nullptr, pos.data(),
                                                 int(colors.size()), tile, interp, &m);
  }
  return SkGradientShader::MakeRadial({0, 0}, rx * hi, colors.data(), nullptr, pos.data(), int(colors.size()), tile,
                                      interp, &m);
}

void paintBackground(SkCanvas* c, const Box& b, const ComputedStyle& s) {
  if (!s.backgroundColor.transparent()) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f(sk(s.backgroundColor));
    drawRRect(c, b.outer, p);
  }
  if (s.backgroundImages.empty()) return;
  // background-origin: padding-box (the gradient is sized to the padding box),
  // background-clip: border-box, background-repeat: repeat -> the tile repeats
  // under the borders.
  SkRect tile = b.inner.rect();
  if (tile.isEmpty()) return;
  bool hasBorder = b.border[0] > 0 || b.border[1] > 0 || b.border[2] > 0 || b.border[3] > 0;
  c->save();
  if (hasBorder || b.rounded) clipRRect(c, b.outer);
  for (auto it = s.backgroundImages.rbegin(); it != s.backgroundImages.rend(); ++it) {
    sk_sp<SkShader> shader = makeGradient(*it, tile);
    if (!shader) continue;
    SkPaint p;
    p.setShader(shader);
    if (!hasBorder) {
      c->drawRect(tile, p);
      continue;
    }
    SkRect area = b.outer.rect();
    for (int ty = -1; ty <= 1; ty++) {
      for (int tx = -1; tx <= 1; tx++) {
        SkRect t = tile.makeOffset(tx * tile.width(), ty * tile.height());
        if (!t.intersect(area)) continue;
        c->save();
        c->translate(tx * tile.width(), ty * tile.height());
        c->drawRect(t.makeOffset(-tx * tile.width(), -ty * tile.height()), p);
        c->restore();
      }
    }
  }
  c->restore();
}

// -------------------------------------------------------------- borders

// Lengths of dashes and gaps fitted to a side like Chrome: the pattern starts
// and ends with a dash at the corners.
void fittedDash(float length, float dash, float gap, float out[2]) {
  out[0] = dash;
  out[1] = gap;
  if (length <= dash) return;
  int n = int(std::floor((length + gap) / (dash + gap)));
  if (n < 2) return;
  out[1] = (length - n * dash) / float(n - 1);
}

void strokeSide(SkCanvas* c, SkPoint a, SkPoint b, float width, BorderStyle style, const SkPaint& base) {
  SkPaint p = base;
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(width);
  float len = SkPoint::Distance(a, b);
  float iv[2];
  if (style == BorderStyle::Dotted) {
    if (width >= 3) {
      p.setStrokeCap(SkPaint::kRound_Cap);
      fittedDash(len - width, 0, width * 2, iv);  // zero-length dashes + round caps = dots
      iv[0] = 0;
      // start the first dot half a width in so it stays inside the border
      SkVector d = b - a;
      d.setLength(width / 2);
      a += d;
      b -= d;
    } else {
      fittedDash(len, width, width, iv);
    }
  } else {
    fittedDash(len, width * 3, width * 3, iv);
  }
  p.setPathEffect(SkDashPathEffect::Make(iv, 2, 0));
  c->drawLine(a, b, p);
}

void paintBorders(SkCanvas* c, const Box& b, const ComputedStyle& s) {
  const float* w = b.border;
  bool any = false;
  for (int i = 0; i < 4; i++) any |= w[i] > 0 && !s.borderColor[i].transparent();
  if (!any) return;
  bool uniformColor = true;
  int first = -1;
  for (int i = 0; i < 4; i++) {
    if (w[i] <= 0) continue;
    if (first < 0) first = i;
    else if (!(s.borderColor[i] == s.borderColor[first])) uniformColor = false;
  }
  SkPaint p;
  p.setAntiAlias(true);
  if (s.borderStyle == BorderStyle::Solid) {
    if (uniformColor) {
      p.setColor4f(sk(s.borderColor[first]));
      if (b.inner.isEmpty()) drawRRect(c, b.outer, p);
      else c->drawDRRect(b.outer, b.inner, p);
      return;
    }
    // Mixed colors: each side is the ring clipped to its region. Like Chrome,
    // the corner joins run from each outer box corner through the matching
    // inner (padding) box corner; the regions extend along those joins to the
    // box's center lines so they also cover the rounded corner arcs, which
    // reach well past the inner box edges when the radius exceeds the widths.
    const SkRect o = b.outer.rect(), in = b.inner.rect();
    const SkPoint oc[4] = {{o.fLeft, o.fTop}, {o.fRight, o.fTop}, {o.fRight, o.fBottom}, {o.fLeft, o.fBottom}};
    const SkPoint ic[4] = {{in.fLeft, in.fTop}, {in.fRight, in.fTop}, {in.fRight, in.fBottom}, {in.fLeft, in.fBottom}};
    const float cx = o.centerX(), cy = o.centerY();
    // Corner k's join, extended from the outer corner until it meets the
    // center line of side `side` (top/bottom: y = cy, left/right: x = cx).
    // Returns the end point and the ray parameter (in units of the join).
    auto joinEnd = [&](int k, int side, float* tOut) {
      SkVector d = ic[k] - oc[k];
      bool horizontalSide = side == 0 || side == 2;
      float dc = horizontalSide ? d.fY : d.fX;
      float target = horizontalSide ? cy - oc[k].fY : cx - oc[k].fX;
      float t = std::fabs(dc) > 1e-4f ? target / dc : -1;
      if (t < 0) {
        // The join runs parallel to the center line: extend straight across.
        SkPoint q = oc[k];
        if (horizontalSide) q.fY = cy;
        else q.fX = cx;
        *tOut = -1;
        return q;
      }
      *tOut = t;
      return oc[k] + d * t;
    };
    // Side i is bounded by the joins of corners i and i+1. When the two joins
    // cross before reaching the center line (narrow or very uneven borders),
    // the region is the triangle up to the crossing so it never spills over
    // the far side of a neighbour's join.
    SkPoint quads[4][4];
    int quadPoints[4];
    for (int i = 0; i < 4; i++) {
      int k0 = i, k1 = (i + 1) % 4;
      float t0, t1;
      SkPoint e0 = joinEnd(k0, i, &t0), e1 = joinEnd(k1, i, &t1);
      quads[i][0] = oc[k0];
      quads[i][1] = oc[k1];
      quadPoints[i] = 4;
      SkVector d0 = ic[k0] - oc[k0], d1 = ic[k1] - oc[k1];
      float den = SkPoint::CrossProduct(d0, d1);
      if (t0 > 0 && t1 > 0 && std::fabs(den) > 1e-6f) {
        SkVector r = oc[k1] - oc[k0];
        float s0 = SkPoint::CrossProduct(r, d1) / den;
        float s1 = SkPoint::CrossProduct(r, d0) / den;
        if (s0 > 0 && s1 > 0 && s0 < t0 && s1 < t1) {
          quads[i][2] = oc[k0] + d0 * s0;
          quadPoints[i] = 3;
          continue;
        }
      }
      quads[i][2] = e1;
      quads[i][3] = e0;
    }
    for (int i = 0; i < 4; i++) {
      if (w[i] <= 0 || s.borderColor[i].transparent()) continue;
      SkPath clip = SkPath::Polygon(quads[i], quadPoints[i], true);
      c->save();
      c->clipPath(clip, true);
      p.setColor4f(sk(s.borderColor[i]));
      if (b.inner.isEmpty()) drawRRect(c, b.outer, p);
      else c->drawDRRect(b.outer, b.inner, p);
      c->restore();
    }
    return;
  }
  // Dashed / dotted.
  bool uniformWidth = w[0] == w[1] && w[1] == w[2] && w[2] == w[3];
  if (b.rounded && uniformWidth && uniformColor) {
    SkRRect mid = outsetRRect(b.outer, -w[0] / 2);
    SkPaint sp = p;
    sp.setColor4f(sk(s.borderColor[first]));
    sp.setStyle(SkPaint::kStroke_Style);
    sp.setStrokeWidth(w[0]);
    float iv[2] = {w[0] * 3, w[0] * 3};
    if (s.borderStyle == BorderStyle::Dotted) {
      iv[0] = 0;
      iv[1] = w[0] * 2;
      sp.setStrokeCap(SkPaint::kRound_Cap);
    }
    sp.setPathEffect(SkDashPathEffect::Make(iv, 2, 0));
    c->drawRRect(mid, sp);
    return;
  }
  const SkRect o = b.outer.rect();
  c->save();
  if (b.rounded) clipRRect(c, b.outer);
  if (w[0] > 0) {
    p.setColor4f(sk(s.borderColor[0]));
    strokeSide(c, {o.fLeft, o.fTop + w[0] / 2}, {o.fRight, o.fTop + w[0] / 2}, w[0], s.borderStyle, p);
  }
  if (w[1] > 0) {
    p.setColor4f(sk(s.borderColor[1]));
    strokeSide(c, {o.fRight - w[1] / 2, o.fTop}, {o.fRight - w[1] / 2, o.fBottom}, w[1], s.borderStyle, p);
  }
  if (w[2] > 0) {
    p.setColor4f(sk(s.borderColor[2]));
    strokeSide(c, {o.fLeft, o.fBottom - w[2] / 2}, {o.fRight, o.fBottom - w[2] / 2}, w[2], s.borderStyle, p);
  }
  if (w[3] > 0) {
    p.setColor4f(sk(s.borderColor[3]));
    strokeSide(c, {o.fLeft + w[3] / 2, o.fTop}, {o.fLeft + w[3] / 2, o.fBottom}, w[3], s.borderStyle, p);
  }
  c->restore();
}

void paintOutline(SkCanvas* c, const Box& b, const ComputedStyle& s) {
  if (s.outlineWidth <= 0 || s.outlineColor.transparent()) return;
  SkRRect in = outsetRRect(b.outer, s.outlineOffset);
  SkRRect out = outsetRRect(b.outer, s.outlineOffset + s.outlineWidth);
  if (out.isEmpty()) return;
  SkPaint p;
  p.setAntiAlias(true);
  p.setColor4f(sk(s.outlineColor));
  if (s.outlineStyle == BorderStyle::Solid) {
    if (in.isEmpty()) drawRRect(c, out, p);
    else c->drawDRRect(out, in, p);
    return;
  }
  SkRRect mid = outsetRRect(b.outer, s.outlineOffset + s.outlineWidth / 2);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(s.outlineWidth);
  float iv[2] = {s.outlineWidth * 3, s.outlineWidth * 3};
  if (s.outlineStyle == BorderStyle::Dotted) {
    iv[0] = 0;
    iv[1] = s.outlineWidth * 2;
    p.setStrokeCap(SkPaint::kRound_Cap);
  }
  p.setPathEffect(SkDashPathEffect::Make(iv, 2, 0));
  c->drawRRect(mid, p);
}

// --------------------------------------------------------------- filters

sk_sp<SkColorFilter> matrixFilter(const float m[20]) { return SkColorFilters::Matrix(m); }

sk_sp<SkImageFilter> buildFilter(const std::vector<FilterOp>& ops) {
  sk_sp<SkImageFilter> f;
  for (const FilterOp& op : ops) {
    float a = op.amount;
    sk_sp<SkColorFilter> cf;
    switch (op.kind) {
      case FilterOp::Kind::Blur:
        if (a > 0) f = SkImageFilters::Blur(a, a, f);  // CSS blur() length is the std deviation
        continue;
      case FilterOp::Kind::DropShadow: {
        const Shadow& sh = op.shadow;
        f = SkImageFilters::DropShadow(sh.offsetX, sh.offsetY, sh.blur / 2, sh.blur / 2, sk(sh.color).toSkColor(), f);
        continue;
      }
      case FilterOp::Kind::Brightness: {
        float m[20] = {a, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Contrast: {
        float t = 0.5f - 0.5f * a;
        float m[20] = {a, 0, 0, 0, t, 0, a, 0, 0, t, 0, 0, a, 0, t, 0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Saturate: {
        float m[20] = {0.213f + 0.787f * a, 0.715f - 0.715f * a, 0.072f - 0.072f * a, 0, 0,
                       0.213f - 0.213f * a, 0.715f + 0.285f * a, 0.072f - 0.072f * a, 0, 0,
                       0.213f - 0.213f * a, 0.715f - 0.715f * a, 0.072f + 0.928f * a, 0, 0,
                       0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Grayscale: {
        float k = 1 - a;
        float m[20] = {0.2126f + 0.7874f * k, 0.7152f - 0.7152f * k, 0.0722f - 0.0722f * k, 0, 0,
                       0.2126f - 0.2126f * k, 0.7152f + 0.2848f * k, 0.0722f - 0.0722f * k, 0, 0,
                       0.2126f - 0.2126f * k, 0.7152f - 0.7152f * k, 0.0722f + 0.9278f * k, 0, 0,
                       0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Sepia: {
        float k = 1 - a;
        float m[20] = {0.393f + 0.607f * k, 0.769f - 0.769f * k, 0.189f - 0.189f * k, 0, 0,
                       0.349f - 0.349f * k, 0.686f + 0.314f * k, 0.168f - 0.168f * k, 0, 0,
                       0.272f - 0.272f * k, 0.534f - 0.534f * k, 0.131f + 0.869f * k, 0, 0,
                       0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Invert: {
        float s = 1 - 2 * a;
        float m[20] = {s, 0, 0, 0, a, 0, s, 0, 0, a, 0, 0, s, 0, a, 0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::Opacity: {
        float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, a, 0};
        cf = matrixFilter(m);
        break;
      }
      case FilterOp::Kind::HueRotate: {
        float r = a * kPi / 180.f, cs = std::cos(r), sn = std::sin(r);
        float m[20] = {0.213f + cs * 0.787f - sn * 0.213f, 0.715f - cs * 0.715f - sn * 0.715f,
                       0.072f - cs * 0.072f + sn * 0.928f, 0, 0,
                       0.213f - cs * 0.213f + sn * 0.143f, 0.715f + cs * 0.285f + sn * 0.140f,
                       0.072f - cs * 0.072f - sn * 0.283f, 0, 0,
                       0.213f - cs * 0.213f - sn * 0.787f, 0.715f - cs * 0.715f + sn * 0.715f,
                       0.072f + cs * 0.928f + sn * 0.072f, 0, 0,
                       0, 0, 0, 1, 0};
        cf = matrixFilter(m);
        break;
      }
    }
    if (cf) f = SkImageFilters::ColorFilter(cf, f);
  }
  return f;
}

SkBlendMode blendMode(uint8_t i) {
  static const SkBlendMode kModes[] = {
      SkBlendMode::kSrcOver,  SkBlendMode::kMultiply,   SkBlendMode::kScreen,    SkBlendMode::kOverlay,
      SkBlendMode::kDarken,   SkBlendMode::kLighten,    SkBlendMode::kColorDodge, SkBlendMode::kColorBurn,
      SkBlendMode::kHardLight, SkBlendMode::kSoftLight, SkBlendMode::kDifference, SkBlendMode::kExclusion,
      SkBlendMode::kHue,      SkBlendMode::kSaturation, SkBlendMode::kColor,     SkBlendMode::kLuminosity,
      SkBlendMode::kPlus};
  return i < sizeof(kModes) / sizeof(kModes[0]) ? kModes[i] : SkBlendMode::kSrcOver;
}

// --------------------------------------------------------------- painting

bool clipsChildren(const Node* n) {
  return n->style().overflow != Overflow::Visible || n->type() == NodeType::ScrollView ||
         n->type() == NodeType::Image;
}

bool paintsChildren(const Node* n) {
  return n->type() != NodeType::Text && n->type() != NodeType::RawText && n->type() != NodeType::TextInput;
}

bool backfaceFlipped(const ComputedStyle& s) {
  bool flipped = false;
  for (auto& op : s.transform) flipped ^= op.flipsBackface;
  return flipped;
}

// LayoutAnimation create / delete effects (1 when none).
float animOpacity(const Node* n) { return n->layoutAnim ? n->layoutAnim->opacity : 1.f; }
bool animScaled(const Node* n) {
  return n->layoutAnim && (n->layoutAnim->scaleX != 1 || n->layoutAnim->scaleY != 1);
}
// Scale about the border box center, outside the node's own transform.
void applyAnimScale(const Node* n, const Rect& r, SkMatrix* m) {
  m->preTranslate(r.width / 2, r.height / 2);
  m->preScale(n->layoutAnim->scaleX, n->layoutAnim->scaleY);
  m->preTranslate(-r.width / 2, -r.height / 2);
}

bool isPainted(const Node* n) {
  const ComputedStyle& s = n->style();
  if (n->layoutAnim && (n->layoutAnim->opacity <= 0 || n->layoutAnim->scaleX <= 0 || n->layoutAnim->scaleY <= 0))
    return false;
  return n->yoga() && s.display && s.opacity > 0 && !(s.backfaceHidden && backfaceFlipped(s));
}

// What the previous frame painted for a node, in device pixels. Comparing
// it with the current frame gives the damaged region (incremental repaint).
struct NodeRecord : Node::Cache {
  SkIRect own = SkIRect::MakeEmpty();      // the node's own painting
  SkIRect subtree = SkIRect::MakeEmpty();  // own + descendants (within overflow clips)
  SkMatrix matrix;                         // device matrix of the (snapped) border box
  uint64_t children = 0;                   // child list identity (insert / remove / reorder)
  uint32_t version = 0;
  uint32_t imageGen = 0;
  float w = -1, h = -1;
  SkIRect bars = SkIRect::MakeEmpty();     // ScrollView overlay indicators (device)
  float barAlpha = 0;
  bool painted = false;
  bool valid = false;
};

// A node with anti-aliased curved or diagonal outlines, in device pixels.
// Damage strictly inside `safe` (the padding box inset by a pixel) never
// crosses an outline; other damage touching `bounds` must include all of it.
struct CurvedNode {
  SkIRect bounds;
  SkRRect safe;
};

// Per-document painter state, kept on the root node.
struct RootRecord : NodeRecord {
  PaintStats stats;
  uint32_t fontGen = 0;
  std::vector<std::pair<float, sk_sp<SkMaskFilter>>> blurs;  // shadow mask filters by blur radius
  std::vector<Node*> order;                                  // zIndex sort scratch (reused)
  std::vector<CurvedNode> curved;                            // this frame's curved-outline nodes
  ShadowCache shadows;
};

ShadowCache& shadowCache(RootRecord* root) { return root->shadows; }

NodeRecord* record(Node* n) {
  if (!n->paintCache) n->paintCache = n->parent() ? std::make_unique<NodeRecord>() : std::make_unique<RootRecord>();
  return static_cast<NodeRecord*>(n->paintCache.get());
}

RootRecord* rootRecord(Document& doc) { return static_cast<RootRecord*>(record(doc.root())); }

struct Ctx {
  Document& doc;
  RootRecord* root;
  const SkRegion* damage;  // null: paint everything
  PaintStats stats;
  // Snap space of the children being painted (see place()): the current
  // parent's unsnapped border-box coordinates to device pixels, without any
  // transform.
  SkMatrix snapSpace;
  bool transformed = false;  // a transform applies (node or ancestor)
};

sk_sp<SkMaskFilter> cachedBlur(RootRecord* root, float blur) {
  if (blur <= 0) return nullptr;
  for (auto& e : root->blurs)
    if (e.first == blur) return e.second;
  sk_sp<SkMaskFilter> f = blurFilter(blur);
  if (root->blurs.size() < 64) root->blurs.emplace_back(blur, f);
  return f;
}

void paintNode(Node* n, SkCanvas* c, Ctx& ctx);

// The node's own painting bounds (shadows, outline, overflowing text) in its
// border-box coordinates, without descendants.
SkRect ownVisualBounds(Node* n, float w, float h) {
  const ComputedStyle& s = n->style();
  SkRect r = SkRect::MakeWH(w, h);
  for (auto& sh : s.boxShadows) {
    if (sh.inset) continue;
    float e = sh.blur * 1.5f + std::max(0.f, sh.spread);
    r.join(SkRect::MakeWH(w, h).makeOffset(sh.offsetX, sh.offsetY).makeOutset(e, e));
  }
  if (s.outlineWidth > 0) {
    float e = s.outlineWidth + std::max(0.f, s.outlineOffset);
    r.join(SkRect::MakeWH(w, h).makeOutset(e, e));
  }
  if (n->type() == NodeType::Text) {
    // Text may overflow its box (long words, shadows): be generous.
    float e = s.text.fontSize.value_or(14);
    for (auto& sh : s.text.textShadows) e = std::max(e, sh.blur * 1.5f + std::fabs(sh.offsetX) + std::fabs(sh.offsetY));
    r.join(SkRect::MakeWH(w, h).makeOutset(e, e));
    if (auto* p = text::layoutParagraph(n, std::max(0.f, w))) r.join(SkRect::MakeWH(p->getLongestLine(), p->getHeight()).makeOutset(e, e));
  }
  return r;
}

// How far filters (blur, drop-shadow) spread painting beyond the content.
float filterOutset(const ComputedStyle& s) {
  float e = 0;
  for (const FilterOp& op : s.filters) {
    if (op.kind == FilterOp::Kind::Blur) e += op.amount * 3;
    else if (op.kind == FilterOp::Kind::DropShadow)
      e += op.shadow.blur * 1.5f + std::max(std::fabs(op.shadow.offsetX), std::fabs(op.shadow.offsetY));
  }
  return e;
}

// Conservative visual bounds of a node's painting (incl. descendants) in its
// own border-box coordinates; used to size opacity / filter layers.
SkRect visualBounds(Node* n) {
  Rect vr = n->visualRect();
  float w = vr.width, h = vr.height;
  SkRect r = ownVisualBounds(n, w, h);
  if (paintsChildren(n) && !clipsChildren(n)) {
    for (Node* ch : n->children()) {
      if (!ch->yoga() || !ch->style().display) continue;
      SkRect cb = visualBounds(ch);
      Rect lr = ch->visualRect();
      SkMatrix m = SkMatrix::Translate(lr.x, lr.y);
      if (animScaled(ch)) applyAnimScale(ch, lr, &m);
      m.preConcat(localTransform(*ch));
      r.join(m.mapRect(cb));
    }
  }
  float e = filterOutset(n->style());
  if (e > 0) r.outset(e, e);
  return r;
}

// Pixel-snaps the border box like Chrome: edges round to pixels
// (round(x), round(x + w)), so boxes stay crisp at fractional layout
// positions. `m` maps the unsnapped border-box origin to the snapping grid;
// returns the snap offset (local units) and the snapped size.
SkVector snapBox(const SkMatrix& m, const Rect& r, float* w, float* h) {
  *w = r.width;
  *h = r.height;
  if (!m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0) return {0, 0};
  float sx = m.getScaleX(), sy = m.getScaleY();
  float dx = m.getTranslateX(), dy = m.getTranslateY();
  float x0 = std::floor(dx + 0.5f), y0 = std::floor(dy + 0.5f);
  *w = (std::floor(dx + r.width * sx + 0.5f) - x0) / sx;
  *h = (std::floor(dy + r.height * sy + 0.5f) - y0) / sy;
  return {(x0 - dx) / sx, (y0 - dy) / sy};
}

// Where a node paints, relative to its parent's unsnapped border box.
//
// Snapping happens in layout space, before the node's own transform and
// LayoutAnimation scale, like Chrome (paint offsets snap in the transform's
// local space; the transform then applies as is). `parentSnap` maps the
// parent's border-box coordinates to device pixels without any transform,
// so a translateX of 0.37 moves a box and its text together by 0.37 device
// pixels. Snapping after the transform (in device space) made boxes step by
// whole pixels while their subpixel-positioned text glided: text wiggled
// inside its box during spring animations.
struct Placement {
  SkMatrix local;      // parent coords -> the node's snapped border-box coords
  SkMatrix snapSpace;  // snap space of the node's children (unsnapped origin)
  SkVector snap;       // offset to undo the snap, in the node's local units
  float w, h;          // snapped border-box size
  bool transformed;    // the node has its own transform (or LayoutAnimation scale)
};

Placement place(const Node* n, const SkMatrix& parentSnap) {
  Placement p;
  Rect r = n->visualRect();
  p.snapSpace = parentSnap;
  p.snapSpace.preTranslate(r.x, r.y);
  SkVector pre = snapBox(p.snapSpace, r, &p.w, &p.h);
  SkMatrix t;  // own transform: LayoutAnimation scale, then style transform
  if (animScaled(n)) applyAnimScale(n, r, &t);
  if (!n->style().transform.empty()) t.preConcat(localTransform(*n));
  p.transformed = !t.isIdentity();
  SkMatrix inv;
  if (!t.invert(&inv)) {
    pre = {0, 0};
    p.w = r.width;
    p.h = r.height;
  }
  // translate(pre) . t == t . translate(t^-1 pre): the snap expressed after
  // the transform, which is what paintContent undoes for text and children.
  p.snap = inv.mapVector(pre.fX, pre.fY);
  p.local = SkMatrix::Translate(r.x + pre.fX, r.y + pre.fY);
  p.local.preConcat(t);
  return p;
}

void paintChildren(Node* n, SkCanvas* c, Ctx& ctx) {
  // Children fading out (LayoutAnimation delete) under the live ones.
  for (Node* g : n->ghosts) paintNode(g, c, ctx);
  const auto& kids = n->children();
  bool sort = false;
  for (Node* ch : kids) sort |= ch->style().hasZIndex && ch->style().zIndex != 0;
  if (!sort) {
    for (Node* ch : kids) paintNode(ch, c, ctx);
    return;
  }
  // Stable sort by zIndex (negative first), document order otherwise. The
  // scratch vector is shared by nested sorts (each uses its own range) and
  // keeps its capacity across frames.
  std::vector<Node*>& order = ctx.root->order;
  size_t begin = order.size();
  order.insert(order.end(), kids.begin(), kids.end());
  std::stable_sort(order.begin() + long(begin), order.end(),
                   [](Node* a, Node* b) { return a->style().zIndex < b->style().zIndex; });
  for (size_t i = begin; i < begin + kids.size(); i++) paintNode(order[i], c, ctx);
  order.resize(begin);
}

// Overlay scroll indicators of a ScrollView, above its content, inside the
// padding box. Fading is driven by Node::scrollIndicatorOpacity.
void paintScrollIndicators(Node* n, SkCanvas* c, const Box& b, float w, float h) {
  float alpha = std::clamp(n->scrollIndicatorOpacity, 0.f, 1.f);
  if (alpha <= 0) return;
  ScrollIndicators si = scrollIndicators(n, w, h);
  if (!si.vertical && !si.horizontal) return;
  c->save();
  clipRRect(c, b.inner);
  SkPaint fill, stroke;
  fill.setAntiAlias(true);
  fill.setColor(si.fill);
  fill.setAlphaf(fill.getAlphaf() * alpha);
  stroke.setAntiAlias(true);
  stroke.setStyle(SkPaint::kStroke_Style);
  stroke.setStrokeWidth(1);
  stroke.setColor(si.stroke);
  stroke.setAlphaf(stroke.getAlphaf() * alpha);
  for (int i = 0; i < 2; i++) {
    if (!(i == 0 ? si.vertical : si.horizontal)) continue;
    const Rect& r = i == 0 ? si.v : si.h;
    SkRRect rr = SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.width, r.height), si.radius, si.radius);
    if (si.stroke) {
      SkRRect rim = rr;
      rim.outset(0.5f, 0.5f);
      c->drawRRect(rim, stroke);
    }
    c->drawRRect(rr, fill);
  }
  c->restore();
}

// Device bounds of the visible scroll indicators (empty when hidden).
SkIRect scrollIndicatorBounds(Node* n, const SkMatrix& m, float w, float h, const SkIRect& clip) {
  if (n->type() != NodeType::ScrollView || n->scrollIndicatorOpacity <= 0) return SkIRect::MakeEmpty();
  ScrollIndicators si = scrollIndicators(n, w, h);
  SkIRect out = SkIRect::MakeEmpty();
  if (si.vertical) out.join(m.mapRect(SkRect::MakeXYWH(si.v.x, si.v.y, si.v.width, si.v.height)).roundOut().makeOutset(2, 2));
  if (si.horizontal) out.join(m.mapRect(SkRect::MakeXYWH(si.h.x, si.h.y, si.h.width, si.h.height)).roundOut().makeOutset(2, 2));
  if (!out.intersect(clip)) out.setEmpty();
  return out;
}

// Draws the node's own decorations, content and children in its border-box
// coordinate space. `snap` is the offset from the layout position to the
// pixel-snapped one (already applied to the canvas); `w`, `h` the snapped size.
void paintContent(Node* n, SkCanvas* c, Ctx& ctx, SkVector snap, float w, float h) {
  const ComputedStyle& s = n->style();
  Box b;
  computeBox(n, w, h, &b);
  ctx.stats.nodesPainted++;
  if (!s.boxShadows.empty()) paintOuterShadows(c, b, s, ctx.root);
  paintBackground(c, b, s);
  if (!s.boxShadows.empty()) paintInsetShadows(c, b, s, ctx.root);
  paintBorders(c, b, s);

  // Text uses the unsnapped layout box, like Chrome's text fragments: a
  // snapped (narrower) width would re-wrap a max-content paragraph.
  if (n->type() == NodeType::Text || n->type() == NodeType::TextInput) {
    float dw = YGNodeLayoutGetWidth(n->yoga()) - w, dh = YGNodeLayoutGetHeight(n->yoga()) - h;
    float x = b.content.left() - snap.fX, y = b.content.top() - snap.fY;
    float cw = std::max(0.f, b.content.width() + dw), ch = std::max(0.f, b.content.height() + dh);
    // Under a transform the text snaps where its box does: in layout space.
    SkMatrix layoutSpace = ctx.snapSpace;
    layoutSpace.preTranslate(snap.fX, snap.fY);
    const SkMatrix* ls = ctx.transformed ? &layoutSpace : nullptr;
    if (n->type() == NodeType::Text) text::paintText(n, c, x, y, cw, ch, ls);
    else text::paintTextInput(n, c, x, y, cw, ch, ls);
  }
  if (n->type() == NodeType::Image) image::paint(n, c, b.inner.rect(), b.inner);

  if (paintsChildren(n) && (!n->children().empty() || !n->ghosts.empty())) {
    bool clip = clipsChildren(n);
    bool scroll = n->type() == NodeType::ScrollView && (n->scrollX != 0 || n->scrollY != 0);
    c->save();
    if (clip) clipRRect(c, b.inner);
    // Children snap from their own unsnapped positions.
    c->translate(-snap.fX, -snap.fY);
    SkMatrix snapSpace = ctx.snapSpace;  // this node's (set by paintNode)
    if (scroll) {
      c->translate(-n->scrollX, -n->scrollY);
      ctx.snapSpace.preTranslate(-n->scrollX, -n->scrollY);
    }
    paintChildren(n, c, ctx);
    ctx.snapSpace = snapSpace;
    c->restore();
  }
  if (n->type() == NodeType::ScrollView) paintScrollIndicators(n, c, b, w, h);
  paintOutline(c, b, s);
}

void paintNode(Node* n, SkCanvas* c, Ctx& ctx) {
  if (!isPainted(n)) return;
  // Incremental repaint: skip subtrees entirely outside the damage. Their
  // records were refreshed by updateDamage() this frame.
  if (ctx.damage) {
    auto* rec = static_cast<NodeRecord*>(n->paintCache.get());
    if (rec && rec->valid && !ctx.damage->intersects(rec->subtree)) {
      ctx.stats.nodesCulled++;
      return;
    }
  }
  const ComputedStyle& s = n->style();
  Placement p = place(n, ctx.snapSpace);
  int saveCount = c->save();
  c->concat(p.local);
  float opacity = s.opacity * animOpacity(n);
  bool layer = opacity < 1 || !s.filters.empty() || s.blendMode != 0;
  if (layer) {
    SkPaint lp;
    lp.setAlphaf(opacity);
    if (!s.filters.empty()) lp.setImageFilter(buildFilter(s.filters));
    if (s.blendMode) lp.setBlendMode(blendMode(s.blendMode));
    SkRect bounds = visualBounds(n);
    c->saveLayer(&bounds, &lp);
  }
  SkMatrix parentSnap = ctx.snapSpace;
  bool parentTransformed = ctx.transformed;
  ctx.snapSpace = p.snapSpace;
  ctx.transformed |= p.transformed;
  paintContent(n, c, ctx, p.snap, p.w, p.h);
  ctx.snapSpace = parentSnap;
  ctx.transformed = parentTransformed;
  c->restoreToCount(saveCount);
}

SkIRect deviceBounds(const SkMatrix& m, const SkRect& local) {
  // +1: anti-aliased edges and blur tails may touch the next pixel.
  return m.mapRect(local).roundOut().makeOutset(1, 1);
}

uint64_t childListHash(const Node* n) {
  uint64_t h = 1469598103934665603ull;
  for (const Node* c : n->children()) h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(c))) * 1099511628211ull;
  for (const Node* c : n->ghosts) h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(c)) ^ 0x9e37ull) * 1099511628211ull;
  return h ^ (n->children().size() + (uint64_t(n->ghosts.size()) << 20));
}

// Nodes with anti-aliased curved or diagonal outlines (radii, rotation,
// dashes, sharp shadows). Skia chops such outlines at the clip, which shifts
// edge coverage by a few levels; an incremental repaint therefore never cuts
// through one (see updateDamage). Returns the outline's local bounds, empty
// if the node has none. Blurred shadows are exempt: their masks are built
// with a blur-sized margin beyond the clip.
SkRect curvedOutline(const Node* n, const SkMatrix& m, float w, float h) {
  const ComputedStyle& s = n->style();
  bool curved = !m.isScaleTranslate() || s.borderStyle != BorderStyle::Solid;
  for (const Length& r : s.borderRadius) curved |= r.value > 0;
  SkRect r = SkRect::MakeWH(w, h);
  if (s.outlineWidth > 0) {
    curved |= s.outlineStyle != BorderStyle::Solid;
    float e = s.outlineWidth + std::max(0.f, s.outlineOffset);
    r.outset(e, e);
  }
  for (const Shadow& sh : s.boxShadows) {
    if (sh.inset || sh.blur > 0) continue;
    curved = true;
    r.join(SkRect::MakeWH(w, h).makeOffset(sh.offsetX, sh.offsetY).makeOutset(std::max(0.f, sh.spread), std::max(0.f, sh.spread)));
  }
  return curved ? r : SkRect::MakeEmpty();
}

// Walks the tree like paintNode (same matrices and snapping), refreshes each
// node's record and adds what changed since the last frame to `damage`.
// Returns the node's device subtree bounds.
SkIRect updateRecords(Node* n, const SkMatrix& parent, const SkMatrix& parentSnap, const SkIRect& clip,
                      uint32_t imageGen, SkRegion* damage, std::vector<CurvedNode>* curved) {
  if (!n->yoga()) return SkIRect::MakeEmpty();  // raw text: painted by its paragraph
  NodeRecord* rec = record(n);
  SkIRect own = SkIRect::MakeEmpty(), sub = SkIRect::MakeEmpty(), bars = SkIRect::MakeEmpty();
  SkMatrix m = parent;
  float w = 0, h = 0;
  bool painted = isPainted(n);
  if (painted) {
    const ComputedStyle& s = n->style();
    Placement p = place(n, parentSnap);
    m.preConcat(p.local);
    w = p.w;
    h = p.h;
    if (!own.intersect(deviceBounds(m, ownVisualBounds(n, w, h)), clip)) own.setEmpty();
    if (!own.isEmpty()) {
      SkRect co = curvedOutline(n, m, w, h);
      SkIRect ci;
      if (!co.isEmpty() && ci.intersect(deviceBounds(m, co), clip)) {
        CurvedNode cn{ci, SkRRect::MakeEmpty()};
        // Outlines and sharp shadows lie outside the box: no safe interior.
        bool outside = s.outlineWidth > 0;
        for (const Shadow& sh : s.boxShadows) outside |= !sh.inset && sh.blur <= 0;
        if (!outside && s.borderStyle == BorderStyle::Solid && m.isScaleTranslate()) {
          Box b;
          computeBox(n, w, h, &b);
          SkRRect dev;
          if (b.inner.transform(m, &dev)) {
            dev.inset(1, 1);
            cn.safe = dev;
          }
        }
        curved->push_back(cn);
      }
    }
    bars = scrollIndicatorBounds(n, m, w, h, clip);
    if (!bars.isEmpty()) {
      own.join(bars);
      curved->push_back({bars, SkRRect::MakeEmpty()});  // rounded thumbs: never cut
    }
    sub = own;
    if (paintsChildren(n) && (!n->children().empty() || !n->ghosts.empty())) {
      SkIRect childClip = clip;
      if (clipsChildren(n) && !childClip.intersect(deviceBounds(m, SkRect::MakeWH(w, h)))) childClip.setEmpty();
      SkMatrix cm = m, cs = p.snapSpace;
      cm.preTranslate(-p.snap.fX, -p.snap.fY);
      if (n->type() == NodeType::ScrollView) {
        cm.preTranslate(-n->scrollX, -n->scrollY);
        cs.preTranslate(-n->scrollX, -n->scrollY);
      }
      for (Node* ch : n->ghosts) sub.join(updateRecords(ch, cm, cs, childClip, imageGen, damage, curved));
      for (Node* ch : n->children()) sub.join(updateRecords(ch, cm, cs, childClip, imageGen, damage, curved));
    }
    float fe = filterOutset(s);
    if (fe > 0 && !sub.isEmpty()) {
      int e = int(std::ceil(fe * std::max(1.f, m.getMaxScale())));
      SkIRect grown = sub.makeOutset(e, e);
      if (!sub.intersect(grown, clip)) sub.setEmpty();
    }
  }
  // TextInput caret blink / move: only the caret's old and new rects.
  SkRect caretA, caretB;
  if (n->type() == NodeType::TextInput && text::inputCaretDamage(n, &caretA, &caretB) && painted) {
    for (const SkRect& cr : {caretA, caretB}) {
      SkIRect dr;
      if (!cr.isEmpty() && dr.intersect(deviceBounds(m, cr.makeOutset(1, 1)), clip)) damage->op(dr, SkRegion::kUnion_Op);
    }
  }
  // A hidden node's descendants keep their old records; the check below
  // repaints where the subtree was.
  uint64_t kids = childListHash(n);
  uint32_t gen = n->type() == NodeType::Image ? imageGen : 0;
  bool contentChanged = !rec->valid || rec->version != n->paintVersion || rec->painted != painted || rec->w != w ||
                        rec->h != h || rec->children != kids || rec->imageGen != gen;
  if (contentChanged) {
    // Paint state of the node itself changed: it may affect the whole
    // subtree (opacity, clip, transform of a layer...).
    damage->op(rec->subtree, SkRegion::kUnion_Op);
    damage->op(sub, SkRegion::kUnion_Op);
  } else if (rec->matrix != m) {
    damage->op(rec->own, SkRegion::kUnion_Op);
    damage->op(own, SkRegion::kUnion_Op);
  }
  float barAlpha = bars.isEmpty() ? 0 : n->scrollIndicatorOpacity;
  if (rec->bars != bars || rec->barAlpha != barAlpha) {
    // Scroll indicators moved or faded: only their rects repaint.
    damage->op(rec->bars, SkRegion::kUnion_Op);
    damage->op(bars, SkRegion::kUnion_Op);
  }
  rec->bars = bars;
  rec->barAlpha = barAlpha;
  rec->own = own;
  rec->subtree = sub;
  rec->matrix = m;
  rec->children = kids;
  rec->version = n->paintVersion;
  rec->imageGen = gen;
  rec->w = w;
  rec->h = h;
  rec->painted = painted;
  rec->valid = true;
  return sub;
}

}  // namespace

SkMatrix localTransform(const Node& n) {
  const ComputedStyle& s = n.style();
  if (s.transform.empty() || !n.yoga()) return SkMatrix::I();
  Rect vr = n.visualRect();
  float w = vr.width, h = vr.height;
  float ox = s.transformOrigin[0].resolve(w), oy = s.transformOrigin[1].resolve(h);
  SkMatrix m = SkMatrix::Translate(ox, oy);
  for (const TransformOp& op : s.transform) {
    switch (op.kind) {
      case TransformOp::Kind::Translate:
        m.preTranslate(op.percentA ? op.a * w / 100.f : op.a, op.percentB ? op.b * h / 100.f : op.b);
        break;
      case TransformOp::Kind::Scale: m.preScale(op.a, op.b); break;
      case TransformOp::Kind::Rotate: m.preRotate(op.a); break;
      case TransformOp::Kind::SkewX: m.preSkew(std::tan(op.a * kPi / 180.f), 0); break;
      case TransformOp::Kind::SkewY: m.preSkew(0, std::tan(op.a * kPi / 180.f)); break;
      case TransformOp::Kind::Matrix:
        m.preConcat(SkMatrix::MakeAll(op.m[0], op.m[2], op.m[4], op.m[1], op.m[3], op.m[5], 0, 0, 1));
        break;
    }
  }
  m.preTranslate(-ox, -oy);
  return m;
}

SkRegion updateDamage(Document& doc, int deviceWidth, int deviceHeight) {
  RootRecord* root = rootRecord(doc);
  SkRegion damage;
  SkIRect screen = SkIRect::MakeWH(deviceWidth, deviceHeight);
  // New fonts change glyphs without changing any node: repaint everything.
  uint32_t fontGen = doc.fonts ? doc.fonts->generation : 0;
  if (fontGen != root->fontGen) {
    root->fontGen = fontGen;
    damage.setRect(screen);
  }
  uint32_t imageGen = doc.images ? doc.images->generation : 0;
  SkMatrix m = SkMatrix::Scale(doc.pixelRatio(), doc.pixelRatio());
  root->curved.clear();
  updateRecords(doc.root(), m, m, screen, imageGen, &damage, &root->curved);
  // Grow the damage until it contains every curved node it touches, so no
  // such outline is cut by the clip (pixels then match a full repaint).
  if (!damage.isEmpty()) {
    for (int pass = 0; pass < 16; pass++) {
      bool grew = false;
      for (const CurvedNode& cn : root->curved) {
        if (!damage.intersects(cn.bounds) || damage.contains(cn.bounds)) continue;
        bool cuts = false;
        for (SkRegion::Iterator it(damage); !it.done() && !cuts; it.next()) {
          const SkIRect& d = it.rect();
          cuts = SkIRect::Intersects(d, cn.bounds) && !cn.safe.contains(SkRect::Make(d));
        }
        if (cuts) {
          damage.op(cn.bounds, SkRegion::kUnion_Op);
          grew = true;
        }
      }
      if (!grew) break;
      if (pass == 15) damage.setRect(screen);
    }
  }
  damage.op(screen, SkRegion::kIntersect_Op);
  return damage;
}

void paintDocument(Document& doc, SkCanvas* canvas, const SkRegion* damage) {
  RootRecord* root = rootRecord(doc);
  root->shadows.frame++;
  Ctx ctx{doc, root, damage, {}, {}};
  canvas->save();
  canvas->scale(doc.pixelRatio(), doc.pixelRatio());
  ctx.snapSpace = canvas->getTotalMatrix();
  paintNode(doc.root(), canvas, ctx);
  canvas->restore();
  if (damage) {
    const SkIRect& b = damage->getBounds();
    ctx.stats.damagedPixels = int64_t(b.width()) * b.height();
  }
  root->stats = ctx.stats;
}

SkSurfaceProps surfaceProps() { return SkSurfaceProps(0, kUnknown_SkPixelGeometry, 0.2f, 1.2f); }

PaintStats lastPaintStats(const Document& doc) {
  auto* rs = static_cast<RootRecord*>(doc.root()->paintCache.get());
  return rs ? rs->stats : PaintStats{};
}

}  // namespace jsui
