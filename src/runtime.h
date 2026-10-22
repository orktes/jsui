// The object behind the JsuiRuntime C handle.
#pragma once

#include <memory>
#include <string>

#include "core/document.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "jsui/jsui.h"
#include "platform/loader.h"
#include "text/font_manager.h"

class SkPicture;

namespace jsui {

class JsEngine;
class InputManager;
class ImageCache;
class DevOverlay;

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
  bool needsRedraw() const;
  void render(void* pixels, int width, int height, size_t rowBytes);
  // Like render(), but `pixels` must still hold the frame this runtime last
  // rendered into it: only the changed region is repainted. Returns the
  // repainted device rect (empty when nothing changed). A different buffer,
  // size or an intervening render() makes it a full repaint.
  SkIRect renderIncremental(void* pixels, int width, int height, size_t rowBytes);

  JsuiConfig config;
  JsuiRuntime* handle;
  std::string baseUrl;
  std::string bundleUrl, bundleSource, bundleName;
  std::string lastError;
  std::string cursor = "default";
  double now = 0;

  Document doc;
  FontManager fonts;
  ResourceLoader loader;
  std::unique_ptr<ImageCache> images;
  std::unique_ptr<JsEngine> js;
  std::unique_ptr<InputManager> input;

 private:
  SkIRect renderImpl(void* pixels, int width, int height, size_t rowBytes, bool incremental);
  void resetEngine();
  // The buffer the last frame was painted into (incremental repaint).
  const void* lastTarget_ = nullptr;
  int lastTargetW_ = 0, lastTargetH_ = 0;
  size_t lastTargetRowBytes_ = 0;
  void devPoll();
  bool redraw_ = true;

  // Dev mode: version polling of <origin>/__jsui/version and the frame shown
  // while a reloaded bundle is broken.
  std::string devOrigin_, devVersion_;
  bool devPollInFlight_ = false;
  double devNextPoll_ = 0;
  bool reloadRequested_ = false;
  sk_sp<SkPicture> lastGoodFrame_;
  std::unique_ptr<DevOverlay> overlay_;
};

}  // namespace jsui
