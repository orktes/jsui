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
  sk_sp<SkImage> get(const std::string& url, State* state = nullptr);
  // Data: / inline sources and embedder-provided images.
  void put(const std::string& url, sk_sp<SkImage> image);
  void clear();

 private:
  struct Entry {
    State state = State::Loading;
    sk_sp<SkImage> image;
  };
  Runtime& rt_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace jsui
