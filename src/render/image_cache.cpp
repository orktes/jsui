#include "render/image_cache.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/document.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "render/image_node.h"
#include "runtime.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace jsui {

namespace {
// Reads only the header: the size is known before any pixel is decoded.
sk_sp<SkImage> lazyImage(const void* data, size_t size) {
  if (!data || !size) return nullptr;
  return SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(data, size));
}
}  // namespace

sk_sp<SkImage> decodeImage(const void* data, size_t size) {
  auto image = lazyImage(data, size);
  // Decode once now so painting never hits the codec.
  return image ? image->makeRasterImage(nullptr) : nullptr;
}

bool decodeDataUri(const std::string& uri, std::string* bytes) {
  if (uri.rfind("data:", 0) != 0) return false;
  size_t comma = uri.find(',');
  if (comma == std::string::npos) return false;
  std::string meta = uri.substr(5, comma - 5);
  bool base64 = meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0;
  bytes->clear();
  if (base64) {
    auto val = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      if (c == '+' || c == '-') return 62;
      if (c == '/' || c == '_') return 63;
      return -1;
    };
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = comma + 1; i < uri.size(); i++) {
      char c = uri[i];
      if (c == '=') break;
      if (std::isspace((unsigned char)c)) continue;
      int v = val(c);
      if (v < 0) return false;
      acc = (acc << 6) | uint32_t(v);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes->push_back(char((acc >> bits) & 0xFF));
      }
    }
  } else {
    for (size_t i = comma + 1; i < uri.size(); i++) {
      if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit((unsigned char)uri[i + 1]) &&
          std::isxdigit((unsigned char)uri[i + 2])) {
        bytes->push_back(char(std::stoi(uri.substr(i + 1, 2), nullptr, 16)));
        i += 2;
      } else {
        bytes->push_back(uri[i]);
      }
    }
  }
  return true;
}

// Decode workers. A job is a lazy (header-only) image; a worker rasters it
// and leaves the result for the runtime thread's pump(). Few threads, started
// on the first large image: decoding competes with the game for cores, and
// only large images come here.
struct ImageCache::Pool {
  struct Job {
    std::string url;
    uint32_t epoch;
    sk_sp<SkImage> lazy;
  };
  using Done = Decoded;

  Pool() {
    const int cores = int(std::thread::hardware_concurrency());
    const int n = std::clamp(cores / 4, 1, 2);
    for (int i = 0; i < n; i++) threads_.emplace_back([this] { run(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
  }
  void add(Job job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }
  void take(std::vector<Done>* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out->swap(done_);
  }
  // Until nothing runs and nothing queued can run.
  void waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return running_ == 0 && (jobs_.empty() || held_); });
  }
  void hold(bool on) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_ = on;
    }
    cv_.notify_all();
    idle_.notify_all();
  }

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stopping_ || (!held_ && !jobs_.empty()); });
      if (stopping_) return;  // queued jobs die with the cache
      Job job = std::move(jobs_.front());
      jobs_.pop_front();
      running_++;
      lock.unlock();
      auto t0 = std::chrono::steady_clock::now();
      sk_sp<SkImage> image = job.lazy->makeRasterImage(nullptr);
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      job.lazy.reset();
      lock.lock();
      done_.push_back({std::move(job.url), job.epoch, std::move(image), us, nullptr});
      running_--;
      if (running_ == 0 && (jobs_.empty() || held_)) idle_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_, idle_;
  std::deque<Job> jobs_;
  std::vector<Done> done_;
  int running_ = 0;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
  bool held_ = false;
};

// ---------------------------------------------------------------- Web
//
// The browser decodes large images: createImageBitmap (off the page's main
// thread in Chromium, WebKit and Gecko) in a small pool of Workers made from
// a Blob URL, which also read the pixels back (OffscreenCanvas
// getImageData) and premultiply them, so the runtime's thread only copies
// premultiplied RGBA into an SkData the image then wraps. Without Workers
// (or when making one fails, e.g. a CSP without blob: workers) the same runs
// on this thread, minus the decode itself. Without createImageBitmap (Node:
// the headless CLI) nothing changes: the codec decodes inline.
//
// Like the fetch backend, JS never holds the cache's pointer: a token looks
// it up, and a completion for a destroyed cache (or a cleared job) is
// dropped. Single-threaded: the thread the module runs on.
#ifdef __EMSCRIPTEN__
namespace {
std::unordered_map<uint32_t, ImageCache*>& webCaches() {
  static std::unordered_map<uint32_t, ImageCache*> m;
  return m;
}
uint32_t gNextWebToken = 1;
}  // namespace

// 1: started (a completion follows, even on failure); 0: the browser cannot.
EM_JS(int, jsui_js_image_decode, (uint32_t token, uint32_t job, const uint8_t* data, size_t size), {
  if (typeof createImageBitmap !== "function") return 0;
  var canvasFor = function(w, h) {
    if (typeof OffscreenCanvas === "function") return new OffscreenCanvas(w, h);
    if (typeof document === "object" && document.createElement) {
      var c = document.createElement("canvas");
      c.width = w;
      c.height = h;
      return c;
    }
    return null;
  };
  if (!canvasFor(1, 1)) return 0;
  // The decoder: in a Worker it is its whole program (made from this
  // function's source, so it uses nothing from outside it).
  var decoder = function(scope) {
    var opts = { premultiplyAlpha: "none", colorSpaceConversion: "none" };
    var run = function(m, reply) {
      var t0 = performance.now();
      createImageBitmap(new Blob([m.bytes]), opts).then(function(bmp) {
        var w = bmp.width, h = bmp.height;
        var c = typeof OffscreenCanvas === "function" ? new OffscreenCanvas(w, h) : null;
        if (!c) { c = document.createElement("canvas"); c.width = w; c.height = h; }
        var g = c.getContext("2d", { willReadFrequently: true });
        g.globalCompositeOperation = "copy";
        g.drawImage(bmp, 0, 0);
        if (bmp.close) bmp.close();
        var px = g.getImageData(0, 0, w, h).data;
        // getImageData is unpremultiplied; Skia's raster images are not.
        var opaque = true;
        for (var i = 0; i < px.length; i += 4) {
          var a = px[i + 3];
          if (a === 255) continue;
          opaque = false;
          for (var k = 0; k < 3; k++) {
            var v = px[i + k] * a + 128;
            px[i + k] = (v + (v >> 8)) >> 8;
          }
        }
        reply({ id: m.id, w: w, h: h, px: px.buffer, opaque: opaque, ms: performance.now() - t0 }, [px.buffer]);
      }).catch(function(e) {
        reply({ id: m.id, error: String(e && e.message || e) }, []);
      });
    };
    if (scope) scope.onmessage = function(e) { run(e.data, function(r, t) { scope.postMessage(r, t); }); };
    return run;
  };
  var D = Module.__jsuiDecode;
  if (!D) {
    D = Module.__jsuiDecode = { workers: [], next: 0, jobs: new Map(), seq: 1, local: null,
                                stats: { started: 0, workers: 0, local: 0, done: 0, failed: 0 } };
    D.finish = function(r) {
      var j = D.jobs.get(r.id);
      if (!j) return;
      D.jobs.delete(r.id);
      var ok = !r.error && r.w > 0 && r.h > 0;
      var dst = ok ? _jsui_emscripten_image_begin(j.token, j.job, r.w, r.h) : 0;
      if (dst) HEAPU8.set(new Uint8Array(r.px), dst);
      D.stats[ok ? "done" : "failed"]++;
      _jsui_emscripten_image_end(j.token, j.job, ok ? 1 : 0, r.opaque ? 1 : 0, r.ms || 0);
    };
    var n = Math.max(1, Math.min(2, ((typeof navigator === "object" && navigator.hardwareConcurrency) || 2) >> 2));
    try {
      if (typeof Worker !== "function" || typeof OffscreenCanvas !== "function" || typeof URL !== "function") throw 0;
      var src = URL.createObjectURL(new Blob(["(" + decoder.toString() + ")(self);"], { type: "text/javascript" }));
      for (var w = 0; w < n; w++) {
        var worker = new Worker(src);
        worker.onmessage = function(e) { D.finish(e.data); };
        // A Worker that cannot start (CSP): its jobs and later ones run here.
        worker.onerror = function(e) {
          if (e && e.preventDefault) e.preventDefault();
          if (!D.workers.length) return;
          D.workers = [];
          D.jobs.forEach(function(j, id) { if (j.bytes) D.local(j.bytes, id); });
        };
        D.workers.push(worker);
      }
    } catch (e) {
      D.workers = [];
    }
    var runLocal = decoder(null);
    D.local = function(bytes, id) {
      D.stats.local++;
      runLocal({ id: id, bytes: bytes }, function(r) { D.finish(r); });
    };
  }
  var id = D.seq++;
  var bytes = HEAPU8.slice(data, data + size);  // a copy: the caller frees it
  D.jobs.set(id, { token: token, job: job, bytes: bytes });
  D.stats.started++;
  if (D.workers.length) {
    D.stats.workers++;
    // Kept (not transferred) so a Worker failing to start can run it here.
    D.workers[D.next++ % D.workers.length].postMessage({ id: id, bytes: bytes });
  } else {
    D.local(bytes, id);
  }
  return 1;
});

EM_JS(void, jsui_js_image_forget, (uint32_t token), {
  var D = Module.__jsuiDecode;
  if (!D) return;
  D.jobs.forEach(function(j, id) { if (j.token === token) D.jobs.delete(id); });
});
#endif

bool ImageCache::decodeOnWeb(const std::string& url, const void* data, size_t size) {
#ifdef __EMSCRIPTEN__
  if (!webToken_) {
    webToken_ = gNextWebToken++;
    webCaches()[webToken_] = this;
  }
  uint32_t job = nextWebJob_++;
  if (!jsui_js_image_decode(webToken_, job, static_cast<const uint8_t*>(data), size)) return false;
  webJobs_[job] = WebJob{url, epoch_, SkData::MakeWithCopy(data, size), nullptr, 0, 0};
  return true;
#else
  (void)url, (void)data, (void)size;
  return false;
#endif
}

void* ImageCache::webDecodeBegin(uint32_t job, int w, int h) {
  auto it = webJobs_.find(job);
  if (it == webJobs_.end() || w <= 0 || h <= 0 || int64_t(w) * h > (int64_t(1) << 28)) return nullptr;
  it->second.w = w;
  it->second.h = h;
  it->second.pixels = SkData::MakeUninitialized(size_t(w) * h * 4);
  return it->second.pixels->writable_data();
}

void ImageCache::webDecodeEnd(uint32_t job, bool ok, bool opaque, double ms) {
  auto it = webJobs_.find(job);
  if (it == webJobs_.end()) return;
  WebJob j = std::move(it->second);
  webJobs_.erase(it);
  sk_sp<SkImage> image;
  if (ok && j.pixels) {
    SkImageInfo info = SkImageInfo::Make(j.w, j.h, kRGBA_8888_SkColorType, opaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
    image = SkImages::RasterFromData(info, std::move(j.pixels), size_t(j.w) * 4);
  }
  sk_sp<SkData> fallback = image ? nullptr : std::move(j.encoded);
  webDone_.push_back({std::move(j.url), j.epoch, std::move(image), ms * 1000, std::move(fallback)});
}

ImageCache::ImageCache(Runtime& rt) : rt_(rt) {}
ImageCache::~ImageCache() {
#ifdef __EMSCRIPTEN__
  if (webToken_) {
    jsui_js_image_forget(webToken_);
    webCaches().erase(webToken_);
  }
#endif
}

void ImageCache::count(double us) {
  // Pool decodes count on the tick that publishes them: the counter is the
  // decode work done, not what a frame waited for.
  PerfCounters& c = rt_.doc.perf.counters;
  c.imageDecodes++;
  c.imageDecodeUs += us;
}

void ImageCache::fail(const std::string& url, Entry& e, std::string error) {
  e.state = State::Failed;
  e.image = nullptr;
  e.error = std::move(error);
  rt_.log(JSUI_LOG_WARN,
          url.rfind("data:", 0) == 0 ? std::string("image failed to decode: data URI") : "image failed to load: " + url);
}

void ImageCache::decodeInto(const std::string& url, Entry& e, const void* data, size_t size, const std::string& what) {
  e.bytes = size;
  auto t0 = std::chrono::steady_clock::now();
  sk_sp<SkImage> lazy = lazyImage(data, size);
  const int64_t pixels = lazy ? int64_t(lazy->width()) * lazy->height() : 0;
  if (lazy && inlineSpent_ + pixels > inlineBudgetPixels) {
#if defined(__EMSCRIPTEN__)
    bool handed = decodeOnWeb(url, data, size);
#else
    if (!pool_) pool_ = std::make_unique<Pool>();
    pool_->add({url, epoch_, std::move(lazy)});
    bool handed = true;
#endif
    if (handed) {
      e.state = State::Loading;
      e.decoding = true;
      decoding_++;
      return;
    }
  }
  inlineSpent_ += pixels;
  e.image = lazy ? lazy->makeRasterImage(nullptr) : nullptr;
  count(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
  if (e.image) e.state = State::Ready;
  else fail(url, e, "failed to decode " + what);
}

int ImageCache::pump() {
  if (decoding_ == 0) return 0;
  std::vector<Decoded> done;
  if (pool_) pool_->take(&done);
  if (!webDone_.empty()) {
    if (done.empty()) done.swap(webDone_);
    else for (Decoded& d : webDone_) done.push_back(std::move(d));
    webDone_.clear();
  }
  int n = 0;
  for (Decoded& d : done) {
    decoding_--;
    if (!d.image && d.encoded && d.epoch == epoch_) {
      // The browser could not (an odd format, no 2D context): the codec can.
      auto t0 = std::chrono::steady_clock::now();
      d.image = decodeImage(d.encoded->data(), d.encoded->size());
      d.us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    } else {
      asyncDecodes++;
    }
    count(d.us);
    auto it = entries_.find(d.url);
    // Cleared, or replaced (put) while it decoded: the newer entry stands.
    if (d.epoch != epoch_ || it == entries_.end() || !it->second.decoding) continue;
    Entry& e = it->second;
    e.decoding = false;
    if (d.image) {
      e.image = std::move(d.image);
      e.state = State::Ready;
    } else {
      fail(d.url, e, d.url.rfind("data:", 0) == 0 ? "failed to decode image data" : "failed to decode image: " + d.url);
    }
    loaded(d.url);
    n++;
  }
  return n;
}

void ImageCache::waitDecodes() {
  if (pool_) pool_->waitIdle();
}

void ImageCache::finishDecodes() {
  waitDecodes();
  pump();
}

void ImageCache::holdDecodes(bool on) {
#if !defined(__EMSCRIPTEN__)
  if (!pool_) pool_ = std::make_unique<Pool>();
  pool_->hold(on);
#else
  (void)on;
#endif
}

std::string ImageCache::resolve(const std::string& uri) const {
  if (uri.empty() || uri.rfind("data:", 0) == 0) return uri;
  return ResourceLoader::resolve(rt_.baseUrl, uri);
}

namespace {
// Images sized by their intrinsic size must be re-measured once decoded.
void dirtyImageNodes(Node* n, const std::string& url) {
  if (n->type() == NodeType::Image && n->imageCache) {
    auto* c = static_cast<image::NodeCache*>(n->imageCache.get());
    if (c->url == url && n->yoga() && YGNodeHasMeasureFunc(n->yoga())) YGNodeMarkDirty(n->yoga());
  }
  for (Node* c : n->children()) dirtyImageNodes(c, url);
}
}  // namespace

void ImageCache::loaded(const std::string& url) {
  generation++;
  for (auto& s : rt_.surfaces()) {
    dirtyImageNodes(s->doc.root(), url);
    s->doc.markLayoutDirty();
  }
}

sk_sp<SkImage> ImageCache::get(const std::string& url, State* state) {
  if (url.empty()) {
    if (state) *state = State::Failed;
    return nullptr;
  }
  auto it = entries_.find(url);
  // A Loading entry with nothing in flight lost its request (reload cancels
  // all loads): ask again.
  // A decoding entry is on the pool, not lost.
  bool retry = it != entries_.end() && it->second.state == State::Loading && !it->second.decoding &&
               !rt_.loader.busy();
  if (it == entries_.end() || retry) {
    if (url.rfind("data:", 0) == 0) {
      std::string bytes;
      auto& slot = entries_[url] = Entry{};
      if (decodeDataUri(url, &bytes)) {
        decodeInto(url, slot, bytes.data(), bytes.size(), "image data");
      } else {
        slot.bytes = bytes.size();
        fail(url, slot, "malformed data URI");
      }
      if (state) *state = slot.state;
      return slot.image;
    }
    entries_[url] = Entry{};
    rt_.loader.load(url, [this, url](Resource res) {
      auto& e = entries_[url];
      if (res.ok()) {
        decodeInto(url, e, res.data.data(), res.data.size(), "image: " + url);
        if (e.decoding) return;  // published by a later pump()
      } else {
        e.bytes = res.data.size();
        fail(url, e,
             "failed to load image: " + url + " (status " + std::to_string(res.status) +
                 (res.error.empty() ? "" : ", " + res.error) + ")");
      }
      loaded(url);
    });
    if (state) *state = State::Loading;
    return nullptr;
  }
  if (state) *state = it->second.state;
  return it->second.image;
}

void ImageCache::put(const std::string& url, sk_sp<SkImage> image) {
  bool ok = image != nullptr;
  entries_[url] = Entry{ok ? State::Ready : State::Failed, std::move(image), 0, ok ? "" : "no image"};
  loaded(url);
}

bool ImageCache::peek(const std::string& url, Info* info) const {
  auto it = entries_.find(url);
  if (it == entries_.end()) return false;
  info->state = it->second.state;
  info->image = it->second.image;
  info->bytes = it->second.bytes;
  info->error = it->second.error;
  return true;
}

void ImageCache::clear() {
  entries_.clear();
  epoch_++;
  generation++;
}

}  // namespace jsui

#ifdef __EMSCRIPTEN__
extern "C" {

// The browser glue's side of ImageCache::webDecodeBegin / webDecodeEnd.
EMSCRIPTEN_KEEPALIVE void* jsui_emscripten_image_begin(uint32_t token, uint32_t job, int w, int h) {
  auto it = jsui::webCaches().find(token);
  return it == jsui::webCaches().end() ? nullptr : it->second->webDecodeBegin(job, w, h);
}

EMSCRIPTEN_KEEPALIVE void jsui_emscripten_image_end(uint32_t token, uint32_t job, int ok, int opaque, double ms) {
  auto it = jsui::webCaches().find(token);
  if (it != jsui::webCaches().end()) it->second->webDecodeEnd(job, ok != 0, opaque != 0, ms);
}

}  // extern "C"
#endif
