// The bridge's mutation command buffer. Every change __jsui_native makes to
// the native side (node tree, props and styles, text, LayoutAnimation arming,
// focus / scroll / selection, LottieView commands, the native Animated graph)
// is recorded here when JS calls it and applied later, in call order, by
// apply().
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
// Payloads (props, styles, text, configs) are written as they are read from
// JS into one flat byte arena (Writer), not into Values: recording
// allocates nothing once the arrays have grown, and apply() builds each
// command's Value right before the node takes it, so the allocator reuses
// the memory the node's previous props free (building every Value of a
// 2000-node commit first and freeing the old props after measured 10-25%
// slower commits). The arrays keep their capacity across batches.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
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
    SetProps,            // a = id, payload = props
    SetPropsKeepStyle,   // a = id, payload = props without style; the node keeps its current style (setNativeProps)
    PatchStyle,          // a = id, payload = partial style object (null values remove keys)
    SetText,             // a = id, payload = text
    Append,              // a = parent, b = child
    Insert,              // a = parent, b = child, c = before
    Remove,              // a = parent, b = child
    Destroy,             // a = id
    ArmLayoutAnim,       // payload = config ({duration, create, update, delete}); kDisarm (no payload): disarm
    // ---- imperative view methods
    Focus,               // a = id
    Blur,                // a = id
    ScrollTo,            // a = id, x, y, kAnimated
    ScrollToEnd,         // a = id, b = content container (0: none), kAnimated, kHorizontal
    FlashScrollIndicators,  // a = id
    SetSelection,        // a = id, x = start, y = end (UTF-16 indices)
    LottieCommand,       // a = id, payload = command, x / y = frames (NaN: whole composition)
    Measure,             // a = id, b = measureAsync request (answered after the next layout)
    ListenGlobal,        // a = 1 / 0 (listening or not), payload = event type
    // ---- native Animated (a, b: tags as int32 bits)
    AnimCreate,          // a = tag, payload = config
    AnimConnect,         // a = parent, b = child
    AnimDisconnect,      // a = parent, b = child
    AnimConnectView,     // a = props tag, b = view id
    AnimDisconnectView,  // a = props tag, b = view id
    AnimDrop,            // a = tag
    AnimSetValue,        // a = tag, x
    AnimSetOffset,       // a = tag, x
    AnimFlattenOffset,   // a = tag
    AnimExtractOffset,   // a = tag
    AnimStart,           // a = animation id, b = value tag, payload = config
    AnimStop,            // a = animation id
    AnimAddEventMapping,     // a = view id, b = value tag, payload = event type, path (array of keys)
    AnimRemoveEventMapping,  // a = view id, b = value tag, payload = event type
    AnimStartListening,  // a = tag
    AnimStopListening,   // a = tag
  };
  enum Flags : uint8_t { kAnimated = 1, kHorizontal = 2, kHidden = 1, kPointerEvents = 2, kDisarm = 1 };

  struct Command {
    Op op;
    uint8_t flags = 0;
    uint32_t a = 0, b = 0, c = 0;
    uint32_t payload = 0;  // offset of the payload in the byte arena
    double x = 0, y = 0;
  };

  // What the last applied command answered (ops applied at once, below).
  struct Result {
    bool ok = true;
    double number = 0;
    std::string error;
  };

  // A growable byte array that keeps its capacity (no zero-filling, one
  // capacity check per value written).
  class Bytes {
   public:
    size_t size() const { return size_; }
    const uint8_t* data() const { return data_.get(); }
    void clear() { size_ = 0; }
    void resize(size_t n) { size_ = n; }  // shrinking only (Writer::rollback)
    uint8_t* grow(size_t n) {
      if (size_ + n > cap_) reserve(size_ + n);
      uint8_t* p = data_.get() + size_;
      size_ += n;
      return p;
    }
    void append(const Bytes& o) {
      if (o.size_) std::memcpy(grow(o.size_), o.data_.get(), o.size_);
    }

   private:
    void reserve(size_t n) {
      size_t cap = cap_ ? cap_ * 2 : 1024;
      while (cap < n) cap *= 2;
      std::unique_ptr<uint8_t[]> d(new uint8_t[cap]);
      if (size_) std::memcpy(d.get(), data_.get(), size_);
      data_ = std::move(d);
      cap_ = cap;
    }
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0, cap_ = 0;
  };

  // Writes values into a payload: scalars, strings, arrays (count first)
  // and objects (key / value pairs up to endObject; a key that comes again
  // replaces the earlier value, like Value::Object::insert_or_assign).
  class Writer {
   public:
    void null() { *buf_.grow(1) = kNull; }
    void boolean(bool b) { *buf_.grow(1) = b ? kTrue : kFalse; }
    void number(double d) {
      uint8_t* p = buf_.grow(1 + sizeof d);
      p[0] = kNumber;
      std::memcpy(p + 1, &d, sizeof d);
    }
    void string(const char* s, size_t len) { tagged(kString, s, len); }
    void beginArray(uint32_t count) {
      uint8_t* p = buf_.grow(1 + sizeof count);
      p[0] = kArray;
      std::memcpy(p + 1, &count, sizeof count);
    }
    void beginObject() { *buf_.grow(1) = kObject; }
    void key(const char* s, size_t len) { tagged(kKey, s, len); }
    void key(const std::string& s) { tagged(kKey, s.data(), s.size()); }
    void endObject() { *buf_.grow(1) = kEnd; }
    // Writing can be undone back to a mark (an object that turns out empty).
    size_t mark() const { return buf_.size(); }
    void rollback(size_t m) { buf_.resize(m); }

   private:
    friend class CommandBuffer;
    explicit Writer(Bytes& b) : buf_(b) {}
    void tagged(uint8_t tag, const char* s, size_t len) {
      uint32_t n = uint32_t(len);
      uint8_t* p = buf_.grow(1 + sizeof n + len);
      p[0] = tag;
      std::memcpy(p + 1, &n, sizeof n);
      if (len) std::memcpy(p + 1 + sizeof n, s, len);
    }
    Bytes& buf_;
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
  // A command with a payload: begin() hands out a writer, end() records the
  // command with what was written (abort() drops it). Payloads are staged
  // per nesting level, so a JS getter that calls the bridge while a payload
  // is being read from JS records its own command (before this one) without
  // mixing the two.
  Writer begin() {
    if (open_ == stage_.size()) stage_.push_back(std::make_unique<Bytes>());
    Bytes& s = *stage_[open_++];
    s.clear();
    return Writer(s);
  }
  void end(Op op, uint32_t a, uint32_t b = 0, uint8_t flags = 0, double x = 0, double y = 0) {
    Bytes& s = *stage_[--open_];
    Command cmd{op, flags, a, b};
    cmd.payload = uint32_t(bytes_.size());
    cmd.x = x;
    cmd.y = y;
    bytes_.append(s);
    cmds_.push_back(cmd);
  }
  void abort() { --open_; }

  bool empty() const { return next_ == cmds_.size(); }
  size_t size() const { return cmds_.size() - next_; }
  // The op recorded last (when not empty()).
  Op last() const { return cmds_.back().op; }

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
  // Forgets what is recorded (the engine is going away).
  void clear();

 private:
  enum : uint8_t { kNull, kFalse, kTrue, kNumber, kString, kArray, kObject, kKey, kEnd };
  void applyOne(Runtime& rt, const Command& cmd);
  // Payload decoding (a whole payload is read before its command runs).
  Value readValue(size_t& at) const;
  Value::Object readObject(size_t& at) const;  // the object at `at` (empty for anything else)
  std::string readString(size_t& at) const;    // a string or key ("" for anything else)
  uint32_t readU32(size_t& at) const {
    uint32_t v;
    std::memcpy(&v, bytes_.data() + at, sizeof v);
    at += sizeof v;
    return v;
  }

  std::vector<Command> cmds_;
  Bytes bytes_;  // payloads
  std::vector<std::unique_ptr<Bytes>> stage_;
  size_t open_ = 0;  // payloads being written (nesting level)
  size_t next_ = 0;  // the next command to apply
  int depth_ = 0;    // nested apply() calls
  Result result_;
};

}  // namespace jsui
