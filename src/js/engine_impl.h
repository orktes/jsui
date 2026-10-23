// Internal state of JsEngine, shared by the engine's translation units:
//   engine.cpp   - context lifetime, entry/exit bookkeeping, errors, timers, rAF
//   bridge.cpp   - __jsui_native (node tree, measure, focus, patchStyle, fonts)
//   host_api.cpp - __jsui_host helpers behind the JS prelude (fetch, UTF-8, log)
//   prelude.cpp  - JS source of the web-style globals (console, fetch, URL, ...)
#pragma once

#include <quickjs.h>

#include <chrono>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.h"
#include "js/engine.h"
#include "platform/loader.h"

namespace jsui {

class Runtime;

struct JsEngine::Impl {
  Runtime& rt;
  JSRuntime* jrt = nullptr;
  JSContext* ctx = nullptr;

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

  // Frame data: one ArrayBuffer per host buffer, over the runtime's storage
  // (not owned by QuickJS), reused while the storage stays where it is.
  struct BufferView {
    JSValue buffer = JS_UNDEFINED;
    const void* data = nullptr;
    size_t bytes = 0;
  };
  std::unordered_map<std::string, BufferView> bufferViews;
  void releaseBuffer(const std::string& key);

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
  void drain();  // jobs + rejections + layout queue (depth must be 0)
  void runJobsRaw();
  void reportException();
  void reportValue(JSValueConst exc, const char* prefix);
  void addTimer(int id, double due, double interval, JSValue fn, std::vector<JSValue> args);
  // Starts a loader request; `onDone` runs on the runtime thread during
  // pump(), and only while this engine is alive.
  uint64_t startLoad(LoadRequest req, std::function<void(uint64_t id, Resource& res)> onDone);
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
// Copies the bytes of a string / ArrayBuffer / typed array / DataView.
bool bytesOf(JSContext* ctx, JSValueConst v, std::vector<uint8_t>* out);

void installBridge(JsEngine::Impl& impl, JSValue global);
// Shared state and calls on __jsui_native (bridge_host.cpp).
void installHostDataBridge(JsEngine::Impl& impl, JSValue native);
void installHostApi(JsEngine::Impl& impl, JSValue global, JSValue host);
// Evaluates the JS prelude; each piece is a function expression called with `host`.
bool runPrelude(JsEngine::Impl& impl, JSValue host);

}  // namespace jsui
