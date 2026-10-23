#include "text/font_decode.h"

#include <algorithm>
#include <cstring>

#if JSUI_WOFF
#include <zlib.h>  // jsui_zinflate: Z_PREFIX'd, so no clash with Skia's or the system zlib
#endif
#if JSUI_WOFF2
#include <woff2/decode.h>
#include <woff2/output.h>
#endif

namespace jsui {

namespace {

constexpr size_t kMaxFontSize = 64u << 20;  // refuse to inflate anything larger

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
void put16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

bool decodeWoff(const uint8_t* d, size_t size, std::vector<uint8_t>* out, std::string* error) {
#if JSUI_WOFF
  if (size < 44) return *error = "truncated WOFF header", false;
  uint32_t flavor = be32(d + 4);
  uint16_t numTables = be16(d + 12);
  uint32_t total = be32(d + 16);
  if (!numTables || size < 44 + size_t(numTables) * 20) return *error = "truncated WOFF table directory", false;
  if (total > kMaxFontSize) return *error = "WOFF font too large", false;
  // sfnt header + table records, then each table 4-byte aligned.
  size_t headerSize = 12 + size_t(numTables) * 16;
  out->assign(headerSize, 0);
  uint16_t entrySelector = 0;
  while ((2u << entrySelector) <= numTables) entrySelector++;
  uint16_t searchRange = uint16_t((1u << entrySelector) * 16);
  put32(out->data(), flavor);
  put16(out->data() + 4, numTables);
  put16(out->data() + 6, searchRange);
  put16(out->data() + 8, entrySelector);
  put16(out->data() + 10, uint16_t(numTables * 16 - searchRange));
  for (uint16_t i = 0; i < numTables; i++) {
    const uint8_t* e = d + 44 + size_t(i) * 20;
    uint32_t offset = be32(e + 4), compLength = be32(e + 8), origLength = be32(e + 12);
    if (size_t(offset) + compLength > size || compLength > origLength || origLength > kMaxFontSize ||
        out->size() + origLength > kMaxFontSize)
      return *error = "corrupt WOFF table", false;
    size_t at = out->size();
    out->resize(at + ((origLength + 3) & ~3u), 0);
    if (compLength == origLength) {
      std::memcpy(out->data() + at, d + offset, origLength);
    } else {
      uLongf len = origLength;
      if (uncompress(out->data() + at, &len, d + offset, compLength) != Z_OK || len != origLength)
        return *error = "corrupt WOFF table data", false;
    }
    uint8_t* rec = out->data() + 12 + size_t(i) * 16;
    std::memcpy(rec, e, 4);           // tag
    std::memcpy(rec + 4, e + 16, 4);  // checksum
    put32(rec + 8, uint32_t(at));
    put32(rec + 12, origLength);
  }
  return true;
#else
  (void)d, (void)size, (void)out;
  return *error = "WOFF support is not compiled in", false;
#endif
}

bool decodeWoff2(const uint8_t* d, size_t size, std::vector<uint8_t>* out, std::string* error) {
#if JSUI_WOFF2
  size_t finalSize = woff2::ComputeWOFF2FinalSize(d, size);
  if (!finalSize || finalSize > kMaxFontSize) return *error = "corrupt WOFF2 header", false;
  std::string buf(std::min(finalSize, kMaxFontSize), '\0');
  woff2::WOFF2StringOut sink(&buf);
  sink.SetMaxSize(kMaxFontSize);
  if (!woff2::ConvertWOFF2ToTTF(d, size, &sink)) return *error = "corrupt WOFF2 data", false;
  out->assign(buf.begin(), buf.begin() + std::min(sink.Size(), buf.size()));
  return true;
#else
  (void)d, (void)size, (void)out;
  return *error = "WOFF2 support is not compiled in (JSUI_WOFF2=OFF)", false;
#endif
}

}  // namespace

FontContainer sniffFont(const void* data, size_t size) {
  if (!data || size < 4) return FontContainer::Unknown;
  const uint8_t* d = static_cast<const uint8_t*>(data);
  uint32_t tag = be32(d);
  if (tag == 0x774F4646) return FontContainer::Woff;   // 'wOFF'
  if (tag == 0x774F4632) return FontContainer::Woff2;  // 'wOF2'
  if (tag == 0x74746366) return FontContainer::Collection;  // 'ttcf'
  if (tag == 0x00010000 || tag == 0x4F54544F || tag == 0x74727565 || tag == 0x74797031)  // 1.0, 'OTTO', 'true', 'typ1'
    return FontContainer::Sfnt;
  return FontContainer::Unknown;
}

bool decodeWebFont(const void* data, size_t size, std::vector<uint8_t>* out, std::string* error) {
  const uint8_t* d = static_cast<const uint8_t*>(data);
  switch (sniffFont(data, size)) {
    case FontContainer::Woff: return decodeWoff(d, size, out, error);
    case FontContainer::Woff2: return decodeWoff2(d, size, out, error);
    default: return *error = "not a WOFF / WOFF2 font", false;
  }
}

int collectionFaceCount(const void* data, size_t size) {
  if (sniffFont(data, size) != FontContainer::Collection || size < 12) return 1;
  uint32_t n = be32(static_cast<const uint8_t*>(data) + 8);
  return int(std::clamp<uint32_t>(n, 1, std::min<uint32_t>(256, uint32_t((size - 12) / 4))));
}

}  // namespace jsui
