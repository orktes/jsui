// Decoded images keyed by resolved URL. Requests go through the
// ResourceLoader; when an image arrives the document is re-laid out (images
// without explicit size use their intrinsic size) and repainted.
//
// Large images decode off the runtime's thread, so a full-screen background
// never costs a frame its decode time: natively on a small worker pool, on
// the web by the browser (createImageBitmap in a Worker). The entry stays
// Loading until a later tick publishes it.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/core/SkData.h"
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
  // images the pool (or the browser) has decoded since. Returns how many.
  int pump();
  // Decodes handed off and not yet published (jsui_pending_loads counts
  // them: headless hosts wait for them like for loads).
  int pendingDecodes() const { return decoding_; }
  // Tests. waitDecodes blocks until the pool is idle (decoded images still
  // wait for a tick to publish them); finishDecodes then publishes them.
  // holdDecodes(true) keeps queued decodes from starting until released, so
  // a test can look at the frames before an image lands.
  void waitDecodes();
  void finishDecodes();
  void holdDecodes(bool on);
  // Inline decode budget per tick, in pixels (decode time is roughly
  // proportional: ~3.5 ns/px measured on an M-series Mac, so 256k px is
  // about 1 ms). Images decode inline while the tick's budget lasts, where
  // the hand-over would only cost a frame of pop-in; the rest (a screen of
  // card art, any image larger than the budget) go off the thread: to the
  // pool natively, to the browser on the web, where the codec in wasm is a
  // few times slower and the budget a quarter. Without createImageBitmap
  // (Node: the headless CLI) everything decodes inline.
#ifdef __EMSCRIPTEN__
  int64_t inlineBudgetPixels = 64 * 1024;
#else
  int64_t inlineBudgetPixels = 256 * 1024;
#endif
  // Before the tick's loads complete: a new inline budget.
  void beginTick() { inlineSpent_ = 0; }
  // Decodes done off the thread so far (tests: proves the work left it).
  uint64_t asyncDecodes = 0;

  // Web: the browser's result for decode `job` (jsui_emscripten_image_*).
  // begin returns where `w` x `h` premultiplied RGBA pixels go (nullptr:
  // the job is gone); end hands them over (ok false: the browser failed,
  // the codec decodes the bytes instead).
  void* webDecodeBegin(uint32_t job, int w, int h);
  void webDecodeEnd(uint32_t job, bool ok, bool opaque, double ms);

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
  // A decode that finished off the thread, waiting for pump().
  struct Decoded {
    std::string url;
    uint32_t epoch;
    sk_sp<SkImage> image;   // nullptr: failed
    double us;
    sk_sp<SkData> encoded;  // web: the bytes, for the codec if the browser failed
  };
  bool decodeOnWeb(const std::string& url, const void* data, size_t size);

  struct Pool;
  struct WebJob {
    std::string url;
    uint32_t epoch;
    sk_sp<SkData> encoded;
    sk_sp<SkData> pixels;
    int w = 0, h = 0;
  };
  Runtime& rt_;
  std::unordered_map<std::string, Entry> entries_;
  std::unique_ptr<Pool> pool_;  // started on the first large image
  uint32_t epoch_ = 0;          // clear() drops decodes still in flight
  int decoding_ = 0;
  int64_t inlineSpent_ = 0;     // pixels decoded inline this tick
  // Web decodes in flight (by job id) and finished ones.
  std::unordered_map<uint32_t, WebJob> webJobs_;
  std::vector<Decoded> webDone_;
  uint32_t nextWebJob_ = 1;
  uint32_t webToken_ = 0;  // this cache in the browser glue (0: not yet)
};

// Decodes encoded image bytes into a raster image (nullptr on failure).
sk_sp<SkImage> decodeImage(const void* data, size_t size);
// Decodes the payload of a data: URI. Returns false if malformed.
bool decodeDataUri(const std::string& uri, std::string* bytes);

}  // namespace jsui
