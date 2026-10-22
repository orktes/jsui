// QuickJS host: evaluates the bundle and exposes the native bridge.
//
// Globals installed for the bundle (see docs/ARCHITECTURE.md "JS bridge"):
//   __jsui_native.createNode(type) -> id      .setProps(id, props)
//   __jsui_native.setText(id, text)           .appendChild(parent, child)
//   __jsui_native.insertBefore(p, c, before)  .removeChild(parent, child)
//   __jsui_native.destroyNode(id)             .rootId
//   __jsui_native.measure(id) -> {x,y,width,height,pageX,pageY}
//   __jsui_native.postMessage(name, json)     .loadFont(family, url, w, italic) -> Promise
//   __jsui_native.focus(id) / blur(id)        .scrollTo(id, x, y)
//   console, setTimeout/Interval, requestAnimationFrame, queueMicrotask,
//   performance.now, fetch/Response/Headers, TextEncoder/TextDecoder, URL.
// Callbacks the bundle installs:
//   globalThis.__jsui_dispatchEvent(event) -> number (1 handled | 2 defaultPrevented)
//   globalThis.__jsui_onHostMessage(name, json)
//   globalThis.__jsui_onLayout(id, {x,y,width,height})
#pragma once

#include <memory>
#include <string>

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
  enum DispatchFlags { kHandled = 1, kDefaultPrevented = 2 };
  int dispatchEvent(const Value& event);
  void hostMessage(const std::string& name, const std::string& json);
  void layoutEvent(uint32_t id, float x, float y, float w, float h);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace jsui
