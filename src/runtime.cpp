#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

#include "animated/native_animated.h"
#include "dev_overlay.h"
#include "dev_session.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"
#include "input/input.h"
#include "js/bytecode.h"
#include "js/engine.h"
#include "render/image_cache.h"
#include "render/lottie.h"
#include "render/shader_view.h"
#include "render/image_node.h"
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

namespace {
Document& mainDocument(std::vector<std::unique_ptr<Surface>>& surfaces, Runtime& rt, uint32_t* nextNodeId) {
  surfaces.push_back(std::make_unique<Surface>(rt, nextNodeId));
  return surfaces.back()->doc;
}
}  // namespace

Runtime::Runtime(const JsuiConfig& cfg, JsuiRuntime* h)
    : config(cfg), handle(h), loader(h, cfg.load, cfg.load_user), doc(mainDocument(surfaces_, *this, &nextNodeId_)) {
  if (cfg.base_url) baseUrl = cfg.base_url;
  images = std::make_unique<ImageCache>(*this);
  lotties = std::make_unique<LottieCache>(*this);
  shaders = std::make_unique<ShaderEffects>(*this);
  audio = std::make_unique<Audio>(*this);
  animated = std::make_unique<NativeAnimated>(doc);
  animated->findView = [this](uint32_t id) { return findNode(id); };
  animated->buffers = &buffers;
  // JS views over storage that is about to be freed are detached first.
  buffers.onRelease = [this](const std::string& key) {
    if (js) js->releaseBuffer(key);
  };
  // Animated.hostBuffer nodes follow their buffer (also in jsui_tick_native).
  buffers.onChange = [this](const std::string& key) { animated->bufferChanged(key); };
  textures.log = [this](JsuiLogLevel level, const std::string& msg) { log(level, msg); };
  main().handle = h;
  attach(main());
  input = main().input.get();
  doc.setViewport(float(cfg.width), float(cfg.height), cfg.pixel_ratio > 0 ? cfg.pixel_ratio : 1.f);
  if (cfg.dev_mode) session = std::make_unique<DevSession>(*this);
  jsKind_ = JsEngine::nextKind;
  resetEngine();
}

Runtime::~Runtime() {
  audio->contextEnded();
  audio.reset();
  loader.cancelAll();
  std::vector<uint64_t> calls = js ? js->pendingCalls() : std::vector<uint64_t>();
  js.reset();
  endCalls(calls, "runtime destroyed");
  deliverUiCallResults();
  doomed_.clear();
  for (auto& s : surfaces_) {
    s->doc.clear();
    s->doc.onNodeDestroyed = nullptr;
    s->doc.onLayout = nullptr;
  }
}

void Runtime::attach(Surface& s) {
  Document& d = s.doc;
  d.fonts = &fonts;
  d.images = images.get();
  d.textures = &textures;
  d.lotties = lotties.get();
  d.shaders = shaders.get();
  s.input = std::make_unique<InputManager>(*this, d, s.cursor);
  InputManager* in = s.input.get();
  d.onNodeDestroyed = [this, in](Node* n) {
    in->nodeRemoved(n);
    animated->viewDestroyed(n->id());
  };
  d.onLayout = [this](Node* n, const Rect& r) {
    if (js) js->layoutEvent(n->id(), r.x, r.y, r.width, r.height);
  };
}

// ------------------------------------------------------------ surfaces

namespace {
bool validProps(const std::string& json, std::string* error) {
  Json v;
  if (!Json::parse(json, &v, error)) return false;
  if (!v.isObject()) {
    *error = "not an object";
    return false;
  }
  return true;
}
}  // namespace

Surface* Runtime::createSurface(const std::string& appKey, const std::string& propsJson, float width, float height,
                                float pixelRatio) {
  std::string props = propsJson.empty() ? "{}" : propsJson;
  std::string error;
  if (appKey.empty()) {
    log(JSUI_LOG_ERROR, "jsui_surface_create: app_key is required");
    return nullptr;
  }
  if (!validProps(props, &error)) {
    log(JSUI_LOG_ERROR, "jsui_surface_create(\"" + appKey + "\"): invalid initial props (" + error + ")");
    return nullptr;
  }
  surfaces_.push_back(std::make_unique<Surface>(*this, &nextNodeId_, &doc.perf));
  Surface& s = *surfaces_.back();
  attach(s);
  s.doc.now = now;
  s.doc.frame = doc.frame;
  s.doc.setViewport(width, height, pixelRatio > 0 ? pixelRatio : 1.f);
  s.appKey = appKey;
  s.propsJson = props;
  s.pending = true;
  return &s;
}

void Runtime::destroySurface(Surface* s) {
  if (!s || s == &main()) return;
  auto it = std::find_if(surfaces_.begin(), surfaces_.end(), [s](const auto& p) { return p.get() == s; });
  if (it == surfaces_.end()) return;
  // Unmount first, while JS can still reach the surface's nodes.
  if (js && bundleRan_) {
    Value arg(double(s->tag()));
    js->callGlobal("__jsui_stopSurface", &arg, 1);
  }
  it = std::find_if(surfaces_.begin(), surfaces_.end(), [s](const auto& p) { return p.get() == s; });
  if (it == surfaces_.end()) return;
  std::unique_ptr<Surface> owned = std::move(*it);
  surfaces_.erase(it);
  if (js && js->inCall()) doomed_.push_back(std::move(owned));
}

bool Runtime::runSurface(Surface* s, const std::string& appKey, const std::string& propsJson) {
  if (!s) return false;
  std::string props = propsJson.empty() ? "{}" : propsJson;
  std::string error;
  if (!validProps(props, &error)) {
    log(JSUI_LOG_ERROR, "jsui_surface_run: invalid props (" + error + ")");
    return false;
  }
  if (appKey.empty() && s->appKey.empty()) {
    log(JSUI_LOG_ERROR, "jsui_surface_run: no app key");
    return false;
  }
  // Hosts often push their state every frame: unchanged props cost nothing.
  if ((appKey.empty() || appKey == s->appKey) && props == s->propsJson) return true;
  if (!appKey.empty()) s->appKey = appKey;
  s->propsJson = std::move(props);
  s->pending = true;
  return true;
}

void Runtime::startSurfaces() { flushSurfaces(true); }

void Runtime::flushSurfaces(bool all) {
  if (!js) return;
  if (!bundleRan_) {
    for (auto& s : surfaces_) s->resizePending = false;  // the bundle reads the sizes when it runs
    return;
  }
  Value::Array batch;
  std::vector<Surface*> resized;
  for (auto& s : surfaces_) {
    if (s->resizePending) resized.push_back(s.get());
    if (!(s->pending || all) || s->appKey.empty()) continue;
    s->pending = false;
    batch.emplace_back(Value::Array{Value(double(s->tag())), Value(s->appKey), Value(s->propsJson)});
  }
  if (batch.empty() && resized.empty()) return;
  // One microtask drain for everything: React commits the resizes and the
  // new props together.
  js->beginBatch();
  for (Surface* s : resized) s->dispatchResize();
  if (!batch.empty()) {
    Value arg(std::move(batch));
    if (!js->callGlobal("__jsui_runSurfaces", &arg, 1))
      log(JSUI_LOG_WARN, "surfaces: the bundle has no AppRegistry (import AppRegistry from 'react-native')");
  }
  js->endBatch();
}

Surface* Runtime::surfaceByTag(uint32_t tag) const {
  for (const auto& s : surfaces_)
    if (s->tag() == tag) return s.get();
  return nullptr;
}

Surface& Runtime::surfaceOf(const Document* d) const {
  for (const auto& s : surfaces_)
    if (&s->doc == d) return *s;
  return main();
}

Node* Runtime::findNode(uint32_t id) const {
  for (const auto& s : surfaces_)
    if (Node* n = s->doc.get(id)) return n;
  return nullptr;
}

void Runtime::redrawAll() {
  for (auto& s : surfaces_) s->requestRedraw();
}

void Runtime::log(JsuiLogLevel level, const std::string& msg) {
  if (session && session->joined()) session->uiEvent("log", {{"level", Value(int(level))}, {"text", Value(msg)}});
  if (config.log) {
    config.log(config.log_user, level, msg.c_str());
    return;
  }
  static const char* names[] = {"debug", "info", "warn", "error"};
  std::fprintf(stderr, "[jsui:%s] %s\n", names[level & 3], msg.c_str());
}

void Runtime::reportError(const std::string& msg) {
  lastError = msg;
  if (session && session->joined()) session->uiEvent("error", {{"text", Value(msg)}});
  log(JSUI_LOG_ERROR, msg);
  if (config.dev_mode) redrawAll();  // show the overlay
}

void Runtime::resetEngine() {
  std::vector<uint64_t> calls = js ? js->pendingCalls() : std::vector<uint64_t>();
  audio->contextEnded();
  js.reset();
  bundleRan_ = false;
  animated->clear();
  for (auto& s : surfaces_) s->doc.clear();
  // Shared state outlives the context (the new bundle reads it as it starts).
  js = JsEngine::create(*this, jsKind_);
  redrawAll();
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
  const std::string* json = state.json(key);
  if (session && session->joined()) session->uiEvent("state", {{"key", Value(key)}, {"value", json ? Value::parseJson(*json) : Value()}});
  if (!config.on_state) return;
  config.on_state(config.state_user, key.c_str(), json ? json->c_str() : nullptr);
}

void Runtime::deliverStateChanges() {
  if (!state.hasDirty()) return;
  std::vector<std::string> keys = state.takeDirty();
  js->stateChanged(keys);
}

void Runtime::deliverBufferChanges() {
  if (!buffers.hasDirty()) return;
  std::vector<std::string> keys = buffers.takeDirty();
  js->buffersChanged(keys);
}

void Runtime::deliverAudioEvents() {
  if (!audio->hasEvents()) return;
  Value::Array list;
  for (auto& e : audio->takeEvents()) {
    if (e.loaded)
      list.emplace_back(Value::Array{Value(1), Value(e.url), Value(e.status), Value(e.duration)});
    else
      list.emplace_back(Value::Array{Value(0), Value(double(e.voice)), Value(e.status)});
  }
  Value arg(std::move(list));
  js->callGlobal("__jsui_onAudio", &arg, 1);
}

void Runtime::deliverTextureChanges() {
  if (!textures.hasDirty()) return;
  std::vector<std::string> keys = textures.takeDirty();
  js->texturesChanged(keys);
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
  // A precompiled bundle for another QuickJS whose source is a separate file
  // (`jsui build --format split`): run that file instead.
  BundleParts parts;
  if (parseBundleContainer(source, parts) && (parts.key != bytecodeKey() || !js->runsBytecode()) && parts.source.empty() &&
      !parts.sourceUrl.empty()) {
    std::string url = ResourceLoader::resolve(name, std::string(parts.sourceUrl));
    log(JSUI_LOG_WARN, name + ": bytecode was compiled for another JSUI runtime (" + std::string(parts.key) +
                           "), loading its source " + url + " instead; rebuild it with this version's jsui CLI");
    loadUrl(url);
    return true;
  }
  bundleSource = source;
  bundleName = name;
  if (baseUrl.empty()) baseUrl = name;
  lastError.clear();
  redrawAll();
  // The bundle reads the current shared state as it starts; earlier changes
  // need no delivery.
  state.takeDirty();
  buffers.takeDirty();
  textures.takeDirty();
  bool ok = js->eval(source, name);
  // Surfaces run their apps once the bundle registered its components (a
  // component registered later starts its surface then, see AppRegistry).
  bundleRan_ = true;
  startSurfaces();
  return ok;
}

void Runtime::loadUrl(const std::string& url) {
  bundleUrl = ResourceLoader::resolve(baseUrl, url);
  if (baseUrl.empty() || !config.base_url) baseUrl = bundleUrl;
  if (config.dev_mode) devOrigin_ = httpOrigin(bundleUrl);
  loader.load(bundleUrl, [this](Resource res) {
    if (!res.ok()) {
      // A dev server answers 503 with the build error.
      std::string body = res.status >= 400 ? trimmed(res.text()) : std::string();
      if (body.size() > 4000) body = body.substr(0, 4000) + "\n...";
      reportError("failed to load bundle " + res.url + " (status " + std::to_string(res.status) +
                  (res.error.empty() ? "" : ": " + res.error) + ")" + (body.empty() ? "" : "\n" + body));
      return;
    }
    // A dev server may stamp the bundle with its version so a change between
    // this response and the first poll is not missed.
    bool session = false;
    for (auto& [k, v] : res.headers) {
      if (lower(k) == "x-jsui-version") devVersion_ = trimmed(v);
      if (lower(k) == "x-jsui-session") session = true;
    }
    sessionOffered_ = session;
    if (this->session) this->session->setOffered(session);
    loadSource(res.text(), bundleUrl);
  });
}

void Runtime::requestReload() {
  reloadRequested_ = true;
  redrawAll();
}

static void invalidateText(Node* n) {
  if (n->type() == NodeType::Text || n->type() == NodeType::TextInput) n->markDirty();
  for (Node* c : n->children()) invalidateText(c);
}

void Runtime::fontsChanged() {
  for (auto& s : surfaces_) {
    if (s->doc.root()) invalidateText(s->doc.root());
    s->doc.markLayoutDirty();
  }
}

void Runtime::reload() {
  if (js && js->inCall()) {  // called from JS or from a callback JS triggered
    requestReload();
    return;
  }
  reloadRequested_ = false;
  if (config.dev_mode) {
    // Keep the current frames on screen until the new bundle renders, and
    // keep showing them if the new bundle throws.
    for (auto& s : surfaces_) s->snapshotFrame();
  }
  loader.cancelAll();
  devPollInFlight_ = false;
  if (session) session->reset();
  images->clear();  // its in-flight loads were just cancelled
  lotties->clear();
  lastError.clear();
  resetEngine();
  if (!bundleUrl.empty()) loadUrl(bundleUrl);
  else if (!bundleSource.empty()) loadSource(bundleSource, bundleName);
}

void Runtime::devUpdate() {
  if (js && !reloadRequested_ && bundleRan_) {
    Value token(devVersion_);
    if (js->callGlobal("__jsui_onDevUpdate", &token, 1)) return;
  }
  reload();
}

bool Runtime::devKey(const char* key, bool down) {
  if (!config.dev_mode || !key || !down || lastError.empty()) return false;
  if (std::strcmp(key, "Escape") != 0) return false;
  lastError.clear();
  redrawAll();
  return true;
}

// Polls <origin>/__jsui/version (an opaque token; any change means the
// bundle changed). The dev server answers immediately; we poll every 500 ms,
// backing off to 2 s while the server is unreachable. `hmr=1`: a failed
// build changes the token too, so a hot-updating bundle can show the error.
void Runtime::devPoll() {
  if (!config.dev_mode || devOrigin_.empty() || devPollInFlight_ || now < devNextPoll_) return;
  devPollInFlight_ = true;
  LoadRequest req;
  req.url = devOrigin_ + "/__jsui/version?hmr=1";
  req.timeoutMs = 5000;
  req.headers.emplace_back("Cache-Control", "no-cache");
  loader.load(std::move(req), [this](Resource res) {
    devPollInFlight_ = false;
    if (!res.ok()) {
      devNextPoll_ = now + 2000;
      return;
    }
    devNextPoll_ = now + 500;
    if (session) {
      bool offered = false;
      for (auto& [k, v] : res.headers)
        if (lower(k) == "x-jsui-session") offered = true;
      // Only on a change: a 404 to the session poll holds until the server restarts.
      if (offered != sessionOffered_) session->setOffered(sessionOffered_ = offered);
    }
    std::string token = trimmed(res.text());
    if (devVersion_.empty()) {
      devVersion_ = token;
    } else if (token != devVersion_) {
      devVersion_ = token;
      log(JSUI_LOG_INFO, "bundle changed (version " + token + ")");
      devUpdate();
    }
  });
}

void Runtime::tick(double nowMs) {
  now = nowMs;
  doomed_.clear();
  for (auto& s : surfaces_) s->doc.now = nowMs;
  if (reloadRequested_) reload();
  for (auto& s : surfaces_) {
    s->doc.frame++;
    s->input->setEventTime(std::numeric_limits<double>::quiet_NaN());  // jsui_set_event_time lasts until a tick
  }
  loader.pump();
  // Shared state the embedder changed since the last tick: subscribers
  // re-render in one batch before this tick's timers and frame callbacks.
  flushSurfaces();
  deliverStateChanges();
  deliverBufferChanges();
  deliverTextureChanges();
  audio->update();
  deliverAudioEvents();
  js->runTimers(nowMs);
  js->runAnimationFrame(nowMs);
  js->runJobs();
  // Surfaces created or destroyed by callbacks below are picked up next tick.
  std::vector<Surface*> list;
  for (auto& s : surfaces_) list.push_back(s.get());
  auto live = [this](Surface* s) {
    for (auto& p : surfaces_)
      if (p.get() == s) return true;
    return false;
  };
  for (Surface* s : list)
    if (live(s)) s->input->tick(nowMs);
  // After this tick's timers and jobs (React's passive effects of the commit
  // that mounted an image have run when its onLoad arrives).
  for (Surface* s : list)
    if (live(s)) deliverImageEvents(s->doc);
  // Native-driven Animated: evaluated on the frame clock after JS and
  // scroll physics of this tick (Animated.event mappings saw the offsets).
  animated->tick(nowMs);
  deliverAnimatedEvents();
  // LottieViews advance on the same clock (and a loaded source sizes its
  // views before this tick's layout).
  lotties->tick(nowMs);
  deliverLottieEvents();
  // ShaderViews whose shader reads `time` repaint on the same clock.
  shaders->tick(nowMs);
  deliverShaderEvents();
  for (auto& s : surfaces_) {
    s->doc.layout();
    s->doc.tickLayoutAnimations(nowMs);
    // A LayoutAnimation applies to the commit that armed it only.
    s->doc.disarmLayoutAnimation();
  }
  devPoll();
  // jsui_call_ui results settled during this tick (or since the last one).
  deliverUiCallResults();
  // Shared-session commands run last, on the frame this tick produced.
  if (session) session->tick(devOrigin_);
}

void Runtime::tickNative(double nowMs) {
  now = nowMs;
  for (auto& s : surfaces_) s->doc.now = nowMs;
  animated->tick(nowMs);  // end events stay queued until the next tick()
  lotties->tick(nowMs);   // so do LottieView events
  shaders->tick(nowMs);   // and ShaderView errors
  for (auto& s : surfaces_) {
    s->doc.layout();
    s->doc.tickLayoutAnimations(nowMs);
  }
}

void Runtime::deliverAnimatedEvents() {
  // End callbacks may start animations that end at once (a zero-duration
  // timing in a sequence, an Animated.loop restarting it): those ends are
  // delivered in this tick too, a few rounds deep, so a chain like
  // loop(sequence([timing, timing({duration: 0})])) does not lose a frame
  // per step (the JS driver runs such chains synchronously).
  for (int round = 0; round < 8 && js && !(animated->ends.empty() && animated->updates.empty()); round++) {
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
}

namespace {
bool listensFor(const Node* n, const char* token) {
  for (const Value& e : n->props()["__events"].asArray())
    if (e.asString() == token) return true;
  return false;
}
}  // namespace

void Runtime::deliverLottieEvents() {
  if (!js || lotties->events.empty()) return;
  std::vector<LottieCache::Event> events = std::move(lotties->events);
  lotties->events.clear();
  for (const LottieCache::Event& e : events) {
    Node* n = findNode(e.node);
    if (!n || n->type() != NodeType::Lottie) continue;
    const char* type = e.kind == LottieCache::Event::Kind::Loaded   ? "load"
                       : e.kind == LottieCache::Event::Kind::Failed ? "error"
                                                                    : "animationfinish";
    if (!listensFor(n, e.kind == LottieCache::Event::Kind::Finished ? "animationFinish" : type)) continue;
    Value::Object ev{{"type", Value(type)}, {"target", Value(double(e.node))}};
    if (e.kind == LottieCache::Event::Kind::Failed) ev.emplace("error", Value(e.error));
    if (e.kind == LottieCache::Event::Kind::Finished) ev.emplace("isCancelled", Value(e.cancelled));
    js->dispatchEvent(Value(std::move(ev)));
  }
}

void Runtime::deliverShaderEvents() {
  if (!js || shaders->events.empty()) return;
  std::vector<ShaderEffects::Event> events = std::move(shaders->events);
  shaders->events.clear();
  for (const ShaderEffects::Event& e : events) {
    Node* n = findNode(e.node);
    if (!n || n->type() != NodeType::Shader || !listensFor(n, "error")) continue;
    js->dispatchEvent(Value(Value::Object{{"type", Value("error")}, {"target", Value(double(e.node))}, {"error", Value(e.error)}}));
  }
}

bool Runtime::imageEventsReady(const Document& doc) const {
  for (const auto& e : doc.imageEvents) {
    if (e.frame >= doc.frame) return true;  // waits for the next tick at most
    Node* n = doc.get(e.node);
    if (!n || n->type() != NodeType::Image || image::settled(n)) return true;
  }
  return false;
}

void Runtime::deliverImageEvents(Document& doc) {
  if (!js || doc.imageEvents.empty()) return;
  std::vector<Document::ImageEvent> pending = std::move(doc.imageEvents);
  doc.imageEvents.clear();
  std::vector<Document::ImageEvent> keep;
  for (size_t i = 0; i < pending.size(); i++) {
    const Document::ImageEvent e = pending[i];
    // Handlers below may change sources: a newer entry for the node wins.
    bool superseded = false;
    for (const auto& q : doc.imageEvents) superseded = superseded || q.node == e.node;
    if (superseded) continue;
    Node* n = doc.get(e.node);
    if (!n || n->type() != NodeType::Image) continue;
    if (e.frame >= doc.frame) {  // source set during this tick
      keep.push_back(e);
      continue;
    }
    image::NodeCache* c = image::ensure(n);
    if (c->url.empty()) continue;
    ImageCache::Info info;
    if (!images->peek(c->url, &info) || info.state == ImageCache::State::Loading) {
      // Still loading (or its request was dropped: ask again).
      if (!images->peek(c->url, &info)) images->get(c->url);
      keep.push_back(e);
      continue;
    }
    double id = double(e.node);
    if (info.state == ImageCache::State::Ready && info.image) {
      if (!e.cached && info.bytes > 0 && listensFor(n, "progress")) {
        Value::Object ev{{"type", Value("progress")},
                         {"target", Value(id)},
                         {"loaded", Value(double(info.bytes))},
                         {"total", Value(double(info.bytes))}};
        js->dispatchEvent(Value(std::move(ev)));
      }
      if (listensFor(n, "load")) {
        Value::Object source{{"uri", Value(c->url)},
                             {"width", Value(double(info.image->width()))},
                             {"height", Value(double(info.image->height()))}};
        Value::Object ev{{"type", Value("load")}, {"target", Value(id)}, {"source", Value(std::move(source))}};
        js->dispatchEvent(Value(std::move(ev)));
      }
    } else if (listensFor(n, "error")) {
      Value::Object ev{{"type", Value("error")}, {"target", Value(id)}, {"error", Value(info.error)}};
      js->dispatchEvent(Value(std::move(ev)));
    }
  }
  // Entries queued by the handlers stay behind the ones still waiting.
  for (auto& q : doc.imageEvents) keep.push_back(q);
  doc.imageEvents = std::move(keep);
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

bool Runtime::needsRedraw() const {
  for (const auto& s : surfaces_)
    if (s->needsRedraw()) return true;
  return false;
}

bool Runtime::wantsFrame() const {
  // A pending repaint alone is not a reason to tick: the host paints after
  // the tick it already ran.
  if (reloadRequested_ || loader.pending() > 0 || (js && js->hasPendingWork(now + 17)) || animated->animating() ||
      !animated->ends.empty() || lotties->animating() || shaders->animating() || state.hasDirty() || buffers.hasDirty() || textures.hasDirty() || audio->hasEvents() || audio->busy() || !uiResults_.empty() || !doomed_.empty())
    return true;
  for (const auto& s : surfaces_)
    if (s->input->animating() || s->doc.layoutAnimating() || imageEventsReady(s->doc) ||
        (bundleRan_ && (s->pending || s->resizePending)))
      return true;
  return false;
}

bool Runtime::animatingFrames(const Surface& s) const {
  return s.input->animating() || s.doc.layoutAnimating() || animated->animating() || !animated->ends.empty() ||
         lotties->animating() || shaders->animating() ||
         (js && js->hasPendingWork(now + 17));
}

}  // namespace jsui
