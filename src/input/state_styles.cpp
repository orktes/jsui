// Interaction state styles (core/state_style.h): each frame, which states
// hold for the elements that declare hoveredStyle / pressedStyle / ... or
// group*Style, and their blended styles.
#include <algorithm>
#include <atomic>

#include "core/document.h"
#include "core/state_style.h"
#include "input/input.h"

namespace jsui {

namespace {

bool within(const Node* n, const Node* ancestor) {
  for (; n; n = n->parent())
    if (n == ancestor) return true;
  return false;
}

// The element whose state `n`'s group*Style follow: the nearest
// ancestor-or-self with `group` (named `name`, when given).
Node* groupOf(Node* n, const std::string& name) {
  for (; n; n = n->parent()) {
    const StateStyles* s = n->stateStyles.get();
    if (s && s->isGroup && (name.empty() || s->groupKey == name)) return n;
  }
  return nullptr;
}

}  // namespace

// Like CSS :hover: the mouse is over the element or one of its descendants.
bool InputManager::stateHovered(const Node* n) const {
  if (!mouseInside_ || !n) return false;
  return std::find(mouse_.chain.begin(), mouse_.chain.end(), n->id()) != mouse_.chain.end();
}

// Like CSS :active, held only while the pointer stays over the element: a
// primary press (mouse or touch, not one that turned into scrolling) began
// on it or a descendant; or the activation key / gamepad A holds it; or a
// press began on it since the last frame.
bool InputManager::statePressed(const Node* n) const {
  if (!n) return false;
  auto pointer = [&](const PointerState& p) {
    if (!(p.buttons & 1) || !p.downId || p.scrollingId) return false;
    Node* d = alive(p.downId);
    return d && within(d, n) && visualRect(n).contains(p.x, p.y);
  };
  if (pointer(mouse_)) return true;
  for (const PointerState& t : touches_)
    if (pointer(t)) return true;
  if (keyPressId_ && within(alive(keyPressId_), n)) return true;
  for (uint32_t id : pressFlash_)
    if (within(alive(id), n)) return true;
  return false;
}

void InputManager::tickStates(double nowMs) {
  statesMoving_ = false;
  doc_.statesKick = false;
  auto& list = doc_.stateNodes;
  if (keyPressId_ && !alive(keyPressId_)) keyPressId_ = 0;
  // Shared by every runtime, which may tick on threads of their own.
  static std::atomic<uint64_t> passes{0};
  uint64_t pass = passes.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t keep = 0;
  for (size_t i = 0; i < list.size(); i++) {
    Node* n = doc_.get(list[i]);
    if (!n || !n->stateStyles || n->stateStyles->pass == pass) continue;  // gone, or listed twice
    list[keep++] = list[i];
    StateStyles& s = *n->stateStyles;
    s.pass = pass;
    if (!s.hasOverlays() && s.interaction.empty()) continue;  // a group only
    bool t[kStateSlots] = {};
    bool enabled = !s.disabled;
    t[kHovered] = enabled && stateHovered(n);
    t[kPressed] = enabled && statePressed(n);
    t[kFocused] = focused_ == n;
    t[kFocusVisible] = t[kFocused] && keyboardModality_;
    t[kDisabled] = s.disabled;
    t[kHidden] = s.hidden;
    if (s.hasGroupOverlays()) {
      if (Node* g = groupOf(n, s.groupName)) {
        inheritGroupTransition(s, *g->stateStyles);
        t[kGroupHovered] = stateHovered(g);
        t[kGroupPressed] = statePressed(g);
        t[kGroupFocused] = within(focused_, g);  // focus within the group
        t[kGroupFocusVisible] = t[kGroupFocused] && keyboardModality_;
      }
    }
    bool moving = false;
    if (stepStateWeights(s, t, nowMs, &moving)) {
      n->applyStateStyles();
      if (!s.interaction.empty()) doc_.interactionsMoved = true;
    }
    statesMoving_ |= moving;
  }
  list.resize(keep);
  pressFlash_.clear();
}

}  // namespace jsui
