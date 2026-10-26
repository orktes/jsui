#include "render/vector_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/document.h"
#include "core/style.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkDiscretePathEffect.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkPerlinNoiseShader.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkTrimPathEffect.h"
#if __has_include("include/effects/SkGradient.h")
#include "include/effects/SkGradient.h"  // Skia m157+ (the WebGPU wasm build)
#define JSUI_VECTOR_SKGRADIENT 1
#else
#include "include/effects/SkGradientShader.h"
#endif
#include "include/utils/SkParsePath.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "render/image_cache.h"
#include "render/shader_view.h"
#include "text/font_manager.h"

namespace jsui {
namespace vector {

namespace {

constexpr float kPi = 3.14159265358979f;
const char* const kDrawPrefix = "draw:";

enum class Kind : uint8_t { Group, Path, Rect, Circle, Ellipse, Oval, Line, Poly, Points, Text, Image, Fill, Unknown };

Kind kindOf(const std::string& t) {
  if (t == "g" || t == "group") return Kind::Group;
  if (t == "path") return Kind::Path;
  if (t == "rect") return Kind::Rect;
  if (t == "circle") return Kind::Circle;
  if (t == "ellipse") return Kind::Ellipse;
  if (t == "oval") return Kind::Oval;
  if (t == "line") return Kind::Line;
  if (t == "poly") return Kind::Poly;
  if (t == "points") return Kind::Points;
  if (t == "text") return Kind::Text;
  if (t == "image") return Kind::Image;
  if (t == "fill") return Kind::Fill;
  return Kind::Unknown;
}

// One parsed element. `props` points into the commands value the state
// keeps alive; SVG path data is parsed once and kept.
struct Elem {
  Kind kind = Kind::Unknown;
  int key = 0;  // Animated key (draw:<key>.<prop> style keys), 0: none
  const Value* props = nullptr;
  std::vector<Elem> kids;
  std::unique_ptr<Elem> clip;  // clip: {c, units, invert, rule}
  std::unique_ptr<Elem> mask;  // mask: {c, units, mode, x, y, width, height}
  // Cached path data (`d` / `path`), re-parsed when an Animated value changes it.
  mutable std::string pathSrc;
  mutable SkPath path;
  mutable bool pathOk = false;
};

struct State : Node::Cache {
  Value commands;  // what the tree points into
  Elem root;
  bool skia = false;
  size_t count = 0;
};

void parseElem(const Value& v, Elem* e, size_t* count) {
  e->props = &v;
  e->kind = kindOf(v["t"].asString());
  e->key = int(v["k"].asNumber(0));
  (*count)++;
  const Value::Array& c = v["c"].asArray();
  e->kids.resize(c.size());
  for (size_t i = 0; i < c.size(); i++) parseElem(c[i], &e->kids[i], count);
  if (v["clip"].isObject()) {
    e->clip = std::make_unique<Elem>();
    parseElem(v["clip"], e->clip.get(), count);
    e->clip->kind = Kind::Group;
  }
  if (v["mask"].isObject()) {
    e->mask = std::make_unique<Elem>();
    parseElem(v["mask"], e->mask.get(), count);
    e->mask->kind = Kind::Group;
  }
}

State* stateOf(Node* node) { return static_cast<State*>(node->imageCache.get()); }

// --------------------------------------------------------------- attributes

// Animated overrides of one paint: element key -> [(prop, value)].
using Overrides = std::unordered_map<int, std::vector<std::pair<std::string, const Value*>>>;

void collectOverrides(const Value& style, Overrides* out) {
  if (!style.isObject()) return;
  for (auto& [k, v] : style.asObject()) {
    if (k.compare(0, 5, kDrawPrefix) != 0) continue;
    char* end = nullptr;
    long key = std::strtol(k.c_str() + 5, &end, 10);
    if (!end || *end != '.' || key <= 0) continue;
    (*out)[int(key)].emplace_back(std::string(end + 1), &v);
  }
}

// An element's props with its Animated values over them.
struct Attrs {
  const Value* props;
  const std::vector<std::pair<std::string, const Value*>>* ov = nullptr;
  const Value& operator[](const char* k) const {
    if (ov)
      for (auto& [n, v] : *ov)
        if (n == k) return *v;
    return (*props)[k];
  }
  bool has(const char* k) const {
    if (ov)
      for (auto& [n, v] : *ov)
        if (n == k) return !v->isNull();
    return props->has(k) && !(*props)[k].isNull();
  }
};

float num(const Value& v, float def) {
  if (v.isNumber()) {
    double d = v.asNumber();
    return d == d ? float(d) : def;
  }
  if (v.isString()) {
    const std::string& s = v.asString();
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && d == d) return float(d);
  }
  if (v.isBool()) return v.asBool() ? 1.f : 0.f;
  return def;
}

SkPoint point(const Value& v, SkPoint def) {
  if (v.isArray() && v.asArray().size() >= 2) return {num(v.asArray()[0], def.fX), num(v.asArray()[1], def.fY)};
  if (v.isObject()) return {num(v["x"], def.fX), num(v["y"], def.fY)};
  return def;
}

SkColor4f color4(const Color& c, float alpha = 1) { return {c.r, c.g, c.b, std::clamp(c.a * alpha, 0.f, 1.f)}; }

bool colorOf(const Value& v, Color* out) {
  if (v.isNumber()) {
    // processColor() form: 0xAARRGGBB.
    uint32_t n = uint32_t(int64_t(v.asNumber()));
    *out = {((n >> 16) & 255) / 255.f, ((n >> 8) & 255) / 255.f, (n & 255) / 255.f, ((n >> 24) & 255) / 255.f};
    return true;
  }
  if (v.isArray() && v.asArray().size() >= 3) {
    // Skia.Color as [r, g, b, a] floats.
    const Value::Array& a = v.asArray();
    *out = {num(a[0], 0), num(a[1], 0), num(a[2], 0), a.size() > 3 ? num(a[3], 1) : 1.f};
    return true;
  }
  return parseColor(v, out);
}

SkBlendMode blendOf(const std::string& s, SkBlendMode def = SkBlendMode::kSrcOver) {
  static const std::pair<const char*, SkBlendMode> kModes[] = {
      {"clear", SkBlendMode::kClear},       {"src", SkBlendMode::kSrc},
      {"dst", SkBlendMode::kDst},           {"srcOver", SkBlendMode::kSrcOver},
      {"dstOver", SkBlendMode::kDstOver},   {"srcIn", SkBlendMode::kSrcIn},
      {"dstIn", SkBlendMode::kDstIn},       {"srcOut", SkBlendMode::kSrcOut},
      {"dstOut", SkBlendMode::kDstOut},     {"srcATop", SkBlendMode::kSrcATop},
      {"dstATop", SkBlendMode::kDstATop},   {"xor", SkBlendMode::kXor},
      {"plus", SkBlendMode::kPlus},         {"modulate", SkBlendMode::kModulate},
      {"screen", SkBlendMode::kScreen},     {"overlay", SkBlendMode::kOverlay},
      {"darken", SkBlendMode::kDarken},     {"lighten", SkBlendMode::kLighten},
      {"colorDodge", SkBlendMode::kColorDodge}, {"colorBurn", SkBlendMode::kColorBurn},
      {"hardLight", SkBlendMode::kHardLight},   {"softLight", SkBlendMode::kSoftLight},
      {"difference", SkBlendMode::kDifference}, {"exclusion", SkBlendMode::kExclusion},
      {"multiply", SkBlendMode::kMultiply}, {"hue", SkBlendMode::kHue},
      {"saturation", SkBlendMode::kSaturation}, {"color", SkBlendMode::kColor},
      {"luminosity", SkBlendMode::kLuminosity}};
  for (auto& [n, m] : kModes)
    if (s == n) return m;
  return def;
}

// A 2D transform: RN-style arrays ([{rotate}, {translateX}, ...], numbers in
// radians for Skia, degrees for SVG, or strings with units) about `origin`,
// or a 6-number matrix (a b c d e f, SVG order).
float angle(const Value& v, bool radians) {
  if (v.isString()) {
    const std::string& s = v.asString();
    float f = num(v, 0);
    if (s.find("rad") != std::string::npos) return f * 180 / kPi;
    if (s.find("turn") != std::string::npos) return f * 360;
    return f;
  }
  float f = num(v, 0);
  return radians ? f * 180 / kPi : f;
}

void concatMatrix6(SkMatrix* m, const Value& v) {
  const Value::Array& a = v.asArray();
  if (a.size() == 6) {
    m->preConcat(SkMatrix::MakeAll(num(a[0], 1), num(a[2], 0), num(a[4], 0), num(a[1], 0), num(a[3], 1), num(a[5], 0),
                                   0, 0, 1));
  } else if (a.size() == 9) {
    m->preConcat(SkMatrix::MakeAll(num(a[0], 1), num(a[1], 0), num(a[2], 0), num(a[3], 0), num(a[4], 1), num(a[5], 0),
                                   num(a[6], 0), num(a[7], 0), num(a[8], 1)));
  } else if (a.size() == 16) {
    // A 4x4 (Skia's Matrix4 / RN's matrix): the 2D part.
    m->preConcat(SkMatrix::MakeAll(num(a[0], 1), num(a[1], 0), num(a[3], 0), num(a[4], 0), num(a[5], 1), num(a[7], 0),
                                   num(a[12], 0), num(a[13], 0), num(a[15], 1)));
  }
}

void concatTransform(SkMatrix* m, const Value& t, bool radians) {
  if (!t.isArray()) return;
  const Value::Array& a = t.asArray();
  if (!a.empty() && (a[0].isNumber() || a[0].isString())) {
    concatMatrix6(m, t);
    return;
  }
  for (const Value& op : a) {
    if (!op.isObject()) continue;
    for (auto& [k, v] : op.asObject()) {
      if (k == "translateX") m->preTranslate(num(v, 0), 0);
      else if (k == "translateY") m->preTranslate(0, num(v, 0));
      else if (k == "translate") {
        SkPoint p = point(v, {0, 0});
        m->preTranslate(p.fX, p.fY);
      } else if (k == "scale") {
        if (v.isArray()) {
          SkPoint p = point(v, {1, 1});
          m->preScale(p.fX, p.fY);
        } else {
          m->preScale(num(v, 1), num(v, 1));
        }
      } else if (k == "scaleX") m->preScale(num(v, 1), 1);
      else if (k == "scaleY") m->preScale(1, num(v, 1));
      else if (k == "rotate" || k == "rotateZ") m->preRotate(angle(v, radians));
      else if (k == "skewX") m->preSkew(std::tan(angle(v, radians) * kPi / 180), 0);
      else if (k == "skewY") m->preSkew(0, std::tan(angle(v, radians) * kPi / 180));
      else if (k == "matrix") concatMatrix6(m, v);
    }
  }
}

// --------------------------------------------------------------- paint state

// What drawing elements inherit from their ancestors.
struct PState {
  bool skia = false;
  Color current{0, 0, 0, 1};  // SVG currentColor / Skia paint color
  float alpha = 1;            // Skia: opacity multiplies down the tree
  // SVG presentation attributes.
  const Value* fill = nullptr;    // null: black
  const Value* stroke = nullptr;  // null: none
  float fillOpacity = 1, strokeOpacity = 1, strokeWidth = 1, miter = 4;
  SkPaint::Cap cap = SkPaint::kButt_Cap;
  SkPaint::Join join = SkPaint::kMiter_Join;
  bool evenOdd = false;
  const Value* dash = nullptr;
  float dashOffset = 0;
  // Skia paint.
  bool strokeStyle = false;
  SkBlendMode blend = SkBlendMode::kSrcOver;
  bool antiAlias = true;
  const Value* shader = nullptr;
  const Value* maskFilter = nullptr;
  const Value* pathEffect = nullptr;
  const Value* colorFilter = nullptr;
  const Value* imageFilter = nullptr;
  // Text.
  float fontSize = 16;
  std::string fontFamily;
  int fontWeight = 400;
  bool italic = false;
  int anchor = 0;  // 0 start, 1 middle, 2 end
  float letterSpacing = 0;
};

SkPaint::Cap capOf(const std::string& s, SkPaint::Cap def) {
  if (s == "round") return SkPaint::kRound_Cap;
  if (s == "square") return SkPaint::kSquare_Cap;
  if (s == "butt") return SkPaint::kButt_Cap;
  return def;
}

SkPaint::Join joinOf(const std::string& s, SkPaint::Join def) {
  if (s == "round") return SkPaint::kRound_Join;
  if (s == "bevel") return SkPaint::kBevel_Join;
  if (s == "miter") return SkPaint::kMiter_Join;
  return def;
}

int weightOf(const Value& v, int def) {
  if (v.isNumber()) return int(v.asNumber());
  const std::string& s = v.asString();
  if (s == "bold") return 700;
  if (s == "normal") return 400;
  if (s == "bolder") return std::min(900, def + 300);
  if (s == "lighter") return std::max(100, def - 300);
  if (!s.empty() && std::isdigit(uint8_t(s[0]))) return std::atoi(s.c_str());
  return def;
}

// The inherited attributes an element sets.
void inherit(const Attrs& a, PState* s) {
  const Value* p = a.props;
  if (s->skia) {
    if (a.has("color")) colorOf(a["color"], &s->current);
    if (a.has("opacity")) s->alpha *= std::clamp(num(a["opacity"], 1), 0.f, 1.f);
    if (a.has("style")) s->strokeStyle = a["style"].asString() == "stroke";
    if (a.has("strokeWidth")) s->strokeWidth = std::max(0.f, num(a["strokeWidth"], 1));
    if (a.has("strokeCap")) s->cap = capOf(a["strokeCap"].asString(), s->cap);
    if (a.has("strokeJoin")) s->join = joinOf(a["strokeJoin"].asString(), s->join);
    if (a.has("strokeMiter")) s->miter = num(a["strokeMiter"], 4);
    if (a.has("blendMode")) s->blend = blendOf(a["blendMode"].asString());
    if (a.has("antiAlias")) s->antiAlias = a["antiAlias"].asBool(true);
    if (p->has("shader")) s->shader = &(*p)["shader"];
    if (p->has("maskFilter")) s->maskFilter = &(*p)["maskFilter"];
    if (p->has("pathEffect")) s->pathEffect = &(*p)["pathEffect"];
    if (p->has("colorFilter")) s->colorFilter = &(*p)["colorFilter"];
    const Value& font = (*p)["font"];
    if (font.isObject()) {
      if (font.has("size")) s->fontSize = num(font["size"], 16);
      if (font["family"].isString()) s->fontFamily = font["family"].asString();
      if (font.has("weight")) s->fontWeight = weightOf(font["weight"], s->fontWeight);
      if (font.has("style")) s->italic = font["style"].asString() == "italic";
    }
    return;
  }
  if (a.has("color")) colorOf(a["color"], &s->current);
  if (p->has("fill")) s->fill = &(*p)["fill"];
  if (p->has("stroke")) s->stroke = &(*p)["stroke"];
  if (a.has("fillOpacity")) s->fillOpacity = std::clamp(num(a["fillOpacity"], 1), 0.f, 1.f);
  if (a.has("strokeOpacity")) s->strokeOpacity = std::clamp(num(a["strokeOpacity"], 1), 0.f, 1.f);
  if (a.has("strokeWidth")) s->strokeWidth = std::max(0.f, num(a["strokeWidth"], 1));
  if (a.has("strokeLinecap")) s->cap = capOf(a["strokeLinecap"].asString(), s->cap);
  if (a.has("strokeLinejoin")) s->join = joinOf(a["strokeLinejoin"].asString(), s->join);
  if (a.has("strokeMiterlimit")) s->miter = num(a["strokeMiterlimit"], 4);
  if (a.has("fillRule")) s->evenOdd = a["fillRule"].asString() == "evenodd";
  if (p->has("strokeDasharray")) s->dash = &(*p)["strokeDasharray"];
  if (a.has("strokeDashoffset")) s->dashOffset = num(a["strokeDashoffset"], 0);
  if (a.has("fontSize")) s->fontSize = num(a["fontSize"], 16);
  if (a["fontFamily"].isString()) s->fontFamily = a["fontFamily"].asString();
  if (a.has("fontWeight")) s->fontWeight = weightOf(a["fontWeight"], s->fontWeight);
  if (a.has("fontStyle")) s->italic = a["fontStyle"].asString() == "italic";
  if (a.has("letterSpacing")) s->letterSpacing = num(a["letterSpacing"], 0);
  if (a.has("textAnchor")) {
    const std::string& t = a["textAnchor"].asString();
    s->anchor = t == "middle" ? 1 : t == "end" ? 2 : 0;
  }
  // Animated fill / stroke colors (the props hold no value then).
  if (a.ov)
    for (auto& [n, v] : *a.ov) {
      if (n == "fill") s->fill = v;
      else if (n == "stroke") s->stroke = v;
      else if (n == "strokeDasharray") s->dash = v;
    }
}

// --------------------------------------------------------------- shaders

SkTileMode tileOf(const std::string& t) {
  if (t == "repeat") return SkTileMode::kRepeat;
  if (t == "reflect" || t == "mirror") return SkTileMode::kMirror;
  if (t == "decal") return SkTileMode::kDecal;
  return SkTileMode::kClamp;
}

struct Stops {
  std::vector<SkColor4f> colors;
  std::vector<float> pos;
};

// SVG stops ([[offset, color, opacity]]) or Skia colors + positions.
bool stopsOf(const Value& g, const PState& s, float alpha, Stops* out) {
  const Value::Array& stops = g["stops"].asArray();
  float last = 0;
  for (const Value& st : stops) {
    const Value::Array& a = st.asArray();
    if (a.size() < 2) continue;
    Color c{0, 0, 0, 1};
    if (a[1].asString() == "currentColor") c = s.current;
    else colorOf(a[1], &c);
    float op = a.size() > 2 ? std::clamp(num(a[2], 1), 0.f, 1.f) : 1.f;
    last = std::max(last, std::clamp(num(a[0], 0), 0.f, 1.f));  // offsets never go back
    out->colors.push_back(color4(c, op * alpha));
    out->pos.push_back(last);
  }
  const Value::Array& colors = g["colors"].asArray();
  const Value::Array& positions = g["positions"].asArray();
  for (size_t i = 0; i < colors.size(); i++) {
    Color c{0, 0, 0, 1};
    colorOf(colors[i], &c);
    out->colors.push_back(color4(c, alpha));
    float p = i < positions.size() ? num(positions[i], 0)
                                   : (colors.size() > 1 ? float(i) / float(colors.size() - 1) : 0.f);
    out->pos.push_back(p);
  }
  if (out->colors.size() == 1) {
    out->colors.push_back(out->colors[0]);
    out->pos.push_back(1);
  }
  return out->colors.size() >= 2;
}

#if JSUI_VECTOR_SKGRADIENT
SkGradient gradientOf(const Stops& st, SkTileMode tile) {
  return SkGradient(SkGradient::Colors({st.colors.data(), st.colors.size()}, {st.pos.data(), st.pos.size()}, tile),
                    SkGradient::Interpolation());
}
#endif

sk_sp<SkShader> linear(SkPoint a, SkPoint b, const Stops& st, SkTileMode tile, const SkMatrix* m) {
  SkPoint pts[2] = {a, b};
#if JSUI_VECTOR_SKGRADIENT
  return SkShaders::LinearGradient(pts, gradientOf(st, tile), m);
#else
  return SkGradientShader::MakeLinear(pts, st.colors.data(), nullptr, st.pos.data(), int(st.colors.size()), tile, 0, m);
#endif
}

sk_sp<SkShader> radial(SkPoint c, float r, const Stops& st, SkTileMode tile, const SkMatrix* m) {
#if JSUI_VECTOR_SKGRADIENT
  return SkShaders::RadialGradient(c, r, gradientOf(st, tile), m);
#else
  return SkGradientShader::MakeRadial(c, r, st.colors.data(), nullptr, st.pos.data(), int(st.colors.size()), tile, 0,
                                      m);
#endif
}

sk_sp<SkShader> conical(SkPoint a, float ra, SkPoint b, float rb, const Stops& st, SkTileMode tile,
                        const SkMatrix* m) {
#if JSUI_VECTOR_SKGRADIENT
  return SkShaders::TwoPointConicalGradient(a, ra, b, rb, gradientOf(st, tile), m);
#else
  return SkGradientShader::MakeTwoPointConical(a, ra, b, rb, st.colors.data(), nullptr, st.pos.data(),
                                               int(st.colors.size()), tile, 0, m);
#endif
}

sk_sp<SkShader> sweep(SkPoint c, float start, float end, const Stops& st, SkTileMode tile, const SkMatrix* m) {
#if JSUI_VECTOR_SKGRADIENT
  return SkShaders::SweepGradient(c, start, end, gradientOf(st, tile), m);
#else
  return SkGradientShader::MakeSweep(c.fX, c.fY, st.colors.data(), nullptr, st.pos.data(), int(st.colors.size()), tile,
                                     start, end, 0, m);
#endif
}

struct Ctx {
  Node* node;
  Document* doc;
  SkCanvas* canvas;
  const Overrides* overrides;
  bool skia;
};

sk_sp<SkImage> imageOf(const Ctx& cx, const Value& spec) {
  if (!cx.doc || !cx.doc->images) return nullptr;
  std::string uri = spec.isString() ? spec.asString() : spec["uri"].asString();
  if (uri.empty()) return nullptr;
  return cx.doc->images->get(cx.doc->images->resolve(uri));
}

// Where an image of size (iw, ih) goes in `dst` for a fit mode (Skia's
// names; SVG's preserveAspectRatio arrives as contain / cover / fill with
// an alignment ax, ay in 0..1).
SkRect fitRect(float iw, float ih, const SkRect& dst, const std::string& fit, float ax, float ay) {
  float sx = dst.width() / iw, sy = dst.height() / ih;
  if (fit == "fill") {
    // stretched
  } else if (fit == "cover") {
    sx = sy = std::max(sx, sy);
  } else if (fit == "fitWidth") {
    sy = sx;
  } else if (fit == "fitHeight") {
    sx = sy;
  } else if (fit == "none") {
    sx = sy = 1;
  } else if (fit == "scaleDown") {
    sx = sy = std::min(1.f, std::min(sx, sy));
  } else {
    sx = sy = std::min(sx, sy);  // contain
  }
  float w = iw * sx, h = ih * sy;
  return SkRect::MakeXYWH(dst.left() + (dst.width() - w) * ax, dst.top() + (dst.height() - h) * ay, w, h);
}

sk_sp<SkShader> shaderOf(const Ctx& cx, const Value& g, const PState& s, const SkRect& bbox, float alpha);

sk_sp<SkShader> runtimeShader(const Ctx& cx, const Value& g, const PState& s, const SkRect& bbox) {
  if (!cx.doc || !cx.doc->shaders) return nullptr;
  const Value& src = g["source"];
  if (!src.isString()) return nullptr;
  std::shared_ptr<const ShaderEffects::Program> prog = cx.doc->shaders->get(src.asString());
  if (!prog || !prog->effect) return nullptr;
  SkRuntimeShaderBuilder b(prog->effect);
  const Value& given = g["uniforms"];
  std::vector<float> vals;
  for (const SkRuntimeEffect::Uniform& u : prog->effect->uniforms()) {
    std::string name(u.name);
    size_t n = u.sizeInBytes() / 4;
    vals.clear();
    std::vector<const Value*> todo{&given[name]};
    while (!todo.empty()) {
      const Value* v = todo.back();
      todo.pop_back();
      if (v->isNumber()) {
        vals.push_back(float(v->asNumber()));
      } else if (v->isBool()) {
        vals.push_back(v->asBool() ? 1.f : 0.f);
      } else if (v->isArray()) {
        for (auto it = v->asArray().rbegin(); it != v->asArray().rend(); ++it) todo.push_back(&*it);
      } else if (v->isObject() && v->has("x")) {
        vals.push_back(num((*v)["x"], 0));
        vals.push_back(num((*v)["y"], 0));
      } else {
        Color c;
        if (colorOf(*v, &c)) vals.insert(vals.end(), {c.r, c.g, c.b, c.a});
      }
    }
    vals.resize(n, 0.f);
    using T = SkRuntimeEffect::Uniform::Type;
    if (u.type == T::kInt || u.type == T::kInt2 || u.type == T::kInt3 || u.type == T::kInt4) {
      std::vector<int> ints(n);
      for (size_t i = 0; i < n; i++) ints[i] = int(std::lround(vals[i]));
      b.uniform(u.name).set(ints.data(), int(n));
    } else {
      b.uniform(u.name).set(vals.data(), int(n));
    }
  }
  const Value::Array& kids = g["children"].asArray();
  size_t i = 0;
  for (const SkRuntimeEffect::Child& c : prog->effect->children()) {
    if (c.type != SkRuntimeEffect::ChildType::kShader) continue;
    sk_sp<SkShader> sh = i < kids.size() ? shaderOf(cx, kids[i], s, bbox, 1) : nullptr;
    b.child(c.name) = sh ? sh : SkShaders::Color(SK_ColorTRANSPARENT);
    i++;
  }
  return b.makeShader();
}

// A paint server: a color, a gradient (SVG or Skia form), a runtime effect,
// an image, noise, or a blend of two shaders. `bbox`: the element's bounds
// (objectBoundingBox units).
sk_sp<SkShader> shaderOf(const Ctx& cx, const Value& g, const PState& s, const SkRect& bbox, float alpha) {
  const std::string& t = g["t"].asString();
  SkMatrix m;
  bool bboxUnits = g["units"].asString() == "bbox";
  if (bboxUnits) {
    if (bbox.isEmpty()) return nullptr;
    m.setTranslate(bbox.left(), bbox.top());
    m.preScale(bbox.width(), bbox.height());
  }
  concatTransform(&m, g["m"], false);
  concatTransform(&m, g["transform"], s.skia);
  SkTileMode tile = tileOf(g["spread"].isString() ? g["spread"].asString() : g["mode"].asString());
  if (t == "color") {
    Color c{0, 0, 0, 1};
    colorOf(g["color"], &c);
    return SkShaders::Color(color4(c, alpha), nullptr);
  }
  if (t == "linear" || t == "radial" || t == "sweep" || t == "conical") {
    Stops st;
    if (!stopsOf(g, s, alpha, &st)) {
      if (st.colors.size() == 1) return SkShaders::Color(st.colors[0], nullptr);
      return nullptr;
    }
    if (t == "linear") {
      SkPoint a = g.has("start") ? point(g["start"], {0, 0}) : SkPoint{num(g["x1"], 0), num(g["y1"], 0)};
      SkPoint b = g.has("end") ? point(g["end"], {1, 0}) : SkPoint{num(g["x2"], 1), num(g["y2"], 0)};
      if (a == b) return SkShaders::Color(st.colors.back(), nullptr);
      return linear(a, b, st, tile, &m);
    }
    if (t == "radial") {
      SkPoint c = g.has("c") ? point(g["c"], {0, 0}) : SkPoint{num(g["cx"], 0.5f), num(g["cy"], 0.5f)};
      float r = num(g["r"], 0.5f);
      if (r <= 0) return SkShaders::Color(st.colors.back(), nullptr);
      SkPoint f{num(g["fx"], c.fX), num(g["fy"], c.fY)};
      float fr = num(g["fr"], 0);
      if (f != c || fr > 0) return conical(f, fr, c, r, st, tile, &m);
      return radial(c, r, st, tile, &m);
    }
    if (t == "conical") {
      return conical(point(g["start"], {0, 0}), num(g["startR"], 0), point(g["end"], {0, 0}), num(g["endR"], 0), st,
                     tile, &m);
    }
    SkPoint c = g.has("c") ? point(g["c"], {0, 0}) : SkPoint{num(g["cx"], 0), num(g["cy"], 0)};
    return sweep(c, num(g["start"], 0), num(g["end"], 360), st, tile, &m);
  }
  if (t == "shader") {
    sk_sp<SkShader> sh = runtimeShader(cx, g, s, bbox);
    return sh && !m.isIdentity() ? sh->makeWithLocalMatrix(m) : sh;
  }
  if (t == "image") {
    sk_sp<SkImage> img = imageOf(cx, g["image"].isNull() ? g : g["image"]);
    if (!img) return nullptr;
    SkTileMode tx = tileOf(g["tx"].asString()), ty = tileOf(g["ty"].asString());
    const Value::Array& r = g["rect"].asArray();
    if (r.size() >= 4) {
      SkRect dst = SkRect::MakeXYWH(num(r[0], 0), num(r[1], 0), num(r[2], 0), num(r[3], 0));
      std::string fit = g["fit"].isString() ? g["fit"].asString() : "none";
      SkRect placed = fitRect(float(img->width()), float(img->height()), dst, fit, 0.5f, 0.5f);
      m.preTranslate(placed.left(), placed.top());
      m.preScale(placed.width() / img->width(), placed.height() / img->height());
    }
    SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    return img->makeShader(tx, ty, sampling, &m);
  }
  if (t == "fractalNoise" || t == "turbulence") {
    float fx = num(g["freqX"], 0.05f), fy = num(g["freqY"], fx);
    int oct = int(num(g["octaves"], 1));
    float seed = num(g["seed"], 0);
    SkISize tileSize = SkISize::Make(int(num(g["tileWidth"], 0)), int(num(g["tileHeight"], 0)));
    sk_sp<SkShader> sh = t == "fractalNoise" ? SkShaders::MakeFractalNoise(fx, fy, oct, seed, &tileSize)
                                             : SkShaders::MakeTurbulence(fx, fy, oct, seed, &tileSize);
    return sh && !m.isIdentity() ? sh->makeWithLocalMatrix(m) : sh;
  }
  if (t == "blend") {
    const Value::Array& kids = g["children"].asArray();
    if (kids.size() < 2) return kids.empty() ? nullptr : shaderOf(cx, kids[0], s, bbox, alpha);
    sk_sp<SkShader> dst = shaderOf(cx, kids[0], s, bbox, alpha);
    for (size_t i = 1; i < kids.size(); i++) {
      sk_sp<SkShader> src = shaderOf(cx, kids[i], s, bbox, alpha);
      if (!dst || !src) return dst ? dst : src;
      dst = SkShaders::Blend(blendOf(g["blendMode"].asString(), SkBlendMode::kSrcOver), dst, src);
    }
    return dst;
  }
  return nullptr;
}

// --------------------------------------------------------------- effects

sk_sp<SkPathEffect> pathEffectOf(const Value& v) {
  if (v.isArray()) {
    sk_sp<SkPathEffect> out;
    for (const Value& e : v.asArray()) {
      sk_sp<SkPathEffect> pe = pathEffectOf(e);
      if (!pe) continue;
      out = out ? SkPathEffect::MakeCompose(pe, out) : pe;
    }
    return out;
  }
  const std::string& t = v["t"].asString();
  if (t == "dash") {
    std::vector<SkScalar> iv;
    for (const Value& e : v["intervals"].asArray()) iv.push_back(std::max(0.f, num(e, 0)));
    if (iv.size() % 2) iv.insert(iv.end(), iv.begin(), iv.end());
    float sum = 0;
    for (float f : iv) sum += f;
    if (iv.empty() || sum <= 0) return nullptr;
#if JSUI_VECTOR_SKGRADIENT
    return SkDashPathEffect::Make({iv.data(), iv.size()}, num(v["phase"], 0));
#else
    return SkDashPathEffect::Make(iv.data(), int(iv.size()), num(v["phase"], 0));
#endif
  }
  if (t == "corner") return SkCornerPathEffect::Make(num(v["r"], 0));
  if (t == "discrete") return SkDiscretePathEffect::Make(num(v["length"], 4), num(v["deviation"], 1), uint32_t(num(v["seed"], 0)));
  return nullptr;
}

sk_sp<SkColorFilter> colorFilterOf(const Value& v) {
  if (v.isArray()) {
    sk_sp<SkColorFilter> out;
    for (const Value& e : v.asArray()) {
      sk_sp<SkColorFilter> cf = colorFilterOf(e);
      if (!cf) continue;
      out = out ? cf->makeComposed(out) : cf;
    }
    return out;
  }
  const std::string& t = v["t"].asString();
  if (t == "matrix") {
    const Value::Array& a = v["matrix"].asArray();
    if (a.size() < 20) return nullptr;
    float m[20];
    for (int i = 0; i < 20; i++) m[i] = num(a[i], 0);
    return SkColorFilters::Matrix(m);
  }
  if (t == "blend") {
    Color c{0, 0, 0, 1};
    colorOf(v["color"], &c);
    return SkColorFilters::Blend(color4(c).toSkColor(), blendOf(v["blendMode"].asString(), SkBlendMode::kSrcOver));
  }
  return nullptr;
}

sk_sp<SkImageFilter> imageFilterOf(const Value& v) {
  if (v.isArray()) {
    sk_sp<SkImageFilter> out;
    for (const Value& e : v.asArray()) {
      sk_sp<SkImageFilter> f = imageFilterOf(e);
      if (!f) continue;
      out = out ? SkImageFilters::Compose(f, out) : f;
    }
    return out;
  }
  const std::string& t = v["t"].asString();
  if (t == "blur") {
    SkPoint b = v["blur"].isArray() || v["blur"].isObject() ? point(v["blur"], {0, 0})
                                                            : SkPoint{num(v["blur"], 0), num(v["blur"], 0)};
    return SkImageFilters::Blur(b.fX, b.fY, tileOf(v["mode"].isString() ? v["mode"].asString() : "decal"), nullptr);
  }
  if (t == "shadow") {
    Color c{0, 0, 0, 1};
    colorOf(v["color"], &c);
    float sigma = num(v["blur"], 0);
    float dx = num(v["dx"], 0), dy = num(v["dy"], 0);
    if (v["inner"].asBool()) {
      // An inner shadow: the source's alpha inverted, offset, blurred and
      // tinted, kept inside the source (Skia's declarative InnerShadow).
      float inv[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1};
      sk_sp<SkImageFilter> f = SkImageFilters::ColorFilter(SkColorFilters::Matrix(inv), nullptr);
      f = SkImageFilters::Offset(dx, dy, f);
      f = SkImageFilters::Blur(sigma, sigma, SkTileMode::kDecal, f);
      f = SkImageFilters::ColorFilter(SkColorFilters::Blend(color4(c).toSkColor(), SkBlendMode::kSrcIn), f);
      sk_sp<SkImageFilter> inside = SkImageFilters::Blend(SkBlendMode::kSrcIn, nullptr, f);
      if (v["shadowOnly"].asBool()) return inside;
      return SkImageFilters::Blend(SkBlendMode::kSrcOver, nullptr, inside);
    }
    if (v["shadowOnly"].asBool()) return SkImageFilters::DropShadowOnly(dx, dy, sigma, sigma, color4(c).toSkColor(), nullptr);
    return SkImageFilters::DropShadow(dx, dy, sigma, sigma, color4(c).toSkColor(), nullptr);
  }
  if (t == "offset") return SkImageFilters::Offset(num(v["x"], 0), num(v["y"], 0), nullptr);
  if (t == "morphology") {
    float r = num(v["radius"], 0);
    return v["op"].asString() == "erode" ? SkImageFilters::Erode(r, r, nullptr) : SkImageFilters::Dilate(r, r, nullptr);
  }
  if (t == "colorFilter") return SkImageFilters::ColorFilter(colorFilterOf(v["filter"]), nullptr);
  return nullptr;
}

sk_sp<SkMaskFilter> maskFilterOf(const Value& v) {
  float blur = num(v["blur"], 0);
  if (blur <= 0) return nullptr;
  const std::string& st = v["style"].asString();
  SkBlurStyle style = st == "solid" ? kSolid_SkBlurStyle
                      : st == "outer" ? kOuter_SkBlurStyle
                      : st == "inner" ? kInner_SkBlurStyle
                                      : kNormal_SkBlurStyle;
  return SkMaskFilter::MakeBlur(style, blur, v["respectCTM"].asBool(true));
}

// --------------------------------------------------------------- geometry

const SkPath* pathData(const Elem& e, const Attrs& a) {
  const Value& d = a.has("d") ? a["d"] : a["path"];
  if (!d.isString()) return nullptr;
  if (!e.pathOk || e.pathSrc != d.asString()) {
    e.pathSrc = d.asString();
    e.path.reset();
    if (!SkParsePath::FromSVGString(e.pathSrc.c_str(), &e.path)) e.path.reset();
    e.pathOk = true;
  }
  return &e.path;
}

// The element's outline (false: it has none).
bool geometry(const Elem& e, const Attrs& a, const PState& s, SkPath* out) {
  SkPathBuilder b;
  switch (e.kind) {
    case Kind::Path: {
      const SkPath* p = pathData(e, a);
      if (!p) return false;
      *out = *p;
      const std::string& ft = a["fillType"].asString();
      if (!s.skia) out->setFillType(s.evenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
      else if (ft == "evenOdd") out->setFillType(SkPathFillType::kEvenOdd);
      else if (ft == "inverseWinding") out->setFillType(SkPathFillType::kInverseWinding);
      else if (ft == "inverseEvenOdd") out->setFillType(SkPathFillType::kInverseEvenOdd);
      else if (ft == "winding") out->setFillType(SkPathFillType::kWinding);
      return true;
    }
    case Kind::Rect: {
      float x = num(a["x"], 0), y = num(a["y"], 0), w = num(a["width"], 0), h = num(a["height"], 0);
      if (w <= 0 || h <= 0) return false;
      float rx = num(a["rx"], -1), ry = num(a["ry"], -1);
      if (a.has("r")) rx = ry = num(a["r"], 0);
      if (rx < 0) rx = ry;
      if (ry < 0) ry = rx;
      rx = std::clamp(rx, 0.f, w / 2);
      ry = std::clamp(ry, 0.f, h / 2);
      SkRect r = SkRect::MakeXYWH(x, y, w, h);
      if (rx > 0 && ry > 0) b.addRRect(SkRRect::MakeRectXY(r, rx, ry));
      else b.addRect(r);
      break;
    }
    case Kind::Circle: {
      float r = num(a["r"], 0);
      if (r <= 0) return false;
      b.addOval(SkRect::MakeLTRB(num(a["cx"], 0) - r, num(a["cy"], 0) - r, num(a["cx"], 0) + r, num(a["cy"], 0) + r));
      break;
    }
    case Kind::Ellipse: {
      float rx = num(a["rx"], 0), ry = num(a["ry"], 0);
      if (rx <= 0 || ry <= 0) return false;
      float cx = num(a["cx"], 0), cy = num(a["cy"], 0);
      b.addOval(SkRect::MakeLTRB(cx - rx, cy - ry, cx + rx, cy + ry));
      break;
    }
    case Kind::Oval: {
      float w = num(a["width"], 0), h = num(a["height"], 0);
      if (w <= 0 || h <= 0) return false;
      b.addOval(SkRect::MakeXYWH(num(a["x"], 0), num(a["y"], 0), w, h));
      break;
    }
    case Kind::Line:
      b.moveTo(num(a["x1"], 0), num(a["y1"], 0));
      b.lineTo(num(a["x2"], 0), num(a["y2"], 0));
      break;
    case Kind::Poly: {
      const Value::Array& p = a["points"].asArray();
      if (p.size() < 4) return false;
      b.moveTo(num(p[0], 0), num(p[1], 0));
      for (size_t i = 2; i + 1 < p.size(); i += 2) b.lineTo(num(p[i], 0), num(p[i + 1], 0));
      if (a["closed"].asBool()) b.close();
      break;
    }
    default:
      return false;
  }
  *out = b.detach();
  if (!s.skia && s.evenOdd) out->setFillType(SkPathFillType::kEvenOdd);
  return true;
}

// --------------------------------------------------------------- drawing

void drawElem(const Ctx& cx, const Elem& e, const PState& parent);

const std::vector<std::pair<std::string, const Value*>>* overridesOf(const Ctx& cx, int key) {
  if (!key || cx.overrides->empty()) return nullptr;
  auto it = cx.overrides->find(key);
  return it == cx.overrides->end() ? nullptr : &it->second;
}

// The clip path of a clip element (union of its shapes, each in its own
// transform), in the clipped element's space.
void clipPathOf(const Ctx& cx, const Elem& clip, const PState& s, const SkMatrix& m, SkPath* out) {
  for (const Elem& k : clip.kids) {
    Attrs a{k.props, overridesOf(cx, k.key)};
    SkMatrix km = m;
    concatTransform(&km, a["m"], false);
    concatTransform(&km, a["transform"], s.skia);
    if (k.kind == Kind::Group) {
      clipPathOf(cx, k, s, km, out);
      continue;
    }
    PState ks = s;
    inherit(a, &ks);
    SkPath p;
    if (!geometry(k, a, ks, &p)) continue;
    if (a["clipRule"].asString() == "evenodd") p.setFillType(SkPathFillType::kEvenOdd);
    if (p.getFillType() == SkPathFillType::kEvenOdd || out->isEmpty()) {
      // (An even-odd shape cannot be merged into a winding union: kept as is
      // when alone, otherwise approximated as winding.)
      if (out->isEmpty()) {
        *out = p.makeTransform(km);
        continue;
      }
    }
    SkPathBuilder b(*out);
    b.addPath(p.makeTransform(km));
    SkPathFillType ft = out->getFillType();
    *out = b.detach();
    out->setFillType(ft == SkPathFillType::kEvenOdd ? SkPathFillType::kWinding : ft);
  }
}

SkFont fontOf(const Ctx& cx, const PState& s) {
  sk_sp<SkTypeface> tf;
  if (cx.doc && cx.doc->fonts) {
    auto coll = cx.doc->fonts->collection();
    SkFontStyle style(s.fontWeight, SkFontStyle::kNormal_Width,
                      s.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    std::vector<SkString> fams;
    if (!s.fontFamily.empty()) {
      // A CSS family list: the first one that is registered.
      size_t i = 0;
      const std::string& f = s.fontFamily;
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
    fams.emplace_back(cx.doc->fonts->defaultFamily().c_str());
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
  SkFont font(tf, s.fontSize);
  font.setSubpixel(true);
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setHinting(SkFontHinting::kNone);
  font.setLinearMetrics(true);
  return font;
}

void setStroke(SkPaint* p, const PState& s) {
  p->setStyle(SkPaint::kStroke_Style);
  p->setStrokeWidth(s.strokeWidth);
  p->setStrokeCap(s.cap);
  p->setStrokeJoin(s.join);
  p->setStrokeMiter(s.miter);
}

// SVG: a fill or stroke paint (false: none).
bool svgPaint(const Ctx& cx, const Value* server, bool isFill, const PState& s, const SkRect& bbox, float opacity,
              SkPaint* p) {
  if (isFill && !server) {
    p->setColor4f({0, 0, 0, opacity}, nullptr);
  } else if (!server || server->isNull()) {
    return false;
  } else if (server->isObject()) {
    sk_sp<SkShader> sh = shaderOf(cx, *server, s, bbox, 1);
    if (!sh) {
      // A gradient that cannot paint (no stops, empty bbox): its fallback color.
      Color c;
      if (!colorOf((*server)["fallback"], &c)) return false;
      p->setColor4f(color4(c, opacity), nullptr);
    } else {
      p->setShader(std::move(sh));
      p->setAlphaf(opacity);
    }
  } else {
    const std::string& str = server->asString();
    if (str == "none") return false;
    Color c{0, 0, 0, 1};
    if (str == "currentColor") c = s.current;
    else if (!colorOf(*server, &c)) return false;
    if (c.a <= 0) return false;
    p->setColor4f(color4(c, opacity), nullptr);
  }
  p->setAntiAlias(true);
  if (!isFill) {
    setStroke(p, s);
    if (s.strokeWidth <= 0) return false;
    if (s.dash && s.dash->isArray() && !s.dash->asArray().empty()) {
      Value::Object d;
      d["t"] = Value("dash");
      d["intervals"] = *s.dash;
      d["phase"] = Value(double(s.dashOffset));
      if (sk_sp<SkPathEffect> pe = pathEffectOf(Value(std::move(d)))) p->setPathEffect(pe);
    }
  }
  return true;
}

// Skia: the paint an element draws with (its inherited paint plus an
// optional <Paint> child's props over it).
void skiaPaint(const Ctx& cx, const PState& s, const SkRect& bbox, SkPaint* p) {
  p->setAntiAlias(s.antiAlias);
  p->setColor4f(color4(s.current, s.alpha), nullptr);
  if (s.shader) {
    if (sk_sp<SkShader> sh = shaderOf(cx, *s.shader, s, bbox, 1)) {
      p->setShader(std::move(sh));
      p->setAlphaf(std::clamp(s.alpha * s.current.a, 0.f, 1.f));
    }
  }
  if (s.strokeStyle) setStroke(p, s);
  p->setBlendMode(s.blend);
  if (s.maskFilter) p->setMaskFilter(maskFilterOf(*s.maskFilter));
  if (s.pathEffect) p->setPathEffect(pathEffectOf(*s.pathEffect));
  if (s.colorFilter) p->setColorFilter(colorFilterOf(*s.colorFilter));
  if (s.imageFilter) p->setImageFilter(imageFilterOf(*s.imageFilter));
}

void drawText(const Ctx& cx, const Elem& e, const Attrs& a, const PState& s) {
  SkCanvas* c = cx.canvas;
  struct Run {
    std::string text;
    PState ps;
    bool hasX = false, hasY = false;
    float x = 0, y = 0, dx = 0, dy = 0, width = 0;
  };
  std::vector<Run> runs;
  auto addRun = [&](const Attrs& ra, const PState& base, bool own) {
    Run r;
    r.ps = base;
    if (own) inherit(ra, &r.ps);
    const Value& t = ra["text"];
    r.text = t.isString() ? t.asString() : t.isNumber() ? std::to_string(t.asNumber()) : std::string();
    r.hasX = ra.has("x");
    r.hasY = ra.has("y");
    r.x = num(ra["x"].isArray() && !ra["x"].asArray().empty() ? ra["x"].asArray()[0] : ra["x"], 0);
    r.y = num(ra["y"].isArray() && !ra["y"].asArray().empty() ? ra["y"].asArray()[0] : ra["y"], 0);
    r.dx = num(ra["dx"], 0);
    r.dy = num(ra["dy"], 0);
    runs.push_back(std::move(r));
  };
  addRun(a, s, false);
  for (const Value& rv : a["runs"].asArray()) addRun(Attrs{&rv, nullptr}, s, true);
  float penX = 0, penY = 0, total = 0;
  for (Run& r : runs) {
    if (r.text.empty()) continue;
    SkFont f = fontOf(cx, r.ps);
    r.width = f.measureText(r.text.data(), r.text.size(), SkTextEncoding::kUTF8) +
              r.ps.letterSpacing * float(r.text.size());
    total += r.width;
  }
  float shift = s.anchor == 1 ? -total / 2 : s.anchor == 2 ? -total : 0;
  bool first = true;
  for (Run& r : runs) {
    if (r.hasX || first) penX = r.x;
    if (r.hasY || first) penY = r.y;
    first = false;
    penX += r.dx;
    penY += r.dy;
    if (r.text.empty()) continue;
    SkFont f = fontOf(cx, r.ps);
    SkRect bbox = SkRect::MakeXYWH(penX + shift, penY - r.ps.fontSize, r.width, r.ps.fontSize * 1.2f);
    auto draw = [&](const SkPaint& p) {
      if (r.ps.letterSpacing == 0) {
        c->drawSimpleText(r.text.data(), r.text.size(), SkTextEncoding::kUTF8, penX + shift, penY, f, p);
        return;
      }
      // Letter spacing: one glyph run per code point.
      float x = penX + shift;
      const char* s8 = r.text.data();
      size_t i = 0;
      while (i < r.text.size()) {
        size_t n = 1;
        uint8_t ch = uint8_t(s8[i]);
        if (ch >= 0xF0) n = 4;
        else if (ch >= 0xE0) n = 3;
        else if (ch >= 0xC0) n = 2;
        n = std::min(n, r.text.size() - i);
        c->drawSimpleText(s8 + i, n, SkTextEncoding::kUTF8, x, penY, f, p);
        x += f.measureText(s8 + i, n, SkTextEncoding::kUTF8) + r.ps.letterSpacing;
        i += n;
      }
    };
    if (cx.skia) {
      SkPaint p;
      skiaPaint(cx, r.ps, bbox, &p);
      draw(p);
    } else {
      SkPaint fill, stroke;
      if (svgPaint(cx, r.ps.fill, true, r.ps, bbox, r.ps.fillOpacity, &fill)) draw(fill);
      if (svgPaint(cx, r.ps.stroke, false, r.ps, bbox, r.ps.strokeOpacity, &stroke)) draw(stroke);
    }
    penX += r.width;
  }
}

void drawImage(const Ctx& cx, const Attrs& a, const PState& s) {
  sk_sp<SkImage> img = imageOf(cx, a["image"].isNull() ? a["href"] : a["image"]);
  if (!img) return;
  float iw = float(img->width()), ih = float(img->height());
  const Value& im = a["image"];
  if (im.isObject() && im.has("width") && im.has("height")) {
    // Logical size of a scaled asset (@2x).
    iw = num(im["width"], iw);
    ih = num(im["height"], ih);
  }
  float w = a.has("width") ? num(a["width"], 0) : iw, h = a.has("height") ? num(a["height"], 0) : ih;
  if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0) return;
  SkRect dst = SkRect::MakeXYWH(num(a["x"], 0), num(a["y"], 0), w, h);
  std::string fit = a["fit"].isString() ? a["fit"].asString() : "contain";
  SkRect placed = fitRect(iw, ih, dst, fit, num(a["alignX"], 0.5f), num(a["alignY"], 0.5f));
  SkPaint p;
  if (s.skia) skiaPaint(cx, s, dst, &p);
  p.setShader(nullptr);
  p.setStyle(SkPaint::kFill_Style);
  p.setColor4f({0, 0, 0, std::clamp(s.skia ? s.alpha : 1.f, 0.f, 1.f)}, nullptr);
  SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
  cx.canvas->save();
  cx.canvas->clipRect(dst, true);
  cx.canvas->drawImageRect(img, SkRect::MakeIWH(img->width(), img->height()), placed, sampling, &p,
                           SkCanvas::kFast_SrcRectConstraint);
  cx.canvas->restore();
}

void drawPoints(const Ctx& cx, const Attrs& a, const PState& s, const SkPaint& paint) {
  const Value::Array& v = a["points"].asArray();
  std::vector<SkPoint> pts;
  for (size_t i = 0; i + 1 < v.size(); i += 2) pts.push_back({num(v[i], 0), num(v[i + 1], 0)});
  for (const Value& p : v)
    if (p.isObject()) pts.push_back(point(p, {0, 0}));
  if (pts.empty()) return;
  const std::string& m = a["mode"].asString();
  SkCanvas::PointMode mode = m == "lines" ? SkCanvas::kLines_PointMode
                             : m == "polygon" ? SkCanvas::kPolygon_PointMode
                                              : SkCanvas::kPoints_PointMode;
  SkPaint p = paint;
  p.setStyle(SkPaint::kStroke_Style);
#if JSUI_VECTOR_SKGRADIENT
  cx.canvas->drawPoints(mode, {pts.data(), pts.size()}, p);
#else
  cx.canvas->drawPoints(mode, pts.size(), pts.data(), p);
#endif
}

// Draws `path` with the element's paints.
void drawShape(const Ctx& cx, const Elem& e, const Attrs& a, const PState& s, const SkPath& path) {
  SkCanvas* c = cx.canvas;
  SkRect bbox = path.getBounds();
  // Skia: start / end trim the path (0..1 of its length).
  sk_sp<SkPathEffect> trim;
  if (a.has("start") || a.has("end")) {
    float t0 = std::clamp(num(a["start"], 0), 0.f, 1.f), t1 = std::clamp(num(a["end"], 1), 0.f, 1.f);
    if (t1 <= t0) return;
    if (t0 > 0 || t1 < 1) trim = SkTrimPathEffect::Make(t0, t1);
  }
  auto withTrim = [&](SkPaint* p) {
    if (!trim) return;
    p->setPathEffect(p->getPathEffect() ? SkPathEffect::MakeCompose(p->refPathEffect(), trim) : trim);
  };
  if (cx.skia) {
    auto drawWith = [&](const PState& ps) {
      SkPaint p;
      skiaPaint(cx, ps, bbox, &p);
      if (e.kind == Kind::Line) setStroke(&p, ps);  // drawLine ignores the style
      withTrim(&p);
      c->drawPath(path, p);
    };
    drawWith(s);
    for (const Value& pv : (*e.props)["paints"].asArray()) {
      PState ps = s;
      ps.shader = nullptr;
      ps.maskFilter = ps.pathEffect = ps.colorFilter = ps.imageFilter = nullptr;
      Attrs pa{&pv, nullptr};
      inherit(pa, &ps);
      if (pv.has("imageFilter")) ps.imageFilter = &pv["imageFilter"];
      drawWith(ps);
    }
    return;
  }
  SkPaint fill, stroke;
  bool hasFill = e.kind != Kind::Line && svgPaint(cx, s.fill, true, s, bbox, s.fillOpacity, &fill);
  bool hasStroke = svgPaint(cx, s.stroke, false, s, bbox, s.strokeOpacity, &stroke);
  if (hasFill) {
    withTrim(&fill);
    c->drawPath(path, fill);
  }
  if (hasStroke) {
    withTrim(&stroke);
    c->drawPath(path, stroke);
  }
}

void drawContent(const Ctx& cx, const Elem& e, const Attrs& a, const PState& s) {
  switch (e.kind) {
    case Kind::Group:
    case Kind::Unknown:
      for (const Elem& k : e.kids) drawElem(cx, k, s);
      return;
    case Kind::Text:
      drawText(cx, e, a, s);
      return;
    case Kind::Image:
      drawImage(cx, a, s);
      return;
    case Kind::Fill: {
      SkPaint p;
      if (cx.skia) skiaPaint(cx, s, cx.canvas->getLocalClipBounds(), &p);
      else svgPaint(cx, s.fill, true, s, cx.canvas->getLocalClipBounds(), s.fillOpacity, &p);
      p.setStyle(SkPaint::kFill_Style);
      cx.canvas->drawPaint(p);
      return;
    }
    case Kind::Points: {
      SkPaint p;
      skiaPaint(cx, s, SkRect::MakeEmpty(), &p);
      if (!cx.skia) svgPaint(cx, s.stroke, false, s, SkRect::MakeEmpty(), s.strokeOpacity, &p);
      drawPoints(cx, a, s, p);
      return;
    }
    default: {
      SkPath path;
      if (geometry(e, a, s, &path)) drawShape(cx, e, a, s, path);
      for (const Elem& k : e.kids) drawElem(cx, k, s);  // (Skia: shapes may hold drawings)
      return;
    }
  }
}

// The element's own geometry bounds (objectBoundingBox units of clips and
// masks); a group: the union of its children's.
SkRect boundsOf(const Ctx& cx, const Elem& e, const PState& s) {
  Attrs a{e.props, overridesOf(cx, e.key)};
  SkPath p;
  if (e.kind != Kind::Group && geometry(e, a, s, &p)) return p.getBounds();
  SkRect r = SkRect::MakeEmpty();
  for (const Elem& k : e.kids) {
    Attrs ka{k.props, overridesOf(cx, k.key)};
    SkMatrix m;
    concatTransform(&m, ka["m"], false);
    concatTransform(&m, ka["transform"], s.skia);
    r.join(m.mapRect(boundsOf(cx, k, s)));
  }
  return r;
}

void drawElem(const Ctx& cx, const Elem& e, const PState& parent) {
  Attrs a{e.props, overridesOf(cx, e.key)};
  if (a["display"].asString() == "none" || a["visibility"].asString() == "hidden") return;
  PState s = parent;
  inherit(a, &s);
  SkCanvas* c = cx.canvas;
  int saved = c->save();
  // Transform: the static matrix, then the RN-style array about `origin`.
  SkMatrix m;
  concatTransform(&m, a["m"], false);
  if (a.has("transform")) {
    SkPoint o = point(a["origin"], {0, 0});
    m.preTranslate(o.fX, o.fY);
    concatTransform(&m, a["transform"], s.skia);
    m.preTranslate(-o.fX, -o.fY);
  }
  if (!m.isIdentity()) c->concat(m);
  // Clip: an SVG clipPath / Skia clip (shapes; units bbox map 0..1 onto the
  // element's bounds).
  if (e.clip) {
    const Value& cp = *e.clip->props;
    SkMatrix cm;
    if (cp["units"].asString() == "bbox") {
      SkRect b = boundsOf(cx, e, s);
      cm.setTranslate(b.left(), b.top());
      cm.preScale(b.width(), b.height());
    }
    concatTransform(&cm, cp["m"], false);
    SkPath clip;
    clipPathOf(cx, *e.clip, s, cm, &clip);
    if (cp["invert"].asBool()) clip.toggleInverseFillType();
    c->clipPath(clip, true);
  }
  // Opacity (SVG: the element as a group) and Skia layers / image filters.
  float opacity = s.skia ? 1.f : std::clamp(num(a["opacity"], 1), 0.f, 1.f);
  if (opacity <= 0) {
    c->restoreToCount(saved);
    return;
  }
  const Value& layer = (*e.props)["layer"];
  const Value& imageFilter = (*e.props)["imageFilter"];
  bool group = e.kind == Kind::Group || !e.kids.empty();
  bool svgBoth = !s.skia && s.fill && s.stroke;
  if (opacity < 1 && (group || svgBoth || e.mask)) {
    c->saveLayerAlphaf(nullptr, opacity);
  } else if (opacity < 1) {
    s.fillOpacity *= opacity;
    s.strokeOpacity *= opacity;
  }
  if (layer.isObject() || (group && imageFilter.isArray() && !imageFilter.asArray().empty())) {
    SkPaint lp;
    if (layer.isObject()) {
      lp.setAlphaf(std::clamp(num(layer["opacity"], 1), 0.f, 1.f));
      if (layer.has("imageFilter")) lp.setImageFilter(imageFilterOf(layer["imageFilter"]));
      if (layer.has("colorFilter")) lp.setColorFilter(colorFilterOf(layer["colorFilter"]));
      if (layer.has("blendMode")) lp.setBlendMode(blendOf(layer["blendMode"].asString()));
    } else {
      lp.setImageFilter(imageFilterOf(imageFilter));
    }
    c->saveLayer(nullptr, &lp);
  } else if (!group && imageFilter.isArray()) {
    s.imageFilter = &imageFilter;  // on the shape's paint
  }
  if (e.mask) {
    // Mask: the content, then the mask's luminance (or alpha) multiplied in.
    c->saveLayer(nullptr, nullptr);
    drawContent(cx, e, a, s);
    const Value& mp = *e.mask->props;
    SkPaint mpaint;
    mpaint.setBlendMode(SkBlendMode::kDstIn);
    if (mp["mode"].asString() != "alpha") {
      // Luminance (linear RGB coefficients of SVG's luminanceToAlpha) into alpha.
      float luma[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0};
      mpaint.setColorFilter(SkColorFilters::Matrix(luma));
    }
    c->saveLayer(nullptr, &mpaint);
    SkMatrix mm;
    if (mp["units"].asString() == "bbox") {
      SkRect b = boundsOf(cx, e, s);
      mm.setTranslate(b.left(), b.top());
      mm.preScale(b.width(), b.height());
    }
    concatTransform(&mm, mp["m"], false);
    c->concat(mm);
    PState ms;
    ms.skia = s.skia;
    ms.current = s.current;
    for (const Elem& k : e.mask->kids) drawElem(cx, k, ms);
    c->restore();
    c->restore();
  } else {
    drawContent(cx, e, a, s);
  }
  c->restoreToCount(saved);
}

// The viewBox transform into a w x h box (SVG preserveAspectRatio).
SkMatrix viewBoxMatrix(const Value& vb, const std::string& align, bool slice, float w, float h) {
  SkMatrix m;
  const Value::Array& v = vb.asArray();
  if (v.size() < 4) return m;
  float x = num(v[0], 0), y = num(v[1], 0), vw = num(v[2], 0), vh = num(v[3], 0);
  if (vw <= 0 || vh <= 0) return m;
  float sx = w / vw, sy = h / vh;
  if (align == "none") {
    m.setScale(sx, sy);
    m.preTranslate(-x, -y);
    return m;
  }
  float s = slice ? std::max(sx, sy) : std::min(sx, sy);
  float ax = 0.5f, ay = 0.5f;
  if (align.size() >= 8) {
    std::string xa = align.substr(1, 3), ya = align.substr(5, 3);
    ax = xa == "Min" ? 0.f : xa == "Max" ? 1.f : 0.5f;
    ay = ya == "Min" ? 0.f : ya == "Max" ? 1.f : 0.5f;
  }
  m.setTranslate((w - vw * s) * ax, (h - vh * s) * ay);
  m.preScale(s, s);
  m.preTranslate(-x, -y);
  return m;
}

}  // namespace

void propsChanged(Node* node, const Value& old) {
  const Value& cmds = node->props()["commands"];
  State* st = stateOf(node);
  if (st && st->commands.equals(cmds)) return;
  auto ns = std::make_unique<State>();
  ns->commands = cmds;
  ns->skia = cmds["mode"].asString() == "skia";
  ns->root.kind = Kind::Group;
  ns->root.props = &ns->commands;
  const Value::Array& c = ns->commands["c"].asArray();
  ns->root.kids.resize(c.size());
  for (size_t i = 0; i < c.size(); i++) parseElem(c[i], &ns->root.kids[i], &ns->count);
  node->imageCache = std::move(ns);
}

size_t elementCount(Node* node) {
  State* st = stateOf(node);
  return st ? st->count : 0;
}

void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip) {
  State* st = stateOf(node);
  if (!st || st->root.kids.empty() || box.isEmpty()) return;
  Overrides ov;
  collectOverrides(node->props()["style"], &ov);
  Ctx cx{node, node->document(), canvas, &ov, st->skia};
  const Value& cmds = st->commands;
  canvas->save();
  if (clip.isRect()) canvas->clipRect(clip.rect(), true);
  else canvas->clipRRect(clip, true);
  canvas->translate(box.left(), box.top());
  canvas->clipRect(SkRect::MakeWH(box.width(), box.height()), true);
  SkMatrix vb = viewBoxMatrix(cmds["vb"], cmds["align"].asString(), cmds["slice"].asBool(), box.width(), box.height());
  if (!vb.isIdentity()) canvas->concat(vb);
  PState s;
  s.skia = st->skia;
  if (cmds.has("color")) colorOf(cmds["color"], &s.current);
  if (!s.skia) {
    // The root <Svg>'s own presentation props.
    Attrs ra{&cmds, nullptr};
    inherit(ra, &s);
  }
  for (const Elem& e : st->root.kids) drawElem(cx, e, s);
  canvas->restore();
}

}  // namespace vector
}  // namespace jsui
