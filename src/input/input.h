// Turns embedder input into DOM-like events: hit testing, pointer capture,
// hover (enter/leave), focus, keyboard routing and gamepad focus navigation.
//
// Events are plain objects (docs/ARCHITECTURE.md "Event objects") posted to
// JS through the runtime's Inbound (js/inbound.h); the JS side does
// capture/bubble. Native decides the target and runs the default actions
// (focus moves, scrolling, click synthesis) unless JS's answer says
// preventDefault(). A default action JS could veto waits for that answer:
// single-threaded it is there as soon as the event is posted; when it comes
// later the action is kept pending (Deferred) until it does, or until
// Runtime::eventDeadlineMs passes (docs/ARCHITECTURE.md "Input routing
// details").
//
// Nodes are tracked by id across dispatches: JS handlers may destroy any node
// while we iterate, and ids are never reused within a Document.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/node.h"
#include "core/value.h"

namespace jsui {

class Runtime;
class Document;

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
  // Input of one surface: `doc` is the surface's document and `cursor` the
  // cursor it reports (jsui_get_cursor).
  InputManager(Runtime& rt, Document& doc, std::string& cursor);
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
  // What the UI would capture at (x, y) (jsui_hit_test): JSUI_HIT_POINTER
  // when the hit node or an ancestor handles pointer input (press / pointer /
  // hover / responder listeners, TextInput), JSUI_HIT_SCROLL when a
  // ScrollView that can scroll (or a wheel listener) is on the path. 0: the
  // point passes through to whatever is behind the UI.
  unsigned capturesAt(float x, float y);
  // capturesAt() for the whole viewport, for hosts that answer the pointer
  // question on another thread from a copy (jsui_hit_regions): rects x, y,
  // w, h, flags (5 floats each, viewport px) in paint order, the last one
  // containing a point gives its flags, none: 0. Transformed nodes give
  // their bounding box, rounded corners are ignored, and a Text with
  // pointer handlers on its spans captures its whole box. Rects that
  // capture nothing are kept only where they cover one that does, and a
  // rect that would give the answer the rects under it already give is left
  // out.
  void hitRegions(std::vector<float>* out);
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
  // JS answers arrived outside the tick (a JS thread's batch applied by
  // jsui_sync): the default actions and gestures waiting for them go on.
  void answersArrived();
  // The JS context went away: nothing it was asked will be answered (the
  // pending default actions are dropped, gestures stop waiting).
  void forgetAnswers();
  // Interaction state styles (core/state_style.h): which states hold for
  // each element that asks, the weights' transitions and the blended styles
  // (state_styles.cpp). After tick() and JS, before native Animated.
  void tickStates(double nowMs);
  // Elements the last tickStates stepped (all with state styles when a target
  // may have changed, else only those still transitioning). For tests.
  size_t statesStepped() const { return statesStepped_; }
  // Whether `n` is hovered / pressed / focused for state styles.
  bool stateHovered(const Node* n) const;
  bool statePressed(const Node* n) const;

  // Event timestamps (jsui_set_event_time): `t` on the runtime clock stamps
  // the input events sent after it until the next tick (NaN: back to the
  // tick time). eventTime() is what the next event carries; it never goes
  // backwards.
  void setEventTime(double t) { eventTime_ = t; }
  double eventTime() const;

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
  // 'scroll' when it changed. Stops a running scroll animation / fling.
  // Returns true if the offset changed.
  bool scrollTo(Node* scrollView, float x, float y);
  // animated: eases to the (clamped) target, driven by tick(). Returns true
  // if the target differs from the current offset.
  bool scrollTo(Node* scrollView, float x, float y, bool animated);
  // Scrolls ScrollView ancestors so `n` is visible, keeping `margin` px of
  // context around it where the viewport allows.
  void scrollIntoView(Node* n, bool animated = false, float margin = 0);
  // Content size of a ScrollView from its children's layout extents.
  void contentSize(const Node* scrollView, float* w, float* h) const;
  // TextInput selection set from JS (ref.setSelection / the selection
  // prop): clamped, scrolled into view, no selectionchange event.
  void setInputSelection(Node* input, int start, int end);
  // Shows the overlay scroll indicators briefly (RN flashScrollIndicators).
  void flashScrollIndicators(Node* scrollView);
  // True while scroll animations, flings, indicator fades or held gamepad
  // input need further ticks (hosts that sleep when idle keep ticking).
  bool animating() const;
  // Animate mouse-wheel notches (whole deltas >= kSmoothWheelMinDelta);
  // fractional / small deltas (trackpads) always scroll pixel-precisely.
  bool smoothWheel = true;

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

  // Scrolling (src/input/scroll.cpp).
  static constexpr float kDecelerationNormal = 0.998f;  // velocity factor per ms (iOS)
  static constexpr float kDecelerationFast = 0.99f;
  static constexpr float kMinFlingVelocity = 0.05f;     // px/ms
  static constexpr float kStopVelocity = 0.02f;         // px/ms: a fling ends below this
  // A drag pushed past an edge banks up to this much overshoot: the finger
  // comes back this far before the content moves again, so touch jitter
  // while pulling against an edge does not shake the content.
  static constexpr float kDragEdgeSlack = 12;
  static constexpr double kVelocityWindowMs = 100;
  static constexpr float kSmoothWheelMinDelta = 40;
  static constexpr double kWheelAnimMs = 150;
  static constexpr double kKeyScrollAnimMs = 180;
  static constexpr double kFocusScrollAnimMs = 220;
  static constexpr double kSnapSettleMs = 150;          // idle time before a trackpad / stick scroll snaps
  static constexpr float kLineStep = 40;                // arrow keys / D-pad past the last focusable
  static constexpr float kPageFraction = 0.875f;        // PageUp / PageDown
  static constexpr float kFocusMarginMax = 32;          // context kept around a focused item
  static constexpr float kScrollStickDeadzone = 0.2f;
  static constexpr float kScrollStickMaxSpeed = 1.6f;   // px/ms at full deflection
  static constexpr double kIndicatorHoldMs = 500;
  // Content scrolling under a still mouse: hover (enter / leave, cursor)
  // catches up once the scrolling has paused this long, like browsers, so
  // rows passing under the cursor do not each re-render. A mouse move
  // updates it at once.
  static constexpr double kHoverAfterScrollMs = 100;
  static constexpr double kIndicatorFadeMs = 300;
  static constexpr double kIndicatorFlashMs = 1000;

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
    bool dragStarted = false; // scrollbegindrag sent for scrollingId
    bool jsOwns = false;      // a JS responder kept this gesture: no native scrolling
    bool jsResponder = false; // a JS responder held it at the last dispatch
    bool owned = false;       // the UI consumed the press: its moves / release are consumed too
    std::vector<uint32_t> chain;  // hovered path, root first
    // JS answers still to come for this gesture (Inbound::take): the
    // pointerdown / pointermove dispatches whose flags set jsOwns /
    // jsResponder / owned, and a responderterminationrequest. While one is
    // out and a JS responder may want the touch, a ScrollView holds it
    // (no scrolling) until the answers or holdUntil come.
    struct Await {
      uint32_t seq;
      bool down;  // pointerdown (else pointermove)
    };
    std::vector<Await> awaiting;
    uint32_t releaseSeq = 0;
    bool released = false;       // the JS responder let the touch go (or its answer timed out)
    bool movePrevented = false;  // the last pointermove answered preventDefault()
    double holdUntil = -1;       // holding for JS until then (-1: not holding)
  };
  struct Repeat {
    int dir = -1;  // NavDirection or -1
    double next = 0;
  };
  struct Pad {
    float axes[4] = {0, 0, 0, 0};
    uint32_t buttons = 0;
    uint32_t consumed = 0;      // buttons whose press the UI consumed (release / repeats match)
    bool stickConsumed = false; // the left stick's current direction moved focus / scrolls
    Repeat stick, dpad;
    bool leftScrolls = false;   // left stick scrolls (nothing focusable to navigate)
    bool rightBlocked = false;  // JS preventDefault()-ed the right stick
    uint32_t stickScrollId = 0; // ScrollView the sticks are scrolling
    float stickScrollDir = 0;
  };
  // Per-ScrollView scroll physics (src/input/scroll.cpp).
  struct ScrollState {
    enum class Anim : uint8_t { None, Tween, Decay };
    Anim anim = Anim::None;
    bool momentum = false;   // user fling / release snap: ends with momentumscrollend
    bool accumulate = false; // wheel / key tween: further input retargets from its end
    bool easeInOut = false;
    double t0 = 0, duration = 0;
    float fromX = 0, fromY = 0, toX = 0, toY = 0;
    // Decay along the main axis: offset(t) = from + v0 (rate^t - 1) / ln(rate).
    float v0 = 0, rate = kDecelerationNormal;
    bool horizontal = false;
    // Touch drag velocity tracking (main-axis offset samples).
    bool dragging = false;
    float dragStart = 0;
    float dragOver = 0;  // overshoot banked past an edge (main axis, <= kDragEdgeSlack)
    struct Sample {
      double t;
      float pos;
    };
    std::array<Sample, 8> samples{};
    int sampleCount = 0;
    // Snap after a trackpad / stick scroll settles (-1: none).
    double settleAt = -1;
    float settleDir = 0;     // -1 / 1: snap in that direction, 0: nearest
    double shownUntil = -1;  // indicators fully visible until, then fade
    // 'scroll' to JS under scrollEventThrottle: when the last one went out
    // (event time, frame), and the time of a held-back one still owed (the
    // trailing event, sent from tick() with the latest offset).
    double scrollSentAt = -std::numeric_limits<double>::infinity();
    uint64_t scrollSentFrame = 0;
    bool scrollOwed = false;
    double scrollOwedAt = 0;
  };

  Node* hitNode(Node* n, float px, float py, float* lx, float* ly);
  struct RegionWalk;
  struct RegionCover;
  void hitRegionsOf(Node* n, const Affine& parent, const Rect& clip, unsigned mask, bool scroll, RegionWalk& w,
                    const RegionCover& cover);
  bool subtreeContained(Node* n);
  bool attached(const Node* n) const;
  bool visible(const Node* n) const;  // attached and no display:none on the path
  Affine localToViewport(const Node* n) const;

  // Sends an event JS's answer does not matter for.
  void post(Value::Object&& ev) { deliver(std::move(ev), true); }
  // post(), or only the native Animated.event mappings when !toJs.
  void deliver(Value::Object&& ev, bool toJs);
  // Sends an event and returns true with JS's answer (JsEngine flags) in
  // *flags when it is already there (always, single-threaded); false: it
  // comes later under *seq (Inbound::take).
  bool ask(Value::Object&& ev, int* flags, uint32_t* seq);
  Value::Object baseEvent(const char* type, const Node* target) const;
  Value::Object pointerEvent(const char* type, Node* target, const PointerState& p, int button, float x,
                             float y) const;
  void postPointer(const char* type, Node* target, const PointerState& p, int button, float x, float y);
  // Pointer event whose answer the caller reads: the flags, or -1 when they
  // come later (then `track`ed in p.awaiting, `down`: a pointerdown).
  int askPointer(const char* type, Node* target, PointerState& p, int button, float x, float y, bool track,
                 bool down = false);
  void updateHover(PointerState& p, Node* hit);
  void leaveAll(PointerState& p);
  void updateCursor(Node* hit);
  bool consumes(const Node* hit, unsigned mask) const;
  // A node from `n` up lists a matching __events name ("*" matches all).
  bool pathListens(const Node* n, unsigned mask) const;
  bool listensForScroll(const Node* sv) const;
  bool pointerDown(PointerState& p, uint32_t hitId, int button);
  bool pointerUp(PointerState& p, uint32_t hitId, int button, bool cancel);
  // A touch dragged past the slop inside a ScrollView turns into scrolling
  // unless a JS responder keeps it; true when the touch is taken (scrolling,
  // kept by JS, or held for JS's answers).
  bool slopScroll(PointerState& p, uint32_t targetId, uint32_t hitId, float x, float y);
  // Takes the JS answers of p's gesture that arrived.
  void absorbAnswers(PointerState& p);
  // A held touch: decides once the answers are in or holdUntil passed.
  void tickHeld(PointerState& p);
  // Forgets the answers p's gesture still waits for.
  void endGesture(PointerState& p);
  Node* alive(uint32_t id) const;
  void addModifiers(Value::Object& ev) const;
  Node* commonAncestor(Node* a, Node* b) const;
  void focusFromPointer(Node* hit);
  void setFocus(Node* n);
  Node* scrollableAncestor(Node* n, float dx, float dy) const;
  void dispatchScroll(Node* sv);

  // TextInput editing (text_editing.cpp). The focused TextInput gets keys
  // and text after JS saw them (unless prevented); see text/text_edit.h.
  bool editKey(Node* input, const std::string& key, int mods);
  bool editText(Node* input, const char* utf8);
  void editPointerDown(Node* input, const PointerState& p, int mods);
  void editPointerDrag(const PointerState& p);
  void editFocus(Node* input, bool focused);
  void editTick();
  enum class EditKind { Typing, Delete, Other };
  bool editReplace(Node* input, int start, int end, std::u16string text, EditKind kind);
  void editSelect(Node* input, int anchor, int focus, bool keepGoal = false);
  bool editUndo(Node* input, bool redo);
  void editNotify(uint32_t id, bool text, bool selection);
  std::u16string editFilter(Node* input, const std::u16string& in) const;
  bool editable(const Node* input) const;

  // Scrolling internals (src/input/scroll.cpp).
  void dispatchScrollEvent(const char* type, Node* sv, float vx = 0, float vy = 0, const float* target = nullptr,
                           bool toJs = true);
  // Sends the trailing 'scroll' a throttle held back once it is due.
  void flushOwedScroll(uint32_t svId, double now);
  // Sets the clamped offset and dispatches 'scroll' (no animation change).
  bool applyScroll(Node* sv, float x, float y, bool showIndicators);
  ScrollState& scrollState(Node* sv);
  // Ends a running animation; a fling / release snap gets momentumscrollend
  // when `notify`. Returns false if the node died in a handler.
  bool stopScrollAnimation(uint32_t svId, bool notify);
  void startTween(Node* sv, float x, float y, double durationMs, bool momentum, bool accumulate, bool easeInOut);
  // Where further input moves from: the running tween's end, else the offset.
  void scrollBase(Node* sv, float* x, float* y);
  // Snap point for a gesture ending at `projected` (main axis) with
  // `velocity` (px/ms) from `current`; NaN when the ScrollView doesn't snap
  // there (no snapping props, or a free zone of snapToOffsets).
  float snapTarget(Node* sv, float projected, float velocity, float current);
  bool snaps(const Node* sv) const;
  void beginDrag(PointerState& p, Node* sv);
  void dragTo(PointerState& p, Node* sv, float x, float y);
  void endDrag(PointerState& p, bool cancelled);
  Node* animatingScrollAncestor(Node* n);
  bool wheelScroll(Node* hit, float dx, float dy);
  // Keyboard / gamepad scroll target: focused, then hovered, then the first
  // visible ScrollView that can scroll along the axis.
  // `preferId` (still eligible) wins over the tree-order fallback.
  Node* scrollTarget(bool horizontal, uint32_t preferId = 0);
  // Moves by `amount` (sign = direction) along an axis, snapping when the
  // ScrollView snaps. Returns true if it will move.
  bool scrollBy(Node* sv, bool horizontal, float amount, double durationMs);
  bool scrollStep(int dir);  // D-pad / arrows past the last focusable
  bool keyboardScroll(const std::string& key, int mods);
  bool hasFocusTargets() const;
  void tickScrolling(double nowMs, double dtMs);
  void tickSticks(double dtMs);

  // key() / gamepadButton() without the press bookkeeping of what they return.
  bool keyAction(const char* key, const char* code, bool down, bool repeat, int mods);
  bool gamepadButtonAction(int pad, int button, bool down, float value);

  // A default action JS may veto with preventDefault(), run once JS's
  // answer to its event is there. Single-threaded that is right after the
  // event; a late answer keeps it in deferred_ (in event order) until the
  // answer or `deadline` comes.
  struct Deferred {
    enum class Kind : uint8_t { Key, Text, PadButton, PadAxis, Wheel };
    Kind kind;
    bool down = false, repeat = false;   // key / button
    bool edge = true;                    // PadButton: pressed or released (not a repeat)
    bool hitAgain = false;               // Wheel: JS saw it, hit test again
    int mods = 0;
    int pad = 0, index = 0;              // gamepad, button / axis; Wheel: the hit node
    float x = 0, y = 0, dx = 0, dy = 0;  // wheel point and deltas; stick axes
    std::string text;                    // key / text
    uint32_t seq = 0;                    // answer waited for (0: `flags` known)
    int flags = 0;
    double deadline = 0;
  };
  // Runs `d` with JS's answer when it is there and nothing is pending before
  // it; otherwise queues it (`vetoable`: a listener on the path, or a global
  // one, may preventDefault() the event, so it waits for the answer; else it
  // runs in turn as unprevented). Returns the action's result, or the
  // prediction (true: consumed) while it is pending.
  bool settle(bool answered, int flags, uint32_t seq, Deferred&& d, bool vetoable);
  // Where key / gamepad declarations (preventDefaultKeys /
  // preventDefaultButtons) are looked up from: the focused node, else the
  // active focus scope.
  Node* declScope() const;
  // The event's default action is declared prevented: JS's answer (forgotten
  // when still out) no longer matters.
  void declare(bool* answered, int* flags, uint32_t seq);
  bool runDefault(const Deferred& d, int flags);
  // Runs the pending defaults whose answer came (or whose deadline passed).
  void runDeferred();
  bool keyDefault(const Deferred& d, bool prevented);
  bool textDefault(const Deferred& d, bool prevented);
  bool padButtonDefault(const Deferred& d, bool prevented);
  bool padAxisDefault(const Deferred& d, bool prevented);
  bool wheelDefault(const Deferred& d, bool prevented);
  // A late answer missed its deadline: the default ran anyway.
  void lateWarning(const char* what);

  Node* modalScope() const;
  void collectFocusable(Node* scope, std::vector<Node*>* out) const;
  Node* preferredFocus(Node* scope) const;
  bool focusedUsable() const;
  bool navStep(int dir);
  void padRepeat(Repeat& r, int dir);
  void tickFocusScopes();

  Runtime& rt_;
  Document& doc_;
  std::string& cursor_;
  Node* focused_ = nullptr;
  Node* hovered_ = nullptr;  // deepest hovered node (mouse)

  PointerState mouse_;
  bool mouseInside_ = false;
  bool hoverDirty_ = false;
  double scrolledUnderMouseAt_ = -std::numeric_limits<double>::infinity();
  bool keyboardModality_ = false;  // last input was key/gamepad: focus is "visible"
  int mods_ = 0;
  std::vector<PointerState> touches_;
  std::array<Pad, 8> pads_;
  std::unordered_set<uint32_t> autoFocusSeen_;
  // Keys (code, else key) whose keydown the UI consumed: their repeats and
  // keyup report consumed too, so the game sees whole presses.
  std::unordered_set<std::string> consumedKeys_;
  struct ScopeMemo {
    uint32_t scopeId;
    uint32_t restoreId;
  };
  std::vector<ScopeMemo> scopeStack_;
  uint32_t scopeId_ = 0;
  std::unordered_map<uint32_t, ScrollState> scrolls_;
  double lastTick_ = -1;
  double eventTime_ = std::numeric_limits<double>::quiet_NaN();
  mutable double lastEventTime_ = -std::numeric_limits<double>::infinity();
  bool pointerFocus_ = false;  // focus change caused by a press: no scroll into view
  // State styles: the element an activation key / gamepad A holds pressed,
  // presses that began since the last tickStates (a click shorter than a
  // frame still shows its pressed state for one frame), and whether a
  // transition is running.
  uint32_t keyPressId_ = 0;
  std::string keyPressKey_;
  int padPress_ = -1;
  std::vector<uint32_t> pressFlash_;
  bool statesMoving_ = false;
  // What the last full tickStates pass derived the targets from (hover,
  // focus, tree and state props). While these hold and no press is under
  // way, only the elements still transitioning are stepped.
  struct StateInputs {
    bool valid = false;
    bool pressing = false;  // a press was under way (its end derives the targets again too)
    bool mouseInside = false;
    std::vector<uint32_t> chain;
    uint32_t focusedId = 0;
    bool keyboardModality = false;
    uint32_t keyPressId = 0;
    uint64_t treeVersion = 0, stateVersion = 0;
    bool operator==(const StateInputs& o) const {
      return valid && o.valid && !pressing && !o.pressing && mouseInside == o.mouseInside && chain == o.chain && focusedId == o.focusedId &&
             keyboardModality == o.keyboardModality && keyPressId == o.keyPressId && treeVersion == o.treeVersion &&
             stateVersion == o.stateVersion;
    }
  };
  StateInputs stateInputs() const;
  StateInputs stateInputs_;
  std::vector<uint32_t> statesAnimating_;  // ids whose weights were still moving
  double statesLastMs_ = -1;               // the previous tickStates frame
  size_t statesStepped_ = 0;
  // Default actions waiting for a late JS answer, in event order.
  std::deque<Deferred> deferred_;

  // TextInput pointer selection: drag target, granularity (0 characters,
  // 1 words, 2 everything) and the word the drag started on.
  uint32_t dragInputId_ = 0;
  int dragMode_ = 0, dragWordStart_ = 0, dragWordEnd_ = 0;
  // Multi-click detection (double: word, triple: all).
  uint32_t lastClickId_ = 0;
  double lastClickTime_ = -1e9;
  float lastClickX_ = 0, lastClickY_ = 0;
  int clickCount_ = 0;
  // selectTextOnFocus selected everything: the press that focused it does
  // not place the caret.
  uint32_t selectedOnFocusId_ = 0;

  // Reused buffers (no per-event allocation in the steady state).
  std::vector<Node*> zOrder_;
  std::vector<uint32_t> chainBuild_;
};

}  // namespace jsui
