// The object behind the JsuiRuntime C handle.
#pragma once

#include <memory>
#include <string>

#include "core/document.h"
#include "jsui/jsui.h"
#include "platform/loader.h"
#include "text/font_manager.h"

namespace jsui {

class JsEngine;
class InputManager;
class ImageCache;

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

  void tick(double nowMs);
  bool needsRedraw() const;
  void render(void* pixels, int width, int height, size_t rowBytes);

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
  void resetEngine();
  bool redraw_ = true;
};

}  // namespace jsui
