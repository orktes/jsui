#include "runtime.h"

#include <cstdio>
#include <cstring>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "input/input.h"
#include "js/engine.h"
#include "render/image_cache.h"
#include "render/painter.h"

namespace jsui {

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
  return js->eval(source, name);
}

void Runtime::loadUrl(const std::string& url) {
  bundleUrl = ResourceLoader::resolve(baseUrl, url);
  if (baseUrl.empty() || !config.base_url) baseUrl = bundleUrl;
  loader.load(bundleUrl, [this](Resource res) {
    if (!res.ok()) {
      reportError("failed to load bundle " + res.url + " (status " + std::to_string(res.status) + ")");
      return;
    }
    loadSource(res.text(), bundleUrl);
  });
}

void Runtime::reload() {
  loader.cancelAll();
  resetEngine();
  if (!bundleUrl.empty()) loadUrl(bundleUrl);
  else if (!bundleSource.empty()) loadSource(bundleSource, bundleName);
}

void Runtime::tick(double nowMs) {
  now = nowMs;
  loader.pump();
  js->runTimers(nowMs);
  js->runAnimationFrame(nowMs);
  js->runJobs();
  input->tick(nowMs);
  doc.layout();
  if (doc.paintDirty()) redraw_ = true;
}

bool Runtime::needsRedraw() const { return redraw_ || doc.paintDirty(); }

void Runtime::render(void* pixels, int width, int height, size_t rowBytes) {
  SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  auto surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
  if (!surface) return;
  doc.layout();
  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  paintDocument(doc, canvas);
  doc.clearPaintDirty();
  redraw_ = false;
}

}  // namespace jsui
