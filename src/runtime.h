// The object behind the JsuiRuntime C handle.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/audio.h"
#include "video/video.h"
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
class JsThread;
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

  // `syncJs` (JSUI_TICK_SYNC_JS): on a JS thread, waits for JS to handle
  // this frame (up to config.js_sync_timeout_ms) and applies what it did
  // before native Animated and layout, and again for the onLayout events.
  void tick(double nowMs, bool syncJs = false);
  // Advances only what runs natively on the runtime clock (native-driven
  // Animated animations, LayoutAnimations, layout) without running JS:
  // timers, rAF, events and animation end callbacks wait for the next tick().
  void tickNative(double nowMs);
  // Any surface has something new to paint.
  bool needsRedraw() const;
  // Animations / timers / held input want another tick soon (jsui_wants_frame).
  bool wantsFrame() const;
  // When JS next has a timer due (jsui_next_timer), infinity when none.
  double nextTimer() const;
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
  // jsui_set_js_frame_budget; <= 0: no limit. On a JS thread: the slice of
  // a task (atomic: the thread reads it).
  std::atomic<double> jsSliceMs{5};
  static double wallMs();
  void prepareText();
  void freeDestroyedNodes();
  bool jsShouldYield() const { return jsSliceMs > 0 && wallMs() - tickWallStart >= jsSliceMs; }

  // ---- JS on its own thread (docs/ARCHITECTURE.md "JS thread", "Shared
  // state")
  // JS-thread mode: JS runs on a thread of its own (jsThread()) and reaches
  // the runtime only the ways a JS thread can. Its commands, replies and
  // the embedder's callbacks it makes come back in batches applied inside
  // the tick (applyJs); sync reads come from the frame snapshot
  // (snapshots) or a proxy call, host buffers from published copies,
  // measure callbacks after the next layout, perf counters through the
  // engine's own block. Off by default.
  bool jsThreadMode() const { return jsThreadMode_; }
  // Switches the mode, which rebuilds the JS engine: before the first
  // bundle load only (false afterwards, or where there are no threads or
  // the host's JS engine runs the bundle). `lockstep`: every message to JS
  // is waited for (tests and tools: the frames of single-threaded mode).
  bool setJsThreadMode(bool on, bool lockstep = false);
  bool jsLockstep() const { return jsThreadMode_ && jsLockstep_; }
  JsThread* jsThread() const { return jsThread_; }
  // JS is running a task on its thread.
  bool jsBusy() const;
  // The time of the last frame JS was given (Frame messages): what the JS
  // thread's own timer passes run at between frames.
  std::atomic<double> publishedNow{0};
  // Applies what the JS thread published (commands, replies, host
  // callbacks), batch by batch. Returns how many.
  int applyJs();
  // Waits until the JS thread is idle (serving its proxied reads), applies
  // what it published and repeats while that gave JS more to do (at most
  // 8 rounds); `layout`: lays the surfaces out and delivers onLayout and
  // measure results each round (jsui_sync). timeoutMs < 0: no limit.
  // Returns true when JS was left idle, false on timeout; batches are
  // applied whole either way.
  bool syncJs(double timeoutMs, bool layout);
  // jsui_get_perf's js_* fields.
  struct JsThreadStats {
    double applyUs = 0, syncUs = 0;
    uint64_t batches = 0;
    int lagFrames = 0;  // frames since JS last published while it had work
  };
  JsThreadStats jsStats;
  // The embedder callbacks JS made, run in order inside the tick (and after
  // a bundle evaluates). On a JS thread most travel with its batches; this
  // queue keeps proxied reads and what the owner thread posts.
  HostQueue hostQueue;
  // From JS: runs `fn` now, or on the owner thread in JS-thread mode (with
  // the batch of the JS task, or in the next drain of hostQueue).
  void toHost(std::function<void()> fn);
  // How long a proxied read (HostQueue::call) waits for the owner thread.
  static constexpr double kProxyTimeoutMs = 250;
  // A read only the owner thread can answer (getClipboard, getProps, font
  // registration, a buffer's first copy): runs `fn` there and returns once
  // it ran, false when it did not run in time. From a JS thread the host
  // callbacks the task made before it and what JS published run first.
  bool proxy(const std::function<void()>& fn);
  // What JS reads synchronously in JS-thread mode (frame_snapshot.h).
  SnapshotStore snapshots;
  FrameSnapshot::Scratch snapshotScratch;  // owner thread (publishSnapshot)
  // Whether snapshots carry layouts: set by JS's first sync measure (most
  // bundles never make one; measureAsync answers from the live tree).
  std::atomic<bool> snapshotLayouts{false};
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
  // <Video> (jsui.h "Video"): requests to the embedder's video handler.
  std::unique_ptr<Video> video;

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
  // BackHandler, gamepad hooks) with a listener that may preventDefault(),
  // reported by __jsui_native.listenGlobal. Input asks it when an answer is
  // late: a global keydown listener may veto the key's default action like
  // one on the focused path. `only` (when not empty): the keys (`key` or
  // `code`) / gamepad button or axis indices the listeners may veto; empty:
  // any. Passive listeners are not reported.
  struct GlobalListen {
    std::vector<std::string> only;
  };
  std::unordered_map<std::string, GlobalListen> globalListeners;
  bool listensGlobally(const char* type) const { return globalListeners.count(type) > 0; }
  // A global listener of `type` may veto the event of `key` / `code`.
  bool globalVetoes(const char* type, std::string_view key, std::string_view code = {}) const {
    auto it = globalListeners.find(type);
    if (it == globalListeners.end()) return false;
    const auto& only = it->second.only;
    if (only.empty()) return true;
    for (const std::string& k : only)
      if (k == key || (!code.empty() && k == code)) return true;
    return false;
  }
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
  bool jsThreadMode_ = false, jsLockstep_ = false;
  JsThread* jsThread_ = nullptr;  // js, in JS-thread mode
  int syncDepth_ = 0;
  uint64_t lagBatches_ = 0;  // jsStats.batches at the last lag check
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
  // Video status reports.
  void deliverVideoEvents();
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
