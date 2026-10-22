#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "animated/native_animated.h"
#include "dev_overlay.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"
#include "input/input.h"
#include "js/engine.h"
#include "render/image_cache.h"
#include "render/painter.h"

namespace jsui {

namespace {

// "http://host:port/a/b.js?x" -> "http://host:port"; empty if not http.
std::string httpOrigin(const std::string& url) {
  if (url.rfind("http://", 0) != 0) return {};
  auto slash = url.find_first_of("/?#", 7);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

}  // namespace

Runtime::Runtime(const JsuiConfig& cfg, JsuiRuntime* h)
    : config(cfg), handle(h), loader(h, cfg.load, cfg.load_user) {
  if (cfg.base_url) baseUrl = cfg.base_url;
  doc.fonts = &fonts;
  images = std::make_unique<ImageCache>(*this);
  doc.images = images.get();
  input = std::make_unique<InputManager>(*this);
  animated = std::make_unique<NativeAnimated>(doc);
  doc.onNodeDestroyed = [this](Node* n) {
    input->nodeRemoved(n);
    animated->viewDestroyed(n->id());
  };
  doc.onLayout = [this](Node* n, const Rect& r) {
    if (js) js->layoutEvent(n->id(), r.x, r.y, r.width, r.height);
  };
  doc.setViewport(float(cfg.width), float(cfg.height), cfg.pixel_ratio > 0 ? cfg.pixel_ratio : 1.f);
  resetEngine();
}

Runtime::~Runtime() {
  loader.cancelAll();
  std::vector<uint64_t> calls = js ? js->pendingCalls() : std::vector<uint64_t>();
  js.reset();
  endCalls(calls, "runtime destroyed");
  deliverUiCallResults();
  doc.clear();
  doc.onNodeDestroyed = nullptr;
  doc.onLayout = nullptr;
}

void Runtime::log(JsuiLogLevel level, const std::string& msg) {
  if (config.log) {
    config.log(config.log_user, level, msg.c_str());
    return;
  }
  static const char* names[] = {"debug", "info", "warn", "error"};
  std::fprintf(stderr, "[jsui:%s] %s\n", names[level & 3], msg.c_str());
}

void Runtime::reportError(const std::string& msg) {
  lastError = msg;
  log(JSUI_LOG_ERROR, msg);
  if (config.dev_mode) redraw_ = true;  // show the overlay
}

void Runtime::resetEngine() {
  std::vector<uint64_t> calls = js ? js->pendingCalls() : std::vector<uint64_t>();
  js.reset();
  animated->clear();
  doc.clear();
  // Shared state outlives the context (the new bundle reads it as it starts).
  js = std::make_unique<JsEngine>(*this);
  redraw_ = true;
  endCalls(calls, "the UI was reloaded");
}

// ------------------------------------------------------------ game data

int Runtime::setState(const char* key, const char* json, bool patch) {
  if (!key) {
    log(JSUI_LOG_ERROR, patch ? "jsui_patch_state: key is NULL" : "jsui_set_state: key is NULL");
    return -1;
  }
  if (!json) return state.remove(key) ? 1 : 0;
  std::string error;
  HostState::Result r = patch ? state.patch(key, json, &error) : state.set(key, json, &error);
  if (r == HostState::Result::Invalid) {
    log(JSUI_LOG_ERROR, std::string(patch ? "jsui_patch_state" : "jsui_set_state") + "(\"" + key +
                            "\"): invalid JSON (" + error + ")");
    return -1;
  }
  return r == HostState::Result::Changed ? 1 : 0;
}

void Runtime::uiStateChanged(const std::string& key) {
  if (!config.on_state) return;
  const std::string* json = state.json(key);
  config.on_state(config.state_user, key.c_str(), json ? json->c_str() : nullptr);
}

void Runtime::deliverStateChanges() {
  if (!state.hasDirty()) return;
  std::vector<std::string> keys = state.takeDirty();
  js->stateChanged(keys);
}

int Runtime::resolveCall(uint64_t id, const char* json) {
  if (!js || !js->hasCall(id)) return -1;
  std::string text = json ? json : "null";
  std::string error;
  if (!Json::parse(text, nullptr, &error)) {
    std::string msg = "jsui_resolve_call: invalid JSON result (" + error + ")";
    log(JSUI_LOG_ERROR, msg);
    js->settleCall(id, false, msg);
    return -2;
  }
  js->settleCall(id, true, text);
  return 0;
}

int Runtime::rejectCall(uint64_t id, const char* message) {
  if (!js || !js->hasCall(id)) return -1;
  js->settleCall(id, false, message ? message : "rejected");
  return 0;
}

void Runtime::hostCallCancelled(uint64_t id) {
  if (config.on_call_cancel) config.on_call_cancel(config.call_user, id);
}

uint64_t Runtime::callUi(const char* name, const char* argsJson, JsuiCallResultFn done, void* user) {
  uint64_t id = newCallId();
  uiCalls_[id] = {done, user};
  std::string n = name ? name : "";
  if (!js) {  // a done callback started a call while the runtime is destroyed
    uiCallSettled(id, false, "runtime destroyed");
    return id;
  }
  std::string args = argsJson ? argsJson : "null";
  std::string error;
  if (!Json::parse(args, nullptr, &error)) {
    uiCallSettled(id, false, "jsui_call_ui(\"" + n + "\"): invalid JSON arguments (" + error + ")");
    return id;
  }
  Value argv[3] = {Value(double(id)), Value(n), Value(args)};
  if (!js->callGlobal("__jsui_onCall", argv, 3))
    uiCallSettled(id, false, "no UI handler for \"" + n + "\" (the bundle has not registered Host.handle)");
  return id;
}

void Runtime::uiCallSettled(uint64_t id, bool ok, std::string payload) {
  // Ignores ids that are not pending (settled twice, or from an old context).
  auto it = uiCalls_.find(id);
  if (it == uiCalls_.end()) return;
  for (auto& r : uiResults_)
    if (r.id == id) return;
  uiResults_.push_back({id, ok, std::move(payload)});
}

void Runtime::deliverUiCallResults() {
  // Callbacks may start new calls; those settle into a fresh list.
  for (int round = 0; round < 8 && !uiResults_.empty(); round++) {
    auto results = std::move(uiResults_);
    uiResults_.clear();
    for (auto& r : results) {
      auto it = uiCalls_.find(r.id);
      if (it == uiCalls_.end()) continue;
      UiCall c = it->second;
      uiCalls_.erase(it);
      if (c.done) c.done(c.user, r.id, r.ok ? 1 : 0, r.payload.c_str());
    }
  }
}

void Runtime::endCalls(const std::vector<uint64_t>& hostCalls, const char* reason) {
  for (uint64_t id : hostCalls) hostCallCancelled(id);
  // Results that already settled keep their outcome; the rest fail.
  std::vector<uint64_t> open;
  for (auto& [id, c] : uiCalls_) {
    bool settled = false;
    for (auto& r : uiResults_) settled = settled || r.id == id;
    if (!settled) open.push_back(id);
  }
  std::sort(open.begin(), open.end());
  for (uint64_t id : open) uiResults_.push_back({id, false, reason});
  deliverUiCallResults();
}

bool Runtime::loadSource(const std::string& source, const std::string& name) {
  bundleSource = source;
  bundleName = name;
  if (baseUrl.empty()) baseUrl = name;
  lastError.clear();
  redraw_ = true;
  // The bundle reads the current shared state as it starts; earlier changes
  // need no delivery.
  state.takeDirty();
  return js->eval(source, name);
}

void Runtime::loadUrl(const std::string& url) {
  bundleUrl = ResourceLoader::resolve(baseUrl, url);
  if (baseUrl.empty() || !config.base_url) baseUrl = bundleUrl;
  if (config.dev_mode) devOrigin_ = httpOrigin(bundleUrl);
  loader.load(bundleUrl, [this](Resource res) {
    if (!res.ok()) {
      reportError("failed to load bundle " + res.url + " (status " + std::to_string(res.status) +
                  (res.error.empty() ? "" : ": " + res.error) + ")");
      return;
    }
    // A dev server may stamp the bundle with its version so a change between
    // this response and the first poll is not missed.
    for (auto& [k, v] : res.headers) {
      if (lower(k) == "x-jsui-version") devVersion_ = trimmed(v);
    }
    loadSource(res.text(), bundleUrl);
  });
}

void Runtime::requestReload() {
  reloadRequested_ = true;
  redraw_ = true;
}

void Runtime::reload() {
  if (js && js->inCall()) {  // called from JS or from a callback JS triggered
    requestReload();
    return;
  }
  reloadRequested_ = false;
  if (config.dev_mode && lastError.empty() && !doc.root()->children().empty()) {
    // Keep the current frame on screen until the new bundle renders, and
    // keep showing it if the new bundle throws.
    doc.layout();
    float pr = doc.pixelRatio();
    SkPictureRecorder rec;
    paintDocument(doc, rec.beginRecording(SkRect::MakeWH(doc.width() * pr, doc.height() * pr)));
    lastGoodFrame_ = rec.finishRecordingAsPicture();
  }
  loader.cancelAll();
  devPollInFlight_ = false;
  images->clear();  // its in-flight loads were just cancelled
  lastError.clear();
  resetEngine();
  if (!bundleUrl.empty()) loadUrl(bundleUrl);
  else if (!bundleSource.empty()) loadSource(bundleSource, bundleName);
}

bool Runtime::devKey(const char* key, bool down) {
  if (!config.dev_mode || !key || !down || lastError.empty()) return false;
  if (std::strcmp(key, "Escape") != 0) return false;
  lastError.clear();
  redraw_ = true;
  return true;
}

// Polls <origin>/__jsui/version (an opaque token; any change means the
// bundle changed). The dev server answers immediately; we poll every 500 ms,
// backing off to 2 s while the server is unreachable.
void Runtime::devPoll() {
  if (!config.dev_mode || devOrigin_.empty() || devPollInFlight_ || now < devNextPoll_) return;
  devPollInFlight_ = true;
  LoadRequest req;
  req.url = devOrigin_ + "/__jsui/version";
  req.timeoutMs = 5000;
  req.headers.emplace_back("Cache-Control", "no-cache");
  loader.load(std::move(req), [this](Resource res) {
    devPollInFlight_ = false;
    if (!res.ok()) {
      devNextPoll_ = now + 2000;
      return;
    }
    devNextPoll_ = now + 500;
    std::string token = trimmed(res.text());
    if (devVersion_.empty()) {
      devVersion_ = token;
    } else if (token != devVersion_) {
      devVersion_ = token;
      log(JSUI_LOG_INFO, "bundle changed (version " + token + "), reloading");
      reload();
    }
  });
}

void Runtime::tick(double nowMs) {
  now = nowMs;
  doc.now = nowMs;
  if (reloadRequested_) reload();
  loader.pump();
  // Shared state the embedder changed since the last tick: subscribers
  // re-render in one batch before this tick's timers and frame callbacks.
  deliverStateChanges();
  js->runTimers(nowMs);
  js->runAnimationFrame(nowMs);
  js->runJobs();
  input->tick(nowMs);
  // Native-driven Animated: evaluated on the frame clock after JS and
  // scroll physics of this tick (Animated.event mappings saw the offsets).
  animated->tick(nowMs);
  deliverAnimatedEvents();
  doc.layout();
  doc.tickLayoutAnimations(nowMs);
  // A LayoutAnimation applies to the commit that armed it only.
  doc.disarmLayoutAnimation();
  devPoll();
  // jsui_call_ui results settled during this tick (or since the last one).
  deliverUiCallResults();
  if (doc.paintDirty()) redraw_ = true;
}

void Runtime::tickNative(double nowMs) {
  now = nowMs;
  doc.now = nowMs;
  animated->tick(nowMs);  // end events stay queued until the next tick()
  doc.layout();
  doc.tickLayoutAnimations(nowMs);
  if (doc.paintDirty()) redraw_ = true;
}

void Runtime::deliverAnimatedEvents() {
  if (!js || (animated->ends.empty() && animated->updates.empty())) return;
  // Value updates first (listeners see the final value), then end callbacks.
  auto updates = std::move(animated->updates);
  animated->updates.clear();
  for (auto& u : updates) {
    Value args[2] = {Value(u.tag), Value(u.value)};
    js->callGlobal("__jsui_onAnimatedValueUpdate", args, 2);
  }
  auto ends = std::move(animated->ends);
  animated->ends.clear();
  for (auto& e : ends) {
    Value args[3] = {Value(e.id), Value(e.finished), Value(e.value)};
    js->callGlobal("__jsui_onAnimatedEnd", args, 3);
  }
}

std::string Runtime::clipboardText() {
  if (clipboardGet) {
    const char* t = clipboardGet(clipboardUser);
    return t ? std::string(t) : std::string();
  }
  return clipboard;
}

void Runtime::setClipboardText(const std::string& text) {
  if (clipboardSet) clipboardSet(clipboardUser, text.c_str());
  else clipboard = text;
}

bool Runtime::needsRedraw() const { return redraw_ || doc.paintDirty(); }

bool Runtime::wantsFrame() const {
  // A pending repaint alone is not a reason to tick: the host paints after
  // the tick it already ran.
  return reloadRequested_ || loader.pending() > 0 || (js && js->hasPendingWork(now + 17)) || input->animating() ||
         doc.layoutAnimating() || animated->animating() || !animated->ends.empty() || state.hasDirty() ||
         !uiResults_.empty();
}

void Runtime::render(void* pixels, int width, int height, size_t rowBytes) {
  renderImpl(pixels, width, height, rowBytes, false);
}

SkIRect Runtime::renderIncremental(void* pixels, int width, int height, size_t rowBytes) {
  return renderImpl(pixels, width, height, rowBytes, true);
}

SkIRect Runtime::renderImpl(void* pixels, int width, int height, size_t rowBytes, bool incremental) {
  SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  SkSurfaceProps props = surfaceProps();
  auto surface = SkSurfaces::WrapPixels(info, pixels, rowBytes, &props);
  if (!surface) return SkIRect::MakeEmpty();
  doc.layout();
  // Nothing marked dirty and the buffer still holds our last frame: done,
  // without walking the tree.
  if (incremental && !doc.paintDirty() && pixels == lastTarget_ && width == lastTargetW_ &&
      height == lastTargetH_ && rowBytes == lastTargetRowBytes_ && lastError.empty()) {
    redraw_ = false;
    return SkIRect::MakeEmpty();
  }
  // Always refresh the paint records so the next incremental frame compares
  // against what is in the buffer.
  SkRegion damage = updateDamage(doc, width, height);
  bool dev = config.dev_mode != 0;
  bool frozen = dev && lastGoodFrame_ && (!lastError.empty() || doc.root()->children().empty());
  bool overlay = dev && !lastError.empty();
  bool partial = incremental && !frozen && !overlay && pixels == lastTarget_ && width == lastTargetW_ &&
                 height == lastTargetH_ && rowBytes == lastTargetRowBytes_;
  // Frames with the error overlay or a frozen picture are not tracked by the
  // records: the next frame repaints fully.
  bool tracked = !frozen && !overlay;
  lastTarget_ = tracked ? pixels : nullptr;
  lastTargetW_ = width;
  lastTargetH_ = height;
  lastTargetRowBytes_ = rowBytes;
  doc.clearPaintDirty();
  redraw_ = false;
  SkCanvas* canvas = surface->getCanvas();
  if (partial) {
    if (damage.isEmpty()) return SkIRect::MakeEmpty();
    canvas->save();
    canvas->clipRegion(damage);
    canvas->clear(SK_ColorTRANSPARENT);
    paintDocument(doc, canvas, &damage);
    canvas->restore();
    return damage.getBounds();
  }
  canvas->clear(SK_ColorTRANSPARENT);
  paintFrame(canvas, frozen, overlay);
  return SkIRect::MakeWH(width, height);
}

void Runtime::paintFrame(SkCanvas* canvas, bool frozen, bool overlay) {
  if (frozen) {
    canvas->drawPicture(lastGoodFrame_);
  } else {
    paintDocument(doc, canvas);
    if (lastGoodFrame_ && lastError.empty()) lastGoodFrame_.reset();
  }
  if (overlay) {
    if (!overlay_) overlay_ = std::make_unique<DevOverlay>(fonts);
    overlay_->paint(canvas, lastError, doc.width(), doc.height(), doc.pixelRatio());
  }
}

void Runtime::renderCanvas(SkCanvas* canvas, int width, int height, bool clear) {
  doc.layout();
  // Keep the paint records current (they are what the CPU incremental path
  // compares against), but nothing here is tracked: a later incremental CPU
  // render repaints fully.
  updateDamage(doc, width, height);
  bool dev = config.dev_mode != 0;
  bool frozen = dev && lastGoodFrame_ && (!lastError.empty() || doc.root()->children().empty());
  bool overlay = dev && !lastError.empty();
  lastTarget_ = nullptr;
  doc.clearPaintDirty();
  redraw_ = false;
  if (clear) canvas->clear(SK_ColorTRANSPARENT);
  paintFrame(canvas, frozen, overlay);
}

}  // namespace jsui
