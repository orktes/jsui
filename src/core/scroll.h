// ScrollView geometry shared by input (scroll physics) and the painter
// (overlay scroll indicators): content extent, scroll range and indicator
// thumbs.
#pragma once

#include <cstdint>

#include "core/node.h"

namespace jsui {

struct ScrollMetrics {
  float viewportWidth = 0, viewportHeight = 0;  // layoutMeasurement (border box)
  float contentWidth = 0, contentHeight = 0;    // contentSize (>= viewport)
  float maxX() const { return contentWidth > viewportWidth ? contentWidth - viewportWidth : 0; }
  float maxY() const { return contentHeight > viewportHeight ? contentHeight - viewportHeight : 0; }
};

// Content extent of a ScrollView from its children's layout (plus the
// ScrollView's trailing padding and border).
ScrollMetrics scrollMetrics(const Node* scrollView);

// Overlay indicator thumbs (mobile / web overlay scrollbar style) in the
// ScrollView's border-box coordinates for a box of size w x h. A thumb is
// present when its axis overflows and the matching
// shows{Vertical,Horizontal}ScrollIndicator prop is not false.
struct ScrollIndicators {
  bool vertical = false, horizontal = false;
  Rect v, h;              // thumb rects
  uint32_t fill = 0;      // ARGB
  uint32_t stroke = 0;    // ARGB, 0: none
  float radius = 0;
};
ScrollIndicators scrollIndicators(const Node* scrollView, float w, float h);

constexpr float kScrollIndicatorThickness = 3;
constexpr float kScrollIndicatorInset = 2;
constexpr float kScrollIndicatorMinLength = 20;

}  // namespace jsui
