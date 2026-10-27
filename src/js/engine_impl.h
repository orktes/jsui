// Internal state of JsEngine, shared by the engine's translation units:
//   engine.cpp   - context lifetime, entry/exit bookkeeping, errors, timers, rAF
//   bridge.cpp   - __jsui_native (node tree, measure, focus, patchStyle, fonts)
//   command_buffer.cpp - applies the mutations the bridge recorded
//   host_api.cpp - __jsui_host helpers behind the JS prelude (fetch, UTF-8, log)
//   prelude.cpp  - JS source of the web-style globals (console, fetch, URL, ...)
#pragma once

#include <quickjs.h>

#include <cassert>
#include <chrono>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/perf.h"
#include "core/value.h"
#include "js/command_buffer.h"
#include "js/engine.h"
#include "platform/loader.h"

namespace jsui {

class Runtime;
class Inspector;
class JsThread;

struct JsEngine::Impl {
  Runtime& rt;
  JSRuntime* jrt = nullptr;
  JSContext* ctx = nullptr;
  // Runtime::jsThreadMode when the engine was made (switching rebuilds it):
  // the engine runs on `thread` (js/js_thread.h) and reaches the runtime
  // only the ways a JS thread can (see the reads below and Runtime::toHost).
  bool threaded = false;
  JsThread* thread = nullptr;

  // ---- perf counters JS bumps: the main document's, or in JS-thread mode
  // a block of its own that the owner thread merges in (mergePerf).
  PerfCounters jsCounters;
  PerfCounters* counters = nullptr;
  // Phase times: the main document's; on a JS thread its own (never
  // profiled: the owner's phases add up to its own frame time).
  Perf jsPerf;
  Perf* phases = nullptr;  // set as the engine is made
  Perf& perf() { return *phases; }
  // The runtime clock as JS sees it (performance.now, timers): the tick's
  // time, or on a JS thread the time of the message it handles.
  double now() const;
  // React's scheduler yields (__jsui_native.shouldYield, zero-delay timer
  // passes): the tick's JS budget, or on a JS thread the task's slice.
  bool shouldYield() const;

  // ---- timers (setTimeout / setInterval / setImmediate / MessageChannel)
  struct Timer {
    double due;
    double interval;  // < 0: one-shot
    uint64_t seq;     // FIFO order among timers due at the same time
    JSValue fn;
    std::vector<JSValue> args;
  };
  std::unordered_map<int, Timer> timers;
  int nextTimer = 1;
  uint64_t timerSeq = 0;
  double earliestDue = 1e300;  // lower bound of all timers' due time
  struct DueRef {
    double due;
    uint64_t seq;
    int id;
  };
  std::vector<DueRef> dueScratch;  // reused every tick: no per-frame allocation

  // ---- requestAnimationFrame
  struct Raf {
    int id;
    JSValue fn;
  };
  std::vector<Raf> rafs, rafsRunning;
  int nextRaf = 1;

  // ---- async native work: callbacks keyed by loader request id
  struct Pending {
    JSValue resolve = JS_UNDEFINED;  // or a plain callback
    JSValue reject = JS_UNDEFINED;
  };
  std::unordered_map<uint64_t, Pending> pending;
  // Invalidated when the engine dies; loader callbacks hold a weak_ptr to it.
  std::shared_ptr<Impl*> alive;

  // ---- unhandled rejections (reported once all jobs have drained)
  struct Rejection {
    JSValue promise;
    JSValue reason;
  };
  std::vector<Rejection> rejections;

  // ---- re-entrancy / watchdog
  int depth = 0;
  double timeoutMs = 5000;  // <= 0: no watchdog
  std::chrono::steady_clock::time_point deadline;
  bool interrupted = false;
  // Ticks a deferred / due cycle collection has waited
  // (QuickJsEngine::collectIdle).
  int gcWaitTicks = 0, gcFullWaitTicks = 0;

  // onLayout events raised while JS is on the stack are queued until it unwinds.
  struct LayoutEvt {
    uint32_t id;
    float x, y, w, h;
  };
  std::vector<LayoutEvt> layoutQueue, layoutScratch;

  // Property-name cache for the props/style bridge (atom -> UTF-8 key).
  // Atoms are dup'ed while cached; bounded so arbitrary keys cannot grow it.
  std::unordered_map<JSAtom, std::string> atomNames;
  const std::string* atomName(JSAtom atom);

  // Pending Host.calls: call id -> the settle(ok, payload) function JS passed.
  std::unordered_map<uint64_t, JSValue> hostCalls;

  // measureAsync callbacks by request id (answered by JsEngine::measured).
  std::unordered_map<uint64_t, JSValue> measureCallbacks;
  uint32_t nextMeasure = 1;

  // ---- JS-thread mode: native Animated reads come from the frame snapshot
  // (Runtime::snapshots), unless JS wrote the value in a batch the snapshot
  // does not include yet. Batches are counted as they are published
  // (single-threaded, applied: Runtime::appliedBatches keeps step).
  uint64_t publishedBatches = 0;
  struct AnimWrite {
    double value;
    uint64_t batch;  // the batch that carries the write
  };
  std::unordered_map<int, AnimWrite> animWrites;  // value tag -> setValue
  std::unordered_map<int, uint64_t> animStarts;   // animation id -> its startAnimation's batch

  // Frame data: one ArrayBuffer per host buffer, over the runtime's storage
  // (not owned by QuickJS), reused while the storage stays where it is. In
  // JS-thread mode, over a published copy (HostBuffers::Copy), which the
  // ArrayBuffer keeps alive.
  struct BufferView {
    JSValue buffer = JS_UNDEFINED;
    const void* data = nullptr;
    size_t bytes = 0;
  };
  std::unordered_map<std::string, BufferView> bufferViews;
  void releaseBuffer(const std::string& key);

  // The runtime's debugger once it is attached to this context (null in
  // builds without JSUI_DEBUGGER).
  std::shared_ptr<Inspector> inspector;

  // ---- native mutations JS made, not applied yet (js/command_buffer.h)
  CommandBuffer commands;
  // Applies the recorded commands (in the Commit phase).
  void applyCommands();
  // The end of a JS task: the microtasks have drained (drain()). The
  // commands JS recorded are published as one batch, so a React commit is
  // applied whole. (Single-threaded, the sync points below have applied
  // them already; on a JS thread the batch goes to the owner thread, with
  // the task's replies and host callbacks: JsThread::publish.)
  void publish();
  // JS returned to native code inside a task (a call() that is part of a
  // batch, a microtask round, a script's evaluation), or a bridge call is
  // about to read or affect native state (measure, getProps, Animated
  // getValue, host callbacks, fonts). Single-threaded, everything recorded
  // so far is applied here, so native never sees the tree behind JS. (On a
  // JS thread nothing happens: reads come from the frame snapshot or a
  // proxy, and only publish() hands commands over.)
  void syncPoint() {
    if (!threaded && !commands.empty()) applyCommands();
  }
  // For an op of CommandBuffer::appliesAtOnce that was just recorded:
  // applies it with everything before it and returns its answer
  // (single-threaded only: on a JS thread it is applied with its batch and
  // answers what it can without waiting, see the bridge functions).
  const CommandBuffer::Result& applyNow() {
    assert(!threaded && !commands.empty() && CommandBuffer::appliesAtOnce(commands.last()));
    applyCommands();
    return commands.result();
  }
  // Runs `fn` against the owner thread's state: single-threaded after what
  // JS recorded is applied, on a JS thread as a proxied call (false when
  // the owner thread did not run it in time).
  bool onOwner(const std::function<void()>& fn);

  // Cached atoms for hot callbacks.
  JSAtom atomDispatch = 0, atomHostMessage = 0, atomOnLayout = 0;

  explicit Impl(Runtime& r) : rt(r) {}

  // Every native -> JS entry goes through call(). It tracks depth, arms the
  // watchdog for the outermost call, reports exceptions and, when the stack
  // unwinds to native, drains microtasks, unhandled rejections and queued
  // layout events.
  JSValue call(JSValueConst fn, JSValueConst thisVal, int argc, JSValueConst* argv);
  void enter();
  void leave();
  // What the next outermost entry into JS is ("(timer)", "(event: press)"):
  // a CPU profile shows it above that entry's JS. Set right before enter()
  // or call(); ignored inside JS.
  // Only built while a profile records: `kind` alone gives "(kind)", with a
  // detail "(kind: detail)".
  void entryLabel(const char* kind, std::string_view detail = {});
  std::string nextEntry;
  int batch = 0;  // beginBatch() depth: calls inside one are entries of their own
  void drain();  // jobs + rejections + layout queue (depth must be 0)
  void runJobsRaw();
  void reportException();
  void reportValue(JSValueConst exc, const char* prefix);
  void addTimer(int id, double due, double interval, JSValue fn, std::vector<JSValue> args);
  // Starts a loader request; `onDone` runs on the runtime thread during
  // pump(), and only while this engine is alive.
  uint64_t startLoad(LoadRequest req, std::function<void(uint64_t id, Resource& res)> onDone);
  // A streamed load (fetch()): `onEvent` gets the head, body pieces and end
  // the same way.
  uint64_t startStream(LoadRequest req, std::function<void(uint64_t id, LoadEvent& ev)> onEvent);
  // Frees and forgets the JS callbacks registered for a request.
  void dropPending(uint64_t id);
  bool draining = false;

};

// Helpers shared between the engine's translation units.
Runtime& runtimeOf(JSContext* ctx);
JsEngine::Impl& implOf(JSContext* ctx);
std::string toStdString(JSContext* ctx, JSValueConst v);
// Formats an exception value as "Name: message\n<stack>".
std::string formatError(JSContext* ctx, JSValueConst exc);
Value toValue(JSContext* ctx, JSValueConst v, int depth = 0);
JSValue fromValue(JSContext* ctx, const Value& v);
// toValue / toStdString into a command payload (js/command_buffer.h).
void writeValue(JSContext* ctx, JSValueConst v, CommandBuffer::Writer& w, int depth = 0);
void writeString(JSContext* ctx, JSValueConst v, CommandBuffer::Writer& w);
// Copies the bytes of a string / ArrayBuffer / typed array / DataView.
bool bytesOf(JSContext* ctx, JSValueConst v, std::vector<uint8_t>* out);

void installBridge(JsEngine::Impl& impl, JSValue global);
// Shared state and calls on __jsui_native (bridge_host.cpp).
void installHostDataBridge(JsEngine::Impl& impl, JSValue native);
void installHostApi(JsEngine::Impl& impl, JSValue global, JSValue host);
// Evaluates the JS prelude; each piece is a function expression called with `host`.
bool runPrelude(JsEngine::Impl& impl, JSValue host);
// The prelude's pieces (the host JS engine evaluates the same sources).
struct PreludePiece {
  const char* name;
  const char* source;
};
const PreludePiece* preludePieces(size_t* count);

}  // namespace jsui
