#include "core/state_style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/style.h"

namespace jsui {

namespace {

const char* const kSlotProps[kStateSlots] = {
    "groupHoveredStyle", "groupFocusedStyle", "groupFocusVisibleStyle", "groupPressedStyle", "hoveredStyle",
    "focusedStyle",      "focusVisibleStyle", "pressedStyle",           "disabledStyle",
    nullptr,  // kHidden: from `visibility`
};
// transition: {hovered: ..., pressed: ...} per state (group states use their state's name)
const char* const kSlotNames[kStateSlots] = {
    "hovered", "focused", "focusVisible", "pressed", "hovered", "focused", "focusVisible", "pressed", "disabled",
    "visibility",
};

// StyleSheet.flatten over Values: nested arrays, falsy entries skipped,
// later objects win.
void flattenInto(const Value& v, Value::Object& out, int depth) {
  if (depth > 32) return;
  if (v.isArray()) {
    for (const Value& e : v.asArray()) flattenInto(e, out, depth + 1);
  } else if (v.isObject()) {
    for (auto& [k, x] : v.asObject()) out.insert_or_assign(k, x);
  }
}

Value flattenStyle(const Value& v) {
  if (v.isObject()) return v;
  if (!v.isArray()) return Value();
  Value::Object out;
  flattenInto(v, out, 0);
  return Value(std::move(out));
}

void setBezier(float* b, float x1, float y1, float x2, float y2) {
  b[0] = x1;
  b[1] = y1;
  b[2] = x2;
  b[3] = y2;
}

void parseEasing(const Value& e, float* b) {
  if (e.isArray() && e.asArray().size() == 4) {
    const auto& a = e.asArray();
    setBezier(b, std::clamp(float(a[0].asNumber()), 0.f, 1.f), float(a[1].asNumber()),
              std::clamp(float(a[2].asNumber()), 0.f, 1.f), float(a[3].asNumber(1)));
    return;
  }
  const std::string& s = e.asString();
  if (s == "linear") setBezier(b, 0, 0, 1, 1);
  else if (s == "easeIn" || s == "ease-in") setBezier(b, 0.42f, 0, 1, 1);
  else if (s == "easeOut" || s == "ease-out") setBezier(b, 0, 0, 0.58f, 1);
  else if (s == "easeInOut" || s == "ease-in-out") setBezier(b, 0.42f, 0, 0.58f, 1);
  else setBezier(b, 0.25f, 0.1f, 0.25f, 1);  // "ease" (CSS's default)
}

// One transition spec (a number of ms, or {duration, easing, delay}) over
// what `inherit` gave.
void parseTiming(const Value& t, double* duration, double* delay, float* bezier) {
  if (t.isNumber()) {
    *duration = std::max(0.0, t.asNumber());
  } else if (t.isObject()) {
    if (t["duration"].isNumber()) *duration = std::max(0.0, t["duration"].asNumber());
    if (t["delay"].isNumber()) *delay = std::max(0.0, t["delay"].asNumber());
    if (!t["easing"].isNull()) parseEasing(t["easing"], bezier);
  }
}

// x(t) of a cubic bezier with P0 = 0, P3 = 1.
double bez(double a, double b, double t) {
  double u = 1 - t;
  return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double ease(const float* c, double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  if (c[0] == c[1] && c[2] == c[3]) return x;  // linear
  // Solve x(t) = x (bisection: monotonic because x1, x2 are in 0..1).
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 40; i++) {
    double v = bez(c[0], c[2], t);
    if (std::fabs(v - x) < 1e-7) break;
    if (v < x) lo = t;
    else hi = t;
    t = (lo + hi) / 2;
  }
  return bez(c[1], c[3], t);
}

bool isColorKey(const std::string& k) {
  return k == "color" || (k.size() > 5 && k.compare(k.size() - 5, 5, "Color") == 0);
}

std::string colorString(const Color& c) {
  char buf[96];
  auto ch = [](float v) { return std::round(std::clamp(v, 0.f, 1.f) * 255 * 1000) / 1000; };
  std::snprintf(buf, sizeof buf, "rgba(%g, %g, %g, %g)", ch(c.r), ch(c.g), ch(c.b),
                std::round(std::clamp(c.a, 0.f, 1.f) * 10000) / 10000);
  return buf;
}

// "45deg" / "0.5rad" / "100grad" / "0.25turn" / a number (degrees) -> degrees.
bool parseAngle(const Value& v, double* deg) {
  if (v.isNumber()) {
    *deg = v.asNumber();
    return true;
  }
  if (!v.isString()) return false;
  const std::string& s = v.asString();
  char* end = nullptr;
  double n = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  std::string unit(end);
  if (unit == "deg" || unit.empty()) *deg = n;
  else if (unit == "rad") *deg = n * 57.29577951308232;
  else if (unit == "grad") *deg = n * 0.9;
  else if (unit == "turn") *deg = n * 360;
  else return false;
  return true;
}

bool isAngleFunction(const std::string& f) { return f.rfind("rotate", 0) == 0 || f.rfind("skew", 0) == 0; }

// The resting value of a transform function (no effect).
Value identityOf(const std::string& f) {
  if (f.rfind("scale", 0) == 0) return Value(1);
  if (isAngleFunction(f)) return Value("0deg");
  if (f == "matrix") {
    Value::Array m(16, Value(0));
    for (int i = 0; i < 4; i++) m[size_t(i * 5)] = Value(1);
    return Value(std::move(m));
  }
  return Value(0);  // translate*, perspective (handled as a number)
}

// The single function name of a transform entry ({rotate: "45deg"}).
const std::string* fnOf(const Value& entry) {
  if (!entry.isObject() || entry.asObject().size() != 1) return nullptr;
  return &entry.asObject().begin()->first;
}

// Resting value for a key the base style does not set, shaped like `b`
// (null: the key has no neutral value; the change switches).
Value restingValue(const std::string& key, const Value& b) {
  if (key == "transform" && b.isArray()) {
    Value::Array out;
    for (const Value& e : b.asArray()) {
      const std::string* f = fnOf(e);
      if (!f) return Value();
      Value::Object o;
      o.emplace(*f, identityOf(*f));
      out.push_back(Value(std::move(o)));
    }
    return Value(std::move(out));
  }
  if (isColorKey(key)) {
    Color c;
    if (!parseColor(b, &c)) return Value();
    c.a = 0;  // the same color, transparent: a fade, not a pass through black
    return Value(colorString(c));
  }
  if (key == "opacity") return Value(1);
  if (key == "shadowOffset" || key == "textShadowOffset") {
    Value::Object o;
    o.emplace("width", Value(0));
    o.emplace("height", Value(0));
    return Value(std::move(o));
  }
  static const char* const kZero[] = {"shadowOpacity", "shadowRadius", "textShadowRadius", "elevation",
                                      "letterSpacing", "outlineWidth", "outlineOffset"};
  for (const char* k : kZero)
    if (key == k) return Value(0);
  auto starts = [&](const char* p) { return key.rfind(p, 0) == 0; };
  bool width = key.size() > 5 && key.compare(key.size() - 5, 5, "Width") == 0;
  bool radius = key.size() > 6 && key.compare(key.size() - 6, 6, "Radius") == 0;
  if (b.isNumber() && ((starts("border") && (width || radius)) || starts("margin") || starts("padding")))
    return Value(0);
  return Value();
}

Value mixNumbers(double a, double b, float w) { return Value(a + (b - a) * double(w)); }

}  // namespace

bool StateStyles::hasGroupOverlays() const {
  for (int i = kGroupHovered; i <= kGroupPressed; i++)
    if (overlay[i].isObject()) return true;
  return false;
}

bool wantsStateStyles(const Value& props) {
  if (!props.isObject()) return false;
  for (const char* k : kSlotProps)
    if (k && props.has(k)) return true;
  if (props["visibility"].isObject()) return true;
  const Value& g = props["group"];
  return g.asBool() || (g.isString() && !g.asString().empty()) || props["interaction"].isObject();
}

void parseStateStyles(const Value& props, StateStyles* s) {
  s->raw = props;
  s->base = flattenStyle(props["style"]);
  s->keys.clear();
  for (int i = 0; i < kStateSlots; i++) {
    Value o = kSlotProps[i] ? flattenStyle(props[kSlotProps[i]]) : Value();
    s->overlay[i] = o.isObject() && !o.asObject().empty() ? o : Value();
    for (auto& [k, v] : s->overlay[i].asObject())
      if (std::find(s->keys.begin(), s->keys.end(), k) == s->keys.end()) s->keys.push_back(k);
  }
  // <Visibility>: visibility {mode: remove | hide, effect: fade | scale |
  // none, scale}; with it, visible: false hides (an Animated `visible` is
  // written natively instead, Node::setHidden).
  const Value& vis = props["visibility"];
  if (vis.isObject()) {
    std::string effect = vis["effect"].isString() ? vis["effect"].asString()
                         : props["transition"].isNull() ? "none" : "fade";
    Value::Object o;
    if (effect == "fade" || effect == "scale") o.emplace("opacity", Value(0));
    if (effect == "scale") {
      Value::Object sc;
      sc.emplace("scale", Value(vis["scale"].asNumber(0.9)));
      o.emplace("transform", Value(Value::Array{Value(std::move(sc))}));
    }
    Value::Object end;
    if (vis["mode"].asString() == "hide") {
      end.emplace("pointerEvents", Value("none"));
      if (!o.count("opacity")) end.emplace("opacity", Value(0));
    } else {
      end.emplace("display", Value("none"));
    }
    s->overlay[kHidden] = o.empty() ? Value() : Value(std::move(o));
    for (auto& [k, v] : s->overlay[kHidden].asObject())
      if (std::find(s->keys.begin(), s->keys.end(), k) == s->keys.end()) s->keys.push_back(k);
    for (auto& [k, v] : end)
      if (std::find(s->keys.begin(), s->keys.end(), k) == s->keys.end()) s->keys.push_back(k);
    s->hiddenEnd = Value(std::move(end));
  } else {
    s->hiddenEnd = Value();
  }
  if (vis.isObject() && props.has("visible")) {
    const Value& v = props["visible"];
    s->hidden = !(v.asBool() || (v.isNumber() && v.asNumber() != 0));
  }
  if (!s->seen) s->weight[kHidden] = s->hidden ? 1.f : 0.f;
  // transition: ms, or {duration, easing, delay, hovered: ..., pressed: ...}
  const Value& t = props["transition"];
  s->hasTransition = t.isNumber() || t.isObject();
  double duration = 0, delay = 0;
  float bezier[4];
  parseEasing(Value(), bezier);
  parseTiming(t, &duration, &delay, bezier);
  for (int i = 0; i < kStateSlots; i++) {
    s->duration[i] = duration;
    s->delay[i] = delay;
    std::copy(bezier, bezier + 4, s->bezier[i]);
    if (t.isObject()) parseTiming(t[kSlotNames[i]], &s->duration[i], &s->delay[i], s->bezier[i]);
  }
  const Value& g = props["group"];
  s->isGroup = g.asBool() || (g.isString() && !g.asString().empty());
  s->groupKey = g.isString() ? g.asString() : std::string();
  s->groupName = props["groupName"].asString();
  s->interaction = props["interaction"]["key"].asString();
  const Value& d = props["disabled"];
  s->disabled = d.asBool() || (d.isNumber() && d.asNumber() != 0);
  // Mounting disabled shows disabledStyle in the first frame.
  if (!s->seen) s->weight[kDisabled] = s->disabled && s->overlay[kDisabled].isObject() ? 1.f : 0.f;
}

Value mixStyleValue(const std::string& key, const Value& a0, const Value& b, float w) {
  if (w >= 1) return b;
  Value a = a0.isNull() ? restingValue(key, b) : a0;
  if (w <= 0) return a0;
  if (a.isNull()) return w >= 0.5f ? b : a0;
  if (a.isNumber() && b.isNumber()) return mixNumbers(a.asNumber(), b.asNumber(), w);
  if (key == "transform" && a.isArray() && b.isArray() && a.asArray().size() == b.asArray().size()) {
    Value::Array out;
    const auto& aa = a.asArray();
    const auto& ba = b.asArray();
    for (size_t i = 0; i < aa.size(); i++) {
      const std::string* fa = fnOf(aa[i]);
      const std::string* fb = fnOf(ba[i]);
      if (!fa || !fb || *fa != *fb) return w >= 0.5f ? b : a0;
      const Value& x = aa[i][*fa];
      const Value& y = ba[i][*fb];
      Value m;
      double dx, dy;
      if (isAngleFunction(*fa) && parseAngle(x, &dx) && parseAngle(y, &dy)) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.17gdeg", dx + (dy - dx) * double(w));
        m = Value(buf);
      } else if (x.isNumber() && y.isNumber()) {
        m = mixNumbers(x.asNumber(), y.asNumber(), w);
      } else if (x.isArray() && y.isArray() && x.asArray().size() == y.asArray().size()) {  // matrix
        Value::Array v;
        for (size_t k = 0; k < x.asArray().size(); k++)
          v.push_back(mixNumbers(x.asArray()[k].asNumber(), y.asArray()[k].asNumber(), w));
        m = Value(std::move(v));
      } else {
        return w >= 0.5f ? b : a0;
      }
      Value::Object o;
      o.emplace(*fa, std::move(m));
      out.push_back(Value(std::move(o)));
    }
    return Value(std::move(out));
  }
  if (a.isString() && b.isString() && a.asString() == b.asString()) return b;
  Color ca, cb;
  if ((isColorKey(key) || a.isString()) && parseColor(a, &ca) && parseColor(b, &cb)) {
    // Non-premultiplied mix, like CSS transitions in sRGB.
    float aw = ca.a + (cb.a - ca.a) * w;
    Color c;
    if (aw > 0) {
      // Weighted by alpha, so a fade from transparent keeps the target hue.
      c.r = (ca.r * ca.a * (1 - w) + cb.r * cb.a * w) / aw;
      c.g = (ca.g * ca.a * (1 - w) + cb.g * cb.a * w) / aw;
      c.b = (ca.b * ca.a * (1 - w) + cb.b * cb.a * w) / aw;
    }
    c.a = aw;
    return Value(colorString(c));
  }
  if (a.isObject() && b.isObject() && a.asObject().size() == b.asObject().size()) {  // shadowOffset
    Value::Object out;
    for (auto& [k, x] : a.asObject()) {
      const Value& y = b[k];
      if (!x.isNumber() || !y.isNumber()) return w >= 0.5f ? b : a0;
      out.emplace(k, mixNumbers(x.asNumber(), y.asNumber(), w));
    }
    return Value(std::move(out));
  }
  return w >= 0.5f ? b : a0;
}

namespace {

Value blendKey(const StateStyles& s, const std::string& key) {
  Value v = s.base[key];
  for (int i = 0; i < kStateSlots; i++) {
    float w = s.weight[i];
    if (w <= 0 || !s.overlay[i].has(key)) continue;
    v = mixStyleValue(key, v, s.overlay[i][key], w);
  }
  if (s.weight[kHidden] >= 1 && s.hiddenEnd.has(key)) v = s.hiddenEnd[key];
  return v;
}

}  // namespace

Value blendStateStyle(const StateStyles& s) {
  bool any = s.weight[kHidden] >= 1 && s.hiddenEnd.isObject();
  for (int i = 0; i < kStateSlots && !any; i++) any = s.weight[i] > 0 && s.overlay[i].isObject();
  if (!any) return s.base;
  Value out = s.base.isObject() ? s.base : Value(Value::Object());
  Value::Object& o = out.mutableObject();
  for (const std::string& k : s.keys) {
    Value v = blendKey(s, k);
    if (v.isNull()) o.erase(k);
    else o.insert_or_assign(k, std::move(v));
  }
  return out;
}

Value::Object blendStateKeys(const StateStyles& s) {
  Value::Object out;
  for (const std::string& k : s.keys) out.emplace(k, blendKey(s, k));
  return out;
}

void inheritGroupTransition(StateStyles& s, const StateStyles& g) {
  if (s.hasTransition) return;
  for (int i = kGroupHovered; i <= kGroupPressed; i++) {
    int own = kHovered + (i - kGroupHovered);  // the group element's own state
    s.duration[i] = g.duration[own];
    s.delay[i] = g.delay[own];
    std::copy(g.bezier[own], g.bezier[own] + 4, s.bezier[i]);
  }
}

bool stepStateWeights(StateStyles& s, const bool targets[kStateSlots], double nowMs, bool* moving) {
  bool changed = false;
  // A change seen a second time in one frame (native Animated toggling
  // Visibility after tickStates) still starts a frame back.
  if (nowMs > s.lastMs) s.frameMs = std::clamp(nowMs - s.lastMs, 0.0, 1000.0 / 60);
  for (int i = 0; i < kStateSlots; i++) {
    float goal = targets[i] ? 1.f : 0.f;
    if (!s.seen) {
      // The state an element mounts in shows at once (no transition from
      // nothing, like CSS on first style).
      s.target[i] = targets[i];
      changed |= s.weight[i] != goal;
      s.weight[i] = s.from[i] = goal;
      continue;
    }
    if (targets[i] != s.target[i]) {
      // The input came after the last frame: the transition starts there
      // (at most a 60 Hz frame back, the host may have idled), so this frame
      // already shows one frame of it.
      s.target[i] = targets[i];
      s.from[i] = s.weight[i];
      s.start[i] = nowMs - s.frameMs;
    }
    if (s.weight[i] == goal) continue;
    double t = s.duration[i] > 0 ? (nowMs - s.start[i] - s.delay[i]) / s.duration[i] : 1;
    float w = t >= 1 ? goal : float(s.from[i] + (goal - s.from[i]) * ease(s.bezier[i], std::max(0.0, t)));
    if (w != s.weight[i]) {
      s.weight[i] = w;
      changed = true;
    }
    if (w != goal) *moving = true;
  }
  s.seen = true;
  s.lastMs = nowMs;
  return changed;
}

}  // namespace jsui
