// Font registry shared by text measurement and painting.
#pragma once

#include <memory>
#include <string>

#include "include/core/SkRefCnt.h"

class SkFontMgr;
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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jsui
