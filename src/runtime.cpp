#include "runtime.h"

#include <cctype>
#include <cstdio>
#include <cstring>

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
  doc.onNodeDestroyed = [this](Node* n) { input->nodeRemoved(n); };
  doc.onLayout = [this](Node* n, const Rect& r) {
    if (js) js->layoutEvent(n->id(), r.x, r.y, r.width, r.height);
  };
  doc.setViewport(float(cfg.width), float(cfg.height), cfg.pixel_ratio > 0 ? cfg.pixel_ratio : 1.f);
  resetEngine();
}

Runtime::~Runtime() {
  loader.cancelAll();
  js.reset();
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
  js.reset();
  doc.clear();
  js = std::make_unique<JsEngine>(*this);
  redraw_ = true;
}

bool Runtime::loadSource(const std::string& source, const std::string& name) {
  bundleSource = source;
  bundleName = name;
  if (baseUrl.empty()) baseUrl = name;
  lastError.clear();
  redraw_ = true;
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
  if (reloadRequested_) reload();
  loader.pump();
  js->runTimers(nowMs);
  js->runAnimationFrame(nowMs);
  js->runJobs();
  input->tick(nowMs);
  doc.layout();
  devPoll();
  if (doc.paintDirty()) redraw_ = true;
}

bool Runtime::needsRedraw() const { return redraw_ || doc.paintDirty(); }

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
  return SkIRect::MakeWH(width, height);
}

}  // namespace jsui
