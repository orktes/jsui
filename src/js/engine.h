// QuickJS host: evaluates the bundle and exposes the native bridge.
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

class JsEngine {
 public:
  explicit JsEngine(Runtime& rt);
  ~JsEngine();

  bool eval(const std::string& source, const std::string& name);

  // Runs due timers, then pending jobs (promises). Called from tick.
  void runTimers(double nowMs);
  // Runs requestAnimationFrame callbacks.
  void runAnimationFrame(double nowMs);
  void runJobs();
  bool hasPendingWork(double nowMs) const;

  // Native -> JS. Returns the bitmask from __jsui_dispatchEvent.
  // kResponder: a JS responder holds the gesture; kBlocksNative: and it
  // blocks native ones (ScrollView touch drags), RN's blockNativeResponder.
  enum DispatchFlags { kHandled = 1, kDefaultPrevented = 2, kBlocksNative = 4, kResponder = 8 };
  int dispatchEvent(const Value& event);
  void hostMessage(const std::string& name, const std::string& json);
  void layoutEvent(uint32_t id, float x, float y, float w, float h);
  // Calls globalThis[name](...args) if it is a function (exceptions are reported).
  // Returns false when there is no such function.
  bool callGlobal(const char* name, const Value* args, int argc);

  // Shared state keys the embedder changed: __jsui_onHostState(keys).
  void stateChanged(const std::vector<std::string>& keys);
  // Frame data buffers the embedder changed: __jsui_onHostBuffer(keys).
  void buffersChanged(const std::vector<std::string>& keys);
  // The storage of buffer `key` is about to be freed: detaches the
  // ArrayBuffer JS sees over it (its typed arrays read as empty).
  void releaseBuffer(const std::string& key);
  // Host textures whose size or handle changed: __jsui_onHostTexture(keys).
  void texturesChanged(const std::vector<std::string>& keys);
  // Host.call bookkeeping: each pending call holds the JS settle function
  // the bundle passed to __jsui_native.hostCall.
  bool hasCall(uint64_t id) const;
  // Runs the call's settle(ok, payload) and forgets it. False if not pending.
  bool settleCall(uint64_t id, bool ok, const std::string& payload);
  // Forgets the call without settling it (JS cancelled it).
  bool dropCall(uint64_t id);
  // Ids of the pending calls (the engine is going away).
  std::vector<uint64_t> pendingCalls() const;

  // True while JS is on the stack (native code called from JS). Destroying
  // the engine then is not allowed; Runtime defers reloads instead.
  bool inCall() const;
  // Calls between begin and end share one microtask drain (at end): React
  // updates scheduled by several native -> JS calls commit together.
  void beginBatch();
  void endBatch();
  // Bytes allocated by the JS heap (for diagnostics / leak checks).
  size_t memoryUsage() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace jsui
