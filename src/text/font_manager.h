// Font registry shared by text measurement and painting.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "include/core/SkRefCnt.h"

// Skia m157 (the WebGPU wasm build) made SkUnicode reference counted, moved
// the ICU factory to SkUnicodes::ICU and passes it to ParagraphBuilder::make.
#if __has_include("modules/skunicode/include/SkUnicode_icu.h")
#define JSUI_SKUNICODE_REFCNT 1
#endif

class SkFontMgr;
class SkUnicode;
namespace skia::textlayout {
class FontCollection;
}

namespace jsui {

class FontRegistry;

class FontManager {
 public:
  FontManager();
  ~FontManager();

  // Registers the faces in `data` (TTF, OTF, TTC / OTC, WOFF, WOFF2) under
  // `family`, like a CSS @font-face rule. Empty `family` = the family name in
  // the file. `weight` 1..1000 and `italic` 0/1 override what the file says
  // (weight <= 0 / italic < 0: from the file). A variable font with a 'wght'
  // axis and no weight override is registered once per 100 weight step of
  // its range, so fontWeight picks the matching instance. Registering the
  // same family + style again replaces that face. Returns the number of faces
  // registered (0 = not a font; `error` says why).
  int registerFont(const std::string& family, const void* data, size_t size, int weight, int italic,
                   std::string* error = nullptr);
  // Drops every face of `family` (case-insensitive). False if none.
  bool unregisterFamily(const std::string& family);

  // The collection used by skparagraph. Falls back to the bundled default
  // font, then to system fonts when available.
  sk_sp<skia::textlayout::FontCollection> collection();
  sk_sp<SkFontMgr> fontMgr();
  // The registered and embedded fonts alone (Lottie text layers).
  sk_sp<SkFontMgr> assetFontMgr();

  // Family used when a style names none (default: "Inter").
  const std::string& defaultFamily() const { return defaultFamily_; }
  void setDefaultFamily(const std::string& family);
  // Families tried, in order, for characters the styled fonts lack (emoji,
  // CJK, symbols), before any other registered font and the system fonts.
  const std::vector<std::string>& fallbackFamilies() const;
  void setFallbackFamilies(std::vector<std::string> families);

  // True if `family` was registered (embedded or via registerFont).
  bool hasFamily(const std::string& family) const;
  // Registered family names, in registration order (embedded first).
  std::vector<std::string> familyNames() const;
  // Bumped whenever the font set changes; text caches built with an older
  // generation are rebuilt (a late-loading font changes shaping).
  uint32_t generation = 0;

  // ICU-backed Unicode helpers (case mapping), created on first use.
  SkUnicode* unicode();
#if JSUI_SKUNICODE_REFCNT
  sk_sp<SkUnicode> unicodeRef();
#endif

 private:
  void changed();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string defaultFamily_ = "Inter";
};

}  // namespace jsui
