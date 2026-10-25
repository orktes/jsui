// The overlay layer: popouts, tooltips and dropdowns that escape the clips
// (overflow, ScrollView) and the z-order of their ancestors while staying
// attached to an anchor, positioned natively every frame.
//
// A node with the `overlay` prop (React: <Overlay>) stays where React put it
// in the logical tree (Node::parent): hover and press chains, group state,
// focus order and event bubbling go through its React parent, so a popout
// rendered inside a hover group keeps the group hovered while the pointer
// is on it. It renders elsewhere (Node::renderParent): Document keeps one
// overlay layer per surface, a native View that is the root's last render
// child (above everything, zIndex raised over the root's other children)
// and fills the viewport; each overlay gets a native host in it, in logical
// tree order (later overlays, nested ones included, paint on top). Layout,
// painting, damage and hit testing see the host as an ordinary absolutely
// positioned View, so they need no special cases.
//
// Document::layout places the overlays after Yoga, every call (scrolling,
// Animated transforms and other paint-only changes of the anchor move them
// without JS):
//  - the host takes the logical parent's coordinate space: its transform is
//    the parent's full matrix (layout, scroll offsets and transforms of
//    every render ancestor), so a popout inside a scaled stage is scaled the
//    same, and its opacity the product of its logical ancestors' (a fading
//    screen fades its popouts);
//  - in that space the overlay box goes on the requested side of the anchor
//    (`placement`, `offset`, `crossOffset`), flips to the opposite side when
//    it does not fit (`flip`), and shifts along the anchor's edge to stay in
//    the viewport or the `boundary` node (`collision`, `collisionPadding`);
//  - `overlayArrow` nodes inside it are moved to the edge facing the anchor,
//    centred on it as far as the box allows;
//  - an overlay whose logical ancestors are not all displayed (display:
//    none, <Visibility mode="remove">, detached) is display: none; one whose
//    anchor is scrolled out of its clips is hidden (`hideWhenClipped`).
#pragma once

#include <cstdint>

#include "core/value.h"

namespace jsui {

class Node;

enum class OverlaySide : uint8_t { Top, Bottom, Left, Right, Center };
enum class OverlayAlign : uint8_t { Start, Center, End };

// The `overlay` prop: {anchor, placement, offset, crossOffset, flip,
// collision, collisionPadding, boundary, matchWidth, hideWhenClipped}.
struct OverlayConfig {
  uint32_t anchor = 0;  // node id; 0: the logical parent
  OverlaySide side = OverlaySide::Bottom;
  bool autoSide = false;  // placement 'auto': the side with the most room
  OverlayAlign align = OverlayAlign::Center;
  float offset = 0, crossOffset = 0;
  bool flip = true;
  bool shift = true;  // collision: 'shift' (default) or 'none'
  float padding = 0;  // collisionPadding
  uint32_t boundary = 0;  // node id; 0: the viewport
  uint8_t matchWidth = 0;  // 1: the anchor's width, 2: at least it
  bool hideWhenClipped = true;
};
OverlayConfig parseOverlayConfig(const Value& v);

// Document's state for one overlay node (Node::overlay).
struct OverlayState {
  OverlayConfig config;
  Node* host = nullptr;
  // Resolved by Document::layout.
  bool active = false;  // displayed: the host is not display: none
  bool hidden = false;  // anchor clipped / missing (host invisible)
  float alpha = 1;      // logical ancestors' opacity
  OverlaySide placed = OverlaySide::Bottom;
  float x = 0, y = 0;   // the overlay box in its logical parent's space
  float anchor[4] = {0, 0, 0, 0};  // the anchor box there (left, top, right, bottom)
  // Written to the host's style (only when they change).
  float hostAlpha = -1;
  bool hostHidden = false;
  float matrix[4] = {1, 0, 0, 1};
  float matchedWidth = -1;
};

}  // namespace jsui
