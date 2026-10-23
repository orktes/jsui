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
#include <atomic>
#include <cstring>
#include <memory>
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
#if defined(SK_GRAPHITE) && __has_include("include/gpu/graphite/Surface.h")
#include "include/gpu/graphite/Surface.h"  // SkSurfaces::AsImage (no copy)
#define JSUI_GRAPHITE_AS_IMAGE 1
#endif
#include "include/effects/SkDashPathEffect.h"
#if __has_include("include/effects/SkGradient.h")
#include "include/effects/SkGradient.h"  // Skia m157+ (the WebGPU wasm build)
#define JSUI_SKGRADIENT 1
#else
#include "include/effects/SkGradientShader.h"
#endif
#include "include/effects/SkImageFilters.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "render/image_cache.h"
#include "render/image_node.h"
#include "render/paint_counter.h"
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

// ------------------------------------------------------------- sprites

// Raster sprites of expensive draws that stay the same from frame to frame
// (gradient backgrounds, scaled images): the CPU raster pipeline evaluates a
// multi-stop gradient or a bilinear, mipmapped image for every pixel, every
// time a pixel is repainted, while blitting a sprite is a load and a blend.
// A sprite holds what the draw paints at a device scale, with the canvas
// translation factored out, so it is reused while the node moves by whole
// pixels. Whether a draw goes through a sprite depends only on the draw and
// the canvas (never on history), so a frame paints the same pixels whether
// the draw was cached or not: incremental frames match full ones.
struct SpriteKey {
  std::string bytes;
  SpriteKey& add(const void* p, size_t n) {
    bytes.append(static_cast<const char*>(p), n);
    return *this;
  }
  SpriteKey& f(float v) { return add(&v, sizeof v); }
  SpriteKey& u(uint64_t v) { return add(&v, sizeof v); }
  SpriteKey& rect(const SkRect& r) { return f(r.fLeft).f(r.fTop).f(r.fRight).f(r.fBottom); }
  SpriteKey& rrect(const SkRRect& r) {
    rect(r.rect());
    for (int i = 0; i < 4; i++) {
      SkVector v = r.radii(SkRRect::Corner(i));
      f(v.fX).f(v.fY);
    }
    return *this;
  }
};
struct Sprite {
  sk_sp<SkImage> image;
  SkIPoint origin;  // device offset of the image from the canvas translation
  uint32_t lastUsed = 0;
};
struct SpriteCache {
  std::unordered_map<std::string, Sprite> sprites;
  size_t bytes = 0;
  uint32_t frame = 0;
  // The paintDocument target: sprites only for raster canvases of 8-bit
  // colour (GPU backends evaluate gradients and images in shaders).
  bool raster = false;
  SkColorType colorType = kUnknown_SkColorType;
  sk_sp<SkColorSpace> colorSpace;
  int draws = 0, renders = 0;  // this paint's sprite draws / of which rendered
  static constexpr size_t kMaxBytes = 48u << 20;
  static constexpr int64_t kMinPixels = 128 * 128;  // smaller draws are cheap enough directly
  static constexpr int64_t kMaxPixels = 2200000;    // per sprite (a full 1080p screen)
};
struct RootRecord;
SpriteCache& spriteCache(RootRecord* root);

// Paints `draw(canvas)` (local coordinates, painting nothing outside
// `bounds`) through a sprite when the canvas allows it, at `alpha`; false
// when it does not (the caller draws directly).
template <class F>
bool drawSprite(SkCanvas* c, RootRecord* root, const SkRect& bounds, SpriteKey& key, F&& draw, float alpha = 1) {
  SpriteCache& sc = spriteCache(root);
  if (!sc.raster) return false;
  const SkMatrix& m = c->getTotalMatrix();
  if (!m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0) return false;
  // Whole-pixel translations only: a sprite keyed by a sub-pixel phase would
  // be re-rendered every frame for content that moves smoothly (measured:
  // more expensive than drawing directly).
  float tx = std::round(m.getTranslateX()), ty = std::round(m.getTranslateY());
  if (std::fabs(m.getTranslateX() - tx) > 1e-3f || std::fabs(m.getTranslateY() - ty) > 1e-3f) return false;
  SkMatrix scale = SkMatrix::Scale(m.getScaleX(), m.getScaleY());
  SkIRect area = scale.mapRect(bounds).roundOut().makeOutset(1, 1);
  int64_t px = int64_t(area.width()) * area.height();
  if (px < SpriteCache::kMinPixels || px > SpriteCache::kMaxPixels) return false;
  if (c->quickReject(bounds)) return true;
  key.f(m.getScaleX()).f(m.getScaleY()).rect(bounds);
  auto it = sc.sprites.find(key.bytes);
  if (it == sc.sprites.end()) {
    SkImageInfo info =
        SkImageInfo::Make(area.width(), area.height(), sc.colorType, kPremul_SkAlphaType, sc.colorSpace);
    SkSurfaceProps props = surfaceProps();
    sk_sp<SkSurface> surf = SkSurfaces::Raster(info, &props);
    if (!surf) return false;
    SkCanvas* oc = surf->getCanvas();
    oc->clear(SK_ColorTRANSPARENT);
    oc->translate(float(-area.left()), float(-area.top()));
    oc->concat(scale);
    draw(oc);
    Sprite sp;
    sp.image = surf->makeImageSnapshot();
    sp.origin = {area.left(), area.top()};
    sc.bytes += size_t(px) * 4;
    if (sc.bytes > SpriteCache::kMaxBytes) {
      // Drop sprites unused for a while (content that changed or left).
      for (auto e = sc.sprites.begin(); e != sc.sprites.end();) {
        if (sc.frame - e->second.lastUsed > 2) {
          sc.bytes -= size_t(e->second.image->width()) * e->second.image->height() * 4;
          e = sc.sprites.erase(e);
        } else {
          ++e;
        }
      }
    }
    it = sc.sprites.emplace(std::move(key.bytes), std::move(sp)).first;
    sc.renders++;
  }
  sc.draws++;
  it->second.lastUsed = sc.frame;
  c->save();
  c->setMatrix(SkMatrix::I());
  SkPaint p;
  p.setAlphaf(alpha);
  c->drawImage(it->second.image.get(), tx + float(it->second.origin.fX), ty + float(it->second.origin.fY),
               SkSamplingOptions(), &p);
  c->restore();
  return true;
}

// ------------------------------------------------------------- shadows

sk_sp<SkMaskFilter> blurFilter(float blur) {
  return blur > 0 ? SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2) : nullptr;  // CSS: sigma = blur / 2
}

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
  float inset;                // 1: an inset shadow (w / h / radii: the padding box it is clipped to)
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
  using Map = std::unordered_map<ShadowKey, ShadowMask, ShadowKeyHash>;
  Map::iterator insert(const ShadowKey& key, ShadowMask&& m) {
    bytes += size_t(m.image->width()) * m.image->height() * 4;
    if (bytes > kMaxBytes) {
      // Drop masks unused for a while (shapes that animated away).
      for (auto e = masks.begin(); e != masks.end();) {
        if (frame - e->second.lastUsed > 2) {
          bytes -= size_t(e->second.image->width()) * e->second.image->height() * 4;
          e = masks.erase(e);
        } else {
          ++e;
        }
      }
    }
    return masks.emplace(key, std::move(m)).first;
  }
};
ShadowCache& shadowCache(RootRecord* root);

// GPU canvases (Graphite, Ganesh) draw shadow masks at their sub-pixel
// position with bilinear filtering instead of keying them by it: a mask is a
// texture there, and a shadow gliding across the screen (a panel sliding in,
// a tab underline springing) made a new one every frame (rasterised and
// blurred on the CPU, a texture created and uploaded). A blurred mask shifted
// by a fraction of a pixel is the mask rendered there to within rounding
// (sigma >= 1 px), and a straight cut-out edge is too (its coverage is
// linear in the phase). The CPU raster keeps exact phases (whole-pixel
// sprite blits are its fast path).
bool phaseFreeMasks(SkCanvas* c) { return c->recorder() != nullptr || c->recordingContext() != nullptr; }

// While a node's scale animates (a pulsing dot, a button springing) or its
// size does (a progress bar's fill), its shadow's device size changes every
// frame: a new mask (blurred on the CPU, a texture created and uploaded) per
// frame. On GPU canvases such a shadow is drawn from a mask of a nearby size
// (8 steps per octave), scaled to the node with bilinear filtering:
//   * scaling: the mask of the node at the nearest quantized scale. A blurred
//     shape scaled is the shape scaled then blurred with the sigma scaled, so
//     the geometry and blur land where they belong; only resampling differs
//     (a few levels at the cut-out edge).
//   * resizing: the mask of the box at the nearest quantized width / height
//     (same corner radii and blur), stretched to the box, which scales the
//     blur and radii along that axis by at most 4.4%.
// The node is repainted exactly once it stops changing
// (NodeRecord::shadowInexact).
float quantizedScale(float s) { return s > 0 ? std::exp2(std::round(std::log2(s) * 8) / 8) : s; }
// Mask key values of a resized box: float noise in the derived sizes must
// not make keys differ.
float keyRound(float v) { return std::round(v * 64) / 64; }

// Draws `shape` (local coords) blurred by CSS `blur` with `paint`'s color
// from the cache, minus `hole` (the border box, when the background does not
// hide the shadow; baking it into the mask avoids an anti-aliased clip, which
// is slow on the CPU). False when the transform is not axis-aligned (the
// caller blurs directly then).
bool drawCachedShadow(SkCanvas* c, const SkRRect& shape, const SkRRect* hole, const SkRRect& box, float blur,
                      const SkPaint& paint, ShadowCache& cache, float alpha = 1, bool scaling = false,
                      bool resizing = false, bool* inexact = nullptr) {
  SkMatrix m = c->getTotalMatrix();
  if (!m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0) return false;
  const bool phaseFree = phaseFreeMasks(c);
  // The mask's device space -> the canvas' device space (see quantizedScale).
  SkMatrix post = SkMatrix::I();
  if (scaling && phaseFree) {
    float sx = m.getScaleX(), sy = m.getScaleY(), qx = quantizedScale(sx), qy = quantizedScale(sy);
    if (qx != sx || qy != sy) {
      float tx = m.getTranslateX(), ty = m.getTranslateY();
      m = SkMatrix::MakeAll(qx, 0, tx, 0, qy, ty, 0, 0, 1);
      post = SkMatrix::Translate(tx, ty);
      post.preScale(sx / qx, sy / qy);
      post.preTranslate(-tx, -ty);
      if (inexact) *inexact = true;
    }
  }
  SkRRect dev;
  if (!shape.transform(m, &dev)) return false;
  SkRRect devHole;
  if (hole && !hole->transform(m, &devHole)) return false;
  if (resizing && phaseFree && post.isIdentity()) {
    SkRect d = dev.rect();
    float qw = keyRound(quantizedScale(d.width())), qh = keyRound(quantizedScale(d.height()));
    if (qw != d.width() || qh != d.height()) {
      SkVector radii[4];
      for (int i = 0; i < 4; i++) radii[i] = dev.radii(SkRRect::Corner(i));
      dev.setRectRadii(SkRect::MakeXYWH(d.left(), d.top(), qw, qh), radii);
      if (hole) {
        // The same distances to the box's edges.
        SkRect hr = devHole.rect();
        SkVector hradii[4];
        for (int i = 0; i < 4; i++) hradii[i] = devHole.radii(SkRRect::Corner(i));
        hr = SkRect::MakeLTRB(hr.left(), hr.top(), hr.right() + qw - d.width(), hr.bottom() + qh - d.height());
        devHole.setRectRadii(hr, hradii);
      }
      post = SkMatrix::Translate(d.left(), d.top());
      post.preScale(d.width() / qw, d.height() / qh);
      post.preTranslate(-d.left(), -d.top());
      if (inexact) *inexact = true;
    }
  }
  const bool roundKey = !post.isIdentity();
  const SkRect& r = dev.rect();
  float ox = phaseFree ? r.left() : std::floor(r.left()), oy = phaseFree ? r.top() : std::floor(r.top());
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
  if (hole) {
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
  if (roundKey) {
    for (float* v : {&key.w, &key.h, &key.sigma}) *v = keyRound(*v);
    for (float& v : key.radii) v = keyRound(v);
    for (float& v : key.hole) v = keyRound(v);
    for (float& v : key.holeRadii) v = keyRound(v);
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
    it = cache.insert(key, std::move(sm));
  }
  it->second.lastUsed = cache.frame;
  SkPaint p;  // colour is baked in (the opacity is not: it may animate); plain src-over sprite
  p.setAlphaf(alpha);
  const SkImage* img = it->second.image.get();
  float x0 = ox - float(it->second.margin), y0 = oy - float(it->second.margin);
  SkIRect all = SkRect::MakeXYWH(x0, y0, float(img->width()), float(img->height())).roundOut();
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
  c->setMatrix(post);
  if (!post.isIdentity()) skip.setEmpty();  // scaled: the ring's clips would not be whole pixels
  // Whole-image draws at integer offsets take Skia's sprite blitter; the
  // ring parts are pixel-aligned clips of the same draw.
  const SkSamplingOptions sampling = post.isIdentity() && x0 == std::floor(x0) && y0 == std::floor(y0)
                                         ? SkSamplingOptions()
                                         : SkSamplingOptions(SkFilterMode::kLinear);
  auto part = [&](const SkIRect& d) {
    if (d.isEmpty()) return;
    c->save();
    c->clipIRect(d);
    c->drawImage(img, x0, y0, sampling, &p);
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

// Inset shadows the same way: the blurred frame-minus-hole, clipped to the
// padding box `inner`, rendered once per distinct shape (device size,
// sub-pixel phase, radii, hole, blur, colour) and then drawn as a sprite.
// Drawn directly, a blurred DRRect has no analytic GPU path: Graphite renders
// its coverage into an offscreen mask and blurs that in several more passes,
// for every shadow in every repaint. False when the transform is not
// axis-aligned (the caller blurs directly then).
bool drawCachedInsetShadow(SkCanvas* c, const SkRRect& inner, const SkRRect& frame, const SkRRect& hole, float blur,
                           const SkColor4f& col, ShadowCache& cache) {
  const SkMatrix& m = c->getTotalMatrix();
  if (!m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0) return false;
  SkRRect dev, devHole;
  if (!inner.transform(m, &dev)) return false;
  if (!hole.isEmpty() && !hole.transform(m, &devHole)) return false;
  const SkRect& r = dev.rect();
  const bool phaseFree = phaseFreeMasks(c);
  float ox = phaseFree ? r.left() : std::floor(r.left()), oy = phaseFree ? r.top() : std::floor(r.top());
  ShadowKey key{};
  key.inset = 1;
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
  if (!hole.isEmpty()) {
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
  key.color[0] = col.fR;
  key.color[1] = col.fG;
  key.color[2] = col.fB;
  key.color[3] = col.fA;
  constexpr int kMargin = 1;  // anti-aliased clip edge
  auto it = cache.masks.find(key);
  if (it == cache.masks.end()) {
    int iw = int(std::ceil(key.fx + key.w)) + kMargin * 2, ih = int(std::ceil(key.fy + key.h)) + kMargin * 2;
    if (iw <= 0 || ih <= 0 || size_t(iw) * size_t(ih) > (4u << 20)) return false;  // huge: blur directly
    sk_sp<SkSurface> surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    if (!surf) return false;
    SkCanvas* sc = surf->getCanvas();
    sc->clear(SK_ColorTRANSPARENT);
    // The same device geometry as a direct draw, moved by whole pixels.
    sc->translate(kMargin - ox, kMargin - oy);
    sc->concat(m);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f(col);
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2));
    clipRRect(sc, inner);
    if (hole.isEmpty()) sc->drawRRect(frame, p);
    else sc->drawDRRect(frame, hole, p);
    ShadowMask sm;
    sm.image = surf->makeImageSnapshot();
    sm.margin = kMargin;
    it = cache.insert(key, std::move(sm));
  }
  it->second.lastUsed = cache.frame;
  c->save();
  c->setMatrix(SkMatrix::I());
  c->drawImage(it->second.image.get(), ox - kMargin, oy - kMargin,
               ox == std::floor(ox) && oy == std::floor(oy) ? SkSamplingOptions()
                                                             : SkSamplingOptions(SkFilterMode::kLinear));
  c->restore();
  return true;
}

// `alpha`: the node's opacity folded into the draws (see opacityFoldable); the
// shadow is then cut out under an opaque background too, which no longer hides it.
void paintOuterShadows(SkCanvas* c, const Box& b, const ComputedStyle& s, RootRecord* root, float alpha,
                       bool scaling = false, bool resizing = false, bool* inexact = nullptr) {
  bool opaqueBg = s.backgroundColor.a >= 1 && s.backgroundImages.empty() && alpha >= 1;
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
    if (sh.blur > 0 &&
        drawCachedShadow(c, shape, opaqueBg ? nullptr : &b.outer, b.outer, sh.blur, p, shadowCache(root), alpha,
                         scaling, resizing, inexact))
      continue;
    p.setAlphaf(p.getAlphaf() * alpha);
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
    if (sh.blur > 0 && drawCachedInsetShadow(c, b.inner, frame, hole, sh.blur, sk(sh.color), shadowCache(root)))
      continue;
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

// Two-interval dash and closed polygon across Skia versions (m157 takes spans).
static sk_sp<SkPathEffect> dashEffect(const SkScalar iv[2]) {
#if JSUI_SKGRADIENT
  return SkDashPathEffect::Make({iv, 2}, 0);
#else
  return SkDashPathEffect::Make(iv, 2, 0);
#endif
}
static SkPath polygonPath(const SkPoint* pts, int count) {
#if JSUI_SKGRADIENT
  return SkPath::Polygon({pts, size_t(count)}, true);
#else
  return SkPath::Polygon(pts, count, true);
#endif
}

// Gradient factories across Skia versions: m124 has SkGradientShader::Make*,
// m157 SkShaders::*Gradient taking an SkGradient. CSS interpolates
// premultiplied.
#if JSUI_SKGRADIENT
using GradColors = std::vector<SkColor4f>;
static SkGradient skGradient(const GradColors& c, const std::vector<float>& p, SkTileMode tile) {
  SkGradient::Interpolation interp;
  interp.fInPremul = SkGradient::Interpolation::InPremul::kYes;
  return SkGradient(SkGradient::Colors({c.data(), c.size()}, {p.data(), p.size()}, tile), interp);
}
static sk_sp<SkShader> linearGradient(const SkPoint pts[2], const GradColors& c, const std::vector<float>& p,
                                      SkTileMode tile) {
  return SkShaders::LinearGradient(pts, skGradient(c, p, tile));
}
static sk_sp<SkShader> radialGradient(SkPoint center, float r, const GradColors& c, const std::vector<float>& p,
                                      SkTileMode tile, const SkMatrix* m) {
  return SkShaders::RadialGradient(center, r, skGradient(c, p, tile), m);
}
static sk_sp<SkShader> conicalGradient(SkPoint a, float ra, SkPoint b, float rb, const GradColors& c,
                                       const std::vector<float>& p, SkTileMode tile, const SkMatrix* m) {
  return SkShaders::TwoPointConicalGradient(a, ra, b, rb, skGradient(c, p, tile), m);
}
#else
using GradColors = std::vector<SkColor4f>;
static SkGradientShader::Interpolation premulInterp() {
  SkGradientShader::Interpolation interp;
  interp.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
  return interp;
}
static sk_sp<SkShader> linearGradient(const SkPoint pts[2], const GradColors& c, const std::vector<float>& p,
                                      SkTileMode tile) {
  return SkGradientShader::MakeLinear(pts, c.data(), nullptr, p.data(), int(c.size()), tile, premulInterp(), nullptr);
}
static sk_sp<SkShader> radialGradient(SkPoint center, float r, const GradColors& c, const std::vector<float>& p,
                                      SkTileMode tile, const SkMatrix* m) {
  return SkGradientShader::MakeRadial(center, r, c.data(), nullptr, p.data(), int(c.size()), tile, premulInterp(), m);
}
static sk_sp<SkShader> conicalGradient(SkPoint a, float ra, SkPoint b, float rb, const GradColors& c,
                                       const std::vector<float>& p, SkTileMode tile, const SkMatrix* m) {
  return SkGradientShader::MakeTwoPointConical(a, ra, b, rb, c.data(), nullptr, p.data(), int(c.size()), tile,
                                               premulInterp(), m);
}
#endif

sk_sp<SkShader> makeGradient(const Gradient& g, const SkRect& box) {
  if (g.stops.size() < 2 || box.isEmpty()) return nullptr;
  GradColors colors;
  std::vector<float> pos;
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
    return linearGradient(pts, colors, pos, tile);
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
    return conicalGradient({0, 0}, rx * lo, {0, 0}, rx * hi, colors, pos, tile, &m);
  }
  return radialGradient({0, 0}, rx * hi, colors, pos, tile, &m);
}

// Everything a gradient layer's pixels depend on besides the box.
void addGradientKey(SpriteKey& k, const Gradient& g) {
  k.u(uint64_t(g.kind) | uint64_t(g.circle) << 8 | uint64_t(g.extent) << 16 | uint64_t(g.repeating) << 24 |
      uint64_t(g.corner) << 32 | uint64_t(g.explicitSize) << 40 | uint64_t(g.centerX.percent) << 48 |
      uint64_t(g.centerY.percent) << 49 | uint64_t(g.sizeX.percent) << 50 | uint64_t(g.sizeY.percent) << 51);
  k.f(g.angle).f(g.centerX.value).f(g.centerY.value).f(g.sizeX.value).f(g.sizeY.value);
  k.u(g.stops.size());
  for (const GradientStop& st : g.stops) {
    k.f(st.color.r).f(st.color.g).f(st.color.b).f(st.color.a);
    k.f(st.position ? *st.position : -1e30f).u(st.px);
  }
}

void paintGradients(SkCanvas* c, const Box& b, const ComputedStyle& s, float alpha = 1);

// `alpha`: the node's opacity folded into the draw (see paintNode).
void paintBackground(SkCanvas* c, const Box& b, const ComputedStyle& s, RootRecord* root, float alpha) {
  if (!s.backgroundColor.transparent()) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f(sk(s.backgroundColor));
    p.setAlphaf(p.getAlphaf() * alpha);
    drawRRect(c, b.outer, p);
  }
  if (s.backgroundImages.empty() || b.inner.rect().isEmpty()) return;
  SpriteKey key;
  key.u('G').rrect(b.outer).rect(b.inner.rect());
  for (const Gradient& g : s.backgroundImages) addGradientKey(key, g);
  if (drawSprite(c, root, b.outer.rect(), key, [&](SkCanvas* oc) { paintGradients(oc, b, s); }, alpha)) return;
  paintGradients(c, b, s, alpha);
}

// The background-image layers, painted within the border box.
void paintGradients(SkCanvas* c, const Box& b, const ComputedStyle& s, float alpha) {
  // background-origin: padding-box (the gradient is sized to the padding box),
  // background-clip: border-box, background-repeat: repeat -> the tile repeats
  // under the borders.
  SkRect tile = b.inner.rect();
  bool hasBorder = b.border[0] > 0 || b.border[1] > 0 || b.border[2] > 0 || b.border[3] > 0;
  if (!hasBorder) {
    // The tile is the border box: fill the (rounded) box with each layer
    // directly, like the background color (no clip mask, which the raster
    // blitter applies row by row, slowly). On a pixel-aligned box it is the
    // same coverage as the tile under the box's clip; on a fractional one
    // (under a transform) the straight edges are anti-aliased too.
    for (auto it = s.backgroundImages.rbegin(); it != s.backgroundImages.rend(); ++it) {
      sk_sp<SkShader> shader = makeGradient(*it, tile);
      if (!shader) continue;
      SkPaint p;
      p.setShader(shader);
      p.setAlphaf(alpha);
      if (b.rounded) {
        p.setAntiAlias(true);
        c->drawRRect(b.outer, p);
      } else {
        c->drawRect(tile, p);
      }
    }
    return;
  }
  c->save();
  clipRRect(c, b.outer);
  for (auto it = s.backgroundImages.rbegin(); it != s.backgroundImages.rend(); ++it) {
    sk_sp<SkShader> shader = makeGradient(*it, tile);
    if (!shader) continue;
    SkPaint p;
    p.setShader(shader);
    p.setAlphaf(alpha);
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
  p.setPathEffect(dashEffect(iv));
  c->drawLine(a, b, p);
}

void paintBorders(SkCanvas* c, const Box& b, const ComputedStyle& s, float alpha) {
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
      p.setAlphaf(p.getAlphaf() * alpha);
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
      SkPath clip = polygonPath(quads[i], quadPoints[i]);
      c->save();
      c->clipPath(clip, true);
      p.setColor4f(sk(s.borderColor[i]));
      p.setAlphaf(p.getAlphaf() * alpha);
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
    sp.setPathEffect(dashEffect(iv));
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
  p.setPathEffect(dashEffect(iv));
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

// The node paints at most one draw: a background colour or a single
// gradient layer, and nothing else (no children, text, image, border,
// shadow, outline or scroll indicators).
bool singleDraw(const Node* n) {
  const ComputedStyle& s = n->style();
  if (n->type() != NodeType::View || !n->children().empty() || !n->ghosts.empty()) return false;
  if (!s.boxShadows.empty() || s.outlineWidth > 0) return false;
  YGNodeRef y = n->yoga();
  for (YGEdge e : {YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom})
    if (YGNodeLayoutGetBorder(y, e) > 0) return false;
  return int(!s.backgroundColor.transparent()) + int(s.backgroundImages.size()) <= 1;
}

// The node's draws never overlap, so its opacity can be applied to each draw
// instead of a layer (the same pixels up to rounding at anti-aliased edges):
// a background (one colour or gradient) with at most one outer shadow, which
// is then cut out of the border box under it, or only a solid border ring of
// one colour. A layer costs an offscreen pass on the GPU (a texture cleared,
// stored and sampled back) and a buffer to clear and composite on the CPU,
// and animated opacity (a pulsing badge, a fading ring) needs one every frame.
bool opacityFoldable(const Node* n) {
  if (singleDraw(n)) return true;
  const ComputedStyle& s = n->style();
  if (n->type() != NodeType::View || !n->children().empty() || !n->ghosts.empty() || s.outlineWidth > 0) return false;
  int outer = 0;
  for (const Shadow& sh : s.boxShadows) {
    if (sh.color.transparent()) continue;
    if (sh.inset) return false;
    outer++;
  }
  int fills = int(!s.backgroundColor.transparent()) + int(s.backgroundImages.size());
  YGNodeRef y = n->yoga();
  const YGEdge edges[4] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};  // as computeBox
  bool border = false;
  int first = -1;
  for (int i = 0; i < 4; i++) {
    if (YGNodeLayoutGetBorder(y, edges[i]) <= 0 || s.borderColor[i].transparent()) continue;
    border = true;
    if (first < 0) first = i;
    else if (!(s.borderColor[i] == s.borderColor[first])) return false;
  }
  if (border) return fills == 0 && outer == 0 && s.borderStyle == BorderStyle::Solid;
  return fills <= 1 && outer <= 1;
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
  float scrollX = 0, scrollY = 0;          // ScrollView offsets painted
  bool painted = false;
  bool valid = false;
  // Layer compositing (LayerCache): what the subtree paints relative to the
  // node. `layerStamp` is unique (global sequence) and changes whenever any
  // of it changes; `stableFrames` counts the frames since it last did.
  SkMatrix local;                          // place().local: the node in its parent's space
  SkVector snap = {0, 0};                  // place().snap
  uint32_t contentVersion = 0;
  uint64_t layerStamp = 0;
  uint32_t stableFrames = 0;
  bool relChanged = true;                  // this frame: its painting changed in its parent's space
  bool moved = false;                      // this frame: the device matrix changed
  bool scaling = false;                    // this frame: the device matrix' scale / skew changed
  bool resizing = false;                   // this frame: its size changed
  // Its shadow was drawn from a mask of a nearby scale (see quantizedScale);
  // repainted exactly once the scale stops changing.
  mutable bool shadowInexact = false;
  // The cached layer was last drawn at a whole-pixel offset from where the
  // node is (while it moved); repainted exactly once it stops.
  mutable bool layerInexact = false;
  // Raster cache (see updateRasterCache): painted nodes in the subtree,
  // whether it paints the same into a texture of its own (no blend modes),
  // and its state: drawn from its texture (rasterOn), the consecutive
  // damaged frames that reached it (rasterHits, the last one rasterLast)
  // and its bounds when last drawn from it.
  uint32_t subtreeNodes = 0;
  bool isolatable = true;
  SkIRect subtreeFull = SkIRect::MakeEmpty();  // `subtree` without the clips of ancestors
  bool rasterOn = false;
  uint8_t rasterHits = 0;
  uint32_t rasterLast = 0;
  SkIRect rasterBounds = SkIRect::MakeEmpty();
  // visualBounds() for layerStamp `vbStamp`.
  mutable uint64_t vbStamp = 0;
  mutable SkRect vb = SkRect::MakeEmpty();
  // Clean-subtree skip (see updateRecords): the pass that last brought the
  // record up to date, the arguments it was visited with, and its subtree's
  // entries in that pass' curved-outline and raster-candidate lists. `quiet`:
  // nothing in the subtree is changing (visiting it again with the same
  // arguments, nodes unchanged, would find the same records and no damage).
  // Pass ids are unique across documents (a node moved to another surface's
  // tree carries none its new root just ran).
  uint64_t pass = 0;
  bool quiet = false;
  SkMatrix inMatrix, inSnap;
  SkIRect inClip = SkIRect::MakeEmpty();
  uint32_t curvedBegin = 0, curvedEnd = 0, rasterBegin = 0, rasterEnd = 0;
};

// A node with anti-aliased curved or diagonal outlines, in device pixels.
// Damage strictly inside `safe` (the padding box inset by a pixel) never
// crosses an outline; other damage touching `bounds` must include all of it.
// `clipDependent`: Graphite may still draw this outline differently under a
// tighter clip (see updateDamage's growCurves).
struct CurvedNode {
  SkIRect bounds;
  SkRRect safe;
  bool clipDependent = true;
};

// Per-document painter state, kept on the root node.
struct RootRecord : NodeRecord {
  PaintStats stats;
  uint32_t fontGen = 0;
  std::vector<std::pair<float, sk_sp<SkMaskFilter>>> blurs;  // shadow mask filters by blur radius
  std::vector<Node*> order;                                  // zIndex sort scratch (reused)
  std::vector<CurvedNode> curved;                            // this frame's curved-outline nodes
  ShadowCache shadows;
  SpriteCache sprites;
  bool counting = false;                                     // setPaintCounting
  std::vector<NodeRecord*> rasterCandidates;                 // this frame's (see updateRasterCache)
  uint32_t damagedFrames = 0;                                // updates that found damage
  // The previous pass' lists, for skipped subtrees (NodeRecord::pass).
  std::vector<CurvedNode> curvedPrev;
  std::vector<NodeRecord*> rasterPrev;
  uint64_t lastPass = 0;  // the id of its last updateDamage pass
  uint32_t passImageGen = 0;
  bool passRaster = false;
  int nodesUpdated = 0;  // PaintStats::nodesUpdated
};

ShadowCache& shadowCache(RootRecord* root) { return root->shadows; }
SpriteCache& spriteCache(RootRecord* root) { return root->sprites; }

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
  float alpha = 1;           // the painted node's opacity, when folded into its one draw
  LayerCache* layers = nullptr;  // composited opacity layers and raster-cached subtrees (null: neither)
  bool opacityLayers = true;     // with `layers`: opacity layers composited (false: plain layers, raster cache only)
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
// A paragraph-root Text node's lines in its border-box coordinates, laid
// out at the width paintContent lays the paragraph out at (the unsnapped
// content width): the paragraph keeps one layout, so any other width would
// re-lay it out here and again when painting. Empty without a paragraph.
SkRect paragraphRect(Node* n) {
  YGNodeRef y = n->yoga();
  float left = YGNodeLayoutGetBorder(y, YGEdgeLeft) + YGNodeLayoutGetPadding(y, YGEdgeLeft);
  float top = YGNodeLayoutGetBorder(y, YGEdgeTop) + YGNodeLayoutGetPadding(y, YGEdgeTop);
  float cw = YGNodeLayoutGetWidth(y) - left - YGNodeLayoutGetBorder(y, YGEdgeRight) -
             YGNodeLayoutGetPadding(y, YGEdgeRight);
  if (auto* p = text::layoutParagraph(n, std::max(0.f, cw)))
    return SkRect::MakeXYWH(left, top, p->getLongestLine(), p->getHeight());
  return SkRect::MakeEmpty();
}

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
    SkRect pr = paragraphRect(n);
    if (!pr.isEmpty()) r.join(pr.makeOutset(e, e));
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
  if (!s.boxShadows.empty()) {
    auto* rec = static_cast<const NodeRecord*>(n->paintCache.get());
    bool inexact = false;
    paintOuterShadows(c, b, s, ctx.root, ctx.alpha, rec && rec->scaling, rec && rec->resizing, &inexact);
    if (inexact) {
      rec->shadowInexact = true;
      n->markPaintTree();
      ctx.doc.markExactRepaint();  // a frame after it stops, to repaint it exactly
    }
  }
  paintBackground(c, b, s, ctx.root, ctx.alpha);
  if (!s.boxShadows.empty()) paintInsetShadows(c, b, s, ctx.root);
  paintBorders(c, b, s, ctx.alpha);

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
  if (n->type() == NodeType::Image) {
    SpriteKey key;
    key.u('I').rrect(b.inner);
    if (!image::paintKey(n, &key.bytes) ||
        !drawSprite(c, ctx.root, b.inner.rect(), key, [&](SkCanvas* oc) { image::paint(n, oc, b.inner.rect(), b.inner); }))
      image::paint(n, c, b.inner.rect(), b.inner);
  }

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

}  // namespace

struct LayerCache::Impl {
  struct Entry {
    uint64_t stamp = 0;
    sk_sp<SkImage> image;
    SkIRect rect;          // device rect of the image when rendered
    bool whole = false;    // the image holds the whole layer (not cut at the canvas edge)
    SkMatrix matrix;       // the node's device matrix then
    const void* owner = nullptr;  // the canvas' Graphite recorder / Ganesh context
    uint32_t lastUsed = 0;
  };
  std::unordered_map<const Node*, Entry> entries;
  size_t bytes = 0, budget = 0;
  uint32_t frame = 0;
  bool opacityLayers = true;  // composite cached opacity layers
  bool raster = true;         // raster-cached subtrees (see updateRasterCache)
  static size_t sizeOf(const Entry& e) { return e.image ? size_t(e.image->width()) * e.image->height() * 4 : 0; }
  void erase(std::unordered_map<const Node*, Entry>::iterator it) {
    bytes -= sizeOf(it->second);
    entries.erase(it);
  }
};

namespace {
thread_local LayerCache* tLayerCache = nullptr;
}  // namespace

LayerCache::LayerCache(size_t budgetBytes) : impl(std::make_unique<Impl>()) { impl->budget = budgetBytes; }
LayerCache::~LayerCache() = default;
void LayerCache::clear() {
  impl->entries.clear();
  impl->bytes = 0;
}
void LayerCache::setModes(bool opacityLayers, bool raster) {
  impl->opacityLayers = opacityLayers;
  impl->raster = raster;
}
size_t LayerCache::bytes() const { return impl->bytes; }
int LayerCache::size() const { return int(impl->entries.size()); }
void LayerCache::endFrame() {
  Impl& c = *impl;
  c.frame++;
  constexpr uint32_t kMaxIdle = 120;  // frames
  for (auto it = c.entries.begin(); it != c.entries.end();) {
    auto next = std::next(it);
    if (c.frame - it->second.lastUsed > kMaxIdle) c.erase(it);
    it = next;
  }
  while (c.bytes > c.budget && !c.entries.empty()) {
    auto lru = std::min_element(c.entries.begin(), c.entries.end(),
                                [](const auto& a, const auto& b) { return a.second.lastUsed < b.second.lastUsed; });
    c.erase(lru);
  }
}
LayerCacheScope::LayerCacheScope(LayerCache* cache) : prev_(tLayerCache) { tLayerCache = cache; }
LayerCacheScope::~LayerCacheScope() { tLayerCache = prev_; }

namespace {

constexpr int kMaxLayerSide = 4096;  // cached layer textures

// visualBounds(n), computed once per layer stamp (it walks the subtree, and
// changes only with what the stamp covers).
const SkRect& stampedVisualBounds(Node* n, const NodeRecord* rec) {
  if (rec->vbStamp != rec->layerStamp) {
    rec->vb = visualBounds(n);
    rec->vbStamp = rec->layerStamp;
  }
  return rec->vb;
}

// Paints node `n` (its canvas already at the node's local space, ctx set up
// as paintNode does) through its cached layer: draws the cached texture at
// `opacity`, rendering it first when it is stale. False when the layer
// cannot be cached here (the caller paints a plain layer). `raster`: a
// subtree without a layer of its own (see updateRasterCache), false when
// it cannot be drawn from a texture (the caller paints it directly).
bool paintCachedLayer(Node* n, SkCanvas* c, Ctx& ctx, float opacity, const Placement& p, bool raster = false) {
  LayerCache::Impl& lc = *ctx.layers->impl;
  auto* rec = static_cast<const NodeRecord*>(n->paintCache.get());
  if (!rec || !rec->valid || !rec->layerStamp) return false;
  const void* owner = c->recorder() ? static_cast<const void*>(c->recorder())
                                    : static_cast<const void*>(c->recordingContext());
  if (!owner) return false;
  const SkMatrix m = c->getTotalMatrix();
  if (m.hasPerspective()) return false;
  SkRect vb = stampedVisualBounds(n, rec);
  constexpr int kMaxSide = kMaxLayerSide;
  SkIRect full = m.mapRect(vb).roundOut(), need;
  if (!need.intersect(full, SkIRect::MakeSize(c->getBaseLayerSize()))) return true;  // offscreen
  if (c->quickReject(vb)) return true;
  auto it = lc.entries.find(n);
  int dx = 0, dy = 0;
  bool hit = false, inexact = false;
  if (it != lc.entries.end()) {
    const auto& e = it->second;
    const SkMatrix& em = e.matrix;
    float fx = m.getTranslateX() - em.getTranslateX(), fy = m.getTranslateY() - em.getTranslateY();
    dx = int(std::lround(fx));
    dy = int(std::lround(fy));
    hit = e.stamp == rec->layerStamp && e.owner == owner && e.image &&
          e.image->colorType() == c->imageInfo().colorType() && m.getScaleX() == em.getScaleX() &&
          m.getScaleY() == em.getScaleY() && m.getSkewX() == em.getSkewX() && m.getSkewY() == em.getSkewY() &&
          (e.whole || e.rect.makeOffset(dx, dy).contains(need));
    // A whole-pixel move is exact. While the node moves (an Animated
    // translation) a sub-pixel one is drawn at the nearest whole pixel, as a
    // compositor would; once it stops it is repainted exactly (layerInexact).
    inexact = std::fabs(fx - float(dx)) >= 1e-4f || std::fabs(fy - float(dy)) >= 1e-4f;
    if (inexact && !rec->moved) hit = false;
  }
  if (!hit) {
    // Content that keeps changing (a spinner inside a fading panel): a
    // texture of the whole layer every frame would cost more than a layer
    // clipped to the damage.
    if (rec->stableFrames < 1) return false;
    // A subtree scaling (a button springing): painting it directly costs
    // less than a texture of it at every scale.
    if (raster && rec->scaling) return false;
    // The whole layer when it fits, so it can move into view without
    // being rendered again.
    if (full.width() <= kMaxSide && full.height() <= kMaxSide &&
        size_t(full.width()) * full.height() * 4 <= lc.budget / 2)
      need = full;
    if (need.width() > kMaxSide || need.height() > kMaxSide ||
        size_t(need.width()) * need.height() * 4 > lc.budget / 2)
      return false;
    SkImageInfo info = c->imageInfo().makeWH(need.width(), need.height()).makeAlphaType(kPremul_SkAlphaType);
    SkSurfaceProps props = surfaceProps();
    sk_sp<SkSurface> surf = c->makeSurface(info, &props);
    if (!surf) return false;
    SkCanvas* oc = surf->getCanvas();
    oc->clear(SK_ColorTRANSPARENT);
    oc->translate(float(-need.left()), float(-need.top()));
    oc->concat(m);
    // The whole layer (no damage culling), with plain layers inside. In an
    // opacity layer the raster-cached subtrees are drawn from their textures
    // (not in a raster-cached subtree's own texture: the two would be
    // rendered together).
    Ctx sub{ctx.doc, ctx.root, nullptr, {}, ctx.snapSpace, ctx.transformed, 1, raster ? nullptr : ctx.layers, false};
    paintContent(n, oc, sub, p.snap, p.w, p.h);
    ctx.stats.nodesPainted += sub.stats.nodesPainted;
    ctx.stats.layerDraws += sub.stats.layerDraws;
    ctx.stats.layerRenders += sub.stats.layerRenders;
#if defined(JSUI_GRAPHITE_AS_IMAGE)
    sk_sp<SkImage> image = oc->recorder() ? SkSurfaces::AsImage(surf) : surf->makeImageSnapshot();
#else
    sk_sp<SkImage> image = surf->makeImageSnapshot();
#endif
    if (!image) return false;
    // Raster-cached subtrees inside may have added entries (a rehash).
    it = lc.entries.find(n);
    if (it != lc.entries.end()) lc.erase(it);
    LayerCache::Impl::Entry e;
    e.stamp = rec->layerStamp;
    e.image = std::move(image);
    e.rect = need;
    e.whole = need == full;
    e.matrix = m;
    e.owner = owner;
    lc.bytes += LayerCache::Impl::sizeOf(e);
    it = lc.entries.emplace(n, std::move(e)).first;
    dx = dy = 0;
    inexact = false;
    ctx.stats.layerRenders++;
  }
  rec->layerInexact = inexact;
  // Another frame even if nothing changes: the one that repaints it exactly.
  if (inexact) {
    n->markPaintTree();
    ctx.doc.markExactRepaint();
  }
  it->second.lastUsed = lc.frame;
  ctx.stats.layerDraws++;
  SkPaint lp;
  lp.setAlphaf(opacity);
  c->save();
  c->resetMatrix();
  c->drawImage(it->second.image.get(), float(it->second.rect.left() + dx), float(it->second.rect.top() + dy),
               SkSamplingOptions(), &lp);
  c->restore();
  return true;
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
  if (opacity * 255 < 0.5f) {  // invisible at 8 bits (fading out): no layer to fill and composite
    c->restoreToCount(saveCount);
    return;
  }
  bool layer = opacity < 1 || !s.filters.empty() || s.blendMode != 0;
  // A node whose draws do not overlap (one background colour or gradient, no
  // children; see opacityFoldable) applies its opacity to each draw: the same
  // pixels as a layer (up to rounding), without allocating, clearing and
  // compositing one.
  float alpha = 1;
  if (layer && s.filters.empty() && !s.blendMode && opacityFoldable(n)) {
    alpha = opacity;
    layer = false;
  }
  SkMatrix parentSnap = ctx.snapSpace;
  bool parentTransformed = ctx.transformed;
  // A raster-cached subtree (see updateRasterCache), or a cached layer.
  bool raster = ctx.layers && ctx.layers->impl->raster && !layer && alpha >= 1 && n->paintCache &&
                static_cast<const NodeRecord*>(n->paintCache.get())->rasterOn;
  if (ctx.layers && (raster || (layer && ctx.opacityLayers && ctx.layers->impl->opacityLayers && s.filters.empty() &&
                                !s.blendMode))) {
    ctx.snapSpace = p.snapSpace;
    ctx.transformed |= p.transformed;
    bool drawn = paintCachedLayer(n, c, ctx, opacity, p, raster);
    ctx.snapSpace = parentSnap;
    ctx.transformed = parentTransformed;
    if (drawn) {
      c->restoreToCount(saveCount);
      return;
    }
  }
  if (layer) {
    SkPaint lp;
    lp.setAlphaf(opacity);
    if (!s.filters.empty()) lp.setImageFilter(buildFilter(s.filters));
    if (s.blendMode) lp.setBlendMode(blendMode(s.blendMode));
    SkRect bounds = visualBounds(n);
    c->saveLayer(&bounds, &lp);
  }
  ctx.snapSpace = p.snapSpace;
  ctx.transformed |= p.transformed;
  ctx.alpha = alpha;
  paintContent(n, c, ctx, p.snap, p.w, p.h);
  ctx.alpha = 1;
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

// Scroll blits (see updateDamage): the ScrollView whose content may move by
// pixels this frame, and what updateRecords found inside it.
struct BlitScan {
  bool enabled = false;
  Node* node = nullptr;   // the ScrollView
  bool inside = false;    // updateRecords is in its content
  bool pure = true;       // every content node moved by exactly (dx, dy), nothing else changed
  float dx = 0, dy = 0;   // device pixels
  SkIRect viewport;       // device padding box (whole pixels)
  SkRegion sub;           // the damage found in the content
  size_t curvedBegin = 0, curvedEnd = 0;
};

bool nearlyEqual(const SkMatrix& a, const SkMatrix& b) {
  for (int i = 0; i < 9; i++)
    if (std::fabs(a[i] - b[i]) > 1e-3f) return false;
  return true;
}

bool wholePixels(const SkRect& r) {
  auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1e-3f; };
  return whole(r.fLeft) && whole(r.fTop) && whole(r.fRight) && whole(r.fBottom);
}

// Whether the ScrollView `n` (its record unchanged but for the scroll
// offset) can move its content by pixels; fills scan's viewport and delta.
bool startBlitScan(Node* n, const NodeRecord* rec, const SkMatrix& m, float w, float h, const SkIRect& clip,
                   BlitScan* scan) {
  if (!rec->valid || !rec->painted || rec->version != n->paintVersion || rec->w != w || rec->h != h ||
      rec->children != childListHash(n) || !n->ghosts.empty() || rec->matrix != m || !m.isScaleTranslate())
    return false;
  if (rec->scrollX == n->scrollX && rec->scrollY == n->scrollY) return false;
  const ComputedStyle& s = n->style();
  if (!s.backgroundImages.empty() || (s.outlineWidth > 0 && s.outlineOffset < 0)) return false;
  for (const Shadow& sh : s.boxShadows)
    if (sh.inset) return false;
  float dx = (rec->scrollX - n->scrollX) * m.getScaleX(), dy = (rec->scrollY - n->scrollY) * m.getScaleY();
  if (std::fabs(dx - std::round(dx)) > 1e-3f || std::fabs(dy - std::round(dy)) > 1e-3f) return false;
  Box b;
  computeBox(n, w, h, &b);
  if (!b.inner.isRect()) return false;
  SkRect vp = m.mapRect(b.inner.rect());
  if (!wholePixels(vp)) return false;
  SkIRect ivp = vp.round();
  if (ivp.isEmpty() || !clip.contains(ivp) || std::fabs(dx) >= ivp.width() || std::fabs(dy) >= ivp.height())
    return false;
  scan->node = n;
  scan->dx = std::round(dx);
  scan->dy = std::round(dy);
  scan->viewport = ivp;
  return true;
}

// Raster cache (GPU canvases, with a LayerCache): a subtree that keeps
// being repainted while nothing in it changes (damage from something moving
// over or under it: a glow drifting behind a top bar and the panels below
// it) is drawn from a texture of it, one draw instead of every node's. Like
// a cached layer the texture holds the subtree as painted relative to the
// node, and is drawn while only the node's device position changes (whole
// pixels; the nearest one while it moves, repainted exactly once it stops).
// A candidate paints the same into a texture of its own (no blend modes
// reaching what is below) and has a few nodes. Inside a layer (a panel
// fading in while a button in it pulses: the layer is painted again every
// frame) its static parts are drawn from their textures too, also when the
// layer is rendered into a cached texture of its own.
// Compositing its texture is source-over of the same draws regrouped: equal
// to painting them directly up to rounding (a level or two where the
// subtree is translucent). So that every repaint gives the same pixels
// however much of the frame it covers (the retained frame equals a full
// repaint), the state follows the damage only, never what was painted: a
// subtree is drawn from its texture once damage reached it in two
// consecutive damaged frames with its stamp unchanged (a one-off repaint
// does not pay for a texture), until its stamp changes or no damage reached
// it for a while, and it is repainted whole whenever that switches.
constexpr uint32_t kRasterMinNodes = 4;
constexpr uint32_t kRasterIdleFrames = 120;  // damaged frames without reaching it
thread_local std::vector<NodeRecord*>* tRasterCandidates = nullptr;

void updateRasterCache(RootRecord* root, SkRegion* damage, const SkIRect& screen) {
  root->damagedFrames++;
  const uint32_t frame = root->damagedFrames;
  SkRegion base(*damage);  // what reached each subtree, before the switches below
  auto area = [](const SkIRect& r) { return int64_t(r.width()) * r.height(); };
  for (NodeRecord* rec : root->rasterCandidates) {
    // Mostly visible: a texture of a scroll view's whole content (hidden by
    // the viewport but for a part) costs more to render than it saves. The
    // texture is the whole subtree when it fits (paintCachedLayer).
    const SkIRect& f = rec->subtreeFull;
    SkIRect tex = f;
    if (f.width() > kMaxLayerSide || f.height() > kMaxLayerSide) tex.intersect(screen);
    bool visible = !rec->subtree.isEmpty() && area(tex) <= 2 * area(rec->subtree);
    bool stable = visible && rec->stableFrames >= 1 && !rec->scaling;
    bool hit = base.intersects(rec->subtree);
    if (hit) {
      rec->rasterHits = rec->rasterLast + 1 == frame ? uint8_t(std::min(rec->rasterHits + 1, 255)) : 1;
      rec->rasterLast = frame;
    }
    bool on = rec->rasterOn;
    if (!stable) on = false;
    else if (!on) on = hit && rec->rasterHits >= 2;
    else if (frame - rec->rasterLast > kRasterIdleFrames) on = false;
    if (on != rec->rasterOn) {
      damage->op(rec->subtree, SkRegion::kUnion_Op);
      damage->op(rec->rasterBounds, SkRegion::kUnion_Op);
      rec->rasterOn = on;
    }
    if (!stable) rec->rasterHits = 0;
    rec->rasterBounds = on ? rec->subtree : SkIRect::MakeEmpty();
  }
}

// Clean-subtree skip. Most of a frame's tree is unchanged: a child that is
// clean (Node::paintTreeDirty), was quiet when last brought up to date in
// the previous pass and is visited with the same matrices and clip would
// get the same records and add no damage, so updateRecords keeps its
// records as they are and only carries its curved outlines and raster
// candidates over from the previous pass' lists. Not across a change of
// fonts or images (generations the records compare against) or of the
// raster cache mode, inside a ghost (laid out on its own) or a scroll blit
// candidate's content (each node is checked for the blit).
struct SkipState {
  bool enabled = false;
  uint64_t pass = 0, prev = 0;  // this pass' id, and the root's previous one
  int visited = 0;
  const std::vector<CurvedNode>* curvedPrev = nullptr;
  const std::vector<NodeRecord*>* rasterPrev = nullptr;
};
thread_local SkipState* tSkip = nullptr;
bool gRecordSkip = true;  // setRecordSkipping

// Adds a rect to a damage region (most rects of a frame lie inside what is
// already damaged: a node's children inside its own rects).
void addDamage(SkRegion* d, const SkIRect& r) {
  if (!r.isEmpty() && !d->contains(r)) d->op(r, SkRegion::kUnion_Op);
}

// Clears the dirty flags of a subtree updateRecords does not walk (hidden,
// or painted by its paragraph), so that a later change marks up to the root.
void clearPaintTree(Node* n) {
  if (!n->paintTreeDirty()) return;
  n->clearPaintTree();
  for (Node* g : n->ghosts) clearPaintTree(g);
  for (Node* ch : n->children()) clearPaintTree(ch);
}

// Walks the tree like paintNode (same matrices and snapping), refreshes each
// node's record and adds what changed since the last frame to `damage`.
// Returns the node's device subtree bounds.
SkIRect updateRecords(Node* n, const SkMatrix& parent, const SkMatrix& parentSnap, const SkIRect& clip,
                      uint32_t imageGen, SkRegion* damage, std::vector<CurvedNode>* curved, BlitScan* scan) {
  if (!n->yoga()) {
    clearPaintTree(n);
    return SkIRect::MakeEmpty();  // raw text: painted by its paragraph
  }
  n->clearPaintTree();
  if (tSkip) tSkip->visited++;
  NodeRecord* rec = record(n);
  const size_t curvedStart = curved->size(), rasterStart = tRasterCandidates ? tRasterCandidates->size() : 0;
  bool quietKids = true;
  SkIRect own = SkIRect::MakeEmpty(), sub = SkIRect::MakeEmpty(), bars = SkIRect::MakeEmpty();
  SkMatrix m = parent;
  float w = 0, h = 0;
  bool painted = isPainted(n);
  SkMatrix local;
  SkVector snap = {0, 0};
  bool below = false;  // a descendant's painting changed in its parent's space
  uint32_t subtreeNodes = painted ? 1 : 0;
  bool isolatable = true;
  SkIRect full = SkIRect::MakeEmpty();  // `sub` without the clips of ancestors
  uint64_t kids = childListHash(n);
  uint32_t gen = n->type() == NodeType::Image ? imageGen : 0;
  bool contentChanged = false;
  // Whether the node's own paint state changed (then the whole subtree's old
  // and new rects are damaged, below); if it only moved, its own rects are
  // damaged here, before the children's (which they often contain).
  auto checkContent = [&] {
    contentChanged = !rec->valid || rec->version != n->paintVersion || rec->painted != painted || rec->w != w ||
                     rec->h != h || rec->children != kids || rec->imageGen != gen;
    if (!contentChanged && rec->matrix != m) {
      addDamage(damage, rec->own);
      addDamage(damage, own);
    }
  };
  if (painted) {
    const ComputedStyle& s = n->style();
    // A blend mode composites with what is below the node.
    isolatable = !s.blendMode;
    Placement p = place(n, parentSnap);
    m.preConcat(p.local);
    w = p.w;
    h = p.h;
    local = p.local;
    snap = p.snap;
    const SkIRect ownFull = deviceBounds(m, ownVisualBounds(n, w, h));
    full = ownFull;
    if (!own.intersect(ownFull, clip)) own.setEmpty();
    if (!own.isEmpty()) {
      SkRect co = curvedOutline(n, m, w, h);
      SkIRect ci;
      if (!co.isEmpty() && ci.intersect(deviceBounds(m, co), clip)) {
        CurvedNode cn{ci, SkRRect::MakeEmpty()};
        {
          // Graphite draws solid rrects (borders, fills, sharp shadows and
          // rounded clips) analytically, and with the Skia patch a damage
          // rect is only a scissor, so those pixels do not depend on it.
          // Dashed or dotted edges become paths, and path masks larger than
          // an atlas plot (1024 px) are still cut to the clip.
          SkIRect full = deviceBounds(m, co);
          cn.clipDependent = s.borderStyle != BorderStyle::Solid ||
                             (s.outlineWidth > 0 && s.outlineStyle != BorderStyle::Solid) ||
                             full.width() > 1024 || full.height() > 1024;
        }
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
      curved->push_back({bars, SkRRect::MakeEmpty(), false});  // rounded thumbs: never cut
    }
    sub = own;
    checkContent();
    if (paintsChildren(n) && (!n->children().empty() || !n->ghosts.empty())) {
      SkIRect childClip = clip;
      if (clipsChildren(n) && !childClip.intersect(deviceBounds(m, SkRect::MakeWH(w, h)))) childClip.setEmpty();
      SkMatrix cm = m, cs = p.snapSpace;
      cm.preTranslate(-p.snap.fX, -p.snap.fY);
      SkRegion* childDamage = damage;
      bool scanning = false;
      if (n->type() == NodeType::ScrollView) {
        cm.preTranslate(-n->scrollX, -n->scrollY);
        cs.preTranslate(-n->scrollX, -n->scrollY);
        if (scan->enabled && !scan->node && startBlitScan(n, rec, m, w, h, clip, scan)) {
          scanning = true;
          scan->inside = true;
          scan->curvedBegin = curved->size();
          childDamage = &scan->sub;
        }
      }
      auto visit = [&](Node* ch) {
        auto* cr = static_cast<NodeRecord*>(ch->paintCache.get());
        SkipState* sk = tSkip;
        bool skip = false, hidden = false;
        SkIRect childFull;
        if (sk && sk->enabled && !scan->inside && !ch->ghostOf && cr && cr->valid && sk->prev &&
            cr->pass == sk->prev && !ch->paintTreeDirty()) {
          childFull = cr->subtreeFull;
          skip = cr->quiet && cr->inClip == childClip && cr->inMatrix == cm && cr->inSnap == cs;
          if (!skip && cr->subtree.isEmpty() && cm.getScaleX() == cr->inMatrix.getScaleX() &&
              cm.getScaleY() == cr->inMatrix.getScaleY() && cm.getSkewX() == cr->inMatrix.getSkewX() &&
              cm.getSkewY() == cr->inMatrix.getSkewY() && !cm.hasPerspective() && !cr->inMatrix.hasPerspective()) {
            // Out of view (scrolled out of its scroll view, or not painted)
            // and still out of view after a translation (whole-pixel
            // snapping may differ by a pixel): its records stay as they were
            // for the old placement, which a visit compares with when it
            // comes into view.
            float dx = cm.getTranslateX() - cr->inMatrix.getTranslateX(),
                  dy = cm.getTranslateY() - cr->inMatrix.getTranslateY();
            SkRect moved = SkRect::Make(cr->subtreeFull).makeOffset(dx, dy);
            childFull = cr->subtreeFull.isEmpty() ? cr->subtreeFull : moved.roundOut();
            hidden = skip = !SkIRect::Intersects(moved.makeOutset(2, 2).roundOut(), childClip);
          }
        }
        if (skip) {
          sub.join(cr->subtree);
          size_t b = curved->size();
          curved->insert(curved->end(), sk->curvedPrev->begin() + cr->curvedBegin,
                         sk->curvedPrev->begin() + cr->curvedEnd);
          cr->curvedBegin = uint32_t(b);
          cr->curvedEnd = uint32_t(curved->size());
          if (tRasterCandidates) {
            b = tRasterCandidates->size();
            tRasterCandidates->insert(tRasterCandidates->end(), sk->rasterPrev->begin() + cr->rasterBegin,
                                      sk->rasterPrev->begin() + cr->rasterEnd);
            cr->rasterBegin = uint32_t(b);
            cr->rasterEnd = uint32_t(tRasterCandidates->size());
          }
          cr->pass = sk->pass;
          // Out of view: nothing in it changed relative to this node.
          if (hidden) cr->relChanged = false;
        } else if (ch->ghostOf && sk && sk->enabled) {
          sk->enabled = false;
          sub.join(updateRecords(ch, cm, cs, childClip, imageGen, childDamage, curved, scan));
          sk->enabled = true;
        } else {
          sub.join(updateRecords(ch, cm, cs, childClip, imageGen, childDamage, curved, scan));
        }
        cr = static_cast<NodeRecord*>(ch->paintCache.get());
        below |= !cr || cr->relChanged;
        quietKids &= cr && cr->quiet && !hidden;
        if (cr) {
          subtreeNodes += cr->subtreeNodes;
          isolatable &= cr->isolatable;
          SkIRect cf = skip ? childFull : cr->subtreeFull;
          if (!clipsChildren(n) || cf.intersect(deviceBounds(m, SkRect::MakeWH(w, h)))) full.join(cf);
        }
      };
      for (Node* ch : n->ghosts) visit(ch);
      for (Node* ch : n->children()) visit(ch);
      if (scanning) {
        scan->inside = false;
        scan->curvedEnd = curved->size();
      }
    } else {
      for (Node* ch : n->ghosts) clearPaintTree(ch);
      for (Node* ch : n->children()) clearPaintTree(ch);
    }
    float fe = filterOutset(s);
    if (fe > 0 && !sub.isEmpty()) {
      int e = int(std::ceil(fe * std::max(1.f, m.getMaxScale())));
      SkIRect grown = sub.makeOutset(e, e);
      if (!sub.intersect(grown, clip)) sub.setEmpty();
    }
    if (fe > 0 && !full.isEmpty()) {
      int e = int(std::ceil(fe * std::max(1.f, m.getMaxScale())));
      full.outset(e, e);
    }
  } else {
    checkContent();
    for (Node* ch : n->ghosts) clearPaintTree(ch);
    for (Node* ch : n->children()) clearPaintTree(ch);
  }
  // TextInput caret blink / move: only the caret's old and new rects.
  SkRect caretA, caretB;
  bool caret = false;
  if (n->type() == NodeType::TextInput && text::inputCaretDamage(n, &caretA, &caretB) && painted) {
    caret = true;
    for (const SkRect& cr : {caretA, caretB}) {
      SkIRect dr;
      if (!cr.isEmpty() && dr.intersect(deviceBounds(m, cr.makeOutset(1, 1)), clip)) addDamage(damage, dr);
    }
  }
  // A hidden node's descendants keep their old records; the check below
  // repaints where the subtree was.
  if (contentChanged) {
    // Paint state of the node itself changed: it may affect the whole
    // subtree (opacity, clip, transform of a layer...).
    addDamage(damage, rec->subtree);
    addDamage(damage, sub);
  }
  float barAlpha = bars.isEmpty() ? 0 : n->scrollIndicatorOpacity;
  if (rec->bars != bars || rec->barAlpha != barAlpha) {
    // Scroll indicators moved or faded: only their rects repaint.
    addDamage(damage, rec->bars);
    addDamage(damage, bars);
  }
  if (scan->inside) {
    // Content of a scroll blit candidate: it must have moved by exactly the
    // scroll delta, with nothing else changed.
    SkMatrix moved = rec->matrix;
    moved.postTranslate(scan->dx, scan->dy);
    if (contentChanged || caret || rec->bars != bars || rec->barAlpha != barAlpha || !nearlyEqual(moved, m))
      scan->pure = false;
  }
  // Layer stamp: the subtree's painting relative to this node, which leaves
  // out the node's own opacity and transform (contentVersion) and where it
  // is placed (local).
  bool selfChanged = !rec->valid || rec->contentVersion != n->contentVersion || rec->painted != painted ||
                     rec->w != w || rec->h != h || rec->children != kids || rec->imageGen != gen || caret ||
                     rec->bars != bars || rec->barAlpha != barAlpha || rec->scrollX != n->scrollX ||
                     rec->scrollY != n->scrollY || rec->snap != snap;
  if (selfChanged || below || !rec->layerStamp) {
    static std::atomic<uint64_t> stamps{0};
    rec->layerStamp = ++stamps;
    rec->stableFrames = 0;
  } else if (rec->stableFrames < 1000000) {
    rec->stableFrames++;
  }
  rec->relChanged = selfChanged || below || rec->version != n->paintVersion || rec->local != local;
  rec->moved = rec->valid && rec->matrix != m;
  rec->scaling = rec->valid && (rec->matrix.getScaleX() != m.getScaleX() || rec->matrix.getScaleY() != m.getScaleY() ||
                                rec->matrix.getSkewX() != m.getSkewX() || rec->matrix.getSkewY() != m.getSkewY());
  rec->resizing = rec->valid && (rec->w != w || rec->h != h);
  if (rec->shadowInexact && !rec->scaling && !rec->resizing) {
    // Came to rest: its shadow from the exact mask.
    addDamage(damage, own);
    rec->shadowInexact = false;
  }
  if (rec->layerInexact && !rec->moved) {
    // Came to rest after moving as a cached layer: paint it exactly.
    addDamage(damage, sub);
    rec->layerInexact = false;
  }
  rec->local = local;
  rec->snap = snap;
  rec->contentVersion = n->contentVersion;
  rec->scrollX = n->scrollX;
  rec->scrollY = n->scrollY;
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
  rec->subtreeNodes = subtreeNodes;
  rec->isolatable = isolatable;
  rec->subtreeFull = full;
  if (tRasterCandidates && painted && isolatable && subtreeNodes >= kRasterMinNodes) tRasterCandidates->push_back(rec);
  // A focused input's caret blinks and moves without changing the node.
  rec->quiet = quietKids && !rec->relChanged && !rec->moved && !rec->scaling && !rec->resizing &&
               rec->stableFrames >= 1 && !caret && !n->layoutAnim && !rec->shadowInexact && !rec->layerInexact &&
               !(n->type() == NodeType::TextInput && n->editState);
  rec->pass = tSkip ? tSkip->pass : 0;
  rec->inMatrix = parent;
  rec->inSnap = parentSnap;
  rec->inClip = clip;
  rec->curvedBegin = uint32_t(curvedStart);
  rec->curvedEnd = uint32_t(curved->size());
  rec->rasterBegin = uint32_t(rasterStart);
  rec->rasterEnd = uint32_t(tRasterCandidates ? tRasterCandidates->size() : 0);
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

namespace {

// Whether `k`'s subtree paints into device rect `vp`. Records are outset by
// a pixel beyond what can be painted (deviceBounds): a box ending at the
// edge of `vp` does not paint into it. Text records are generous (a font
// size around the box); here a text paints within its box and lines plus
// a quarter em (overhangs) and its shadows.
bool paintsInto(Node* k, const SkIRect& vp) {
  const auto* kr = static_cast<const NodeRecord*>(k->paintCache.get());
  if (!kr || !kr->valid || !kr->painted || !SkIRect::Intersects(kr->subtree.makeInset(1, 1), vp)) return false;
  const ComputedStyle& s = k->style();
  if (!s.filters.empty()) return true;  // spreads the children's painting
  if (SkIRect::Intersects(kr->own.makeInset(1, 1), vp)) {
    if (k->type() != NodeType::Text) return true;
    float e = s.text.fontSize.value_or(14) * 0.25f;
    for (auto& sh : s.text.textShadows) e = std::max(e, sh.blur * 1.5f + std::fabs(sh.offsetX) + std::fabs(sh.offsetY));
    SkRect t = SkRect::MakeWH(kr->w, kr->h);
    SkRect pr = paragraphRect(k);
    if (!pr.isEmpty()) t.join(pr);
    if (SkIRect::Intersects(kr->matrix.mapRect(t.makeOutset(e, e)).roundOut(), vp)) return true;
  }
  if (!paintsChildren(k)) return false;
  for (Node* g : k->ghosts)
    if (paintsInto(g, vp)) return true;
  for (Node* ch : k->children())
    if (paintsInto(ch, vp)) return true;
  return false;
}

// Whether `k`'s subtree paints the same everywhere in device rect `vp`
// (nothing, or colours that each cover all of it): a backdrop that a scroll
// blit can move content over.
bool uniformOver(Node* k, const SkIRect& vp) {
  if (!paintsInto(k, vp)) return true;
  const auto* kr = static_cast<const NodeRecord*>(k->paintCache.get());
  const ComputedStyle& s = k->style();
  if (k->type() != NodeType::View || !s.filters.empty() || s.blendMode || s.outlineWidth > 0 ||
      !s.backgroundImages.empty() || !s.boxShadows.empty() || !kr->matrix.isScaleTranslate())
    return false;
  if (SkIRect::Intersects(kr->own.makeInset(1, 1), vp)) {
    const SkRect vpf = SkRect::Make(vp);
    Box b;
    computeBox(k, kr->w, kr->h, &b);
    SkRRect dev;
    if (!s.backgroundColor.transparent() && (!b.outer.transform(kr->matrix, &dev) || !dev.contains(vpf))) return false;
    bool border = b.border[0] > 0 || b.border[1] > 0 || b.border[2] > 0 || b.border[3] > 0;
    if (border && (!b.inner.transform(kr->matrix, &dev) || !dev.contains(vpf))) return false;
  }
  for (Node* g : k->ghosts)
    if (!uniformOver(g, vp)) return false;
  for (Node* ch : k->children())
    if (!uniformOver(ch, vp)) return false;
  return true;
}

// A blit candidate's surroundings: true when every pixel of the viewport
// is the content (moved by the blit) over a backdrop that is the same
// everywhere in the viewport (each ancestor paints either nothing there or
// one colour over all of it), with nothing painted between the backdrop and
// the content. Nodes painted above the content are added to `extra`.
bool blitSurroundings(const BlitScan& scan, SkRegion* extra) {
  const SkIRect& vp = scan.viewport;
  const SkRect vpf = SkRect::Make(vp);
  Node* c = scan.node;
  {
    // The ScrollView's own indicators are drawn over its content.
    const auto* rec = static_cast<const NodeRecord*>(c->paintCache.get());
    SkIRect b;
    if (b.intersect(rec->bars, vp)) extra->op(b, SkRegion::kUnion_Op);
  }
  for (Node* a = c->parent(); a; c = a, a = a->parent()) {
    const auto* ar = static_cast<const NodeRecord*>(a->paintCache.get());
    if (!ar || !ar->valid || !ar->painted) return false;
    const ComputedStyle& s = a->style();
    if (s.opacity * animOpacity(a) < 1 || !s.filters.empty() || s.blendMode) return false;
    bool over = SkIRect::Intersects(ar->own, vp);
    if (over && (!s.backgroundImages.empty() || !s.boxShadows.empty())) return false;
    Box b;
    computeBox(a, ar->w, ar->h, &b);
    SkRRect dev;
    if (!s.backgroundColor.transparent() && (!b.outer.transform(ar->matrix, &dev) || !dev.contains(vpf))) return false;
    bool border = b.border[0] > 0 || b.border[1] > 0 || b.border[2] > 0 || b.border[3] > 0;
    if ((border || clipsChildren(a)) && (!b.inner.transform(ar->matrix, &dev) || !dev.contains(vpf))) return false;
    SkIRect r;
    if (s.outlineWidth > 0 && r.intersect(ar->own, vp)) extra->op(r, SkRegion::kUnion_Op);
    if (r.intersect(ar->bars, vp)) extra->op(r, SkRegion::kUnion_Op);
    // Siblings: those painted before must not show in the viewport, those
    // painted after repaint where they do.
    for (Node* g : a->ghosts)
      if (!uniformOver(g, vp)) return false;
    bool after = false;
    for (Node* k : a->children()) {
      if (k->style().hasZIndex && k->style().zIndex != 0) return false;  // painted out of order
      if (k == c) {
        after = true;
        continue;
      }
      const auto* kr = static_cast<const NodeRecord*>(k->paintCache.get());
      if (!kr || !kr->valid || !kr->painted) continue;
      if (!after && !uniformOver(k, vp)) return false;
      if (after && r.intersect(kr->subtree, vp)) extra->op(r, SkRegion::kUnion_Op);
    }
  }
  return true;
}

}  // namespace

void applyScrollBlit(void* pixels, size_t rowBytes, int width, int height, const ScrollBlit& b) {
  SkIRect dst = b.rect;
  if (!dst.intersect(SkIRect::MakeWH(width, height)) || !dst.intersect(b.rect.makeOffset(b.dx, b.dy))) return;
  auto* base = static_cast<uint8_t*>(pixels);
  size_t n = size_t(dst.width()) * 4;
  auto row = [&](int y) {
    std::memmove(base + size_t(y) * rowBytes + size_t(dst.left()) * 4,
                 base + size_t(y - b.dy) * rowBytes + size_t(dst.left() - b.dx) * 4, n);
  };
  if (b.dy > 0) {
    for (int y = dst.bottom() - 1; y >= dst.top(); y--) row(y);
  } else {
    for (int y = dst.top(); y < dst.bottom(); y++) row(y);
  }
}

void noteScrollBlit(Document& doc, const ScrollBlit& b) {
  SkIRect moved = b.rect;
  rootRecord(doc)->stats.blitPixels =
      moved.intersect(b.rect.makeOffset(b.dx, b.dy)) ? int64_t(moved.width()) * moved.height() : 0;
}

SkRegion updateDamage(Document& doc, int deviceWidth, int deviceHeight, ScrollBlit* blit, bool growCurves) {
  RootRecord* root = rootRecord(doc);
  SkRegion damage;
  if (blit) *blit = ScrollBlit{};
  SkIRect screen = SkIRect::MakeWH(deviceWidth, deviceHeight);
  // New fonts change glyphs without changing any node: repaint everything.
  uint32_t fontGen = doc.fonts ? doc.fonts->generation : 0;
  if (fontGen != root->fontGen) {
    root->fontGen = fontGen;
    damage.setRect(screen);
  }
  uint32_t imageGen = doc.images ? doc.images->generation : 0;
  SkMatrix m = SkMatrix::Scale(doc.pixelRatio(), doc.pixelRatio());
  BlitScan scan;
  scan.enabled = blit && damage.isEmpty();
  const bool raster = tLayerCache && tLayerCache->impl->raster;
  std::swap(root->curved, root->curvedPrev);
  std::swap(root->rasterCandidates, root->rasterPrev);
  root->curved.clear();
  root->rasterCandidates.clear();
  SkipState skip;
  static std::atomic<uint64_t> passes{0};
  skip.prev = root->lastPass;
  skip.pass = root->lastPass = ++passes;
  skip.enabled = gRecordSkip && damage.isEmpty() && imageGen == root->passImageGen && raster == root->passRaster;
  skip.curvedPrev = &root->curvedPrev;
  skip.rasterPrev = &root->rasterPrev;
  root->passImageGen = imageGen;
  root->passRaster = raster;
  tSkip = &skip;
  tRasterCandidates = raster ? &root->rasterCandidates : nullptr;
  updateRecords(doc.root(), m, m, screen, imageGen, &damage, &root->curved, &scan);
  tRasterCandidates = nullptr;
  tSkip = nullptr;
  root->nodesUpdated = root->stats.nodesUpdated = skip.visited;
  // Frames without damage leave the raster cache as it is (a host may
  // update the records of an unchanged frame or not).
  if (raster && !damage.isEmpty()) updateRasterCache(root, &damage, screen);
  if (scan.node) {
    // Scroll blit: the viewport's pixels move with the content, so only
    // what the scroll exposes repaints, plus what is painted above the
    // content and whatever else is damaged in the viewport, both where it
    // is and where the blit moves its old pixels, plus curved outlines the
    // viewport edge cuts (now or before the move: Skia chops them at the
    // clip bounds).
    SkRegion extra;
    if (scan.pure && blitSurroundings(scan, &extra)) {
      const SkIRect& vp = scan.viewport;
      int dx = int(scan.dx), dy = int(scan.dy);
      extra.op(damage, SkRegion::kUnion_Op);
      extra.op(vp, SkRegion::kIntersect_Op);
      SkRegion moved(extra);
      moved.translate(dx, dy);
      extra.op(moved, SkRegion::kUnion_Op);
      SkRegion exposed(vp);
      exposed.op(vp.makeOffset(dx, dy), SkRegion::kDifference_Op);
      extra.op(exposed, SkRegion::kUnion_Op);
      // Curved bounds are clipped to the viewport (plus a pixel): one that
      // reaches its edge may be cut by it.
      SkIRect in = vp.makeInset(1, 1);
      for (size_t i = scan.curvedBegin; i < scan.curvedEnd; i++) {
        const SkIRect& b = root->curved[i].bounds;
        SkIRect r;
        if ((!in.contains(b) || !in.contains(b.makeOffset(-dx, -dy))) && r.intersect(b, vp))
          extra.op(r, SkRegion::kUnion_Op);
      }
      extra.op(vp, SkRegion::kIntersect_Op);
      damage.op(extra, SkRegion::kUnion_Op);
      *blit = ScrollBlit{vp, dx, dy};
    } else {
      damage.op(scan.sub, SkRegion::kUnion_Op);
    }
  }
  // Damage that crosses a curved or diagonal outline (an anti-aliased edge)
  // grows to the whole node, and on to the curved nodes that growth
  // crosses: such an edge is then repainted under the same clip spans as in
  // a full frame. Cut by the clip, Skia does not reproduce the full frame's
  // edge coverage exactly; on macOS arm64 not even when the clip's bounds
  // contain the whole outline and only the region's spans cut it (growing
  // just the bounds, by a pixel at each corner, is exact on x86 but left a
  // few pixels off there: render_incremental_* in CI).
  // growCurves false (Graphite): only outlines whose pixels depend on the
  // clip grow, see CurvedNode.
  if (!damage.isEmpty()) {
    for (int pass = 0; pass < 16; pass++) {
      bool grew = false;
      for (const CurvedNode& cn : root->curved) {
        if (!growCurves && !cn.clipDependent) continue;
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
  SpriteCache& sprites = root->sprites;
  sprites.frame++;
  sprites.draws = sprites.renders = 0;
  {
    SkImageInfo info = canvas->imageInfo();
    sprites.raster = !canvas->recordingContext() && !canvas->recorder() &&
                     (info.colorType() == kRGBA_8888_SkColorType || info.colorType() == kBGRA_8888_SkColorType);
    if (sprites.raster && (info.colorType() != sprites.colorType ||
                           !SkColorSpace::Equals(info.colorSpace(), sprites.colorSpace.get()))) {
      sprites.sprites.clear();
      sprites.bytes = 0;
      sprites.colorType = info.colorType();
      sprites.colorSpace = info.refColorSpace();
    }
  }
  Ctx ctx{doc, root, damage, {}, {}};
  // Composited layers on GPU canvases only (a raster canvas has no recorder
  // or context, and the counting canvas none either).
  ctx.layers = tLayerCache;
  std::unique_ptr<SkCanvas> counter;
  if (root->counting && (counter = makeCountingCanvas(canvas, &ctx.stats))) {
    // Same clip and matrix as the target, so quick rejects match (the
    // target gets the same clip again, within a save).
    counter->save();
    SkM44 ctm = canvas->getLocalToDevice();
    if (damage) counter->clipRegion(*damage);
    else counter->clipRect(SkRect::Make(canvas->getDeviceClipBounds()));
    counter->setMatrix(ctm);
    ctx.stats.clips = 0;
    canvas = counter.get();
  }
  canvas->save();
  canvas->scale(doc.pixelRatio(), doc.pixelRatio());
  ctx.snapSpace = canvas->getTotalMatrix();
  paintNode(doc.root(), canvas, ctx);
  canvas->restore();
  if (counter) counter->restore();
  if (damage) {
    const SkIRect& b = damage->getBounds();
    ctx.stats.damagedPixels = int64_t(b.width()) * b.height();
    int64_t area = 0;
    int rects = 0;
    for (SkRegion::Iterator it(*damage); !it.done(); it.next()) {
      area += int64_t(it.rect().width()) * it.rect().height();
      rects++;
    }
    ctx.stats.repaintedPixels = area;
    ctx.stats.damageRects = rects;
  } else {
    SkISize sz = canvas->getBaseLayerSize();
    ctx.stats.repaintedPixels = int64_t(sz.width()) * sz.height();
  }
  ctx.stats.spriteDraws = sprites.draws;
  ctx.stats.spriteRenders = sprites.renders;
  ctx.stats.nodesUpdated = root->nodesUpdated;
  root->stats = ctx.stats;
}

void paintDocumentRects(Document& doc, SkCanvas* canvas, const std::vector<SkIRect>& rects) {
  PaintStats sum;
  for (const SkIRect& r : rects) {
    SkRegion part(r);
    canvas->save();
    canvas->clipIRect(r);
    canvas->clear(SK_ColorTRANSPARENT);
    paintDocument(doc, canvas, &part);
    canvas->restore();
    const PaintStats& s = rootRecord(doc)->stats;
    sum.nodesPainted += s.nodesPainted;
    sum.nodesCulled += s.nodesCulled;
    sum.damagedPixels += s.damagedPixels;
    sum.repaintedPixels += s.repaintedPixels;
    sum.damageRects += s.damageRects;
    sum.draws += s.draws;
    sum.textDraws += s.textDraws;
    sum.imageDraws += s.imageDraws;
    sum.saveLayers += s.saveLayers;
    sum.clips += s.clips;
    sum.spriteDraws += s.spriteDraws;
    sum.spriteRenders += s.spriteRenders;
    sum.layerDraws += s.layerDraws;
    sum.layerRenders += s.layerRenders;
  }
  sum.nodesUpdated = rootRecord(doc)->nodesUpdated;
  rootRecord(doc)->stats = sum;
}

void setPaintCounting(Document& doc, bool on) { rootRecord(doc)->counting = on; }

void setRecordSkipping(bool on) { gRecordSkip = on; }

SkSurfaceProps surfaceProps() { return SkSurfaceProps(0, kUnknown_SkPixelGeometry, 0.2f, 1.2f); }

PaintStats lastPaintStats(const Document& doc) {
  auto* rs = static_cast<RootRecord*>(doc.root()->paintCache.get());
  return rs ? rs->stats : PaintStats{};
}

}  // namespace jsui
