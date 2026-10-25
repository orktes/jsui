// The object behind the JsuiRuntime C handle.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/audio.h"
#include "core/document.h"
#include "frame_snapshot.h"
#include "host_buffers.h"
#include "host_queue.h"
#include "js/engine.h"
#include "js/inbound.h"
#include "host_state.h"
#include "render/host_textures.h"
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
class LottieCache;
class ShaderEffects;
class NativeAnimated;

class DevSession;
class Inspector;

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
  // The font set changed (register / unregister / fallbacks / default
  // family): re-measure and repaint the text on every surface.
  void fontsChanged();
  // Reload at the start of the next tick (safe to call from JS / callbacks).
  void requestReload();
  // Dev mode: the dev server has a new build (or a build error). A bundle
  // that hot-updates (jsui dev) handles it in __jsui_onDevUpdate; any other
  // bundle is reloaded.
  void devUpdate();
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
  // `propsJson` (an object; empty: {}), started on the next tick. Returns
  // nullptr (and logs) for invalid props.
  Surface* createSurface(const std::string& appKey, const std::string& propsJson, float width, float height,
                         float pixelRatio);
  // Unmounts the surface's app and frees it (later in the tick when JS is on
  // the stack).
  void destroySurface(Surface* s);
  // (Re)runs `appKey` (empty: the current one) with `propsJson` on `s`
  // (React Native's appProperties / runApplication) on the next tick; the
  // same key and props again do nothing. Returns false for invalid props.
  bool runSurface(Surface* s, const std::string& appKey, const std::string& propsJson);
  // The surface whose root container is `tag`, or nullptr.
  Surface* surfaceByTag(uint32_t tag) const;
  // The surface that owns `doc` (main when unknown).
  Surface& surfaceOf(const Document* doc) const;
  // A node of any surface.
  Node* findNode(uint32_t id) const;
  // An id for a node created later (the bridge's Create command,
  // js/command_buffer.h). It comes from the counter all surfaces number
  // their nodes with, so ids keep creation order across native and JS.
  uint32_t allocNodeId() { return nextNodeId_.fetch_add(1); }
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
  // Wall clock (steady, ms) when the current tick began, and how much JS
  // work a tick may do before React's scheduler yields to the next frame
  // (__jsui_native.shouldYield): concurrent renders (startTransition) are
  // time-sliced across frames instead of blocking one.
  double tickWallStart = 0;
  double jsSliceMs = 5;  // jsui_set_js_frame_budget; <= 0: no limit
  static double wallMs();
  void prepareText();
  void freeDestroyedNodes();
  bool jsShouldYield() const { return jsSliceMs > 0 && wallMs() - tickWallStart >= jsSliceMs; }

  // ---- JS on its own thread (docs/ARCHITECTURE.md "Shared state")
  // JS-thread mode: JS reaches the runtime only the ways a JS thread will
  // (JS still runs on the owner thread for now). The embedder's callbacks
  // JS makes are queued for the tick (hostQueue), sync reads come from the
  // frame snapshot (snapshots) or a proxy call, host buffers from published
  // copies, measure callbacks after the next layout, perf counters through
  // the engine's own block. Off by default.
  bool jsThreadMode() const { return jsThreadMode_; }
  // Switches the mode, which rebuilds the JS engine: before the first
  // bundle load only (false afterwards).
  bool setJsThreadMode(bool on);
  // The embedder callbacks JS made, run in order inside the tick (and after
  // a bundle evaluates).
  HostQueue hostQueue;
  // From JS: runs `fn` now, or on the owner thread in the next drain of
  // hostQueue in JS-thread mode.
  void toHost(std::function<void()> fn);
  // How long a proxied read (HostQueue::call) waits for the owner thread.
  static constexpr double kProxyTimeoutMs = 250;
  // What JS reads synchronously in JS-thread mode (frame_snapshot.h).
  SnapshotStore snapshots;
  // Command batches applied (the engine bumps it; FrameSnapshot::appliedBatches).
  std::atomic<uint64_t> appliedBatches{0};
  // measureAsync requests the engine's commands handed over; answered after
  // this tick's layout, in order.
  struct MeasureRequest {
    uint64_t req;
    uint32_t id;
  };
  std::vector<MeasureRequest> measureRequests;
  // log / reportError on behalf of JS (queued in JS-thread mode).
  void jsLog(JsuiLogLevel level, const std::string& msg);
  void jsReportError(const std::string& msg);

  // Shared host state (jsui_set_state / HostState in JS). Lives as long as
  // the runtime, across JS contexts.
  HostState state;
  // jsui_set_state / jsui_patch_state (json NULL removes). Logs invalid JSON.
  int setState(const char* key, const char* json, bool patch);
  // A JS-originated change of `key`: tells the embedder (on_state). `json`:
  // the key's text as JS left it (nullptr: removed), else read now.
  void uiStateChanged(const std::string& key);
  void uiStateChanged(const std::string& key, const std::string* json);
  // jsui_get_state's answer (valid until the next call into the runtime).
  std::string stateText;

  // Frame data (jsui_set_buffer / jsui_map_buffer; HostBuffer in JS). Lives
  // as long as the runtime; JS views point into it (declared before js).
  HostBuffers buffers;
  // Host textures (jsui_set_texture; HostTexture in JS). Declared before the
  // surfaces: their Image nodes unregister from it when destroyed.
  HostTextures textures;

  // Calls: Host.call <-> on_call / jsui_resolve_call, and
  // jsui_call_ui <-> Host.handle.
  uint64_t newCallId() { return nextCallId_.fetch_add(1); }
  int resolveCall(uint64_t id, const char* json);
  int rejectCall(uint64_t id, const char* message);
  // A Host.call ended unanswered on the JS side (timeout): tells the embedder.
  void hostCallCancelled(uint64_t id);
  uint64_t callUi(const char* name, const char* argsJson, JsuiCallResultFn done, void* user);
  // A Host.handle handler settled (or could not run); delivered at the end of the tick.
  void uiCallSettled(uint64_t id, bool ok, std::string payload);

  FontManager fonts;
  ResourceLoader loader;
  // Dev mode: this runtime's side of the dev server's shared session.
  std::unique_ptr<DevSession> session;
  // Dev mode with a debugging dev server: the JS debugger (js/inspector.h;
  // null in builds without JSUI_DEBUGGER). Outlives the JS contexts, so
  // DevTools stays attached across reloads.
  std::shared_ptr<Inspector> inspector;
  std::unique_ptr<ImageCache> images;
  // Lottie sources and playing LottieViews (render/lottie.h).
  std::unique_ptr<LottieCache> lotties;
  // Compiled SkSL sources and ShaderViews (render/shader_view.h).
  std::unique_ptr<ShaderEffects> shaders;
  // UI audio (jsui.h "Audio"): Sound requests to the handler / built-in
  // backend. After the loader (the built-in backend reads through it).
  std::unique_ptr<Audio> audio;

 private:
  // Node ids are unique across the surfaces (JS addresses nodes by id alone).
  // Atomic: the bridge takes ids for the nodes its commands create.
  std::atomic<uint32_t> nextNodeId_{1};
  std::vector<std::unique_ptr<Surface>> surfaces_;  // [0]: the main surface
  // Destroyed while JS was on the stack: freed at the start of the next tick.
  std::vector<std::unique_ptr<Surface>> doomed_;

 public:
  // The main surface's document and input (most of the runtime predates
  // surfaces; per-surface code takes the Surface).
  Document& doc;
  InputManager* input = nullptr;
  std::unique_ptr<JsEngine> js;
  // Every call into JS goes through here (js/inbound.h).
  Inbound inbound{*this};
  // How long a default action JS could veto (Tab, Enter, wheel scrolling,
  // a ScrollView taking a touch from a JS responder, ...) waits for JS's
  // answer before it runs anyway (InputManager; single-threaded the answer
  // is always there at once).
  double eventDeadlineMs = 100;
  // Event types the bundle listens to globally (addGlobalListener: modals,
  // BackHandler, gamepad hooks), reported by __jsui_native.listenGlobal.
  // Input asks it when an answer is late: a global keydown listener may
  // veto the key's default action like one on the focused path.
  std::unordered_set<std::string> globalListeners;
  bool listensGlobally(const char* type) const { return globalListeners.count(type) > 0; }
  // Animated graphs driven natively (useNativeDriver: true).
  std::unique_ptr<NativeAnimated> animated;

 private:
  // Wires a new surface to the runtime's services.
  void attach(Surface& s);
  // Hands every surface with an app to JS (after a bundle evaluated).
  void startSurfaces();
  // Hands the pending surfaces' (all: every surface's) apps and props to JS
  // in one call: __jsui_runSurfaces([[rootTag, appKey, propsJson], ...]).
  void flushSurfaces(bool all = false);
  bool bundleRan_ = false;  // the current context evaluated the bundle
  // The engine this runtime runs its bundle on (JsEngine::nextKind when it
  // was created; reloads keep it).
  JsEngine::Kind jsKind_ = JsEngine::Kind::QuickJs;
  void resetEngine();
  // Hands native animation end events / value updates to JS.
  void deliverAnimatedEvents();
  // JS-thread mode: runs the queued host callbacks and merges the engine's
  // perf counters.
  void drainHost();
  // JS-thread mode: takes and publishes a frame snapshot.
  void publishSnapshot();
  // Answers measureRequests (after layout).
  void answerMeasures();
  bool jsThreadMode_ = false;
  // Image onLoad / onError / onProgress for images that settled (React
  // Native: always asynchronous, also for cached images and data URIs).
  void deliverImageEvents(Document& doc);
  // An image event can be delivered on the next tick.
  bool imageEventsReady(const Document& doc) const;
  void devPoll();

  // Hands the state keys the embedder changed since the last tick to JS.
  void deliverStateChanges();
  // Same for frame data buffers.
  void deliverBufferChanges();
  // ... and the host textures whose size or handle changed.
  void deliverTextureChanges();
  // LottieView load / error / finish events.
  void deliverLottieEvents();
  // ShaderView compile errors.
  void deliverShaderEvents();
  // Voice-ended / sound-loaded reports.
  void deliverAudioEvents();
  // Runs the done callbacks of settled jsui_call_ui calls.
  void deliverUiCallResults();
  // The JS context is going away: cancels its unanswered Host.calls (ids
  // taken from the engine before it is destroyed) and fails the unsettled
  // jsui_call_ui calls with `reason`.
  void endCalls(const std::vector<uint64_t>& hostCalls, const char* reason);
  std::atomic<uint64_t> nextCallId_{1};
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
  bool sessionOffered_ = false;  // the last /__jsui/version had X-Jsui-Session
  double devNextPoll_ = 0;
  bool reloadRequested_ = false;
};

// The Runtime behind a C handle (tools and tests that read internals such as
// Document::perf).
Runtime* internalRuntime(JsuiRuntime* handle);

}  // namespace jsui
