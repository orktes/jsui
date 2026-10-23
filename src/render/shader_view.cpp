#include "render/shader_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/document.h"
#include "core/style.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"
#include "render/host_textures.h"
#include "render/image_cache.h"
#include "runtime.h"

#if defined(JSUI_GPU)
#if __has_include("include/gpu/ganesh/GrDirectContext.h")
#include "include/gpu/ganesh/GrDirectContext.h"
#else
#include "include/gpu/GrDirectContext.h"
#endif
#endif

namespace jsui {

namespace {
// Sources kept compiled (views hold their own reference).
constexpr size_t kMaxPrograms = 256;
const char* const kUniformPrefix = "uniform:";
}  // namespace

ShaderEffects::ShaderEffects(Runtime&) {}
ShaderEffects::~ShaderEffects() = default;

std::shared_ptr<const ShaderEffects::Program> ShaderEffects::get(const std::string& sksl) {
  auto it = programs_.find(sksl);
  if (it != programs_.end()) return it->second;
  if (programs_.size() >= kMaxPrograms) programs_.clear();  // generated sources: start over
  auto p = std::make_shared<Program>();
  SkRuntimeEffect::Result r = SkRuntimeEffect::MakeForShader(SkString(sksl.c_str()));
  if (!r.effect) {
    p->error = r.errorText.c_str();
    if (p->error.empty()) p->error = "the shader failed to compile";
  } else {
    p->effect = r.effect;
    const SkRuntimeEffect::Uniform* t = p->effect->findUniform("time");
    p->usesTime = t && t->type == SkRuntimeEffect::Uniform::Type::kFloat && !t->isArray();
    const SkRuntimeEffect::Child* c = p->effect->findChild("content");
    p->content = c && c->type == SkRuntimeEffect::ChildType::kShader;
  }
  programs_.emplace(sksl, p);
  return p;
}

std::shared_ptr<const ShaderEffects::Program> ShaderEffects::preload(const std::string& sksl, const Value& images) {
  std::shared_ptr<const Program> p = get(sksl);
  if (!p->effect) return p;
  for (const Warm& w : warm_)
    if (w.program == p && styleValueEquals(w.images, images)) return p;
  // Image children (`content` as the view's own painting is not one): each
  // loaded or not yet (images load after the view shows), every combination
  // when there are few, else all and none. The likeliest first: all loaded.
  std::vector<std::string> named;
  for (const SkRuntimeEffect::Child& c : p->effect->children()) {
    std::string name(c.name);
    if (c.type == SkRuntimeEffect::ChildType::kShader && images.has(name)) named.push_back(name);
  }
  const uint32_t all = (1u << std::min<size_t>(named.size(), 31)) - 1;
  std::vector<uint32_t> masks;
  if (named.size() <= 3) {
    for (uint32_t m = 0; m <= all; m++) masks.push_back(m);
    std::stable_sort(masks.begin(), masks.end(),
                     [](uint32_t a, uint32_t b) { return __builtin_popcount(a) > __builtin_popcount(b); });
  } else {
    masks = {all, 0};
  }
  for (uint32_t m : masks) {
    Value::Object loaded;
    for (size_t i = 0; i < named.size(); i++)
      if (m & (1u << i)) loaded[named[i]] = images[named[i]];
    for (int variant = 0; variant < kWarmVariants; variant++) warm_.push_back({p, images, Value(loaded), variant});
  }
  return p;
}

void ShaderEffects::addView(Node* node) { views_.insert(node); }
void ShaderEffects::removeView(Node* node) { views_.erase(node); }

void ShaderEffects::tick(double nowMs) {
  now_ = nowMs;
  for (Node* n : views_) {
    auto* st = static_cast<shader::NodeState*>(n->imageCache.get());
    if (!st || !st->program) continue;
    if (!st->program->effect && !st->reported) {
      st->reported = true;
      events.push_back({n->id(), st->program->error});
    }
    if (shader::clocked(n)) n->markPaintDirty();
  }
}

bool ShaderEffects::animating() const {
  if (!events.empty()) return true;
  for (Node* n : views_) {
    auto* st = static_cast<const shader::NodeState*>(n->imageCache.get());
    if (!st || !st->program) continue;
    if (!st->program->effect && !st->reported) return true;
    if (shader::clocked(n)) return true;
  }
  return false;
}

namespace shader {

namespace {

// Appends the numbers of a uniform value: a number, an array of numbers
// (nested arrays flatten), a boolean or a color (r, g, b, a in 0..1).
void appendFloats(const Value& v, std::vector<float>* out) {
  if (v.isNumber()) {
    out->push_back(float(v.asNumber()));
  } else if (v.isBool()) {
    out->push_back(v.asBool() ? 1.f : 0.f);
  } else if (v.isArray()) {
    for (const Value& e : v.asArray()) appendFloats(e, out);
  } else {
    Color c;
    if (parseColor(v, &c)) out->insert(out->end(), {c.r, c.g, c.b, c.a});
  }
}

bool givenByUser(Node* node, const std::string& name) {
  return node->props()["uniforms"].has(name) || node->props()["style"].has(kUniformPrefix + name);
}

bool isInt(SkRuntimeEffect::Uniform::Type t) {
  using T = SkRuntimeEffect::Uniform::Type;
  return t == T::kInt || t == T::kInt2 || t == T::kInt3 || t == T::kInt4;
}

double timeOf(NodeState* st) {
  if (st->frozen >= 0) return st->frozen;
  return std::max(0.0, (st->effects->now() - st->start) / 1000);
}

// The uniform values of one view this frame.
void fillUniforms(Node* node, NodeState* st, SkRuntimeEffectBuilder& b, float w, float h) {
  const Value& given = node->props()["uniforms"];
  const Value& style = node->props()["style"];
  std::vector<float> vals;
  for (const SkRuntimeEffect::Uniform& u : st->program->effect->uniforms()) {
    std::string name(u.name);
    size_t n = u.sizeInBytes() / 4;
    vals.clear();
    std::string key = kUniformPrefix + name;
    if (style.has(key)) appendFloats(style[key], &vals);
    else if (given.has(name)) appendFloats(given[name], &vals);
    else if (name == "time") vals.push_back(float(timeOf(st)));
    else if (name == "resolution") vals.insert(vals.end(), {w, h});
    else if (name == "pixelRatio") vals.push_back(node->document() ? node->document()->pixelRatio() : 1.f);
    vals.resize(n, 0.f);
    // One component at a time (Animated values of a vector: uniform:<name>.<i>).
    if (style.isObject()) {
      std::string dotted = key + ".";
      for (auto& [k, v] : style.asObject()) {
        if (k.compare(0, dotted.size(), dotted) != 0 || !v.isNumber()) continue;
        char* end = nullptr;
        unsigned long i = std::strtoul(k.c_str() + dotted.size(), &end, 10);
        if (end && *end == 0 && i < n) vals[i] = float(v.asNumber());
      }
    }
    if (isInt(u.type)) {
      std::vector<int> ints(n);
      for (size_t i = 0; i < n; i++) ints[i] = int(std::lround(vals[i]));
      b.uniform(u.name).set(ints.data(), int(n));
    } else {
      b.uniform(u.name).set(vals.data(), int(n));
    }
  }
}

SkTileMode tileOf(const std::string& t) {
  if (t == "repeat") return SkTileMode::kRepeat;
  if (t == "mirror") return SkTileMode::kMirror;
  if (t == "decal") return SkTileMode::kDecal;
  return SkTileMode::kClamp;
}

// `img` as a child shader fitted to a w x h view (in the view's coordinates)
// as `spec` says. `texture`: a host texture (no mipmaps), `flip`: one with
// its origin at the bottom-left.
sk_sp<SkShader> fitImage(const sk_sp<SkImage>& img, const Value& spec, float w, float h, bool texture, bool flip) {
  float scale = std::max(0.01f, float(spec["scale"].asNumber(1)));
  float iw = img->width() / scale, ih = img->height() / scale;
  if (iw <= 0 || ih <= 0) return nullptr;
  const std::string& fit = spec["fit"].isString() ? spec["fit"].asString() : spec["resizeMode"].asString();
  float sx = w / iw, sy = h / ih;
  if (fit == "contain") sx = sy = std::min(sx, sy);
  else if (fit == "center") sx = sy = std::min(1.f, std::min(sx, sy));
  else if (fit == "none" || fit == "repeat") sx = sy = 1;
  else if (fit != "stretch" && fit != "fill") sx = sy = std::max(sx, sy);  // cover
  float dw = iw * sx, dh = ih * sy;
  SkMatrix m = (fit == "none" || fit == "repeat") ? SkMatrix::I() : SkMatrix::Translate((w - dw) / 2, (h - dh) / 2);
  m.preScale(sx / scale, sy / scale);
  if (flip) {
    m.preTranslate(0, float(img->height()));
    m.preScale(1, -1);
  }
  std::string tile = spec["tile"].asString();
  if (tile.empty() && fit == "repeat") tile = "repeat";
  SkTileMode tm = tileOf(tile);
  SkSamplingOptions sampling(SkFilterMode::kLinear, texture ? SkMipmapMode::kNone : SkMipmapMode::kLinear);
  return img->makeShader(tm, tm, sampling, &m);
}

// A child image fitted to a w x h view (in the view's coordinates).
sk_sp<SkShader> childImage(Node* node, const Value& spec, float w, float h) {
  Document* doc = node->document();
  if (!doc) return nullptr;
  std::string uri = spec.isString() ? spec.asString() : spec["uri"].asString();
  const std::string& texture = spec["texture"].asString();
  sk_sp<SkImage> img;
  bool flip = false;
  if (!texture.empty()) {
    if (!doc->textures) return nullptr;
    img = doc->textures->image(texture);
    const HostTextures::Entry* e = doc->textures->find(texture);
    flip = e && !e->pixels && (e->desc.flags & JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN);
  } else if (!uri.empty() && doc->images) {
    img = doc->images->get(doc->images->resolve(uri));
  }
  if (!img) return nullptr;
  return fitImage(img, spec, w, h, !texture.empty(), flip);
}

void fillChildren(Node* node, NodeState* st, SkRuntimeEffectBuilder& b, float w, float h, bool content) {
  const Value& images = node->props()["images"];
  for (const SkRuntimeEffect::Child& c : st->program->effect->children()) {
    std::string name(c.name);
    if (c.type != SkRuntimeEffect::ChildType::kShader) continue;  // color filters / blenders: none
    if (content && name == "content" && !images.has(name)) continue;  // the image filter's source
    sk_sp<SkShader> s = images.has(name) ? childImage(node, images[name], w, h) : nullptr;
    // Not loaded yet (or none): transparent.
    b.child(c.name) = s ? s : SkShaders::Color(SK_ColorTRANSPARENT);
  }
}

void registerTextures(NodeState* st) {
  Document* doc = st->node->document();
  if (!doc || !doc->textures) return;
  for (auto& [name, spec] : st->node->props()["images"].asObject()) {
    const std::string& t = spec["texture"].asString();
    if (t.empty()) continue;
    doc->textures->addUser(t, st->node);
    st->textures.push_back(t);
  }
}

void unregisterTextures(NodeState* st) {
  Document* doc = st->node ? st->node->document() : nullptr;
  if (doc && doc->textures)
    for (const std::string& t : st->textures) doc->textures->removeUser(t, st->node);
  st->textures.clear();
}

}  // namespace

NodeState::~NodeState() {
  unregisterTextures(this);
  if (effects) effects->removeView(node);
}

NodeState* ensure(Node* node) {
  if (node->imageCache) return static_cast<NodeState*>(node->imageCache.get());
  auto st = std::make_unique<NodeState>();
  st->node = node;
  Document* doc = node->document();
  st->effects = doc ? doc->shaders : nullptr;
  if (st->effects) {
    st->effects->addView(node);
    st->start = st->effects->now();
    const Value& props = node->props();
    const Value& src = props["source"].isString() ? props["source"] : props["sksl"];
    if (src.isString() && !src.asString().empty()) st->program = st->effects->get(src.asString());
    if (props["paused"].asBool()) st->frozen = 0;
  }
  registerTextures(st.get());
  NodeState* raw = st.get();
  node->imageCache = std::move(st);
  return raw;
}

void propsChanged(Node* node, const Value& old) {
  const Value& props = node->props();
  NodeState* st = ensure(node);
  if (!styleValueEquals(old["source"], props["source"]) || !styleValueEquals(old["sksl"], props["sksl"])) {
    const Value& src = props["source"].isString() ? props["source"] : props["sksl"];
    st->program = st->effects && src.isString() && !src.asString().empty() ? st->effects->get(src.asString()) : nullptr;
    st->reported = false;
  }
  if (!styleValueEquals(old["images"], props["images"])) {
    unregisterTextures(st);
    registerTextures(st);
  }
  bool paused = props["paused"].asBool();
  if (paused && st->frozen < 0) {
    st->frozen = timeOf(st);
  } else if (!paused && st->frozen >= 0) {
    // Resume where it stopped.
    if (st->effects) st->start = st->effects->now() - st->frozen * 1000;
    st->frozen = -1;
  }
}

bool wantsContent(Node* node) {
  NodeState* st = ensure(node);
  return st->program && st->program->content && !node->props()["images"].has("content");
}

bool clocked(Node* node) {
  auto* st = static_cast<NodeState*>(node->imageCache.get());
  return st && st->program && st->program->usesTime && st->frozen < 0 && !givenByUser(node, "time");
}

void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip) {
  NodeState* st = ensure(node);
  if (!st->program || !st->program->effect || box.isEmpty()) return;
  if (st->program->content && !node->props()["images"].has("content")) return;  // contentFilter() runs it
  SkRuntimeShaderBuilder b(st->program->effect);
  fillUniforms(node, st, b, box.width(), box.height());
  fillChildren(node, st, b, box.width(), box.height(), false);
  // The box's origin by translating the canvas, not by a local matrix on the
  // shader: a local matrix is part of the GPU pipeline's key (and the
  // warm-up's draws have none).
  sk_sp<SkShader> shader = b.makeShader();
  if (!shader) return;
  SkPaint p;
  p.setShader(std::move(shader));
  p.setAntiAlias(true);
  canvas->save();
  if (clip.isRect()) canvas->clipRect(clip.rect(), true);
  else canvas->clipRRect(clip, true);
  canvas->translate(box.left(), box.top());
  canvas->drawRect(SkRect::MakeWH(box.width(), box.height()), p);
  canvas->restore();
}

sk_sp<SkImageFilter> contentFilter(Node* node, const SkRect& box) {
  NodeState* st = ensure(node);
  if (!st->program || !st->program->effect || box.isEmpty()) return nullptr;
  SkRuntimeShaderBuilder b(st->program->effect);
  fillUniforms(node, st, b, box.width(), box.height());
  fillChildren(node, st, b, box.width(), box.height(), true);
  // The shader may sample its content anywhere in the view: keep all of it
  // as input even when only part of the view repaints.
  float radius = std::max(box.width(), box.height());
  return SkImageFilters::RuntimeShader(b, radius, "content", nullptr);
}

}  // namespace shader

namespace {

// The off-screen surface the warm-up draws into (device px).
constexpr int kWarmSize = 64;
// Warm-up draws per frame: a GPU may take a few hundred ms to build one
// runtime effect's pipeline (WebGPU / Metal, cold), so a preload spreads
// over frames.
constexpr int kWarmPerFrame = 4;

// What image children sample during the warm-up: a small opaque image,
// uploaded like any loaded image.
sk_sp<SkImage> warmImage() {
  static sk_sp<SkImage> image = [] {
    SkBitmap bm;
    bm.allocN32Pixels(4, 4);
    bm.eraseColor(SK_ColorWHITE);
    bm.setImmutable();
    return bm.asImage();
  }();
  return image;
}

}  // namespace

// One draw the way a ShaderView draws `program` (shader::paint, and
// painter.cpp's layer for `content`). The variant's bits pick the pipeline
// key's parts: 1 fractional device coordinates (an AA rect: AnalyticRRect
// rather than CoverBounds), 2 the view's own clip rounded, 4 inside another
// rounded clip (a parent's overflow: an AnalyticClip). `loaded`: the image
// children drawn as images; the others are not loaded yet (transparent).
void ShaderEffects::warmDraw(SkCanvas* c, const Warm& w) {
  const Program& program = *w.program;
  SkRuntimeShaderBuilder b(program.effect);  // uniforms: zeros (values are not in the key)
  const float size = kWarmSize / 2;
  for (const SkRuntimeEffect::Child& ch : program.effect->children()) {
    if (ch.type != SkRuntimeEffect::ChildType::kShader) continue;
    std::string name(ch.name);
    if (program.content && name == "content" && !w.images.has(name)) continue;
    sk_sp<SkShader> s;
    if (w.loaded.has(name)) {
      const Value& spec = w.loaded[name];
      s = shader::fitImage(warmImage(), spec, size, size, !spec["texture"].asString().empty(), false);
    }
    b.child(ch.name) = s ? s : SkShaders::Color(SK_ColorTRANSPARENT);
  }
  const bool fractional = w.variant & 1, round = w.variant & 2, outer = w.variant & 4;
  const SkRect r = fractional ? SkRect::MakeXYWH(0.5f, 0.5f, size - 1, size - 1) : SkRect::MakeWH(size, size);
  c->save();
  if (outer) c->clipRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(4, 4, size, size), 10, 10), true);
  if (round) c->clipRRect(SkRRect::MakeRectXY(r, 6, 6), true);
  else c->clipRect(r, true);
  if (program.content && !w.images.has("content")) {
    float radius = std::max(r.width(), r.height());
    SkPaint lp;
    lp.setImageFilter(SkImageFilters::RuntimeShader(b, radius, "content", nullptr));
    c->saveLayer(&r, &lp);
    SkPaint fill;
    fill.setColor(SK_ColorWHITE);
    fill.setAntiAlias(true);
    c->drawRRect(SkRRect::MakeRectXY(r.makeInset(4, 4), 4, 4), fill);
    c->restore();
  } else if (sk_sp<SkShader> shader = b.makeShader()) {
    SkPaint p;
    p.setShader(std::move(shader));
    p.setAntiAlias(true);
    c->translate(r.left(), r.top());
    c->drawRect(SkRect::MakeWH(r.width(), r.height()), p);
  }
  c->restore();
}

bool ShaderEffects::warm(SkCanvas* canvas) {
  if (warm_.empty() || !canvas) return false;
  if (!canvas->recorder() && !canvas->recordingContext()) {  // raster: compiled is all there is
    warm_.clear();
    return false;
  }
  size_t n = std::min<size_t>(warm_.size(), kWarmPerFrame);
  std::vector<Warm> batch(warm_.begin(), warm_.begin() + n);
  warm_.erase(warm_.begin(), warm_.begin() + n);
  SkImageInfo info = canvas->imageInfo().makeWH(kWarmSize, kWarmSize);
  if (info.colorType() == kUnknown_SkColorType) return false;
  SkSurfaceProps props = canvas->getBaseProps();
  sk_sp<SkSurface> surface = canvas->makeSurface(info, &props);
  if (!surface) return false;
  for (const Warm& w : batch) warmDraw(surface->getCanvas(), w);
#if defined(JSUI_GPU)
  // Ganesh: into the frame's flush. (Graphite records the surface's draws
  // when it goes.)
  if (GrDirectContext* d = canvas->recordingContext() ? canvas->recordingContext()->asDirectContext() : nullptr)
    d->flush(surface.get());
#endif
  return true;
}

}  // namespace jsui
