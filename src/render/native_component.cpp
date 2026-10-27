#include "render/native_component.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "core/document.h"
#include "core/style.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#if __has_include("include/effects/SkGradient.h")
#include "include/effects/SkGradient.h"  // Skia m157+
#define JSUI_COMPONENT_SKGRADIENT 1
#else
#include "include/effects/SkGradientShader.h"
#endif
#include "include/utils/SkParsePath.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "render/host_textures.h"
#include "render/image_cache.h"
#include "runtime.h"
#include "text/font_manager.h"

// The canvas handed to `draw`: the Skia canvas plus what the prop readers
// and image calls need. Lives on the stack of component::paint.
struct JsuiCanvas {
  SkCanvas* canvas = nullptr;
  jsui::Node* node = nullptr;
  jsui::Document* doc = nullptr;
  jsui::component::NodeState* st = nullptr;
  std::deque<std::string> strings;  // jsui_prop_json results, valid during draw
  int baseSave = 0;                  // restore never pops below this
};

// A path being built (SkPath is immutable in newer Skia: build, then
// snapshot once per change).
struct JsuiPath {
  SkPathBuilder builder;
  SkPath path;
  bool dirty = false;
  bool evenOdd = false;
  const SkPath& get() {
    if (dirty) {
      path = builder.snapshot();
      path.setFillType(evenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
      dirty = false;
    }
    return path;
  }
};

namespace jsui {

namespace {
const char* const kPropPrefix = "prop:";
// Props the bridge uses itself (never part of a class's props).
bool internalProp(const std::string& k) {
  return k == "style" || k == "component" || k.compare(0, 2, "__") == 0;
}
}  // namespace

NativeComponents::NativeComponents(Runtime& rt) : rt_(rt) {}

NativeComponents::~NativeComponents() {
  // The views outlive this only during teardown: unbind them first so their
  // instances are destroyed while the classes still exist.
  std::vector<Node*> views(views_.begin(), views_.end());
  for (Node* n : views)
    if (auto* st = static_cast<component::NodeState*>(n->imageCache.get())) {
      component::unbind(st);
      st->owner = nullptr;
    }
  views_.clear();
}

int NativeComponents::registerClass(const std::string& name, const JsuiComponentClass& cls) {
  if (name.empty() || !cls.draw) return -1;
  auto c = std::make_shared<Class>();
  c->name = name;
  // Copy what the caller's struct has (an older, smaller struct leaves the
  // newer fields zero).
  size_t size = cls.struct_size ? std::min(cls.struct_size, sizeof(JsuiComponentClass)) : sizeof(JsuiComponentClass);
  std::memcpy(&c->cls, &cls, size);
  c->cls.struct_size = sizeof(JsuiComponentClass);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    classes_[name] = c;
  }
  std::vector<Node*> views(views_.begin(), views_.end());
  for (Node* n : views) {
    auto* st = static_cast<component::NodeState*>(n->imageCache.get());
    if (st && st->name == name) component::bind(st);
  }
  return 1;
}

bool NativeComponents::unregisterClass(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!classes_.erase(name)) return false;
  }
  std::vector<Node*> views(views_.begin(), views_.end());
  for (Node* n : views) {
    auto* st = static_cast<component::NodeState*>(n->imageCache.get());
    if (st && st->cls && st->cls->name == name) {
      component::unbind(st);
      n->markPaintDirty();
    }
  }
  return true;
}

bool NativeComponents::has(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return classes_.count(name) > 0;
}

std::shared_ptr<const NativeComponents::Class> NativeComponents::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = classes_.find(name);
  return it == classes_.end() ? nullptr : it->second;
}

int NativeComponents::invalidate(uint32_t view, const char* name) {
  int marked = 0;
  if (view) {
    Node* n = rt_.findNode(view);
    if (!n || n->type() != NodeType::Component) return 0;
    n->markPaintDirty();
    return 1;
  }
  for (Node* n : views_) {
    auto* st = static_cast<component::NodeState*>(n->imageCache.get());
    if (!st || (name && st->name != name)) continue;
    n->markPaintDirty();
    marked++;
  }
  return marked;
}

int NativeComponents::emit(uint32_t view, const std::string& event, const std::string& json) {
  Node* n = rt_.findNode(view);
  if (!n || n->type() != NodeType::Component) return -1;
  bool listens = false;
  for (const Value& e : n->props()["__componentEvents"].asArray())
    if (e.asString() == event) listens = true;
  if (!listens) return 0;
  events.push_back({view, event, json});
  return 1;
}

bool NativeComponents::command(Node* node, const std::string& name, const Value& args) {
  if (!node || node->type() != NodeType::Component) return false;
  auto* st = static_cast<component::NodeState*>(node->imageCache.get());
  if (!st || !st->cls || !st->cls->cls.command) return false;
  std::shared_ptr<const Class> keep = st->cls;  // the callback may unregister it
  std::string json = args.isArray() ? args.toJson() : "[]";
  keep->cls.command(st->instance, int(node->id()), name.c_str(), json.c_str());
  return true;
}

void NativeComponents::tick(double nowMs) {
  now_ = nowMs;
  for (Node* n : views_)
    if (component::clocked(n)) n->markPaintDirty();
}

bool NativeComponents::animating() const {
  if (!events.empty()) return true;
  for (Node* n : views_)
    if (component::clocked(n)) return true;
  return false;
}

void NativeComponents::addView(Node* node) { views_.insert(node); }
void NativeComponents::removeView(Node* node) { views_.erase(node); }

namespace component {

namespace {

void unregisterTextures(NodeState* st) {
  Document* doc = st->node ? st->node->document() : nullptr;
  if (doc && doc->textures)
    for (const std::string& k : st->textures) doc->textures->removeUser(k, st->node);
  st->textures.clear();
}

}  // namespace

NodeState::~NodeState() {
  unbind(this);
  unregisterTextures(this);
  if (owner) owner->removeView(node);
}

NodeState* ensure(Node* node) {
  if (node->imageCache) return static_cast<NodeState*>(node->imageCache.get());
  auto st = std::make_unique<NodeState>();
  st->node = node;
  Document* doc = node->document();
  st->owner = doc ? doc->components : nullptr;
  if (st->owner) st->owner->addView(node);
  NodeState* raw = st.get();
  node->imageCache = std::move(st);
  return raw;
}

Value publicProps(const Value& props) {
  Value::Object out;
  for (const auto& [k, v] : props.asObject())
    if (!internalProp(k)) out.emplace(k, v);
  return Value(std::move(out));
}

void bind(NodeState* st) {
  if (!st->owner) return;
  std::shared_ptr<const NativeComponents::Class> cls = st->owner->find(st->name);
  if (cls == st->cls) return;
  unbind(st);
  if (!cls) return;
  st->cls = cls;
  const JsuiComponentClass& c = cls->cls;
  int view = int(st->node->id());
  st->instance = c.create ? c.create(c.user, st->owner->runtime().handle, view) : c.user;
  if (c.props && st->cls == cls) {
    std::string json = publicProps(st->node->props()).toJson();
    c.props(st->instance, view, json.c_str());
  }
  st->node->markPaintDirty();
}

void unbind(NodeState* st) {
  if (!st->cls) return;
  std::shared_ptr<const NativeComponents::Class> cls = std::move(st->cls);
  st->cls = nullptr;
  void* instance = st->instance;
  st->instance = nullptr;
  if (cls->cls.destroy) cls->cls.destroy(instance, int(st->node->id()));
}

void propsChanged(Node* node, const Value& old) {
  NodeState* st = ensure(node);
  const std::string& name = node->props()["component"].asString();
  if (name != st->name) {
    unbind(st);
    st->name = name;
    bind(st);  // calls props
    return;
  }
  if (!st->cls) {
    bind(st);
    return;
  }
  const JsuiComponentClass& c = st->cls->cls;
  if (!c.props) return;
  // Only when a prop the class sees changed (not on style-only updates).
  bool changed = false;
  const Value::Object& a = old.asObject();
  const Value::Object& b = node->props().asObject();
  for (const auto& [k, v] : b)
    if (!internalProp(k) && !v.equals(old[k])) changed = true;
  for (const auto& [k, v] : a)
    if (!internalProp(k) && !node->props().has(k)) changed = true;
  if (!changed) return;
  std::shared_ptr<const NativeComponents::Class> keep = st->cls;
  std::string json = publicProps(node->props()).toJson();
  keep->cls.props(st->instance, int(node->id()), json.c_str());
}

void detach(Node* node) {
  auto* st = static_cast<NodeState*>(node->imageCache.get());
  if (!st) return;
  unbind(st);
  st->name.clear();
  if (st->owner) st->owner->removeView(node);
  st->owner = nullptr;
}

bool clocked(const Node* node) {
  auto* st = static_cast<const NodeState*>(node->imageCache.get());
  return st && st->cls && (st->cls->cls.flags & JSUI_COMPONENT_CLOCKED);
}

void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip) {
  NodeState* st = ensure(node);
  if (!st->cls || box.isEmpty()) return;
  std::shared_ptr<const NativeComponents::Class> keep = st->cls;
  JsuiCanvas jc;
  jc.canvas = canvas;
  jc.node = node;
  jc.doc = node->document();
  jc.st = st;
  JsuiDrawInfo info{};
  info.view = int(node->id());
  info.width = box.width();
  info.height = box.height();
  info.pixel_ratio = jc.doc ? jc.doc->pixelRatio() : 1.f;
  info.time_ms = st->owner ? st->owner->now() : 0;
  int count = canvas->save();
  if (clip.isRect()) canvas->clipRect(clip.rect(), true);
  else canvas->clipRRect(clip, true);
  canvas->translate(box.left(), box.top());
  jc.baseSave = canvas->getSaveCount();
  keep->cls.draw(st->instance, &jc, &info);
  canvas->restoreToCount(count);
}

}  // namespace component

// ------------------------------------------------------------------ drawing

namespace {

const Value& propOf(JsuiCanvas* c, const char* name) {
  static const Value null;
  if (!c || !c->node || !name) return null;
  const Value& props = c->node->props();
  const Value& style = props["style"];
  if (style.isObject()) {
    const Value& v = style[std::string(kPropPrefix) + name];
    if (!v.isNull()) return v;
  }
  return props[name];
}

void appendNumbers(const Value& v, float* out, int max, int* n) {
  if (v.isNumber()) {
    if (*n < max) out[*n] = float(v.asNumber());
    (*n)++;
  } else if (v.isArray()) {
    for (const Value& e : v.asArray()) appendNumbers(e, out, max, n);
  }
}

SkColor4f color4(uint32_t c) { return SkColor4f::FromColor(SkColor(c)); }

sk_sp<SkShader> gradientShader(const JsuiGradient& g) {
  if (g.type == JSUI_GRADIENT_NONE || g.count < 2 || !g.colors) return nullptr;
  std::vector<SkColor4f> colors;
  std::vector<float> pos;
  for (int i = 0; i < g.count; i++) {
    colors.push_back(color4(g.colors[i]));
    pos.push_back(g.stops ? g.stops[i] : float(i) / float(g.count - 1));
  }
  SkPoint a{g.x0, g.y0}, b{g.x1, g.y1};
  SkTileMode tile = SkTileMode::kClamp;
#if JSUI_COMPONENT_SKGRADIENT
  SkGradient grad(SkGradient::Colors({colors.data(), colors.size()}, {pos.data(), pos.size()}, tile),
                  SkGradient::Interpolation());
  switch (g.type) {
    case JSUI_GRADIENT_LINEAR: {
      SkPoint pts[2] = {a, b};
      return SkShaders::LinearGradient(pts, grad, nullptr);
    }
    case JSUI_GRADIENT_RADIAL: return SkShaders::RadialGradient(a, std::max(g.r, 1e-3f), grad, nullptr);
    case JSUI_GRADIENT_SWEEP: return SkShaders::SweepGradient(a, g.start, g.end, grad, nullptr);
  }
#else
  std::vector<SkColor> cs;
  for (int i = 0; i < g.count; i++) cs.push_back(SkColor(g.colors[i]));
  switch (g.type) {
    case JSUI_GRADIENT_LINEAR: {
      SkPoint pts[2] = {a, b};
      return SkGradientShader::MakeLinear(pts, cs.data(), pos.data(), g.count, tile);
    }
    case JSUI_GRADIENT_RADIAL:
      return SkGradientShader::MakeRadial(a, std::max(g.r, 1e-3f), cs.data(), pos.data(), g.count, tile);
    case JSUI_GRADIENT_SWEEP:
      return SkGradientShader::MakeSweep(a.fX, a.fY, cs.data(), pos.data(), g.count, tile, g.start, g.end, 0,
                                         nullptr);
  }
#endif
  return nullptr;
}

SkBlendMode blendOf(int b) {
  switch (b) {
    case JSUI_BLEND_PLUS: return SkBlendMode::kPlus;
    case JSUI_BLEND_MULTIPLY: return SkBlendMode::kMultiply;
    case JSUI_BLEND_SCREEN: return SkBlendMode::kScreen;
    case JSUI_BLEND_SRC: return SkBlendMode::kSrc;
    case JSUI_BLEND_DST_OUT: return SkBlendMode::kDstOut;
    case JSUI_BLEND_DST_IN: return SkBlendMode::kDstIn;
    default: return SkBlendMode::kSrcOver;
  }
}

SkPaint skPaint(const JsuiPaint* jp) {
  JsuiPaint def;
  if (!jp) {
    jsui_paint_init(&def);
    jp = &def;
  }
  SkPaint p;
  p.setAntiAlias(jp->antialias != 0);
  p.setColor(SkColor(jp->color));
  switch (jp->style) {
    case JSUI_PAINT_STROKE: p.setStyle(SkPaint::kStroke_Style); break;
    case JSUI_PAINT_FILL_AND_STROKE: p.setStyle(SkPaint::kStrokeAndFill_Style); break;
    default: p.setStyle(SkPaint::kFill_Style); break;
  }
  p.setStrokeWidth(std::max(0.f, jp->stroke_width));
  p.setStrokeCap(jp->cap == JSUI_CAP_ROUND    ? SkPaint::kRound_Cap
                 : jp->cap == JSUI_CAP_SQUARE ? SkPaint::kSquare_Cap
                                              : SkPaint::kButt_Cap);
  p.setStrokeJoin(jp->join == JSUI_JOIN_ROUND   ? SkPaint::kRound_Join
                  : jp->join == JSUI_JOIN_BEVEL ? SkPaint::kBevel_Join
                                                : SkPaint::kMiter_Join);
  if (jp->blend) p.setBlendMode(blendOf(jp->blend));
  if (jp->blur > 0) p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, jp->blur));
  if (jp->dash && jp->dash_count >= 2) {
    std::vector<SkScalar> iv(jp->dash, jp->dash + (jp->dash_count & ~1));
    float sum = 0;
    for (float& f : iv) sum += (f = std::max(0.f, f));
#if JSUI_COMPONENT_SKGRADIENT
    if (sum > 0) p.setPathEffect(SkDashPathEffect::Make({iv.data(), iv.size()}, jp->dash_phase));
#else
    if (sum > 0) p.setPathEffect(SkDashPathEffect::Make(iv.data(), int(iv.size()), jp->dash_phase));
#endif
  }
  if (jp->gradient) {
    if (sk_sp<SkShader> s = gradientShader(*jp->gradient)) {
      p.setShader(std::move(s));
      p.setColor(SkColorSetA(SK_ColorBLACK, SkColorGetA(SkColor(jp->color))));
    }
  }
  return p;
}

SkFont skFont(JsuiCanvas* c, const JsuiFont* jf) {
  JsuiFont def{};
  if (!jf) jf = &def;
  float size = jf->size > 0 ? jf->size : 14.f;
  int weight = jf->weight > 0 ? jf->weight : 400;
  sk_sp<SkTypeface> tf;
  FontManager* fonts = c && c->doc ? c->doc->fonts : nullptr;
  if (fonts) {
    auto coll = fonts->collection();
    SkFontStyle style(weight, SkFontStyle::kNormal_Width,
                      jf->italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    std::vector<SkString> fams;
    if (jf->family) {
      std::string f = jf->family;
      size_t i = 0;
      while (i < f.size()) {
        size_t j = f.find(',', i);
        if (j == std::string::npos) j = f.size();
        std::string name = f.substr(i, j - i);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(0, 1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (!name.empty()) fams.emplace_back(name.c_str());
        i = j + 1;
      }
    }
    fams.emplace_back(fonts->defaultFamily().c_str());
    if (coll) {
      for (const SkString& fam : fams) {
        std::vector<sk_sp<SkTypeface>> faces = coll->findTypefaces({fam}, style);
        if (!faces.empty() && faces[0]) {
          tf = faces[0];
          break;
        }
      }
      if (!tf) tf = coll->defaultFallback();
    }
  }
  SkFont font(tf, size);
  font.setSubpixel(true);
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setHinting(SkFontHinting::kNone);
  font.setLinearMetrics(true);
  return font;
}

SkRect rectOf(float x, float y, float w, float h) { return SkRect::MakeXYWH(x, y, w, h); }

}  // namespace
}  // namespace jsui

using jsui::Value;

extern "C" {

void jsui_component_class_init(JsuiComponentClass* cls) {
  if (!cls) return;
  std::memset(cls, 0, sizeof *cls);
  cls->struct_size = sizeof *cls;
}

double jsui_prop_number(JsuiCanvas* c, const char* name, double fallback) {
  const Value& v = jsui::propOf(c, name);
  if (v.isNumber()) return v.asNumber();
  if (v.isBool()) return v.asBool() ? 1 : 0;
  return fallback;
}

int jsui_prop_bool(JsuiCanvas* c, const char* name, int fallback) {
  const Value& v = jsui::propOf(c, name);
  if (v.isBool()) return v.asBool() ? 1 : 0;
  if (v.isNumber()) return v.asNumber() != 0;
  return fallback;
}

uint32_t jsui_prop_color(JsuiCanvas* c, const char* name, uint32_t fallback) {
  const Value& v = jsui::propOf(c, name);
  jsui::Color col;
  if (v.isNull() || !jsui::parseColor(v, &col)) return fallback;
  auto byte = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.f, 1.f) * 255)); };
  return byte(col.a) << 24 | byte(col.r) << 16 | byte(col.g) << 8 | byte(col.b);
}

const char* jsui_prop_string(JsuiCanvas* c, const char* name) {
  const Value& v = jsui::propOf(c, name);
  return v.isString() ? v.asString().c_str() : nullptr;
}

int jsui_prop_numbers(JsuiCanvas* c, const char* name, float* out, int max) {
  int n = 0;
  jsui::appendNumbers(jsui::propOf(c, name), out, out ? max : 0, &n);
  return n;
}

const char* jsui_prop_json(JsuiCanvas* c, const char* name) {
  const Value& v = jsui::propOf(c, name);
  if (!c || (v.isNull() && !(c->node && c->node->props().has(name ? name : "")))) return nullptr;
  c->strings.push_back(v.toJson());
  return c->strings.back().c_str();
}

void jsui_paint_init(JsuiPaint* p) {
  if (!p) return;
  std::memset(p, 0, sizeof *p);
  p->color = 0xff000000u;
  p->stroke_width = 1;
  p->antialias = 1;
}

void jsui_canvas_save(JsuiCanvas* c) {
  if (c) c->canvas->save();
}
void jsui_canvas_save_layer(JsuiCanvas* c, float alpha) {
  if (c) c->canvas->saveLayerAlphaf(nullptr, std::clamp(alpha, 0.f, 1.f));
}
void jsui_canvas_restore(JsuiCanvas* c) {
  // Never below the state paint() saved.
  if (c && c->canvas->getSaveCount() > c->baseSave) c->canvas->restore();
}
void jsui_canvas_translate(JsuiCanvas* c, float dx, float dy) {
  if (c) c->canvas->translate(dx, dy);
}
void jsui_canvas_scale(JsuiCanvas* c, float sx, float sy) {
  if (c) c->canvas->scale(sx, sy);
}
void jsui_canvas_rotate(JsuiCanvas* c, float degrees) {
  if (c) c->canvas->rotate(degrees);
}
void jsui_canvas_clip_rect(JsuiCanvas* c, float x, float y, float w, float h) {
  if (c) c->canvas->clipRect(jsui::rectOf(x, y, w, h), true);
}
void jsui_canvas_clip_rrect(JsuiCanvas* c, float x, float y, float w, float h, float radius) {
  if (c) c->canvas->clipRRect(SkRRect::MakeRectXY(jsui::rectOf(x, y, w, h), radius, radius), true);
}
void jsui_canvas_clip_path(JsuiCanvas* c, const JsuiPath* path) {
  if (c && path) c->canvas->clipPath(const_cast<JsuiPath*>(path)->get(), true);
}

void jsui_canvas_clear(JsuiCanvas* c, uint32_t color) {
  if (!c) return;
  SkPaint p;
  p.setColor(SkColor(color));
  p.setBlendMode(SkBlendMode::kSrc);
  c->canvas->drawPaint(p);
}
void jsui_canvas_draw_rect(JsuiCanvas* c, float x, float y, float w, float h, const JsuiPaint* p) {
  if (c) c->canvas->drawRect(jsui::rectOf(x, y, w, h), jsui::skPaint(p));
}
void jsui_canvas_draw_rrect(JsuiCanvas* c, float x, float y, float w, float h, float radius, const JsuiPaint* p) {
  if (c) c->canvas->drawRRect(SkRRect::MakeRectXY(jsui::rectOf(x, y, w, h), radius, radius), jsui::skPaint(p));
}
void jsui_canvas_draw_circle(JsuiCanvas* c, float cx, float cy, float r, const JsuiPaint* p) {
  if (c) c->canvas->drawCircle(cx, cy, r, jsui::skPaint(p));
}
void jsui_canvas_draw_oval(JsuiCanvas* c, float x, float y, float w, float h, const JsuiPaint* p) {
  if (c) c->canvas->drawOval(jsui::rectOf(x, y, w, h), jsui::skPaint(p));
}
void jsui_canvas_draw_line(JsuiCanvas* c, float x0, float y0, float x1, float y1, const JsuiPaint* p) {
  if (!c) return;
  SkPaint paint = jsui::skPaint(p);
  if (paint.getStyle() == SkPaint::kFill_Style) paint.setStyle(SkPaint::kStroke_Style);  // a line has no inside
  c->canvas->drawLine(x0, y0, x1, y1, paint);
}
void jsui_canvas_draw_arc(JsuiCanvas* c, float x, float y, float w, float h, float start, float sweep, int use_center,
                          const JsuiPaint* p) {
  if (c) c->canvas->drawArc(jsui::rectOf(x, y, w, h), start, sweep, use_center != 0, jsui::skPaint(p));
}
void jsui_canvas_draw_path(JsuiCanvas* c, const JsuiPath* path, const JsuiPaint* p) {
  if (c && path) c->canvas->drawPath(const_cast<JsuiPath*>(path)->get(), jsui::skPaint(p));
}

float jsui_canvas_measure_text(JsuiCanvas* c, const char* utf8, const JsuiFont* font) {
  if (!c || !utf8) return 0;
  SkFont f = jsui::skFont(c, font);
  return f.measureText(utf8, std::strlen(utf8), SkTextEncoding::kUTF8);
}

float jsui_canvas_draw_text(JsuiCanvas* c, const char* utf8, float x, float y, const JsuiFont* font,
                            const JsuiPaint* p) {
  if (!c || !utf8) return 0;
  SkFont f = jsui::skFont(c, font);
  size_t len = std::strlen(utf8);
  float w = f.measureText(utf8, len, SkTextEncoding::kUTF8);
  int align = font ? font->align : JSUI_TEXT_LEFT;
  if (align == JSUI_TEXT_CENTER) x -= w / 2;
  else if (align == JSUI_TEXT_RIGHT) x -= w;
  c->canvas->drawSimpleText(utf8, len, SkTextEncoding::kUTF8, x, y, f, jsui::skPaint(p));
  return w;
}

int jsui_canvas_draw_image(JsuiCanvas* c, const char* uri, float x, float y, float w, float h, float alpha) {
  if (!c || !uri || !*uri || !c->doc || !c->doc->images) return 0;
  sk_sp<SkImage> img = c->doc->images->get(c->doc->images->resolve(uri));
  if (!img) return 0;
  SkPaint p;
  p.setAlphaf(std::clamp(alpha, 0.f, 1.f));
  p.setAntiAlias(true);
  c->canvas->drawImageRect(img, jsui::rectOf(x, y, w, h), SkSamplingOptions(SkFilterMode::kLinear), &p);
  return 1;
}

int jsui_canvas_draw_texture(JsuiCanvas* c, const char* key, float x, float y, float w, float h, float alpha) {
  if (!c || !key || !*key || !c->doc || !c->doc->textures) return 0;
  std::string k = key;
  // A user of the texture: new frames (jsui_texture_updated) repaint the view.
  auto& used = c->st->textures;
  if (std::find(used.begin(), used.end(), k) == used.end()) {
    used.push_back(k);
    c->doc->textures->addUser(k, c->node);
  }
  sk_sp<SkImage> img = c->doc->textures->image(k);
  if (!img) return 0;
  const jsui::HostTextures::Entry* e = c->doc->textures->find(k);
  bool flip = e && !e->pixels && (e->desc.flags & JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN);
  SkPaint p;
  p.setAlphaf(std::clamp(alpha, 0.f, 1.f));
  p.setAntiAlias(true);
  SkRect dst = jsui::rectOf(x, y, w, h);
  if (flip) {
    c->canvas->save();
    c->canvas->translate(0, dst.top() + dst.bottom());
    c->canvas->scale(1, -1);
  }
  c->canvas->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear), &p);
  if (flip) c->canvas->restore();
  return 1;
}

void* jsui_canvas_skia(JsuiCanvas* c) { return c ? c->canvas : nullptr; }

JsuiPath* jsui_path_create(void) { return new JsuiPath; }
JsuiPath* jsui_path_from_svg(const char* d) {
  if (!d) return nullptr;
  SkPath p;
  if (!SkParsePath::FromSVGString(d, &p)) return nullptr;
  auto* out = new JsuiPath;
  out->builder = SkPathBuilder(p);
  out->dirty = true;
  return out;
}
void jsui_path_destroy(JsuiPath* path) { delete path; }
void jsui_path_reset(JsuiPath* path) {
  if (!path) return;
  path->builder.reset();
  path->dirty = true;
}
void jsui_path_move_to(JsuiPath* path, float x, float y) {
  if (!path) return;
  path->builder.moveTo(x, y);
  path->dirty = true;
}
void jsui_path_line_to(JsuiPath* path, float x, float y) {
  if (!path) return;
  path->builder.lineTo(x, y);
  path->dirty = true;
}
void jsui_path_quad_to(JsuiPath* path, float cx, float cy, float x, float y) {
  if (!path) return;
  path->builder.quadTo(cx, cy, x, y);
  path->dirty = true;
}
void jsui_path_cubic_to(JsuiPath* path, float c1x, float c1y, float c2x, float c2y, float x, float y) {
  if (!path) return;
  path->builder.cubicTo(c1x, c1y, c2x, c2y, x, y);
  path->dirty = true;
}
void jsui_path_close(JsuiPath* path) {
  if (!path) return;
  path->builder.close();
  path->dirty = true;
}
void jsui_path_add_rect(JsuiPath* path, float x, float y, float w, float h) {
  if (!path) return;
  path->builder.addRect(jsui::rectOf(x, y, w, h));
  path->dirty = true;
}
void jsui_path_add_circle(JsuiPath* path, float cx, float cy, float r) {
  if (!path) return;
  path->builder.addOval(SkRect::MakeLTRB(cx - r, cy - r, cx + r, cy + r));
  path->dirty = true;
}
void jsui_path_add_arc(JsuiPath* path, float x, float y, float w, float h, float start, float sweep) {
  if (!path) return;
  path->builder.addArc(jsui::rectOf(x, y, w, h), start, sweep);
  path->dirty = true;
}
void jsui_path_set_even_odd(JsuiPath* path, int even_odd) {
  if (!path) return;
  path->evenOdd = even_odd != 0;
  path->dirty = true;
}

}  // extern "C"
