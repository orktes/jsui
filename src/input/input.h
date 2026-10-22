// Turns embedder input into DOM-like events: hit testing, pointer capture,
// hover (enter/leave), focus, keyboard routing and gamepad focus navigation.
//
// Events are plain objects (docs/ARCHITECTURE.md "Event objects") handed to
// JsEngine::dispatchEvent(); the JS side does capture/bubble. Native decides
// the target and runs the default actions (focus moves, scrolling, click
// synthesis) unless the dispatch reported handled / preventDefault().
//
// Nodes are tracked by id across dispatches: JS handlers may destroy any node
// while we iterate, and ids are never reused within a Document.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/node.h"
#include "core/value.h"

namespace jsui {

class Runtime;

// 2D affine matrix in CSS matrix(a, b, c, d, e, f) form:
//   x' = a x + c y + e,  y' = b x + d y + f
struct Affine {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Affine translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
  // (A * B)(p) == A(B(p)): B is applied first.
  Affine operator*(const Affine& o) const;
  void apply(float x, float y, float* ox, float* oy) const {
    *ox = a * x + c * y + e;
    *oy = b * x + d * y + f;
  }
  bool invert(Affine* out) const;
};

// Matrix of one transform function for a box of size w x h (percent
// translates resolve against the box).
Affine transformOpMatrix(const TransformOp& op, float w, float h);
// Maps a node's local (border-box) coordinates to its parent's coordinate
// space (the parent's border box, before the parent's scroll offset):
// translate(layout) * translate(origin) * transform * translate(-origin).
Affine nodeLocalToParent(const Node* n);

enum class NavDirection : uint8_t { Up, Down, Left, Right };

class InputManager {
 public:
  explicit InputManager(Runtime& rt);
  ~InputManager();

  bool mouseMove(float x, float y);
  bool mouseButton(float x, float y, int button, bool down, int mods);
  bool mouseWheel(float x, float y, float dx, float dy);
  bool mouseLeave();
  bool touch(int id, int phase, float x, float y);
  bool key(const char* key, const char* code, bool down, bool repeat, int mods);
  bool text(const char* utf8);
  bool gamepadButton(int pad, int button, bool down, float value);
  bool gamepadAxis(int pad, int axis, float value);
  void gamepadConnected(int pad, bool connected, const char* id);

  // Topmost node accepting pointer events at (x, y), honoring transforms,
  // overflow clipping, zIndex and pointerEvents.
  Node* hitTest(float x, float y);
  // Same, also returning the point in the target's local coordinates.
  Node* hitTest(float x, float y, float* localX, float* localY);

  Node* focused() const { return focused_; }
  void focus(Node* n);
  // Blurs `n` if it is focused (nullptr: blur whatever is focused).
  void blur(Node* n = nullptr);
  // Called before a node is destroyed so dangling pointers are dropped.
  void nodeRemoved(Node* n);
  // Per-tick work (stick repeat, hover refresh after layout changes).
  void tick(double nowMs);

  // --- Additional API (additive to the skeleton contract) ---

  // Viewport point -> node-local coordinates (inverse of all transforms and
  // scroll offsets on the path). False when a transform is singular.
  bool toLocal(const Node* n, float x, float y, float* lx, float* ly) const;
  // Viewport-space bounding box of a node including transforms.
  Rect visualRect(const Node* n) const;
  // focusable === true, pressable (unless focusable === false), TextInput;
  // not disabled. Does not check visibility.
  bool isFocusable(const Node* n) const;
  // Tab order (tree order) focus move inside the active focus scope.
  bool focusNext(bool backward);
  // Spatial (TV-style) focus move. Focuses the preferred / first focusable
  // node when nothing is focused. Returns true if focus changed.
  bool navigate(NavDirection dir);
  // Sets a ScrollView's offset (clamped to its content) and dispatches
  // 'scroll' when it changed. Returns true if the offset changed.
  bool scrollTo(Node* scrollView, float x, float y);
  // Scrolls ScrollView ancestors so `n` is visible.
  void scrollIntoView(Node* n);
  // Content size of a ScrollView from its children's layout extents.
  void contentSize(const Node* scrollView, float* w, float* h) const;

  // Hit testing inside a paragraph (Text node): returns the inline span (or
  // the Text itself) under a Text-local point. Set by the text layout.
  std::function<Node*(Node* text, float localX, float localY)> textHitTest;
  // Arrow keys perform spatial navigation like the D-pad (not inside TextInput).
  bool arrowKeyNavigation = true;

  static constexpr float kStickDeadzone = 0.5f;
  static constexpr double kRepeatDelayMs = 400;
  static constexpr double kRepeatIntervalMs = 120;
  static constexpr float kTouchSlop = 8;
  static constexpr int kMousePointerId = 0;  // touches use id + 1

 private:
  struct PointerState {
    int pointerId = 0;
    bool touch = false;
    int touchId = 0;
    float x = 0, y = 0;
    float startX = 0, startY = 0;
    int buttons = 0;          // DOM buttons bitmask
    uint32_t captureId = 0;   // implicit capture target while buttons are held
    uint32_t downId = 0;      // primary-button down target (click rule)
    uint32_t scrollingId = 0; // touch drag scrolling this ScrollView
    std::vector<uint32_t> chain;  // hovered path, root first
  };
  struct Repeat {
    int dir = -1;  // NavDirection or -1
    double next = 0;
  };
  struct Pad {
    float axes[4] = {0, 0, 0, 0};
    uint32_t buttons = 0;
    Repeat stick, dpad;
  };

  Node* hitNode(Node* n, float px, float py, float* lx, float* ly);
  bool attached(const Node* n) const;
  bool visible(const Node* n) const;  // attached and no display:none on the path
  Affine localToViewport(const Node* n) const;

  // Returns true when JS called preventDefault().
  bool dispatch(Value::Object&& ev);
  Value::Object baseEvent(const char* type, const Node* target) const;
  bool dispatchPointer(const char* type, Node* target, const PointerState& p, int button, float x, float y);
  void updateHover(PointerState& p, Node* hit);
  void leaveAll(PointerState& p);
  void updateCursor(Node* hit);
  bool consumes(const Node* hit, unsigned mask) const;
  bool pointerDown(PointerState& p, uint32_t hitId, int button);
  bool pointerUp(PointerState& p, uint32_t hitId, int button, bool cancel);
  Node* alive(uint32_t id) const;
  void addModifiers(Value::Object& ev) const;
  Node* commonAncestor(Node* a, Node* b) const;
  void focusFromPointer(Node* hit);
  void setFocus(Node* n);
  Node* scrollableAncestor(Node* n, float dx, float dy) const;
  bool dispatchScroll(Node* sv);

  Node* modalScope() const;
  void collectFocusable(Node* scope, std::vector<Node*>* out) const;
  Node* preferredFocus(Node* scope) const;
  bool focusedUsable() const;
  bool navStep(int dir);
  void padRepeat(Repeat& r, int dir);
  void tickFocusScopes();

  Runtime& rt_;
  Node* focused_ = nullptr;
  Node* hovered_ = nullptr;  // deepest hovered node (mouse)

  PointerState mouse_;
  bool mouseInside_ = false;
  bool hoverDirty_ = false;
  bool keyboardModality_ = false;  // last input was key/gamepad: focus is "visible"
  int mods_ = 0;
  std::vector<PointerState> touches_;
  std::array<Pad, 8> pads_;
  std::unordered_set<uint32_t> autoFocusSeen_;
  struct ScopeMemo {
    uint32_t scopeId;
    uint32_t restoreId;
  };
  std::vector<ScopeMemo> scopeStack_;
  uint32_t scopeId_ = 0;

  // Reused buffers (no per-event allocation in the steady state).
  std::vector<Node*> zOrder_;
  std::vector<uint32_t> chainBuild_;
};

}  // namespace jsui
