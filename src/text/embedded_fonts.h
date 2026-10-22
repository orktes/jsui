// Fonts compiled into the library (generated from assets/fonts by
// cmake/EmbedFonts.cmake) and registered by every FontManager.
#pragma once

#include <cstddef>

namespace jsui {

struct EmbeddedFont {
  const char* family;
  int weight;
  bool italic;
  const unsigned char* data;
  size_t size;
};

extern const EmbeddedFont kEmbeddedFonts[];
extern const size_t kEmbeddedFontCount;

}  // namespace jsui
