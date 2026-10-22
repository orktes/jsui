#include "core/scroll.h"

#include <algorithm>
#include <cmath>

namespace jsui {

ScrollMetrics scrollMetrics(const Node* sv) {
  ScrollMetrics m;
  if (!sv || !sv->yoga()) return m;
  Rect r = sv->layoutRect();
  float right = 0, bottom = 0;
  for (Node* c : sv->children()) {
    if (!c->yoga() || !c->style().display) continue;
    Rect cr = c->layoutRect();
    if (!std::isfinite(cr.x) || !std::isfinite(cr.y) || !std::isfinite(cr.width) || !std::isfinite(cr.height)) continue;
    right = std::max(right, cr.x + cr.width + YGNodeLayoutGetMargin(c->yoga(), YGEdgeRight));
    bottom = std::max(bottom, cr.y + cr.height + YGNodeLayoutGetMargin(c->yoga(), YGEdgeBottom));
  }
  YGNodeRef y = sv->yoga();
  right += YGNodeLayoutGetPadding(y, YGEdgeRight) + YGNodeLayoutGetBorder(y, YGEdgeRight);
  bottom += YGNodeLayoutGetPadding(y, YGEdgeBottom) + YGNodeLayoutGetBorder(y, YGEdgeBottom);
  m.viewportWidth = std::isfinite(r.width) ? r.width : 0;
  m.viewportHeight = std::isfinite(r.height) ? r.height : 0;
  m.contentWidth = std::max(right, m.viewportWidth);
  m.contentHeight = std::max(bottom, m.viewportHeight);
  return m;
}

ScrollIndicators scrollIndicators(const Node* sv, float w, float h) {
  ScrollIndicators out;
  if (!sv || sv->type() != NodeType::ScrollView) return out;
  const Value& props = sv->props();
  ScrollMetrics m = scrollMetrics(sv);
  // Tolerate sub-pixel overflow from fractional layout.
  bool overflowY = m.contentHeight > m.viewportHeight + 0.5f;
  bool overflowX = m.contentWidth > m.viewportWidth + 0.5f;
  const Value& showV = props["showsVerticalScrollIndicator"];
  const Value& showH = props["showsHorizontalScrollIndicator"];
  out.vertical = overflowY && !(showV.isBool() && !showV.asBool());
  out.horizontal = overflowX && !(showH.isBool() && !showH.asBool());
  if (!out.vertical && !out.horizontal) return out;
  const float t = kScrollIndicatorThickness, inset = kScrollIndicatorInset;
  out.radius = t / 2;
  // Leave the corner free when both thumbs show.
  float corner = out.vertical && out.horizontal ? t + inset : 0;
  if (out.vertical) {
    float track = std::max(0.f, h - 2 * inset - corner);
    float len = std::min(track, std::max(kScrollIndicatorMinLength, track * m.viewportHeight / m.contentHeight));
    float frac = m.maxY() > 0 ? std::clamp(sv->scrollY / m.maxY(), 0.f, 1.f) : 0;
    out.v = {w - inset - t, inset + (track - len) * frac, t, len};
  }
  if (out.horizontal) {
    float track = std::max(0.f, w - 2 * inset - corner);
    float len = std::min(track, std::max(kScrollIndicatorMinLength, track * m.viewportWidth / m.contentWidth));
    float frac = m.maxX() > 0 ? std::clamp(sv->scrollX / m.maxX(), 0.f, 1.f) : 0;
    out.h = {inset + (track - len) * frac, h - inset - t, len, t};
  }
  // indicatorStyle (iOS): 'default' is dark with a light rim so it reads on
  // any background, 'black' / 'white' are plain.
  const std::string& style = props["indicatorStyle"].asString();
  if (style == "white") {
    out.fill = 0x99FFFFFF;
  } else if (style == "black") {
    out.fill = 0x80000000;
  } else {
    out.fill = 0x73000000;
    out.stroke = 0x59FFFFFF;
  }
  return out;
}

}  // namespace jsui
