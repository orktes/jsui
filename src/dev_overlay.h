// Dev-mode error overlay: a red-tinted panel with the error message and
// stack, drawn with Skia directly over the frame.
#pragma once

#include <string>

#include "include/core/SkRefCnt.h"

class SkCanvas;
class SkTypeface;

namespace jsui {

class FontManager;

class DevOverlay {
 public:
  explicit DevOverlay(FontManager& fonts);
  ~DevOverlay();

  // `width`/`height` are logical px; the canvas is in device px.
  void paint(SkCanvas* canvas, const std::string& message, float width, float height, float pixelRatio);

 private:
  void resolveFonts();
  FontManager& fonts_;
  bool resolved_ = false;
  sk_sp<SkTypeface> mono_, sans_;
};

}  // namespace jsui
