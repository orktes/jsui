#include "render/image_node.h"

#include <algorithm>
#include <cmath>

#include "core/document.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "render/host_textures.h"
#include "render/image_cache.h"

namespace jsui::image {

namespace {

// Picks the entry of a source array whose scale best matches the display.
const Value& pickSource(const Value& src, float pixelRatio) {
  if (!src.isArray()) return src;
  const auto& a = src.asArray();
  if (a.empty()) return src;
  const Value* best = &a[0];
  float bestDiff = 1e9f;
  for (auto& e : a) {
    float s = float(e["scale"].asNumber(1));
    float d = std::fabs(s - pixelRatio);
    if (d < bestDiff) {
      bestDiff = d;
      best = &e;
    }
  }
  return *best;
}

YGSize measureImage(YGNodeConstRef y, float width, YGMeasureMode wm, float height, YGMeasureMode hm) {
  Node* n = static_cast<Node*>(YGNodeGetContext(y));
  NodeCache* c = n ? ensure(n) : nullptr;
  if (!c) return {0, 0};
  float w = c->sourceWidth, h = c->sourceHeight;
  c->measuredLoaded = false;
  if (!c->texture.empty()) {
    // A host texture: its size in px (HostTextures re-measures on resize).
    const HostTextures::Entry* e = c->textures ? c->textures->find(c->texture) : nullptr;
    w = e ? e->desc.width / c->scale : 0;
    h = e ? e->desc.height / c->scale : 0;
    c->measuredLoaded = e != nullptr;
  } else if (!(w > 0 && h > 0) && n->document()->images) {
    ImageCache::State st;
    if (sk_sp<SkImage> img = n->document()->images->get(c->url, &st)) {
      w = img->width() / c->scale;
      h = img->height() / c->scale;
      c->measuredLoaded = true;
    }
  }
  if (!(w > 0 && h > 0)) {
    return {wm == YGMeasureModeExactly ? width : 0, hm == YGMeasureModeExactly ? height : 0};
  }
  // Keep the aspect ratio when only one dimension is constrained.
  float ar = w / h;
  if (wm == YGMeasureModeExactly && hm == YGMeasureModeExactly) return {width, height};
  if (wm == YGMeasureModeExactly) return {width, hm == YGMeasureModeAtMost ? std::min(height, width / ar) : width / ar};
  if (hm == YGMeasureModeExactly) return {wm == YGMeasureModeAtMost ? std::min(width, height * ar) : height * ar, height};
  if (wm == YGMeasureModeAtMost && w > width) {
    w = width;
    h = w / ar;
  }
  if (hm == YGMeasureModeAtMost && h > height) {
    h = height;
    w = h * ar;
  }
  return {w, h};
}

}  // namespace

NodeCache::~NodeCache() {
  if (textures && !texture.empty()) textures->removeUser(texture, node);
}

void installMeasure(Node* node) {
  if (node->type() == NodeType::Image && node->yoga() && YGNodeGetChildCount(node->yoga()) == 0)
    YGNodeSetMeasureFunc(node->yoga(), measureImage);
}

NodeCache* ensure(Node* node) {
  if (node->imageCache) return static_cast<NodeCache*>(node->imageCache.get());
  auto c = std::make_unique<NodeCache>();
  Document* doc = node->document();
  const Value& props = node->props();
  c->node = node;
  if (props["texture"].isString() && !props["texture"].asString().empty()) {
    c->texture = props["texture"].asString();
    c->scale = std::max(0.01f, float(props["scale"].asNumber(1)));
    c->textures = doc ? doc->textures : nullptr;
    if (c->textures) c->textures->addUser(c->texture, node);
    NodeCache* raw = c.get();
    node->imageCache = std::move(c);
    return raw;
  }
  const Value* srcp = &props["source"];
  if (srcp->isNull()) srcp = &props["src"];
  if (srcp->isNull()) srcp = &props["defaultSource"];
  const Value& src = pickSource(*srcp, doc ? doc->pixelRatio() : 1);
  std::string uri;
  if (src.isString()) uri = src.asString();
  else if (src.isObject()) {
    uri = src["uri"].asString();
    c->sourceWidth = float(src["width"].asNumber(0));
    c->sourceHeight = float(src["height"].asNumber(0));
    c->scale = std::max(0.01f, float(src["scale"].asNumber(1)));
  }
  if (!uri.empty() && doc && doc->images) c->url = doc->images->resolve(uri);
  else c->url = uri;
  NodeCache* raw = c.get();
  node->imageCache = std::move(c);
  return raw;
}

void request(Node* node) {
  NodeCache* c = ensure(node);
  Document* doc = node->document();
  if (c && !c->url.empty() && doc && doc->images) doc->images->get(c->url);
}

bool settled(Node* node) {
  NodeCache* c = ensure(node);
  Document* doc = node->document();
  if (!c || c->url.empty() || !doc || !doc->images) return true;
  ImageCache::Info info;
  return doc->images->peek(c->url, &info) && info.state != ImageCache::State::Loading;
}

namespace {
ResizeMode resizeModeOf(Node* node) {
  const ComputedStyle& s = node->style();
  if (s.hasResizeMode) return s.resizeMode;
  const std::string& m = node->props()["resizeMode"].asString();
  if (m == "contain") return ResizeMode::Contain;
  if (m == "stretch") return ResizeMode::Stretch;
  if (m == "center") return ResizeMode::Center;
  if (m == "repeat") return ResizeMode::Repeat;
  return ResizeMode::Cover;
}

template <class T>
void append(std::string* key, const T& v) {
  key->append(reinterpret_cast<const char*>(&v), sizeof v);
}
}  // namespace

bool paintKey(Node* node, std::string* key) {
  NodeCache* c = ensure(node);
  Document* doc = node->document();
  // Host textures change behind the painter's back (a video): never cached.
  if (!c || !c->texture.empty() || c->url.empty() || !doc || !doc->images) return false;
  sk_sp<SkImage> img = doc->images->get(c->url);
  if (!img) return false;
  append(key, img->uniqueID());
  append(key, int(resizeModeOf(node)));
  append(key, c->scale);
  const ComputedStyle& s = node->style();
  Color tint = s.tintColor ? *s.tintColor : Color{0, 0, 0, -1};
  append(key, tint);
  return true;
}

void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip) {
  NodeCache* c = ensure(node);
  Document* doc = node->document();
  if (!c || !doc || box.isEmpty()) return;
  sk_sp<SkImage> img;
  bool flip = false;  // a GPU texture whose row 0 is its bottom
  if (!c->texture.empty()) {
    if (!c->textures) return;
    img = c->textures->image(c->texture);
    const HostTextures::Entry* e = c->textures->find(c->texture);
    flip = e && !e->pixels && (e->desc.flags & JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN);
  } else if (!c->url.empty() && doc->images) {
    img = doc->images->get(c->url);
  }
  if (!img) return;
  const ComputedStyle& s = node->style();
  ResizeMode mode = resizeModeOf(node);
  float iw = img->width() / c->scale, ih = img->height() / c->scale;
  if (iw <= 0 || ih <= 0) return;

  SkPaint paint;
  paint.setAntiAlias(true);
  if (s.tintColor) {
    const Color& t = *s.tintColor;
    // tintColor replaces every opaque pixel's color, keeping alpha.
    paint.setColorFilter(SkColorFilters::Blend(SkColor4f{t.r, t.g, t.b, t.a}, nullptr, SkBlendMode::kSrcIn));
  }
  // Chrome samples images bilinearly with mipmaps when scaling down.
  // Host textures have no mipmaps (a new video frame would need new ones).
  SkSamplingOptions sampling(SkFilterMode::kLinear, c->texture.empty() ? SkMipmapMode::kLinear : SkMipmapMode::kNone);

  canvas->save();
  if (clip.isRect()) canvas->clipRect(clip.rect(), true);
  else canvas->clipRRect(clip, true);
  if (mode == ResizeMode::Repeat) {
    // react-native-web: background-position "0" = left edge, centered vertically.
    SkMatrix m = SkMatrix::Translate(box.left(), box.top() + (box.height() - ih) / 2);
    m.preScale(1 / c->scale, 1 / c->scale);
    if (flip) m.preConcat(SkMatrix::MakeAll(1, 0, 0, 0, -1, float(img->height()), 0, 0, 1));
    paint.setShader(img->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, sampling, &m));
    canvas->drawRect(box, paint);
  } else {
    float sx = box.width() / iw, sy = box.height() / ih;
    float dw = box.width(), dh = box.height();
    if (mode == ResizeMode::Cover || mode == ResizeMode::Contain || mode == ResizeMode::Center) {
      float k = mode == ResizeMode::Cover ? std::max(sx, sy) : std::min(sx, sy);
      if (mode == ResizeMode::Center) k = std::min(1.f, k);  // never scales up
      dw = iw * k;
      dh = ih * k;
    }
    SkRect dst = SkRect::MakeXYWH(box.centerX() - dw / 2, box.centerY() - dh / 2, dw, dh);
    if (flip) {
      canvas->translate(0, dst.top() + dst.bottom());
      canvas->scale(1, -1);
    }
    canvas->drawImageRect(img, SkRect::MakeIWH(img->width(), img->height()), dst, sampling, &paint,
                          SkCanvas::kFast_SrcRectConstraint);
  }
  canvas->restore();
}

}  // namespace jsui::image
