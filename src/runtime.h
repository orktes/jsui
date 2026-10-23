// The object behind the JsuiRuntime C handle.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/document.h"
#include "host_state.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkRegion.h"
#include "jsui/jsui.h"
#include "platform/loader.h"
#include "surface.h"
#include "text/font_manager.h"

class SkCanvas;

namespace jsui {

class JsEngine;
class InputManager;
class ImageCache;
class NativeAnimated;

class Runtime {
 public:
  explicit Runtime(const JsuiConfig& cfg, JsuiRuntime* handle);
  ~Runtime();

  void log(JsuiLogLevel level, const std::string& msg);
  void reportError(const std::string& msg);  // logs + stores last error (+ overlay in dev mode)

  // Bundle lifecycle
  bool loadSource(const std::string& source, const std::string& name);
  void loadUrl(const std::string& url);
  void reload();
  // Reload at the start of the next tick (safe to call from JS / callbacks).
  void requestReload();
  // Dev-mode key handling (Escape dismisses the error overlay, F5 reloads).
  // Returns true if the key was consumed.
  bool devKey(const char* key, bool down);

  void tick(double nowMs);
  // Advances only what runs natively on the runtime clock (native-driven
  // Animated animations, LayoutAnimations, layout) without running JS:
  // timers, rAF, events and animation end callbacks wait for the next tick().
  void tickNative(double nowMs);
  // Any surface has something new to paint.
  bool needsRedraw() const;
  // Animations / timers / held input want another tick soon (jsui_wants_frame).
  bool wantsFrame() const;
  // Animations, scroll physics or JS frame callbacks drive the next frames
  // of surface `s` (Surface::needsRedraw).
  bool animatingFrames(const Surface& s) const;
  // The main surface (tests and tools that render a runtime directly).
  void render(void* pixels, int width, int height, size_t rowBytes) { main().render(pixels, width, height, rowBytes); }
  SkIRect renderIncremental(void* pixels, int width, int height, size_t rowBytes) {
    return main().renderIncremental(pixels, width, height, rowBytes);
  }
  void renderCanvas(SkCanvas* canvas, int width, int height, bool clear) {
    main().renderCanvas(canvas, width, height, clear);
  }

  // ---- surfaces (React Native root views; see surface.h)
  Surface& main() const { return *surfaces_.front(); }
  const std::vector<std::unique_ptr<Surface>>& surfaces() const { return surfaces_; }
  // A new surface running the AppRegistry component `appKey` with
  // `propsJson` (an object; empty: {}). Returns nullptr (and logs) for
  // invalid props.
  Surface* createSurface(const std::string& appKey, const std::string& propsJson, float width, float height,
                         float pixelRatio);
  // Unmounts the surface's app and frees it (later in the tick when JS is on
  // the stack).
  void destroySurface(Surface* s);
  // (Re)runs `appKey` (empty: the current one) with `propsJson` on `s`
  // (React Native's appProperties / runApplication). Returns false for
  // invalid props.
  bool runSurface(Surface* s, const std::string& appKey, const std::string& propsJson);
  // The surface whose root container is `tag`, or nullptr.
  Surface* surfaceByTag(uint32_t tag) const;
  // The surface that owns `doc` (main when unknown).
  Surface& surfaceOf(const Document* doc) const;
  // A node of any surface.
  Node* findNode(uint32_t id) const;
  // Every surface repaints on the next render (errors, reloads).
  void redrawAll();

  JsuiConfig config;
  JsuiRuntime* handle;
  std::string baseUrl;
  std::string bundleUrl, bundleSource, bundleName;
  std::string lastError;

  // Clipboard: the embedder's callbacks (jsui_set_clipboard_callbacks),
  // else an in-process string.
  std::string clipboardText();
  void setClipboardText(const std::string& text);
  JsuiClipboardGetFn clipboardGet = nullptr;
  JsuiClipboardSetFn clipboardSet = nullptr;
  void* clipboardUser = nullptr;
  std::string clipboard;
  double now = 0;

  // Shared host state (jsui_set_state / HostState in JS). Lives as long as
  // the runtime, across JS contexts.
  HostState state;
  // jsui_set_state / jsui_patch_state (json NULL removes). Logs invalid JSON.
  int setState(const char* key, const char* json, bool patch);
  // A JS-originated change of `key`: tells the embedder (on_state).
  void uiStateChanged(const std::string& key);

  // Calls: Host.call <-> on_call / jsui_resolve_call, and
  // jsui_call_ui <-> Host.handle.
  uint64_t newCallId() { return nextCallId_++; }
  int resolveCall(uint64_t id, const char* json);
  int rejectCall(uint64_t id, const char* message);
  // A Host.call ended unanswered on the JS side (timeout): tells the embedder.
  void hostCallCancelled(uint64_t id);
  uint64_t callUi(const char* name, const char* argsJson, JsuiCallResultFn done, void* user);
  // A Host.handle handler settled (or could not run); delivered at the end of the tick.
  void uiCallSettled(uint64_t id, bool ok, std::string payload);

  FontManager fonts;
  ResourceLoader loader;
  std::unique_ptr<ImageCache> images;

 private:
  // Node ids are unique across the surfaces (JS addresses nodes by id alone).
  uint32_t nextNodeId_ = 1;
  std::vector<std::unique_ptr<Surface>> surfaces_;  // [0]: the main surface
  // Destroyed while JS was on the stack: freed at the start of the next tick.
  std::vector<std::unique_ptr<Surface>> doomed_;

 public:
  // The main surface's document and input (most of the runtime predates
  // surfaces; per-surface code takes the Surface).
  Document& doc;
  InputManager* input = nullptr;
  std::unique_ptr<JsEngine> js;
  // Animated graphs driven natively (useNativeDriver: true).
  std::unique_ptr<NativeAnimated> animated;

 private:
  // Wires a new surface to the runtime's services.
  void attach(Surface& s);
  // Hands surfaces with an app to JS (__jsui_runSurface).
  void startSurface(Surface& s);
  void startSurfaces();
  bool bundleRan_ = false;  // the current context evaluated the bundle
  void resetEngine();
  // Hands native animation end events / value updates to JS.
  void deliverAnimatedEvents();
  // Image onLoad / onError / onProgress for images that settled (React
  // Native: always asynchronous, also for cached images and data URIs).
  void deliverImageEvents(Document& doc);
  // An image event can be delivered on the next tick.
  bool imageEventsReady(const Document& doc) const;
  void devPoll();

  // Hands the state keys the embedder changed since the last tick to JS.
  void deliverStateChanges();
  // Runs the done callbacks of settled jsui_call_ui calls.
  void deliverUiCallResults();
  // The JS context is going away: cancels its unanswered Host.calls (ids
  // taken from the engine before it is destroyed) and fails the unsettled
  // jsui_call_ui calls with `reason`.
  void endCalls(const std::vector<uint64_t>& hostCalls, const char* reason);
  uint64_t nextCallId_ = 1;
  struct UiCall {
    JsuiCallResultFn done;
    void* user;
  };
  std::unordered_map<uint64_t, UiCall> uiCalls_;  // jsui_call_ui calls not yet settled
  struct UiResult {
    uint64_t id;
    bool ok;
    std::string payload;
  };
  std::vector<UiResult> uiResults_;  // settled, waiting for delivery

  // Dev mode: version polling of <origin>/__jsui/version and the frame shown
  // while a reloaded bundle is broken.
  std::string devOrigin_, devVersion_;
  bool devPollInFlight_ = false;
  double devNextPoll_ = 0;
  bool reloadRequested_ = false;
};

// The Runtime behind a C handle (tools and tests that read internals such as
// Document::perf).
Runtime* internalRuntime(JsuiRuntime* handle);

}  // namespace jsui
