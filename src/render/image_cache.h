// Decoded images keyed by resolved URL. Requests go through the
// ResourceLoader; when an image arrives the document is re-laid out (images
// without explicit size use their intrinsic size) and repainted.
#pragma once

#include <string>
#include <unordered_map>

#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

namespace jsui {

class Runtime;

class ImageCache {
 public:
  explicit ImageCache(Runtime& rt);
  ~ImageCache();

  enum class State { Loading, Ready, Failed };

  // Returns the image if decoded; starts loading on first request.
  // data: URIs (base64 or percent-encoded) decode synchronously.
  sk_sp<SkImage> get(const std::string& url, State* state = nullptr);
  // Data: / inline sources and embedder-provided images.
  void put(const std::string& url, sk_sp<SkImage> image);
  void clear();

  // Resolves an image source URI against the runtime's base URL.
  std::string resolve(const std::string& uri) const;
  // Bumped whenever an image finishes loading (or fails).
  uint32_t generation = 0;

 private:
  struct Entry {
    State state = State::Loading;
    sk_sp<SkImage> image;
  };
  void loaded(const std::string& url);

  Runtime& rt_;
  std::unordered_map<std::string, Entry> entries_;
};

// Decodes encoded image bytes into a raster image (nullptr on failure).
sk_sp<SkImage> decodeImage(const void* data, size_t size);
// Decodes the payload of a data: URI. Returns false if malformed.
bool decodeDataUri(const std::string& uri, std::string* bytes);

}  // namespace jsui
