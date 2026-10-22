// Style parsing: React Native style objects (plus the CSS-flavoured values
// react-native-web and RN >= 0.76 accept) into ComputedStyle + Yoga.
//
// applyStyle() writes *every* Yoga property on each call (unset ones to their
// RN default). Yoga's setters compare against the current value and only
// dirty the node on a real change, so a paint-only style update (opacity,
// transform, colors) never invalidates layout.
#include "core/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace jsui {

const char* const kBlendModeNames[] = {"normal",     "multiply",   "screen",      "overlay",    "darken",
                                       "lighten",    "color-dodge", "color-burn", "hard-light", "soft-light",
                                       "difference", "exclusion",  "hue",         "saturation", "color",
                                       "luminosity", "plus-lighter"};
const int kBlendModeCount = int(sizeof(kBlendModeNames) / sizeof(kBlendModeNames[0]));

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ------------------------------------------------------------ string utils

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(std::tolower((unsigned char)c));
  return r;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

// Splits at `sep` outside parentheses.
std::vector<std::string_view> splitTopLevel(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '(') depth++;
    else if (c == ')') depth = std::max(0, depth - 1);
    else if (c == sep && depth == 0) {
      out.push_back(trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  out.push_back(trim(s.substr(start)));
  return out;
}

// Splits on whitespace outside parentheses.
std::vector<std::string_view> tokens(std::string_view s) {
  std::vector<std::string_view> out;
  int depth = 0;
  size_t start = std::string_view::npos;
  for (size_t i = 0; i <= s.size(); i++) {
    char c = i < s.size() ? s[i] : ' ';
    bool space = std::isspace((unsigned char)c) && depth == 0;
    if (c == '(') depth++;
    else if (c == ')') depth = std::max(0, depth - 1);
    if (space) {
      if (start != std::string_view::npos) out.push_back(s.substr(start, i - start));
      start = std::string_view::npos;
    } else if (start == std::string_view::npos) {
      start = i;
    }
  }
  return out;
}

// "name(args)" -> name, args. Returns false if not a function call.
bool splitFunction(std::string_view s, std::string_view* name, std::string_view* args) {
  s = trim(s);
  size_t open = s.find('(');
  if (open == std::string_view::npos || s.empty() || s.back() != ')') return false;
  *name = trim(s.substr(0, open));
  *args = s.substr(open + 1, s.size() - open - 2);
  return true;
}

// Parses a leading number; `unit` receives the (lower-cased) rest.
bool parseNumberUnit(std::string_view s, float* num, std::string* unit) {
  s = trim(s);
  if (s.empty()) return false;
  std::string tmp(s);
  char* end = nullptr;
  double d = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str() || !std::isfinite(d)) return false;
  *num = float(d);
  *unit = lower(trim(std::string_view(end)));
  return true;
}

// A dimension: px (also unitless), %, or angle units converted to degrees.
struct Dim {
  float v = 0;
  enum Unit { Px, Percent, Deg, None } unit = Px;
};

bool parseDim(std::string_view s, Dim* out) {
  float n;
  std::string u;
  if (!parseNumberUnit(s, &n, &u)) return false;
  if (u.empty()) *out = {n, Dim::None};
  else if (u == "px") *out = {n, Dim::Px};
  else if (u == "%") *out = {n, Dim::Percent};
  else if (u == "deg") *out = {n, Dim::Deg};
  else if (u == "rad") *out = {n * 180.f / kPi, Dim::Deg};
  else if (u == "turn") *out = {n * 360.f, Dim::Deg};
  else if (u == "grad") *out = {n * 0.9f, Dim::Deg};
  else if (u == "rem" || u == "em") *out = {n * 16.f, Dim::Px};  // no font context here: 1em = 16px like the web root
  else if (u == "pt") *out = {n * 4.f / 3.f, Dim::Px};
  else return false;
  return true;
}

bool dimFromValue(const Value& v, Dim* out) {
  if (v.isNumber()) {
    double d = v.asNumber();
    if (!std::isfinite(d)) return false;
    *out = {float(d), Dim::None};
    return true;
  }
  if (v.isString()) return parseDim(v.asString(), out);
  return false;
}

// Number (or "12px") in px.
bool pxFromValue(const Value& v, float* out) {
  Dim d;
  if (!dimFromValue(v, &d) || (d.unit != Dim::Px && d.unit != Dim::None)) return false;
  *out = d.v;
  return true;
}

// Plain number (or numeric string).
bool numFromValue(const Value& v, float* out) {
  Dim d;
  if (!dimFromValue(v, &d)) return false;
  if (d.unit == Dim::Percent) d.v /= 100.f;
  else if (d.unit != Dim::None && d.unit != Dim::Px) return false;
  *out = d.v;
  return true;
}

// Angle in degrees. Unitless numbers are degrees too (RN accepts "45" rarely; 0 is common).
bool angleFromString(std::string_view s, float* deg) {
  Dim d;
  if (!parseDim(s, &d)) return false;
  if (d.unit == Dim::Deg || d.unit == Dim::None) {
    *deg = d.v;
    return true;
  }
  return false;
}
bool angleFromValue(const Value& v, float* deg) {
  if (v.isNumber()) {
    *deg = float(v.asNumber());
    return std::isfinite(*deg);
  }
  return v.isString() && angleFromString(v.asString(), deg);
}

// ------------------------------------------------------------------ colors

struct NamedColor {
  const char* name;
  uint32_t rgb;
};
// The 148 CSS named colors (CSS Color 4), sorted for binary search.
const NamedColor kNamedColors[] = {
    {"aliceblue", 0xf0f8ff},
    {"antiquewhite", 0xfaebd7},
    {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4},
    {"black", 0x000000},
    {"blanchedalmond", 0xffebcd},
    {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2},
    {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0},
    {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e},
    {"coral", 0xff7f50},
    {"cornflowerblue", 0x6495ed},
    {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c},
    {"cyan", 0x00ffff},
    {"darkblue", 0x00008b},
    {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9},
    {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a},
    {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969},
    {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff},
    {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700},
    {"goldenrod", 0xdaa520},
    {"gray", 0x808080},
    {"green", 0x008000},
    {"greenyellow", 0xadff2f},
    {"grey", 0x808080},
    {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4},
    {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082},
    {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c},
    {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd},
    {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0},
    {"lime", 0x00ff00},
    {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff},
    {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db},
    {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970},
    {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead},
    {"navy", 0x000080},
    {"oldlace", 0xfdf5e6},
    {"olive", 0x808000},
    {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500},
    {"orangered", 0xff4500},
    {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa},
    {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f},
    {"pink", 0xffc0cb},
    {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6},
    {"purple", 0x800080},
    {"rebeccapurple", 0x663399},
    {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0},
    {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd},
    {"slategray", 0x708090},
    {"slategrey", 0x708090},
    {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c},
    {"teal", 0x008080},
    {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee},
    {"wheat", 0xf5deb3},
    {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
};

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// rgb() channel: number 0..255 or percentage.
bool colorChannel(std::string_view s, float* out) {
  Dim d;
  if (!parseDim(s, &d)) return false;
  if (d.unit == Dim::Percent) *out = clamp01(d.v / 100.f);
  else if (d.unit == Dim::None) *out = clamp01(d.v / 255.f);
  else return false;
  return true;
}
bool alphaChannel(std::string_view s, float* out) {
  Dim d;
  if (!parseDim(s, &d)) return false;
  if (d.unit == Dim::Percent) *out = clamp01(d.v / 100.f);
  else if (d.unit == Dim::None) *out = clamp01(d.v);
  else return false;
  return true;
}

// Splits color function args: commas or spaces, optional "/ alpha".
std::vector<std::string_view> colorArgs(std::string_view args) {
  std::vector<std::string_view> out;
  std::string_view alpha;
  size_t slash = args.find('/');
  std::string_view main = args;
  if (slash != std::string_view::npos) {
    main = args.substr(0, slash);
    alpha = trim(args.substr(slash + 1));
  }
  size_t i = 0;
  while (i < main.size()) {
    while (i < main.size() && (std::isspace((unsigned char)main[i]) || main[i] == ',')) i++;
    size_t start = i;
    while (i < main.size() && !std::isspace((unsigned char)main[i]) && main[i] != ',') i++;
    if (i > start) out.push_back(main.substr(start, i - start));
  }
  if (!alpha.empty()) out.push_back(alpha);
  return out;
}

float hueToRgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.f / 6) return p + (q - p) * 6 * t;
  if (t < 1.f / 2) return q;
  if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
  return p;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool parseColor(const std::string& input, Color* out) {
  std::string_view s = trim(input);
  if (s.empty()) return false;
  if (s[0] == '#') {
    std::string_view h = s.substr(1);
    for (char c : h)
      if (hexDigit(c) < 0) return false;
    int v[4] = {0, 0, 0, 255};
    if (h.size() == 3 || h.size() == 4) {
      for (size_t i = 0; i < h.size(); i++) v[i] = hexDigit(h[i]) * 17;
    } else if (h.size() == 6 || h.size() == 8) {
      for (size_t i = 0; i < h.size() / 2; i++) v[i] = hexDigit(h[2 * i]) * 16 + hexDigit(h[2 * i + 1]);
    } else {
      return false;
    }
    *out = Color::rgba(v[0], v[1], v[2], v[3] / 255.f);
    return true;
  }
  std::string_view name, args;
  if (splitFunction(s, &name, &args)) {
    std::string fn = lower(name);
    auto a = colorArgs(args);
    if (fn == "rgb" || fn == "rgba") {
      if (a.size() != 3 && a.size() != 4) return false;
      Color c;
      c.a = 1;
      if (!colorChannel(a[0], &c.r) || !colorChannel(a[1], &c.g) || !colorChannel(a[2], &c.b)) return false;
      if (a.size() == 4 && !alphaChannel(a[3], &c.a)) return false;
      *out = c;
      return true;
    }
    if (fn == "hsl" || fn == "hsla") {
      if (a.size() != 3 && a.size() != 4) return false;
      float h, sat, l, alpha = 1;
      Dim ds, dl;
      if (!angleFromString(a[0], &h) || !parseDim(a[1], &ds) || !parseDim(a[2], &dl)) return false;
      sat = clamp01(ds.v / 100.f);
      l = clamp01(dl.v / 100.f);
      if (a.size() == 4 && !alphaChannel(a[3], &alpha)) return false;
      h = std::fmod(h, 360.f);
      if (h < 0) h += 360;
      h /= 360.f;
      Color c;
      if (sat == 0) {
        c.r = c.g = c.b = l;
      } else {
        float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
        float p = 2 * l - q;
        c.r = hueToRgb(p, q, h + 1.f / 3);
        c.g = hueToRgb(p, q, h);
        c.b = hueToRgb(p, q, h - 1.f / 3);
      }
      c.a = alpha;
      *out = c;
      return true;
    }
    return false;
  }
  std::string n = lower(s);
  if (n == "transparent") {
    *out = {};
    return true;
  }
  auto it = std::lower_bound(std::begin(kNamedColors), std::end(kNamedColors), n,
                             [](const NamedColor& c, const std::string& k) { return std::strcmp(c.name, k.c_str()) < 0; });
  if (it != std::end(kNamedColors) && n == it->name) {
    *out = Color::rgba(int(it->rgb >> 16), int((it->rgb >> 8) & 255), int(it->rgb & 255));
    return true;
  }
  return false;
}

bool parseColor(const Value& v, Color* out) {
  if (v.isString()) return parseColor(v.asString(), out);
  if (v.isNumber()) {
    // processColor() output: 0xAARRGGBB.
    uint32_t c = uint32_t(int64_t(v.asNumber()));
    *out = Color::rgba(int((c >> 16) & 255), int((c >> 8) & 255), int(c & 255), float(c >> 24) / 255.f);
    return true;
  }
  return false;
}

bool parseLength(const Value& v, Length* out) {
  Dim d;
  if (!dimFromValue(v, &d)) return false;
  if (d.unit == Dim::Percent) *out = {d.v, true};
  else if (d.unit == Dim::Px || d.unit == Dim::None) *out = {d.v, false};
  else return false;
  return true;
}

// ------------------------------------------------------------- box-shadow

namespace {

// One CSS shadow ("inset? <len>{2,4} <color>?" in any order). allowSpread
// false for text-shadow / drop-shadow.
bool parseShadowString(std::string_view s, Shadow* out, bool allowSpread) {
  Shadow sh;
  sh.color = {0, 0, 0, 1};  // currentcolor; black is RN's default text color
  float lens[4];
  int nlen = 0;
  bool haveColor = false;
  for (auto t : tokens(s)) {
    if (iequals(t, "inset")) {
      if (!allowSpread) return false;
      sh.inset = true;
      continue;
    }
    Dim d;
    if (parseDim(t, &d) && (d.unit == Dim::Px || d.unit == Dim::None)) {
      if (nlen >= (allowSpread ? 4 : 3)) return false;
      lens[nlen++] = d.v;
      continue;
    }
    Color c;
    if (!haveColor && parseColor(std::string(t), &c)) {
      sh.color = c;
      haveColor = true;
      continue;
    }
    return false;
  }
  if (nlen < 2) return false;
  sh.offsetX = lens[0];
  sh.offsetY = lens[1];
  sh.blur = nlen > 2 ? std::max(0.f, lens[2]) : 0;
  sh.spread = nlen > 3 ? lens[3] : 0;
  *out = sh;
  return true;
}

bool parseShadowObject(const Value& o, Shadow* out) {
  Shadow sh;
  sh.color = {0, 0, 0, 1};
  if (!pxFromValue(o["offsetX"], &sh.offsetX) || !pxFromValue(o["offsetY"], &sh.offsetY)) return false;
  pxFromValue(o["blurRadius"], &sh.blur);
  sh.blur = std::max(0.f, sh.blur);
  if (!pxFromValue(o["spreadDistance"], &sh.spread)) pxFromValue(o["spreadRadius"], &sh.spread);
  if (o.has("color")) parseColor(o["color"], &sh.color);
  sh.inset = o["inset"].asBool();
  *out = sh;
  return true;
}

}  // namespace

bool parseBoxShadow(const Value& v, std::vector<Shadow>* out) {
  out->clear();
  if (v.isString()) {
    std::string_view s = trim(v.asString());
    if (s.empty() || iequals(s, "none")) return true;
    for (auto part : splitTopLevel(s, ',')) {
      Shadow sh;
      if (!parseShadowString(part, &sh, true)) {
        out->clear();
        return false;
      }
      out->push_back(sh);
    }
    return true;
  }
  if (v.isArray()) {
    for (auto& e : v.asArray()) {
      Shadow sh;
      bool ok = e.isString() ? parseShadowString(e.asString(), &sh, true) : parseShadowObject(e, &sh);
      if (!ok) {
        out->clear();
        return false;
      }
      out->push_back(sh);
    }
    return true;
  }
  if (v.isObject()) {
    Shadow sh;
    if (!parseShadowObject(v, &sh)) return false;
    out->push_back(sh);
    return true;
  }
  return false;
}

// --------------------------------------------------------------- gradients

namespace {

// "to right", "to top left" -> angle (or a corner code resolved at paint time).
bool parseSideOrCorner(std::string_view s, Gradient* g) {
  auto t = tokens(s);
  if (t.size() < 2 || t.size() > 3 || !iequals(t[0], "to")) return false;
  bool top = false, bottom = false, left = false, right = false;
  for (size_t i = 1; i < t.size(); i++) {
    if (iequals(t[i], "top")) top = true;
    else if (iequals(t[i], "bottom")) bottom = true;
    else if (iequals(t[i], "left")) left = true;
    else if (iequals(t[i], "right")) right = true;
    else return false;
  }
  if ((top && bottom) || (left && right)) return false;
  g->corner = 0;
  if (t.size() == 2) {
    g->angle = top ? 0 : right ? 90 : bottom ? 180 : 270;
  } else {
    if (top && right) g->corner = 1;
    else if (bottom && right) g->corner = 2;
    else if (bottom && left) g->corner = 3;
    else if (top && left) g->corner = 4;
    else return false;
    g->angle = g->corner == 1 ? 45 : g->corner == 2 ? 135 : g->corner == 3 ? 225 : 315;  // square-box fallback
  }
  return true;
}

// "<color> [<pos> [<pos>]]" -> 1 or 2 stops.
bool parseColorStop(std::string_view s, std::vector<GradientStop>* stops) {
  auto t = tokens(s);
  if (t.empty() || t.size() > 3) return false;
  GradientStop st;
  size_t colorIdx = 0;
  // CSS allows the position before the color too ("10% red" is invalid, but be lenient on order).
  if (!parseColor(std::string(t[0]), &st.color)) {
    if (t.size() < 2 || !parseColor(std::string(t.back()), &st.color)) return false;
    colorIdx = t.size() - 1;
  }
  int added = 0;
  for (size_t i = 0; i < t.size(); i++) {
    if (i == colorIdx) continue;
    Dim d;
    if (!parseDim(t[i], &d)) return false;
    GradientStop p = st;
    if (d.unit == Dim::Percent) p.position = d.v / 100.f;
    else if (d.unit == Dim::Px || (d.unit == Dim::None && d.v == 0)) {
      p.position = d.v;
      p.px = d.v != 0;
    } else return false;
    stops->push_back(p);
    added++;
  }
  if (!added) stops->push_back(st);
  return true;
}

bool parsePositionKeyword(std::string_view t, bool horizontal, Length* out) {
  if (iequals(t, "center")) *out = {50, true};
  else if (horizontal && iequals(t, "left")) *out = {0, true};
  else if (horizontal && iequals(t, "right")) *out = {100, true};
  else if (!horizontal && iequals(t, "top")) *out = {0, true};
  else if (!horizontal && iequals(t, "bottom")) *out = {100, true};
  else return parseLength(Value(std::string(t)), out);
  return true;
}

// "<x> <y>" / single keyword position, for radial "at ..." and transformOrigin.
bool parsePosition2(const std::vector<std::string_view>& t, Length* x, Length* y) {
  if (t.empty() || t.size() > 2) return false;
  Length lx{50, true}, ly{50, true};
  if (t.size() == 1) {
    if (iequals(t[0], "top") || iequals(t[0], "bottom")) {
      if (!parsePositionKeyword(t[0], false, &ly)) return false;
    } else if (!parsePositionKeyword(t[0], true, &lx)) {
      return false;
    }
  } else {
    // Keywords may come in either order ("top left").
    std::string_view a = t[0], b = t[1];
    if (iequals(a, "top") || iequals(a, "bottom") || iequals(b, "left") || iequals(b, "right")) std::swap(a, b);
    if (!parsePositionKeyword(a, true, &lx) || !parsePositionKeyword(b, false, &ly)) return false;
  }
  *x = lx;
  *y = ly;
  return true;
}

bool parseGradientString(std::string_view s, Gradient* out) {
  std::string_view name, args;
  if (!splitFunction(s, &name, &args)) return false;
  std::string fn = lower(name);
  Gradient g;
  if (fn == "linear-gradient" || fn == "repeating-linear-gradient") {
    g.kind = Gradient::Kind::Linear;
  } else if (fn == "radial-gradient" || fn == "repeating-radial-gradient") {
    g.kind = Gradient::Kind::Radial;
  } else {
    return false;
  }
  g.repeating = fn.rfind("repeating-", 0) == 0;
  auto parts = splitTopLevel(args, ',');
  size_t first = 0;
  if (!parts.empty()) {
    std::string_view p0 = parts[0];
    if (g.kind == Gradient::Kind::Linear) {
      float deg;
      if (angleFromString(p0, &deg)) {
        g.angle = deg;
        first = 1;
      } else if (parseSideOrCorner(p0, &g)) {
        first = 1;
      }
    } else {
      auto t = tokens(p0);
      bool isConfig = false;
      std::vector<std::string_view> pos, sizes;
      bool inAt = false;
      for (auto tok : t) {
        if (inAt) {
          pos.push_back(tok);
          continue;
        }
        if (iequals(tok, "at")) inAt = isConfig = true;
        else if (iequals(tok, "circle")) g.circle = isConfig = true;
        else if (iequals(tok, "ellipse")) isConfig = true;
        else if (iequals(tok, "closest-side")) g.extent = Gradient::Extent::ClosestSide, isConfig = true;
        else if (iequals(tok, "closest-corner")) g.extent = Gradient::Extent::ClosestCorner, isConfig = true;
        else if (iequals(tok, "farthest-side")) g.extent = Gradient::Extent::FarthestSide, isConfig = true;
        else if (iequals(tok, "farthest-corner")) g.extent = Gradient::Extent::FarthestCorner, isConfig = true;
        else sizes.push_back(tok);
      }
      if (isConfig || !sizes.empty()) {
        // Sizes only count if the part is not a color stop.
        Color c;
        if (!isConfig && !sizes.empty() && parseColor(std::string(sizes[0]), &c)) {
          // color stop; leave first = 0
        } else {
          if (!sizes.empty()) {
            if (sizes.size() > 2) return false;
            if (!parseLength(Value(std::string(sizes[0])), &g.sizeX)) return false;
            g.sizeY = g.sizeX;
            if (sizes.size() == 2 && !parseLength(Value(std::string(sizes[1])), &g.sizeY)) return false;
            if (sizes.size() == 1 && !inAt && !g.circle) g.circle = true;  // single length means circle
            g.explicitSize = true;
          }
          if (inAt && !parsePosition2(pos, &g.centerX, &g.centerY)) return false;
          first = 1;
        }
      }
    }
  }
  for (size_t i = first; i < parts.size(); i++) {
    if (!parseColorStop(parts[i], &g.stops)) {
      // A lone position is a color hint; ignore it rather than failing the gradient.
      Dim d;
      if (tokens(parts[i]).size() == 1 && parseDim(parts[i], &d)) continue;
      return false;
    }
  }
  if (g.stops.empty()) return false;
  if (g.stops.size() == 1) g.stops.push_back(g.stops[0]);
  *out = std::move(g);
  return true;
}

// RN object form: {type: 'linear-gradient', direction, colorStops: [{color, positions}]}.
bool parseGradientObject(const Value& o, Gradient* out) {
  std::string type = lower(o["type"].asString());
  Gradient g;
  if (type == "linear-gradient" || type == "lineargradient") g.kind = Gradient::Kind::Linear;
  else if (type == "radial-gradient" || type == "radialgradient") g.kind = Gradient::Kind::Radial;
  else return false;
  if (g.kind == Gradient::Kind::Linear && o.has("direction")) {
    float deg;
    const Value& d = o["direction"];
    if (angleFromValue(d, &deg)) g.angle = deg;
    else if (!d.isString() || !parseSideOrCorner(d.asString(), &g)) return false;
  }
  if (g.kind == Gradient::Kind::Radial) {
    std::string shape = lower(o["shape"].asString());
    g.circle = shape == "circle";
    const Value& size = o["size"];
    if (size.isString()) {
      std::string sz = lower(size.asString());
      if (sz == "closest-side") g.extent = Gradient::Extent::ClosestSide;
      else if (sz == "closest-corner") g.extent = Gradient::Extent::ClosestCorner;
      else if (sz == "farthest-side") g.extent = Gradient::Extent::FarthestSide;
    } else if (size.isObject()) {
      if (parseLength(size["x"], &g.sizeX) && parseLength(size["y"], &g.sizeY)) g.explicitSize = true;
    }
    const Value& pos = o["position"];
    if (pos.isString()) parsePosition2(tokens(pos.asString()), &g.centerX, &g.centerY);
    else if (pos.isObject()) {
      Length l;
      if (parseLength(pos["left"], &l)) g.centerX = l;
      else if (parseLength(pos["right"], &l)) g.centerX = l.percent ? Length{100 - l.value, true} : Length{-l.value, false};
      if (parseLength(pos["top"], &l)) g.centerY = l;
      else if (parseLength(pos["bottom"], &l)) g.centerY = l.percent ? Length{100 - l.value, true} : Length{-l.value, false};
    }
  }
  for (auto& st : o["colorStops"].asArray()) {
    GradientStop s;
    if (!parseColor(st["color"], &s.color)) continue;
    std::vector<Value> positions;
    if (st["positions"].isArray()) positions = st["positions"].asArray();
    else if (!st["position"].isNull()) positions.push_back(st["position"]);
    if (positions.empty()) g.stops.push_back(s);
    for (auto& p : positions) {
      Dim d;
      GradientStop ps = s;
      if (dimFromValue(p, &d)) {
        if (d.unit == Dim::Percent) ps.position = d.v / 100.f;
        else {
          ps.position = d.v;
          ps.px = true;
        }
      }
      g.stops.push_back(ps);
    }
  }
  if (g.stops.empty()) return false;
  if (g.stops.size() == 1) g.stops.push_back(g.stops[0]);
  *out = std::move(g);
  return true;
}

}  // namespace

bool parseBackgroundImage(const Value& v, std::vector<Gradient>* out) {
  out->clear();
  if (v.isString()) {
    std::string_view s = trim(v.asString());
    if (s.empty() || iequals(s, "none")) return true;
    for (auto part : splitTopLevel(s, ',')) {
      // splitTopLevel on ',' keeps each gradient's own commas (inside parens).
      Gradient g;
      if (!parseGradientString(part, &g)) {
        out->clear();
        return false;
      }
      out->push_back(std::move(g));
    }
    return true;
  }
  if (v.isArray()) {
    for (auto& e : v.asArray()) {
      Gradient g;
      bool ok = e.isString() ? parseGradientString(e.asString(), &g) : parseGradientObject(e, &g);
      if (!ok) {
        out->clear();
        return false;
      }
      out->push_back(std::move(g));
    }
    return true;
  }
  if (v.isObject()) {
    Gradient g;
    if (!parseGradientObject(v, &g)) return false;
    out->push_back(std::move(g));
    return true;
  }
  return false;
}

// --------------------------------------------------------------- transform

namespace {

bool translateArg(const Value& v, float* out, bool* percent) {
  Dim d;
  if (!dimFromValue(v, &d)) return false;
  if (d.unit == Dim::Deg) return false;
  *out = d.v;
  *percent = d.unit == Dim::Percent;
  return true;
}

TransformOp opMatrix(float a, float b, float c, float d, float e, float f) {
  TransformOp op{TransformOp::Kind::Matrix};
  float m[6] = {a, b, c, d, e, f};
  std::memcpy(op.m, m, sizeof m);
  return op;
}

// Adds one transform function given its name and argument values.
bool addTransform(std::string_view nameIn, const std::vector<Value>& args, std::vector<TransformOp>* out) {
  std::string name(nameIn);
  auto argc = args.size();
  auto ang = [&](size_t i, float* d) { return i < argc && angleFromValue(args[i], d); };
  auto num = [&](size_t i, float* d) { return i < argc && numFromValue(args[i], d); };
  if (name == "translate" || name == "translate3d" || name == "translateX" || name == "translateY") {
    TransformOp op{TransformOp::Kind::Translate};
    if (name == "translateY") {
      if (argc != 1 || !translateArg(args[0], &op.b, &op.percentB)) return false;
    } else {
      if (argc < 1 || !translateArg(args[0], &op.a, &op.percentA)) return false;
      if (name != "translateX" && argc >= 2 && !translateArg(args[1], &op.b, &op.percentB)) return false;
    }
    out->push_back(op);
    return true;
  }
  if (name == "scale" || name == "scale3d" || name == "scaleX" || name == "scaleY") {
    TransformOp op{TransformOp::Kind::Scale};
    float x = 1, y = 1;
    if (name == "scaleX") {
      if (!num(0, &x)) return false;
    } else if (name == "scaleY") {
      if (!num(0, &y)) return false;
    } else {
      if (!num(0, &x)) return false;
      y = x;
      if (argc >= 2 && !num(1, &y)) return false;
    }
    op.a = x;
    op.b = y;
    out->push_back(op);
    return true;
  }
  if (name == "rotate" || name == "rotateZ") {
    TransformOp op{TransformOp::Kind::Rotate};
    if (!ang(0, &op.a)) return false;
    out->push_back(op);
    return true;
  }
  // 3D rotations without perspective flatten to a scale along one axis (CSS
  // does the same when there is no perspective), which keeps card flips cheap.
  if (name == "rotateX" || name == "rotateY") {
    float d;
    if (!ang(0, &d)) return false;
    TransformOp op{TransformOp::Kind::Scale};
    float c = std::cos(d * kPi / 180.f);
    op.a = name == "rotateY" ? c : 1;
    op.b = name == "rotateX" ? c : 1;
    op.flipsBackface = c < 0;
    out->push_back(op);
    return true;
  }
  if (name == "skewX" || name == "skewY") {
    TransformOp op{name == "skewX" ? TransformOp::Kind::SkewX : TransformOp::Kind::SkewY};
    if (!ang(0, &op.a)) return false;
    out->push_back(op);
    return true;
  }
  if (name == "skew") {
    float ax = 0, ay = 0;
    if (!ang(0, &ax)) return false;
    if (argc >= 2 && !ang(1, &ay)) return false;
    out->push_back(opMatrix(1, std::tan(ay * kPi / 180.f), std::tan(ax * kPi / 180.f), 1, 0, 0));
    return true;
  }
  if (name == "matrix") {
    float m[16];
    if (argc != 6 && argc != 16 && argc != 9) return false;
    for (size_t i = 0; i < argc; i++)
      if (!num(i, &m[i])) return false;
    if (argc == 6) out->push_back(opMatrix(m[0], m[1], m[2], m[3], m[4], m[5]));
    else if (argc == 9) out->push_back(opMatrix(m[0], m[1], m[3], m[4], m[6], m[7]));  // column-major 3x3
    else out->push_back(opMatrix(m[0], m[1], m[4], m[5], m[12], m[13]));               // column-major 4x4
    return true;
  }
  if (name == "matrix3d") {
    float m[16];
    if (argc != 16) return false;
    for (size_t i = 0; i < 16; i++)
      if (!num(i, &m[i])) return false;
    out->push_back(opMatrix(m[0], m[1], m[4], m[5], m[12], m[13]));
    return true;
  }
  if (name == "perspective" || name == "translateZ" || name == "scaleZ" || name == "rotate3d") return true;  // no 3D
  return false;
}

std::vector<Value> splitArgsAsValues(std::string_view args) {
  std::vector<Value> out;
  for (auto a : splitTopLevel(args, ',')) {
    if (a.empty()) continue;
    // CSS also allows space separated args in some functions.
    for (auto t : tokens(a)) out.emplace_back(std::string(t));
  }
  return out;
}

}  // namespace

bool parseTransform(const Value& v, std::vector<TransformOp>* out) {
  out->clear();
  if (v.isArray()) {
    for (auto& e : v.asArray()) {
      if (!e.isObject() || e.asObject().empty()) continue;
      auto& [k, arg] = *e.asObject().begin();
      std::vector<Value> args;
      if (arg.isArray()) args = arg.asArray();
      else args.push_back(arg);
      if (!addTransform(k, args, out)) {
        out->clear();
        return false;
      }
    }
    return true;
  }
  if (v.isString()) {
    std::string_view s = trim(v.asString());
    if (s.empty() || iequals(s, "none")) return true;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
      if (i >= s.size()) break;
      size_t open = s.find('(', i), close = s.find(')', i);
      if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        out->clear();
        return false;
      }
      std::string_view name = trim(s.substr(i, open - i));
      std::string_view args = s.substr(open + 1, close - open - 1);
      if (!addTransform(name, splitArgsAsValues(args), out)) {
        out->clear();
        return false;
      }
      i = close + 1;
    }
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ filter

namespace {

bool addFilter(std::string_view nameIn, std::string_view arg, const Value& rawArg, std::vector<FilterOp>* out) {
  std::string name = lower(nameIn);
  FilterOp op;
  Value av = rawArg.isNull() ? Value(std::string(trim(arg))) : rawArg;
  bool emptyArg = av.isString() && trim(av.asString()).empty();
  if (name == "blur") {
    op.kind = FilterOp::Kind::Blur;
    if (!emptyArg && !pxFromValue(av, &op.amount)) return false;
  } else if (name == "hue-rotate" || name == "hueRotate") {
    op.kind = FilterOp::Kind::HueRotate;
    if (!emptyArg && !angleFromValue(av, &op.amount)) return false;
  } else if (name == "drop-shadow" || name == "dropShadow") {
    op.kind = FilterOp::Kind::DropShadow;
    if (av.isObject()) {
      if (!parseShadowObject(av, &op.shadow)) {
        // RN form: {offsetX, offsetY, standardDeviation, color}
        return false;
      }
      float sd;
      if (pxFromValue(av["standardDeviation"], &sd)) op.shadow.blur = sd * 2;
    } else if (!parseShadowString(av.asString(), &op.shadow, false)) {
      return false;
    }
  } else {
    static const struct {
      const char* n;
      FilterOp::Kind k;
      float def;
    } kinds[] = {{"brightness", FilterOp::Kind::Brightness, 1}, {"contrast", FilterOp::Kind::Contrast, 1},
                 {"saturate", FilterOp::Kind::Saturate, 1},     {"grayscale", FilterOp::Kind::Grayscale, 1},
                 {"sepia", FilterOp::Kind::Sepia, 1},           {"invert", FilterOp::Kind::Invert, 1},
                 {"opacity", FilterOp::Kind::Opacity, 1}};
    bool found = false;
    for (auto& k : kinds) {
      if (name == k.n) {
        op.kind = k.k;
        op.amount = k.def;
        found = true;
        break;
      }
    }
    if (!found) return false;
    if (!emptyArg && !numFromValue(av, &op.amount)) return false;
    op.amount = std::max(0.f, op.amount);
    if (op.kind == FilterOp::Kind::Grayscale || op.kind == FilterOp::Kind::Sepia ||
        op.kind == FilterOp::Kind::Invert || op.kind == FilterOp::Kind::Opacity)
      op.amount = std::min(1.f, op.amount);
  }
  out->push_back(op);
  return true;
}

}  // namespace

bool parseFilter(const Value& v, std::vector<FilterOp>* out) {
  out->clear();
  if (v.isString()) {
    std::string_view s = trim(v.asString());
    if (s.empty() || iequals(s, "none")) return true;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
      if (i >= s.size()) break;
      size_t open = s.find('(', i);
      if (open == std::string_view::npos) {
        out->clear();
        return false;
      }
      // Find the matching ')' (drop-shadow args may contain rgba(...)).
      int depth = 0;
      size_t close = std::string_view::npos;
      for (size_t j = open; j < s.size(); j++) {
        if (s[j] == '(') depth++;
        else if (s[j] == ')' && --depth == 0) {
          close = j;
          break;
        }
      }
      if (close == std::string_view::npos) {
        out->clear();
        return false;
      }
      if (!addFilter(trim(s.substr(i, open - i)), s.substr(open + 1, close - open - 1), Value(), out)) {
        out->clear();
        return false;
      }
      i = close + 1;
    }
    return true;
  }
  if (v.isArray()) {
    for (auto& e : v.asArray()) {
      if (!e.isObject() || e.asObject().empty()) continue;
      auto& [k, arg] = *e.asObject().begin();
      if (!addFilter(k, "", arg, out)) {
        out->clear();
        return false;
      }
    }
    return true;
  }
  return false;
}

// -------------------------------------------------------------- comparison

bool operator==(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool operator==(const Shadow& a, const Shadow& b) {
  return a.offsetX == b.offsetX && a.offsetY == b.offsetY && a.blur == b.blur && a.spread == b.spread &&
         a.color == b.color && a.inset == b.inset;
}

bool textMetricsEqual(const TextStyle& a, const TextStyle& b) {
  return a.fontFamily == b.fontFamily && a.fontSize == b.fontSize && a.fontWeight == b.fontWeight &&
         a.italic == b.italic && a.lineHeight == b.lineHeight && a.lineHeightRelative == b.lineHeightRelative &&
         a.letterSpacing == b.letterSpacing && a.textTransform == b.textTransform && a.fontVariant == b.fontVariant &&
         a.writingDirection == b.writingDirection;
}

bool operator==(const TextStyle& a, const TextStyle& b) {
  return textMetricsEqual(a, b) && a.color == b.color && a.textAlign == b.textAlign && a.decoration == b.decoration &&
         a.decorationColor == b.decorationColor && a.decorationStyle == b.decorationStyle &&
         a.textShadows == b.textShadows && a.hasTextShadow == b.hasTextShadow;
}

// --------------------------------------------------------------- applyStyle

namespace {

// Read-only view of a style object for lookups by literal key. applyStyle
// asks for ~150 keys per call and Value::operator[] builds a std::string for
// each; this indexes the object's (already sorted) entries once on the stack
// and binary-searches them without allocating. patchStyle-driven animation
// calls applyStyle for every animated node every frame.
class StyleMap {
 public:
  explicit StyleMap(const Value& v) : value_(v) {
    if (!v.isObject()) return;
    const auto& o = v.asObject();
    if (o.size() > kMax) {
      big_ = true;
      return;
    }
    for (auto& [k, val] : o) {
      e_[n_++] = {k.c_str(), &val};
      firstChars_ |= bit(k.c_str());
    }
  }
  bool isNull() const { return !value_.isObject(); }
  const Value& operator[](const char* key) const {
    if (big_) return value_[key];
    const Entry* e = find(key);
    return e ? *e->v : null_;
  }
  const Value& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
  bool has(const char* key) const { return big_ ? value_.has(key) : find(key) != nullptr; }

 private:
  struct Entry {
    const char* k;
    const Value* v;
  };
  static constexpr size_t kMax = 96;
  // Most lookups are for absent keys: a set of the keys' first letters
  // answers those with one test.
  static uint32_t bit(const char* k) {
    unsigned c = unsigned(k[0]) - 'a';
    return c < 26 ? 1u << c : 1u << 26;
  }
  const Entry* find(const char* key) const {
    if (!(firstChars_ & bit(key))) return nullptr;
    size_t lo = 0, hi = n_;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      int c = std::strcmp(e_[mid].k, key);
      if (c == 0) return &e_[mid];
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
    return nullptr;
  }
  const Value& value_;
  Entry e_[kMax];
  size_t n_ = 0;
  uint32_t firstChars_ = 0;
  bool big_ = false;
  inline static const Value null_;
};

// Writes a length-valued Yoga property: number/"px" -> points, "%" -> percent,
// "auto" -> auto (when supported), anything else -> `unset`.
template <class Points, class Percent, class Unset>
void yogaLength(const Value& v, Points points, Percent percent, Unset unset) {
  if (v.isString()) {
    std::string_view s = trim(v.asString());
    if (iequals(s, "auto")) {
      unset(true);
      return;
    }
  }
  Dim d;
  if (dimFromValue(v, &d)) {
    if (d.unit == Dim::Percent) return percent(d.v);
    if (d.unit == Dim::Px || d.unit == Dim::None) return points(d.v);
  }
  unset(false);
}

// Edge-indexed style keys in increasing precedence (later wins on the same edge).
struct EdgeKey {
  const char* key;
  YGEdge edge;
};

const EdgeKey kMarginKeys[] = {
    {"margin", YGEdgeAll},           {"marginHorizontal", YGEdgeHorizontal}, {"marginInline", YGEdgeHorizontal},
    {"marginVertical", YGEdgeVertical}, {"marginBlock", YGEdgeVertical},     {"marginLeft", YGEdgeLeft},
    {"marginRight", YGEdgeRight},    {"marginTop", YGEdgeTop},               {"marginBlockStart", YGEdgeTop},
    {"marginBottom", YGEdgeBottom},  {"marginBlockEnd", YGEdgeBottom},       {"marginStart", YGEdgeStart},
    {"marginInlineStart", YGEdgeStart}, {"marginEnd", YGEdgeEnd},            {"marginInlineEnd", YGEdgeEnd},
};
const EdgeKey kPaddingKeys[] = {
    {"padding", YGEdgeAll},            {"paddingHorizontal", YGEdgeHorizontal}, {"paddingInline", YGEdgeHorizontal},
    {"paddingVertical", YGEdgeVertical}, {"paddingBlock", YGEdgeVertical},     {"paddingLeft", YGEdgeLeft},
    {"paddingRight", YGEdgeRight},     {"paddingTop", YGEdgeTop},              {"paddingBlockStart", YGEdgeTop},
    {"paddingBottom", YGEdgeBottom},   {"paddingBlockEnd", YGEdgeBottom},      {"paddingStart", YGEdgeStart},
    {"paddingInlineStart", YGEdgeStart}, {"paddingEnd", YGEdgeEnd},            {"paddingInlineEnd", YGEdgeEnd},
};
const EdgeKey kPositionKeys[] = {
    {"inset", YGEdgeAll},           {"insetInline", YGEdgeHorizontal},  {"insetBlock", YGEdgeVertical},
    {"left", YGEdgeLeft},           {"right", YGEdgeRight},             {"top", YGEdgeTop},
    {"insetBlockStart", YGEdgeTop}, {"bottom", YGEdgeBottom},           {"insetBlockEnd", YGEdgeBottom},
    {"start", YGEdgeStart},         {"insetInlineStart", YGEdgeStart},  {"end", YGEdgeEnd},
    {"insetInlineEnd", YGEdgeEnd},
};
const EdgeKey kBorderKeys[] = {
    {"borderWidth", YGEdgeAll},       {"borderLeftWidth", YGEdgeLeft},   {"borderRightWidth", YGEdgeRight},
    {"borderTopWidth", YGEdgeTop},    {"borderBottomWidth", YGEdgeBottom}, {"borderStartWidth", YGEdgeStart},
    {"borderEndWidth", YGEdgeEnd},
};
constexpr YGEdge kAllEdges[] = {YGEdgeLeft,  YGEdgeTop,        YGEdgeRight,    YGEdgeBottom, YGEdgeStart,
                                YGEdgeEnd,   YGEdgeHorizontal, YGEdgeVertical, YGEdgeAll};
constexpr int kEdgeCount = 9;

template <size_t N>
void collectEdges(const StyleMap& style, const EdgeKey (&keys)[N], const Value* slots[kEdgeCount]) {
  for (int i = 0; i < kEdgeCount; i++) slots[i] = nullptr;
  for (auto& k : keys) {
    const Value& v = style[k.key];
    if (v.isNull()) continue;
    slots[int(k.edge)] = &v;  // YGEdge values are 0..8 in Yoga's own order
  }
}

int clampWeight(float w) { return std::clamp(int(std::lround(w / 100.f)) * 100, 100, 900); }

bool parseFontWeight(const Value& v, int* out) {
  if (v.isNumber()) {
    *out = clampWeight(float(v.asNumber()));
    return true;
  }
  if (!v.isString()) return false;
  std::string s = lower(trim(v.asString()));
  if (s == "normal" || s == "regular") *out = 400;
  else if (s == "bold") *out = 700;
  else if (s == "thin") *out = 100;
  else if (s == "ultralight" || s == "extralight") *out = 200;
  else if (s == "light") *out = 300;
  else if (s == "medium") *out = 500;
  else if (s == "semibold" || s == "demibold") *out = 600;
  else if (s == "heavy" || s == "extrabold") *out = 800;
  else if (s == "black") *out = 900;
  else if (s == "lighter") *out = 300;
  else if (s == "bolder") *out = 700;
  else {
    float n;
    std::string u;
    if (!parseNumberUnit(s, &n, &u) || !u.empty()) return false;
    *out = clampWeight(n);
  }
  return true;
}

void parseTextStyle(const StyleMap& style, TextStyle* t) {
  Color c;
  if (parseColor(style["color"], &c)) t->color = c;
  if (style["fontFamily"].isString() && !style["fontFamily"].asString().empty()) t->fontFamily = style["fontFamily"].asString();
  float f;
  if (pxFromValue(style["fontSize"], &f) && f >= 0) t->fontSize = f;
  int w;
  if (parseFontWeight(style["fontWeight"], &w)) t->fontWeight = w;
  if (style["fontStyle"].isString()) {
    std::string fs = lower(style["fontStyle"].asString());
    t->italic = fs == "italic" || fs == "oblique";
  }
  const Value& lh = style["lineHeight"];
  if (lh.isNumber()) {
    t->lineHeight = float(lh.asNumber());
  } else if (lh.isString()) {
    Dim d;
    if (iequals(trim(lh.asString()), "normal")) {
      // leave unset
    } else if (parseDim(lh.asString(), &d)) {
      if (d.unit == Dim::Px) t->lineHeight = d.v;
      else if (d.unit == Dim::None) t->lineHeight = d.v, t->lineHeightRelative = true;
      else if (d.unit == Dim::Percent) t->lineHeight = d.v / 100.f, t->lineHeightRelative = true;
    }
  }
  if (pxFromValue(style["letterSpacing"], &f)) t->letterSpacing = f;
  if (style["textAlign"].isString()) {
    std::string a = lower(style["textAlign"].asString());
    if (a == "left") t->textAlign = TextAlign::Left;
    else if (a == "right") t->textAlign = TextAlign::Right;
    else if (a == "center") t->textAlign = TextAlign::Center;
    else if (a == "justify") t->textAlign = TextAlign::Justify;
    else if (a == "end") t->textAlign = TextAlign::End;
    else if (a == "auto" || a == "start") t->textAlign = TextAlign::Auto;
  }
  if (style["textTransform"].isString()) {
    std::string a = lower(style["textTransform"].asString());
    if (a == "uppercase") t->textTransform = TextTransform::Uppercase;
    else if (a == "lowercase") t->textTransform = TextTransform::Lowercase;
    else if (a == "capitalize") t->textTransform = TextTransform::Capitalize;
    else if (a == "none") t->textTransform = TextTransform::None;
  }
  const Value& dl = style.has("textDecorationLine") ? style["textDecorationLine"] : style["textDecoration"];
  if (dl.isString()) {
    uint8_t bits = DecorNone;
    for (auto tok : tokens(dl.asString())) {
      if (iequals(tok, "underline")) bits |= DecorUnderline;
      else if (iequals(tok, "line-through")) bits |= DecorLineThrough;
    }
    t->decoration = bits;
  }
  if (parseColor(style["textDecorationColor"], &c)) t->decorationColor = c;
  if (style["textDecorationStyle"].isString()) {
    std::string a = lower(style["textDecorationStyle"].asString());
    t->decorationStyle = a == "double" ? 1 : a == "dotted" ? 2 : a == "dashed" ? 3 : a == "wavy" ? 4 : 0;
  }
  if (style["fontVariant"].isArray()) {
    std::vector<std::string> fv;
    for (auto& e : style["fontVariant"].asArray())
      if (e.isString()) fv.push_back(e.asString());
    t->fontVariant = std::move(fv);
  } else if (style["fontVariant"].isString()) {
    std::vector<std::string> fv;
    for (auto tok : tokens(style["fontVariant"].asString())) fv.emplace_back(tok);
    t->fontVariant = std::move(fv);
  }
  if (style["writingDirection"].isString()) {
    std::string a = lower(style["writingDirection"].asString());
    t->writingDirection = a == "ltr" ? 1 : a == "rtl" ? 2 : 0;
  }

  // Text shadows: CSS `textShadow` string, then RN textShadow* (react-native-web
  // only emits one when a color is given and some offset/radius is non-zero).
  t->textShadows.clear();
  if (style["textShadow"].isString()) {
    for (auto part : splitTopLevel(style["textShadow"].asString(), ',')) {
      Shadow sh;
      if (parseShadowString(part, &sh, false)) t->textShadows.push_back(sh);
    }
  }
  Color sc;
  if (parseColor(style["textShadowColor"], &sc)) {
    Shadow sh;
    sh.color = sc;
    const Value& off = style["textShadowOffset"];
    pxFromValue(off["width"], &sh.offsetX);
    pxFromValue(off["height"], &sh.offsetY);
    pxFromValue(style["textShadowRadius"], &sh.blur);
    if (sh.offsetX != 0 || sh.offsetY != 0 || sh.blur != 0) t->textShadows.push_back(sh);
  }
  t->hasTextShadow = !t->textShadows.empty();
}

bool parseBorderStyle(const Value& v, BorderStyle* out) {
  if (!v.isString()) return false;
  std::string s = lower(v.asString());
  if (s == "solid") *out = BorderStyle::Solid;
  else if (s == "dashed") *out = BorderStyle::Dashed;
  else if (s == "dotted") *out = BorderStyle::Dotted;
  else return false;
  return true;
}

bool parseResizeMode(const Value& v, ResizeMode* out) {
  if (!v.isString()) return false;
  std::string s = lower(v.asString());
  if (s == "cover") *out = ResizeMode::Cover;
  else if (s == "contain" || s == "scale-down") *out = ResizeMode::Contain;
  else if (s == "stretch" || s == "fill") *out = ResizeMode::Stretch;
  else if (s == "center" || s == "none") *out = ResizeMode::Center;
  else if (s == "repeat") *out = ResizeMode::Repeat;
  else return false;
  return true;
}

void parseTransformOrigin(const Value& v, Length out[2]) {
  out[0] = {50, true};
  out[1] = {50, true};
  if (v.isArray()) {
    auto& a = v.asArray();
    if (a.size() >= 1) {
      if (a[0].isString()) parsePositionKeyword(trim(a[0].asString()), true, &out[0]);
      else parseLength(a[0], &out[0]);
    }
    if (a.size() >= 2) {
      if (a[1].isString()) parsePositionKeyword(trim(a[1].asString()), false, &out[1]);
      else parseLength(a[1], &out[1]);
    }
  } else if (v.isString()) {
    auto t = tokens(v.asString());
    if (t.size() == 3) t.pop_back();  // z offset
    parsePosition2(t, &out[0], &out[1]);
  }
}

// Corner radius slots: 0 topLeft, 1 topRight, 2 bottomRight, 3 bottomLeft (LTR).
void parseRadii(const StyleMap& style, Length out[4]) {
  static const struct {
    const char* key;
    int corner;  // -1 = all
  } kKeys[] = {
      {"borderRadius", -1},
      {"borderTopLeftRadius", 0},
      {"borderTopRightRadius", 1},
      {"borderBottomRightRadius", 2},
      {"borderBottomLeftRadius", 3},
      {"borderTopStartRadius", 0},
      {"borderTopEndRadius", 1},
      {"borderBottomEndRadius", 2},
      {"borderBottomStartRadius", 3},
      {"borderStartStartRadius", 0},
      {"borderStartEndRadius", 1},
      {"borderEndEndRadius", 2},
      {"borderEndStartRadius", 3},
  };
  for (int i = 0; i < 4; i++) out[i] = {};
  for (auto& k : kKeys) {
    Length l;
    if (!parseLength(style[k.key], &l)) continue;
    l.value = std::max(0.f, l.value);
    if (k.corner < 0)
      for (int i = 0; i < 4; i++) out[i] = l;
    else
      out[k.corner] = l;
  }
}

// Side slots: 0 top, 1 right, 2 bottom, 3 left (LTR: start = left).
void parseBorderColors(const StyleMap& style, Color out[4]) {
  static const struct {
    const char* key;
    int sides;  // bitmask of sides
  } kKeys[] = {
      {"borderColor", 15},          {"borderBlockColor", 1 | 4}, {"borderTopColor", 1},
      {"borderBlockStartColor", 1}, {"borderRightColor", 2},     {"borderEndColor", 2},
      {"borderBottomColor", 4},     {"borderBlockEndColor", 4},  {"borderLeftColor", 8},
      {"borderStartColor", 8},
  };
  for (int i = 0; i < 4; i++) out[i] = {0, 0, 0, 1};  // RN default border color is black
  for (auto& k : kKeys) {
    Color c;
    if (!parseColor(style[k.key], &c)) continue;
    for (int i = 0; i < 4; i++)
      if (k.sides & (1 << i)) out[i] = c;
  }
}

}  // namespace

float snapBorderWidth(float w, float /*pixelRatio*/) {
  if (!(w > 0)) return w;
  // Chrome (checked on 141 at device scale 1, 1.5, 2, 3 and 4) snaps in CSS
  // px rather than device px: 0.25 -> 1, 1.5 -> 1, 2.5 -> 2, 3.3 -> 3.
  return w < 1 ? 1.f : std::floor(w);
}

void applyStyle(const Value& style, ComputedStyle* out, YGNodeRef yoga, float pixelRatio) {
  *out = ComputedStyle();
  const StyleMap s(style);  // may be null: everything resets to defaults

  // ---- Layout (Yoga). Every property is written; Yoga ignores no-op sets.
  auto enumKey = [&](const char* key) -> std::string { return s[key].isString() ? s[key].asString() : std::string(); };

  {
    std::string v = enumKey("flexDirection");
    YGFlexDirection d = YGFlexDirectionColumn;
    if (v == "row") d = YGFlexDirectionRow;
    else if (v == "row-reverse") d = YGFlexDirectionRowReverse;
    else if (v == "column-reverse") d = YGFlexDirectionColumnReverse;
    YGNodeStyleSetFlexDirection(yoga, d);
  }
  auto justify = [](const std::string& v, YGJustify def) {
    if (v == "flex-start" || v == "start") return YGJustifyFlexStart;
    if (v == "center") return YGJustifyCenter;
    if (v == "flex-end" || v == "end") return YGJustifyFlexEnd;
    if (v == "space-between") return YGJustifySpaceBetween;
    if (v == "space-around") return YGJustifySpaceAround;
    if (v == "space-evenly") return YGJustifySpaceEvenly;
    return def;
  };
  YGNodeStyleSetJustifyContent(yoga, justify(enumKey("justifyContent"), YGJustifyFlexStart));
  auto align = [](const std::string& v, YGAlign def) {
    if (v == "auto") return YGAlignAuto;
    if (v == "flex-start" || v == "start") return YGAlignFlexStart;
    if (v == "center") return YGAlignCenter;
    if (v == "flex-end" || v == "end") return YGAlignFlexEnd;
    if (v == "stretch") return YGAlignStretch;
    if (v == "baseline") return YGAlignBaseline;
    if (v == "space-between") return YGAlignSpaceBetween;
    if (v == "space-around") return YGAlignSpaceAround;
    if (v == "space-evenly") return YGAlignSpaceEvenly;
    return def;
  };
  YGNodeStyleSetAlignItems(yoga, align(enumKey("alignItems"), YGAlignStretch));
  YGNodeStyleSetAlignSelf(yoga, align(enumKey("alignSelf"), YGAlignAuto));
  YGNodeStyleSetAlignContent(yoga, align(enumKey("alignContent"), YGAlignFlexStart));
  {
    std::string v = enumKey("position");
    YGNodeStyleSetPositionType(yoga, v == "absolute" ? YGPositionTypeAbsolute
                                     : v == "static" ? YGPositionTypeStatic
                                                     : YGPositionTypeRelative);
  }
  {
    std::string v = enumKey("flexWrap");
    YGNodeStyleSetFlexWrap(yoga, v == "wrap" ? YGWrapWrap : v == "wrap-reverse" ? YGWrapWrapReverse : YGWrapNoWrap);
  }
  {
    std::string v = enumKey("overflow");
    YGOverflow o = YGOverflowVisible;
    if (v == "hidden" || v == "clip") o = YGOverflowHidden, out->overflow = Overflow::Hidden;
    else if (v == "scroll" || v == "auto") o = YGOverflowScroll, out->overflow = Overflow::Scroll;
    YGNodeStyleSetOverflow(yoga, o);
  }
  {
    std::string v = enumKey("display");
    out->display = v != "none";
    YGNodeStyleSetDisplay(yoga, v == "none" ? YGDisplayNone : v == "contents" ? YGDisplayContents : YGDisplayFlex);
  }
  {
    std::string v = enumKey("direction");
    YGNodeStyleSetDirection(yoga, v == "ltr" ? YGDirectionLTR : v == "rtl" ? YGDirectionRTL : YGDirectionInherit);
  }
  YGNodeStyleSetBoxSizing(yoga, enumKey("boxSizing") == "content-box" ? YGBoxSizingContentBox : YGBoxSizingBorderBox);

  float f;
  YGNodeStyleSetFlex(yoga, numFromValue(s["flex"], &f) ? f : YGUndefined);
  YGNodeStyleSetFlexGrow(yoga, numFromValue(s["flexGrow"], &f) ? std::max(0.f, f) : YGUndefined);
  YGNodeStyleSetFlexShrink(yoga, numFromValue(s["flexShrink"], &f) ? std::max(0.f, f) : YGUndefined);
  {
    // aspectRatio: number or "16/9" / "16 / 9" string.
    float ar = YGUndefined;
    const Value& v = s["aspectRatio"];
    if (v.isNumber()) ar = float(v.asNumber());
    else if (v.isString()) {
      auto parts = splitTopLevel(v.asString(), '/');
      float a, b;
      std::string u;
      if (parts.size() == 2 && parseNumberUnit(parts[0], &a, &u) && parseNumberUnit(parts[1], &b, &u) && b != 0) ar = a / b;
      else if (parts.size() == 1 && parseNumberUnit(parts[0], &a, &u)) ar = a;
    }
    YGNodeStyleSetAspectRatio(yoga, ar > 0 && std::isfinite(ar) ? ar : YGUndefined);
  }

  yogaLength(
      s["flexBasis"], [&](float v) { YGNodeStyleSetFlexBasis(yoga, v); },
      [&](float v) { YGNodeStyleSetFlexBasisPercent(yoga, v); }, [&](bool) { YGNodeStyleSetFlexBasisAuto(yoga); });
  yogaLength(
      s["width"], [&](float v) { YGNodeStyleSetWidth(yoga, v); }, [&](float v) { YGNodeStyleSetWidthPercent(yoga, v); },
      [&](bool) { YGNodeStyleSetWidthAuto(yoga); });
  yogaLength(
      s["height"], [&](float v) { YGNodeStyleSetHeight(yoga, v); },
      [&](float v) { YGNodeStyleSetHeightPercent(yoga, v); }, [&](bool) { YGNodeStyleSetHeightAuto(yoga); });
  yogaLength(
      s["minWidth"], [&](float v) { YGNodeStyleSetMinWidth(yoga, v); },
      [&](float v) { YGNodeStyleSetMinWidthPercent(yoga, v); }, [&](bool) { YGNodeStyleSetMinWidth(yoga, YGUndefined); });
  yogaLength(
      s["minHeight"], [&](float v) { YGNodeStyleSetMinHeight(yoga, v); },
      [&](float v) { YGNodeStyleSetMinHeightPercent(yoga, v); },
      [&](bool) { YGNodeStyleSetMinHeight(yoga, YGUndefined); });
  yogaLength(
      s["maxWidth"], [&](float v) { YGNodeStyleSetMaxWidth(yoga, v); },
      [&](float v) { YGNodeStyleSetMaxWidthPercent(yoga, v); }, [&](bool) { YGNodeStyleSetMaxWidth(yoga, YGUndefined); });
  yogaLength(
      s["maxHeight"], [&](float v) { YGNodeStyleSetMaxHeight(yoga, v); },
      [&](float v) { YGNodeStyleSetMaxHeightPercent(yoga, v); },
      [&](bool) { YGNodeStyleSetMaxHeight(yoga, YGUndefined); });

  // Gaps: gap sets both; rowGap / columnGap override.
  {
    auto gap = [&](const char* key, YGGutter g) {
      yogaLength(
          s[key], [&](float v) { YGNodeStyleSetGap(yoga, g, v); }, [&](float v) { YGNodeStyleSetGapPercent(yoga, g, v); },
          [&](bool) { YGNodeStyleSetGap(yoga, g, YGUndefined); });
    };
    gap("gap", YGGutterAll);
    gap("rowGap", YGGutterRow);
    gap("columnGap", YGGutterColumn);
  }

  const Value* slots[kEdgeCount];
  collectEdges(s, kMarginKeys, slots);
  for (YGEdge e : kAllEdges) {
    const Value null;
    const Value& v = slots[int(e)] ? *slots[int(e)] : null;
    yogaLength(
        v, [&](float x) { YGNodeStyleSetMargin(yoga, e, x); }, [&](float x) { YGNodeStyleSetMarginPercent(yoga, e, x); },
        [&](bool isAuto) {
          if (isAuto) YGNodeStyleSetMarginAuto(yoga, e);
          else YGNodeStyleSetMargin(yoga, e, YGUndefined);
        });
  }
  collectEdges(s, kPaddingKeys, slots);
  for (YGEdge e : kAllEdges) {
    const Value null;
    const Value& v = slots[int(e)] ? *slots[int(e)] : null;
    yogaLength(
        v, [&](float x) { YGNodeStyleSetPadding(yoga, e, std::max(0.f, x)); },
        [&](float x) { YGNodeStyleSetPaddingPercent(yoga, e, std::max(0.f, x)); },
        [&](bool) { YGNodeStyleSetPadding(yoga, e, YGUndefined); });
  }
  collectEdges(s, kPositionKeys, slots);
  for (YGEdge e : kAllEdges) {
    const Value null;
    const Value& v = slots[int(e)] ? *slots[int(e)] : null;
    yogaLength(
        v, [&](float x) { YGNodeStyleSetPosition(yoga, e, x); },
        [&](float x) { YGNodeStyleSetPositionPercent(yoga, e, x); },
        [&](bool isAuto) {
          if (isAuto) YGNodeStyleSetPositionAuto(yoga, e);
          else YGNodeStyleSetPosition(yoga, e, YGUndefined);
        });
  }
  collectEdges(s, kBorderKeys, slots);
  float bw[kEdgeCount];
  for (YGEdge e : kAllEdges) {
    float w = YGUndefined;
    if (slots[int(e)] && pxFromValue(*slots[int(e)], &w)) w = snapBorderWidth(std::max(0.f, w), pixelRatio);
    else w = YGUndefined;
    bw[int(e)] = w;
    YGNodeStyleSetBorder(yoga, e, w);
  }
  // Physical border widths for painting (LTR resolution of start/end). The
  // painter prefers Yoga's resolved layout borders, which handle RTL.
  auto pick = [&](std::initializer_list<YGEdge> order) {
    for (YGEdge e : order)
      if (!std::isnan(bw[int(e)])) return bw[int(e)];
    return 0.f;
  };
  out->borderWidth[0] = pick({YGEdgeTop, YGEdgeVertical, YGEdgeAll});
  out->borderWidth[1] = pick({YGEdgeEnd, YGEdgeRight, YGEdgeHorizontal, YGEdgeAll});
  out->borderWidth[2] = pick({YGEdgeBottom, YGEdgeVertical, YGEdgeAll});
  out->borderWidth[3] = pick({YGEdgeStart, YGEdgeLeft, YGEdgeHorizontal, YGEdgeAll});

  if (s.isNull()) return;

  // ---- Paint
  parseColor(s["backgroundColor"], &out->backgroundColor);
  if (numFromValue(s["opacity"], &f)) out->opacity = clamp01(f);
  parseBorderColors(s, out->borderColor);
  parseRadii(s, out->borderRadius);
  parseBorderStyle(s["borderStyle"], &out->borderStyle);

  if (s.has("boxShadow")) parseBoxShadow(s["boxShadow"], &out->boxShadows);
  // Legacy RN shadow props, mapped like react-native-web: one outer shadow
  // with alpha multiplied by shadowOpacity, appended after boxShadow.
  if (s.has("shadowColor") || s.has("shadowOffset") || s.has("shadowOpacity") || s.has("shadowRadius")) {
    Shadow sh;
    sh.color = {0, 0, 0, 1};
    if (s.has("shadowColor")) parseColor(s["shadowColor"], &sh.color);
    float op = 1;
    if (numFromValue(s["shadowOpacity"], &op)) sh.color.a *= clamp01(op);
    pxFromValue(s["shadowOffset"]["width"], &sh.offsetX);
    pxFromValue(s["shadowOffset"]["height"], &sh.offsetY);
    pxFromValue(s["shadowRadius"], &sh.blur);
    sh.blur = std::max(0.f, sh.blur);
    if (!sh.color.transparent()) out->boxShadows.push_back(sh);
  }
  if (s.has("experimental_backgroundImage")) parseBackgroundImage(s["experimental_backgroundImage"], &out->backgroundImages);
  else if (s.has("backgroundImage")) parseBackgroundImage(s["backgroundImage"], &out->backgroundImages);

  if (s.has("transform")) parseTransform(s["transform"], &out->transform);
  if (s.has("transformOrigin")) parseTransformOrigin(s["transformOrigin"], out->transformOrigin);

  if (s.has("filter")) {
    parseFilter(s["filter"], &out->filters);
    for (auto& op : out->filters) {
      switch (op.kind) {
        case FilterOp::Kind::Blur: out->blur = op.amount; break;
        case FilterOp::Kind::Brightness: out->brightness = op.amount; break;
        case FilterOp::Kind::Contrast: out->contrast = op.amount; break;
        case FilterOp::Kind::Saturate: out->saturate = op.amount; break;
        case FilterOp::Kind::Grayscale: out->grayscale = op.amount; break;
        default: break;
      }
    }
  }

  const Value& z = s["zIndex"];
  if (z.isNumber() && std::isfinite(z.asNumber())) {
    out->zIndex = int(z.asNumber());
    out->hasZIndex = true;
  }
  if (s["pointerEvents"].isString()) {
    std::string v = s["pointerEvents"].asString();
    out->pointerEvents = v == "none"       ? PointerEvents::None
                         : v == "box-none" ? PointerEvents::BoxNone
                         : v == "box-only" ? PointerEvents::BoxOnly
                                           : PointerEvents::Auto;
  }
  if (s["cursor"].isString()) out->cursor = s["cursor"].asString();
  if (parseResizeMode(s["resizeMode"], &out->resizeMode) || parseResizeMode(s["objectFit"], &out->resizeMode))
    out->hasResizeMode = true;
  Color c;
  if (parseColor(s["tintColor"], &c)) out->tintColor = c;
  out->backfaceHidden = enumKey("backfaceVisibility") == "hidden";
  if (s["mixBlendMode"].isString()) {
    std::string m = lower(s["mixBlendMode"].asString());
    for (int i = 0; i < kBlendModeCount; i++)
      if (m == kBlendModeNames[i]) out->blendMode = uint8_t(i);
  }
  pxFromValue(s["outlineWidth"], &out->outlineWidth);
  out->outlineWidth = snapBorderWidth(std::max(0.f, out->outlineWidth), pixelRatio);
  pxFromValue(s["outlineOffset"], &out->outlineOffset);
  parseColor(s["outlineColor"], &out->outlineColor);
  parseBorderStyle(s["outlineStyle"], &out->outlineStyle);

  parseTextStyle(s, &out->text);
}

}  // namespace jsui
