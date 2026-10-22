// Style parsing. SKELETON: only a handful of properties are handled; the
// full React Native / CSS surface is implemented on top of this.
#include "core/style.h"

#include <cstdlib>

namespace jsui {

bool parseColor(const std::string& s, Color* out) {
  if (s == "transparent") {
    *out = {};
    return true;
  }
  if (!s.empty() && s[0] == '#') {
    std::string h = s.substr(1);
    if (h.size() == 3 || h.size() == 4) {
      std::string e;
      for (char c : h) e += std::string(2, c);
      h = e;
    }
    if (h.size() != 6 && h.size() != 8) return false;
    unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    if (h.size() == 6) v = (v << 8) | 0xff;
    *out = Color::rgba(int((v >> 24) & 255), int((v >> 16) & 255), int((v >> 8) & 255), float(v & 255) / 255.f);
    return true;
  }
  return false;
}

bool parseColor(const Value& v, Color* out) { return v.isString() && parseColor(v.asString(), out); }

bool parseBoxShadow(const Value&, std::vector<Shadow>*) { return false; }
bool parseBackgroundImage(const Value&, std::vector<Gradient>*) { return false; }
bool parseTransform(const Value&, std::vector<TransformOp>*) { return false; }

void applyStyle(const Value& style, ComputedStyle* out, YGNodeRef yoga) {
  *out = ComputedStyle();
  YGNodeStyleSetFlexDirection(yoga, YGFlexDirectionColumn);
  YGNodeStyleSetWidthAuto(yoga);
  YGNodeStyleSetHeightAuto(yoga);
  for (auto& [key, v] : style.asObject()) {
    if (key == "backgroundColor") parseColor(v, &out->backgroundColor);
    else if (key == "width" && v.isNumber()) YGNodeStyleSetWidth(yoga, float(v.asNumber()));
    else if (key == "height" && v.isNumber()) YGNodeStyleSetHeight(yoga, float(v.asNumber()));
    else if (key == "flex" && v.isNumber()) YGNodeStyleSetFlex(yoga, float(v.asNumber()));
    else if (key == "padding" && v.isNumber()) YGNodeStyleSetPadding(yoga, YGEdgeAll, float(v.asNumber()));
    else if (key == "margin" && v.isNumber()) YGNodeStyleSetMargin(yoga, YGEdgeAll, float(v.asNumber()));
  }
}

}  // namespace jsui
