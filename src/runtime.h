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
#include "text/font_manager.h"

class SkCanvas;
class SkPicture;

namespace jsui {

class JsEngine;
class InputManager;
class ImageCache;
class DevOverlay;
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
  bool needsRedraw() const;
  // Animations / timers / held input want another tick soon (jsui_wants_frame).
  bool wantsFrame() const;
  void render(void* pixels, int width, int height, size_t rowBytes);
  // Like render(), but `pixels` must still hold the frame this runtime last
  // rendered into it: only the changed region is repainted. Returns the
  // repainted device rect (empty when nothing changed). A different buffer,
  // size or an intervening render() makes it a full repaint.
  SkIRect renderIncremental(void* pixels, int width, int height, size_t rowBytes);
  // Full repaint into any canvas of width x height device px (the GPU
  // backend's surfaces). `clear`: clear to transparent first, else draw over
  // what the canvas holds. The next renderIncremental() repaints fully.
  void renderCanvas(SkCanvas* canvas, int width, int height, bool clear);
  // Damage-tracked repaint for a GPU backend that may keep the frame in a
  // surface of its own (the WebGPU backend's retained frame texture; its
  // targets, canvas textures, start cleared every frame). `key` names that
  // retained surface (nullptr: none yet); it is valid when it holds the frame
  // this runtime last painted. Once the frame's damage is known (the bounds
  // of the changed device pixels), `choose` says where to paint:
  //   * {canvas, key}: `canvas` is the retained surface `key`. When it was
  //     valid only the damage bounds are repainted, clipped to that one rect
  //     (a scissor: complex clips cost GPU backends extra passes); otherwise
  //     the whole frame. The next frame can repaint incrementally again.
  //   * {canvas, nullptr}: a full repaint into some other canvas (the target
  //     itself, when most of the frame changed anyway); the retained surface
  //     misses this frame, so its next use repaints fully.
  //   * {nullptr}: nothing is painted (the backend failed).
  // Returns the repainted device rect.
  struct CanvasChoice {
    SkCanvas* canvas = nullptr;
    const void* key = nullptr;  // the retained surface painted into, or nullptr
    bool clear = true;          // full repaints: clear first, else draw over the canvas
    // Damage-only repaints: a few rects covering the damage, each painted
    // in its own pass scissored to it (see paintDocumentRects), instead of
    // the one bounding rect. Fewer pixels when the damage is scattered;
    // a pass costs a tree walk and the draws it crosses again.
    bool splitDamage = false;
  };
  // While `choose` runs: the frame's exact damage region (device pixels).
  const SkRegion& trackedDamage() const { return trackedDamage_; }
  template <typename Choose>
  SkIRect renderCanvasTracked(const void* key, int width, int height, Choose&& choose) {
    bool keyValid = false;
    SkIRect damage = beginCanvasTracked(key, width, height, &keyValid);
    return finishCanvasTracked(choose(damage, keyValid), width, height, keyValid, damage);
  }

  JsuiConfig config;
  JsuiRuntime* handle;
  std::string baseUrl;
  std::string bundleUrl, bundleSource, bundleName;
  std::string lastError;
  std::string cursor = "default";

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

  Document doc;
  FontManager fonts;
  ResourceLoader loader;
  std::unique_ptr<ImageCache> images;
  std::unique_ptr<JsEngine> js;
  std::unique_ptr<InputManager> input;
  // Animated graphs driven natively (useNativeDriver: true).
  std::unique_ptr<NativeAnimated> animated;

 private:
  SkIRect renderImpl(void* pixels, int width, int height, size_t rowBytes, bool incremental);
  SkIRect paintTracked(SkCanvas* canvas, const void* key, int width, int height, size_t rowBytes,
                       bool incremental);
  // The full frame: document (or the frozen last good frame) + dev overlay.
  void paintFrame(SkCanvas* canvas, bool frozen, bool overlay);
  void resetEngine();
  // Hands native animation end events / value updates to JS.
  void deliverAnimatedEvents();
  // Image onLoad / onError / onProgress for images that settled (React
  // Native: always asynchronous, also for cached images and data URIs).
  void deliverImageEvents();
  // An image event can be delivered on the next tick.
  bool imageEventsReady() const;
  // renderCanvasTracked(): layout + damage (the bounds, or empty), then the paint.
  SkIRect beginCanvasTracked(const void* key, int width, int height, bool* keyValid);
  SkIRect finishCanvasTracked(const CanvasChoice& c, int width, int height, bool keyValid, const SkIRect& damage);
  bool trackedFrozen_ = false, trackedOverlay_ = false;
  const void* trackedKey_ = nullptr;
  SkRegion trackedDamage_;
  // The buffer the last frame was painted into (incremental repaint).
  const void* lastTarget_ = nullptr;
  int lastTargetW_ = 0, lastTargetH_ = 0;
  size_t lastTargetRowBytes_ = 0;
  void devPoll();
  bool redraw_ = true;

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
  sk_sp<SkPicture> lastGoodFrame_;
  std::unique_ptr<DevOverlay> overlay_;
};

// The Runtime behind a C handle (tools and tests that read internals such as
// Document::perf).
Runtime* internalRuntime(JsuiRuntime* handle);

}  // namespace jsui
