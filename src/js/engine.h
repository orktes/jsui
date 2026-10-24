// The JS engine: evaluates the bundle and exposes the native bridge.
//
// Globals installed for the bundle (see docs/ARCHITECTURE.md "JS bridge"):
//   __jsui_native.createNode(type) -> id      .setProps(id, props)
//   __jsui_native.setText(id, text)           .appendChild(parent, child)
//   __jsui_native.insertBefore(p, c, before)  .removeChild(parent, child)
//   __jsui_native.destroyNode(id)             .rootId
//   __jsui_native.measure(id) -> {x,y,width,height,pageX,pageY}
//   __jsui_native.postMessage(name, json)     .loadFont(family, url, w, italic) -> Promise
//   __jsui_native.focus(id) / blur(id)        .scrollTo(id, x, y, animated)
//   __jsui_native.patchStyle(id, partialStyle) .reload()
//   __jsui_native.devError(text | null)       .evalScript(source, name)
//   console, setTimeout/Interval, requestAnimationFrame, queueMicrotask,
//   performance.now, fetch/Response/Headers, TextEncoder/TextDecoder, URL.
// Callbacks the bundle installs:
//   globalThis.__jsui_dispatchEvent(event) -> number (1 handled | 2 defaultPrevented)
//   globalThis.__jsui_onHostMessage(name, json)
//   globalThis.__jsui_onLayout(id, {x,y,width,height})
//   globalThis.__jsui_onHostState(keys)            shared state changed
//   globalThis.__jsui_onHostBuffer(keys)           frame data buffers changed
//   globalThis.__jsui_onHostTexture(keys)          host textures set / resized / removed
//   globalThis.__jsui_onCall(id, name, json)       jsui_call_ui -> Host.handle
//   globalThis.__jsui_onDevUpdate(token)           dev server has a new build
//                                                  (hot update; absent: reload)
// Shared state / calls (bridge_host.cpp): __jsui_native.getState, stateVersion,
//   setState, patchState, stateKeys, hostCall, cancelCall, callResult,
//   buffer, bufferVersion, bufferKeys, texture, textureKeys.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/value.h"

namespace jsui {

class Runtime;

// The engine behind the bundle. Two implementations: QuickJsEngine
// (engine.cpp, every platform) and, in WASM builds with JSUI_HOST_JS, the
// host's own JS engine (engine_hostjs.cpp: V8 / JSC / SpiderMonkey run the
// bundle, the bridge calls back into WASM; see docs/ARCHITECTURE.md "Host JS
// engine"). Runtime only talks to this interface.
class JsEngine {
 public:
  enum class Kind { QuickJs, Host };
  // The engine a new runtime gets (Runtime reads it as it is constructed and
  // keeps it across reloads). Host falls back to QuickJS where the build has
  // no host engine (hostAvailable()).
  static Kind nextKind;
  static bool hostAvailable();
  static std::unique_ptr<JsEngine> create(Runtime& rt, Kind kind);
  virtual ~JsEngine() = default;

  virtual bool eval(const std::string& source, const std::string& name) = 0;
  // False when precompiled bundles (js/bytecode.h) never run as bytecode
  // here (the host engine): containers run their source.
  virtual bool runsBytecode() const { return true; }

  // Runs due timers, then pending jobs (promises). Called from tick.
  virtual void runTimers(double nowMs) = 0;
  // Runs requestAnimationFrame callbacks.
  virtual void runAnimationFrame(double nowMs) = 0;
  virtual void runJobs() = 0;
  virtual bool hasPendingWork(double nowMs) const = 0;

  // Native -> JS. Returns the bitmask from __jsui_dispatchEvent.
  // kResponder: a JS responder holds the gesture; kBlocksNative: and it
  // blocks native ones (ScrollView touch drags), RN's blockNativeResponder.
  enum DispatchFlags { kHandled = 1, kDefaultPrevented = 2, kBlocksNative = 4, kResponder = 8 };
  virtual int dispatchEvent(const Value& event) = 0;
  virtual void hostMessage(const std::string& name, const std::string& json) = 0;
  virtual void layoutEvent(uint32_t id, float x, float y, float w, float h) = 0;
  // Calls globalThis[name](...args) if it is a function (exceptions are reported).
  // Returns false when there is no such function.
  virtual bool callGlobal(const char* name, const Value* args, int argc) = 0;

  // Shared state keys the embedder changed: __jsui_onHostState(keys).
  void stateChanged(const std::vector<std::string>& keys) { callWithKeys("__jsui_onHostState", keys); }
  // Frame data buffers the embedder changed: __jsui_onHostBuffer(keys).
  void buffersChanged(const std::vector<std::string>& keys) { callWithKeys("__jsui_onHostBuffer", keys); }
  // The storage of buffer `key` is about to be freed: detaches the
  // ArrayBuffer JS sees over it (its typed arrays read as empty).
  virtual void releaseBuffer(const std::string& key) = 0;
  // Host textures whose size or handle changed: __jsui_onHostTexture(keys).
  void texturesChanged(const std::vector<std::string>& keys) { callWithKeys("__jsui_onHostTexture", keys); }
  // Host.call bookkeeping: each pending call holds the JS settle function
  // the bundle passed to __jsui_native.hostCall.
  virtual bool hasCall(uint64_t id) const = 0;
  // Runs the call's settle(ok, payload) and forgets it. False if not pending.
  virtual bool settleCall(uint64_t id, bool ok, const std::string& payload) = 0;
  // Forgets the call without settling it (JS cancelled it).
  virtual bool dropCall(uint64_t id) = 0;
  // Ids of the pending calls (the engine is going away).
  virtual std::vector<uint64_t> pendingCalls() const = 0;

  // True while JS is on the stack (native code called from JS). Destroying
  // the engine then is not allowed; Runtime defers reloads instead.
  virtual bool inCall() const = 0;
  // Calls between begin and end share one microtask drain (at end): React
  // updates scheduled by several native -> JS calls commit together.
  virtual void beginBatch() = 0;
  virtual void endBatch() = 0;
  // Bytes allocated by the JS heap (for diagnostics / leak checks; 0 when
  // the engine cannot tell).
  virtual size_t memoryUsage() const = 0;
  // Garbage collection between frames. The QuickJS engine collects cycles
  // generationally and defers collections that come due while JS runs.
  // After each tick: a waiting young collection runs (a slice of it) when
  // the tick was `light` or it has waited a few ticks; a full collection
  // that came due runs when the runtime is `idle` (nothing to animate or
  // repaint), or after a long wait on a light tick. Returns whether it
  // collected.
  virtual bool collectIdle(bool light, bool idle) { return false; }
  struct GcStats {
    uint64_t collections = 0, full = 0;
    double us = 0, maxUs = 0;
  };
  // Collections since the last call with reset.
  virtual GcStats gcStats(bool reset) { return {}; }

  // QuickJS state shared by engine.cpp and the bridge files (engine_impl.h).
  struct Impl;

 private:
  void callWithKeys(const char* fn, const std::vector<std::string>& keys);
};

// engine_hostjs.cpp (null where the build has no host engine).
std::unique_ptr<JsEngine> makeHostJsEngine(Runtime& rt);

}  // namespace jsui
