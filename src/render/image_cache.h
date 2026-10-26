// Decoded images keyed by resolved URL. Requests go through the
// ResourceLoader; when an image arrives the document is re-laid out (images
// without explicit size use their intrinsic size) and repainted.
//
// Large images decode on a small worker pool (native) instead of the
// runtime's thread, so a full-screen background never costs a frame its
// decode time: the entry stays Loading until a later tick publishes it.
#pragma once

#include <memory>
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
  // data: URIs (base64 or percent-encoded) decode right away, small ones
  // inline and large ones on the decode pool (Loading until published).
  sk_sp<SkImage> get(const std::string& url, State* state = nullptr);
  // Data: / inline sources and embedder-provided images.
  void put(const std::string& url, sk_sp<SkImage> image);
  // What is known about `url` without starting a load (false: never requested).
  struct Info {
    State state = State::Loading;
    sk_sp<SkImage> image;
    size_t bytes = 0;       // encoded size (loaded images)
    std::string error;      // why it failed
  };
  bool peek(const std::string& url, Info* info) const;
  void clear();

  // Resolves an image source URI against the runtime's base URL.
  std::string resolve(const std::string& uri) const;
  // Bumped whenever an image finishes loading (or fails).
  uint32_t generation = 0;

  // On the runtime thread each tick (after the loader's pump): publishes the
  // images the pool has decoded since. Returns how many.
  int pump();
  // Decodes handed to the pool and not yet published (jsui_pending_loads
  // counts them: headless hosts wait for them like for loads).
  int pendingDecodes() const { return decoding_; }
  // Tests. waitDecodes blocks until the pool is idle (decoded images still
  // wait for a tick to publish them); finishDecodes then publishes them.
  // holdDecodes(true) keeps queued decodes from starting until released, so
  // a test can look at the frames before an image lands.
  void waitDecodes();
  void finishDecodes();
  void holdDecodes(bool on);
  // Images of at least this many pixels decode on the pool; smaller ones
  // (icons) inline, where handing over would only cost a frame of pop-in.
  // No pool on the web: there everything decodes inline.
  int64_t asyncMinPixels = 256 * 256;
  // Decodes the pool ran so far (tests: proves the work left this thread).
  uint64_t asyncDecodes = 0;

 private:
  struct Entry {
    State state = State::Loading;
    sk_sp<SkImage> image;
    size_t bytes = 0;
    std::string error;
    bool decoding = false;  // on the pool: not a lost request
  };
  void loaded(const std::string& url);
  // Decodes `data` into `url`'s entry: inline (Ready / Failed now) or, for a
  // large image, on the pool (still Loading, decoding). `what` names the
  // source in the failure message.
  void decodeInto(const std::string& url, Entry& e, const void* data, size_t size, const std::string& what);
  void fail(const std::string& url, Entry& e, std::string error);
  void count(double us);

  struct Pool;
  Runtime& rt_;
  std::unordered_map<std::string, Entry> entries_;
  std::unique_ptr<Pool> pool_;  // started on the first large image
  uint32_t epoch_ = 0;          // clear() drops decodes still on the pool
  int decoding_ = 0;
};

// Decodes encoded image bytes into a raster image (nullptr on failure).
sk_sp<SkImage> decodeImage(const void* data, size_t size);
// Decodes the payload of a data: URI. Returns false if malformed.
bool decodeDataUri(const std::string& uri, std::string* bytes);

}  // namespace jsui
