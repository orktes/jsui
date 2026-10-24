// Interaction state styles, resolved natively: a host element's
// hoveredStyle / pressedStyle / focusedStyle / focusVisibleStyle /
// disabledStyle (and groupHoveredStyle ... for descendants of an element with
// `group`) are merged over its style by the runtime as input changes, with an
// optional `transition`, without a JS round trip or a React render. React's
// `style` stays the base: a re-render with other props keeps the overlays.
//
// InputManager::tickStates decides which states hold each frame (hover
// chain, pointer / key presses, focus) and eases each state's weight toward
// it; Node::setProps applies the blend to new props. The same weights feed
// Animated.interaction values (NativeAnimated Type::Interaction).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/value.h"

namespace jsui {

// The states, in blend order (a later one wins where two set the same key):
// group states under the element's own, pressed over focus over hover,
// disabled last.
enum StateSlot : uint8_t {
  kGroupHovered,
  kGroupFocused,
  kGroupFocusVisible,
  kGroupPressed,
  kHovered,
  kFocused,
  kFocusVisible,
  kPressed,
  kDisabled,
  kHidden,  // <Visibility> / `visible`: fades / scales out, then display none
  kStateSlots
};

struct StateStyles {
  Value raw;   // the props as React sent them (what setProps diffs against)
  Value base;  // their flattened style
  Value overlay[kStateSlots];  // flattened style objects (null: none)
  std::vector<std::string> keys;  // every key some overlay sets
  // transition, per slot
  double duration[kStateSlots] = {};
  double delay[kStateSlots] = {};
  float bezier[kStateSlots][4] = {};  // cubic-bezier control points (linear: 0, 0, 1, 1)
  // the eased weight of each state, 0..1, and the transition it is in
  float weight[kStateSlots] = {};
  float from[kStateSlots] = {};
  bool target[kStateSlots] = {};
  double start[kStateSlots] = {};
  bool seen = false;  // weights were set once (the first frame does not transition)
  uint64_t pass = 0;  // InputManager::tickStates pass that last visited it
  double lastMs = 0;   // the frame the weights were last stepped at
  double frameMs = 0;  // the frame interval before it (at most a 60 Hz frame)
  bool isGroup = false;     // `group`: descendants' group*Style follow this element
  std::string groupKey;     // `group="name"`
  std::string groupName;    // `groupName`: the group this element follows (empty: nearest)
  std::string interaction;  // `interaction` key (Animated.interaction values read this element)
  bool disabled = false;
  bool hidden = false;  // `visible` false (a prop, or written by native Animated: Node::setHidden)
  Value hiddenEnd;      // keys set once fully hidden (display / pointerEvents)
  bool hasTransition = false;  // its own `transition` (else group states take the group's)
  bool hasOverlays() const { return !keys.empty(); }
  bool hasGroupOverlays() const;
};

// Whether `props` asks for any of the above.
bool wantsStateStyles(const Value& props);
// Parses the state props (keeps the weights).
void parseStateStyles(const Value& props, StateStyles* s);
// The base style with the overlays at their weights.
Value blendStateStyle(const StateStyles& s);
// The overlay keys only, as a patchStyle partial (null: the base lacks the
// key and no state sets it).
Value::Object blendStateKeys(const StateStyles& s);
// Moves each weight toward its target at `nowMs`. Returns true if any weight
// changed; `*moving` is set while a transition is still running.
bool stepStateWeights(StateStyles& s, const bool targets[kStateSlots], double nowMs, bool* moving);
// Group states of an element without its own `transition` use the group
// element's.
void inheritGroupTransition(StateStyles& s, const StateStyles& group);

// One key of the base style with the overlays at their weights.
Value blendKey(const StateStyles& s, const std::string& key);

// One style value between `a` (null: the key's resting default) and `b`.
// Numbers, colors, transforms with the same functions and objects of
// numbers interpolate; anything else switches at the halfway point.
Value mixStyleValue(const std::string& key, const Value& a, const Value& b, float w);

}  // namespace jsui
