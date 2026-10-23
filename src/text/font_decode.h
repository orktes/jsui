// Web font containers (WOFF, WOFF2) -> plain sfnt (TTF / OTF) for Skia.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jsui {

enum class FontContainer { Unknown, Sfnt, Collection, Woff, Woff2 };

// What the first bytes say the data is (sfnt = TrueType / OpenType).
FontContainer sniffFont(const void* data, size_t size);

// Unpacks WOFF / WOFF2 into `out`. False (with `error` set) when the data is
// corrupt or the decoder is not compiled in (JSUI_WOFF / JSUI_WOFF2).
bool decodeWebFont(const void* data, size_t size, std::vector<uint8_t>* out, std::string* error);

// Number of faces in a TrueType / OpenType collection ('ttcf'), else 1.
int collectionFaceCount(const void* data, size_t size);

// Face `index` of a collection as a standalone sfnt in `out` (CoreText only
// loads the first face of collection data). False if the data is not a
// collection or the face's tables run past the end.
bool extractCollectionFace(const void* data, size_t size, int index, std::vector<uint8_t>* out);

}  // namespace jsui
