// Font registry shared by text measurement and painting.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

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

class FontManager {
 public:
  FontManager();
  ~FontManager();

  // Registers a face from memory under `family`. Weight/italic are taken from
  // the font file; the arguments are hints used when the file is ambiguous.
  bool registerFont(const std::string& family, const void* data, size_t size, int weight, bool italic);

  // The collection used by skparagraph. Falls back to the bundled default
  // font, then to system fonts when available.
  sk_sp<skia::textlayout::FontCollection> collection();
  sk_sp<SkFontMgr> fontMgr();

  // Family used when a style names none (default: "Inter").
  std::string defaultFamily = "Inter";

  // True if `family` was registered (embedded or via registerFont).
  bool hasFamily(const std::string& family) const;
  // Bumped whenever a font is registered; text caches built with an older
  // generation are rebuilt (a late-loading font changes shaping).
  uint32_t generation = 0;

  // ICU-backed Unicode helpers (case mapping), created on first use.
  SkUnicode* unicode();
#if JSUI_SKUNICODE_REFCNT
  sk_sp<SkUnicode> unicodeRef();
#endif

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jsui
