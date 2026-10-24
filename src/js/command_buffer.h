// The bridge's mutation command buffer. Every change __jsui_native makes to
// the native side (node tree, props and styles, text, LayoutAnimation arming,
// focus / scroll / selection, LottieView commands, the native Animated graph)
// is recorded here when JS calls it and applied later, in call order, by
// apply(). The JSValue -> Value conversion happens when the command is
// recorded (on the JS side); apply() only does native work.
//
// Publication (docs/ARCHITECTURE.md "Command buffer"): a JS task (everything
// between two returns to native, microtasks included) publishes its commands
// as a whole, so a React commit is never applied in parts. Single-threaded,
// the engine applies them as soon as JS returns to native code
// (JsEngine::Impl::syncPoint / publish), before native looks at the tree
// again, and before a bridge call that reads native state (measure,
// getProps, Animated getValue) answers. JS never observes the difference:
// pixels, events and ids are the same as when every call applied at once.
//
// Node ids: createNode takes the id from the runtime's node counter right
// away (Runtime::allocNodeId) and records Create(id); later commands of the
// same batch can name the node before it exists natively. Unknown ids are
// ignored when applied, as the bridge always did.
//
// Allocation: the buffer keeps its arrays (commands, values, objects,
// strings) across batches, so recording allocates nothing beyond the Values
// the conversion builds, which the old direct calls built too.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/node.h"
#include "core/value.h"

namespace jsui {

class Runtime;

class CommandBuffer {
 public:
  enum class Op : uint8_t {
    // ---- node tree (a, b, c: node ids; 0 = none)
    Create,              // a = id, b = NodeType, c = rootTag (0: main surface)
    SetProps,            // a = id, value = props
    SetPropsKeepStyle,   // a = id, value = props without style; the node keeps its current style (setNativeProps)
    PatchStyle,          // a = id, object = partial style (null values remove keys)
    SetText,             // a = id, string = text
    Append,              // a = parent, b = child
    Insert,              // a = parent, b = child, c = before
    Remove,              // a = parent, b = child
    Destroy,             // a = id
    ArmLayoutAnim,       // value = config ({duration, create, update, delete}); kDisarm flag: disarm
    // ---- imperative view methods
    Focus,               // a = id
    Blur,                // a = id
    ScrollTo,            // a = id, x, y, kAnimated
    ScrollToEnd,         // a = id, b = content container (0: none), kAnimated, kHorizontal
    FlashScrollIndicators,  // a = id
    SetSelection,        // a = id, x = start, y = end (UTF-16 indices)
    LottieCommand,       // a = id, string = command, x / y = frames (NaN: whole composition)
    // ---- native Animated (a, b: tags as int32 bits)
    AnimCreate,          // a = tag, value = config
    AnimConnect,         // a = parent, b = child
    AnimDisconnect,      // a = parent, b = child
    AnimConnectView,     // a = props tag, b = view id
    AnimDisconnectView,  // a = props tag, b = view id
    AnimDrop,            // a = tag
    AnimSetValue,        // a = tag, x
    AnimSetOffset,       // a = tag, x
    AnimFlattenOffset,   // a = tag
    AnimExtractOffset,   // a = tag
    AnimStart,           // a = animation id, b = value tag, value = config
    AnimStop,            // a = animation id
    AnimAddEventMapping,     // a = view id, b = value tag, string = event type, then `count` path keys
    AnimRemoveEventMapping,  // a = view id, b = value tag, string = event type
    AnimStartListening,  // a = tag
    AnimStopListening,   // a = tag
  };
  enum Flags : uint8_t { kAnimated = 1, kHorizontal = 2, kHidden = 1, kPointerEvents = 2, kDisarm = 1 };

  struct Command {
    Op op;
    uint8_t flags = 0;
    uint32_t a = 0, b = 0, c = 0;
    uint32_t arg = 0;    // index into values_ / objects_ / strings_
    uint32_t count = 0;  // AnimAddEventMapping: path keys after the event type in strings_
    double x = 0, y = 0;
  };

  // What the last applied command answered (ops applied at once, below).
  struct Result {
    bool ok = true;
    double number = 0;
    std::string error;
  };

  // ---- recording
  void push(Op op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint8_t flags = 0) {
    cmds_.push_back(Command{op, flags, a, b, c});
  }
  void pushXY(Op op, uint32_t a, double x, double y, uint8_t flags = 0) {
    Command cmd{op, flags, a};
    cmd.x = x;
    cmd.y = y;
    cmds_.push_back(cmd);
  }
  void pushValue(Op op, uint32_t a, Value v, uint32_t b = 0, uint8_t flags = 0) {
    Command cmd{op, flags, a, b};
    cmd.arg = uint32_t(values_.size());
    values_.push_back(std::move(v));
    cmds_.push_back(cmd);
  }
  void pushObject(Op op, uint32_t a, Value::Object o) {
    Command cmd{op, 0, a};
    cmd.arg = uint32_t(objects_.size());
    objects_.push_back(std::move(o));
    cmds_.push_back(cmd);
  }
  // `extra` strings (pushString) follow this one in the arena.
  void pushString(Op op, uint32_t a, std::string s, uint32_t b = 0, uint32_t extra = 0, double x = 0, double y = 0) {
    Command cmd{op, 0, a, b};
    cmd.arg = uint32_t(strings_.size());
    cmd.count = extra;
    cmd.x = x;
    cmd.y = y;
    strings_.push_back(std::move(s));
    cmds_.push_back(cmd);
  }
  void pushString(std::string s) { strings_.push_back(std::move(s)); }

  bool empty() const { return next_ == cmds_.size(); }
  size_t size() const { return cmds_.size() - next_; }

  // Single-threaded, these ops are applied as soon as they are recorded
  // (with everything recorded before them): they call back into JS while
  // native runs them (focus / blur / scroll / selection events) or answer
  // the caller (lottieCommand's bool, Animated create / start errors,
  // stopAnimation's velocity). Every other op waits for the publish point.
  static bool appliesAtOnce(Op op) {
    switch (op) {
      case Op::Focus:
      case Op::Blur:
      case Op::ScrollTo:
      case Op::ScrollToEnd:
      case Op::FlashScrollIndicators:
      case Op::SetSelection:
      case Op::LottieCommand:
      case Op::AnimCreate:
      case Op::AnimStart:
      case Op::AnimStop:
        return true;
      default:
        return false;
    }
  }

  // Applies every recorded command in order. Re-entrant: a command that
  // calls into JS (focus events) may record more commands, and a nested
  // apply() (JS returning to native inside that command) continues with
  // them, so the order is always the call order. The arrays are emptied
  // (capacity kept) when the outermost apply() returns.
  void apply(Runtime& rt);
  // The answer of the last command applied.
  const Result& result() const { return result_; }
  // Commands applied since the runtime was created (tests).
  uint64_t applied() const { return applied_; }
  // Forgets what is recorded (the engine is going away).
  void clear();

 private:
  void applyOne(Runtime& rt, const Command& cmd);

  std::vector<Command> cmds_;
  std::vector<Value> values_;
  std::vector<Value::Object> objects_;
  std::vector<std::string> strings_;
  size_t next_ = 0;  // the next command to apply
  int depth_ = 0;    // nested apply() calls
  Result result_;
  uint64_t applied_ = 0;
};

}  // namespace jsui
