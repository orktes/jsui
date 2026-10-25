// Native -> JS messages: the counterpart of the command buffer
// (js/command_buffer.h). Every call native makes into the bundle (input and
// view events, host messages, onLayout, the frame's timers and rAF, host
// data changes, Animated / audio reports, Host.call results, surface starts,
// loader completions, the bundle's evaluation) is posted here as a Message
// and delivered to the JsEngine in post order (docs/ARCHITECTURE.md
// "Inbound messages").
//
// Single-threaded, post() delivers at once, so every caller behaves as when
// it called the engine itself. In JS-thread mode (js/js_thread.h) the
// messages queue for the JS thread instead, which delivers them in order,
// and what native needs back arrives later as a reply, applied with the
// batch of the task that answered (Runtime::applyJs):
//
// * Events posted with a reply (Inbound::event(ev, true)) get a sequence
//   number; JS's answer (JsEngine::DispatchFlags) is stored under it until
//   the poster takes it (take). InputManager runs a default action JS could
//   veto when the answer is there, or keeps it pending until the answer or
//   its deadline (eventDeadlineMs) comes (docs/ARCHITECTURE.md "Input
//   routing details").
// * A global function the bundle does not define replies "no handler"
//   (Missing): Runtime then warns, fails the jsui_call_ui call or reloads.
//
// A test hook (replyDelayMs) holds event replies back for that long on the
// runtime clock, the way a busy JS thread would.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/value.h"

namespace jsui {

class JsEngine;
class Runtime;

class Inbound {
 public:
  // What happens when the global function of a Call message is missing.
  enum class Missing : uint8_t {
    Ignore,
    WarnNoAppRegistry,  // __jsui_runSurfaces: the bundle has no AppRegistry
    FailUiCall,         // __jsui_onCall: jsui_call_ui `id` fails ("no UI handler")
    Reload,             // __jsui_onDevUpdate: the bundle does not hot-update
  };

  struct Message {
    enum class Kind : uint8_t {
      Event,           // value: event object -> __jsui_dispatchEvent; seq: a reply is wanted
      HostMessage,     // name, text: __jsui_onHostMessage(name, json)
      Layout,          // id, rect: __jsui_onLayout(id, {x, y, width, height})
      Call,            // name: globalThis[name](...value), missing / id: see Missing
      StateChanged,    // keys: __jsui_onHostState(keys)
      BuffersChanged,  // keys: __jsui_onHostBuffer(keys)
      TexturesChanged, // keys: __jsui_onHostTexture(keys)
      SettleCall,      // id, ok, text: a Host.call's settle(ok, payload)
      Frame,           // t: due timers, requestAnimationFrame callbacks, then jobs
      Eval,            // text: source, name: its URL; seq: reply 1 when it ran without an exception
      Task,            // task: engine work that runs JS (loader completions of fetch / loadFont)
      BeginBatch,      // messages until EndBatch share one microtask drain (JsEngine::beginBatch)
      EndBatch,
    };
    Kind kind;
    bool ok = false;
    // The runtime clock when it was posted: what JS reads (performance.now,
    // new timers) while it handles the message on a JS thread.
    double now = 0;
    Missing missing = Missing::Ignore;
    uint32_t seq = 0;
    uint64_t id = 0;
    double t = 0;
    float rect[4] = {0, 0, 0, 0};
    std::string name, text;
    Value value;
    std::vector<std::string> keys;
    std::function<void()> task;
  };

  explicit Inbound(Runtime& rt) : rt_(rt) {}

  // ---- posting (single-threaded: delivered before these return; JS
  // thread: queued, and in lockstep mode waited for outside a batch)
  // An event for __jsui_dispatchEvent. `reply`: the poster wants JS's
  // DispatchFlags; returns the sequence number to take() them under (0
  // without a reply).
  uint32_t event(Value&& ev, bool reply);
  void hostMessage(std::string name, std::string json);
  void layout(uint32_t id, float x, float y, float w, float h);
  // globalThis[fn](...args).
  void call(const char* fn, Value::Array&& args, Missing missing = Missing::Ignore, uint64_t id = 0);
  void keysChanged(Message::Kind kind, std::vector<std::string>&& keys);
  void settleCall(uint64_t id, bool ok, std::string payload);
  void frame(double t);
  // Evaluates a script; take(seq) answers 1 when it ran without an exception.
  uint32_t eval(std::string source, std::string name);
  void task(std::function<void()> fn);
  void beginBatch();
  void endBatch();
  void post(Message&& m);

  // Delivers a message to `js` (single-threaded at once; a JS thread for
  // each message it takes from its queue).
  void deliver(Message& m, JsEngine& js);

  // ---- replies
  // JS's answer to message `seq` (the engine's return single-threaded, the
  // applied batch on a JS thread).
  void reply(uint32_t seq, int flags, bool event);
  // Takes the answer to `seq` when it is there (and, with replyDelayMs, due).
  bool take(uint32_t seq, int* flags);
  // Nobody waits for `seq` any more (its deadline passed): drops the answer
  // now or when it comes.
  void forget(uint32_t seq);
  // Answers waited for (tests).
  size_t awaiting() const { return replies_.size(); }
  // The JS context went away: no answer will come.
  void clearReplies() { replies_.clear(); }

  // Test hook ("late replies"): event replies become visible this long
  // after they were posted, on the runtime clock (Runtime::now), as if JS
  // answered from another thread. 0: at once.
  double replyDelayMs = 0;

 private:
  void noHandler(const Message& m);
  uint32_t nextSeq() { return ++seq_ ? seq_ : ++seq_; }

  struct Reply {
    uint32_t seq;
    bool arrived;
    int flags;
    double dueAt;  // replyDelayMs: when the poster may see it
  };
  Runtime& rt_;
  uint32_t seq_ = 0;
  std::vector<Reply> replies_;  // in post order; a handful at most
  int batchDepth_ = 0;          // BeginBatch / EndBatch posted (lockstep waits outside)
  int heldBegins_ = 0;          // BeginBatch not posted yet: an empty batch never wakes JS
};

}  // namespace jsui
