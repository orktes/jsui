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
  struct Done {
    std::string url;
    uint32_t epoch;
    sk_sp<SkImage> image;  // nullptr: the codec failed
    double us;
  };

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
      done_.push_back({std::move(job.url), job.epoch, std::move(image), us});
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

ImageCache::ImageCache(Runtime& rt) : rt_(rt) {}
ImageCache::~ImageCache() = default;

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
#if !defined(__EMSCRIPTEN__)
  if (lazy && inlineSpent_ + pixels > inlineBudgetPixels) {
    if (!pool_) pool_ = std::make_unique<Pool>();
    e.state = State::Loading;
    e.decoding = true;
    decoding_++;
    pool_->add({url, epoch_, std::move(lazy)});
    return;
  }
#endif
  inlineSpent_ += pixels;
  e.image = lazy ? lazy->makeRasterImage(nullptr) : nullptr;
  count(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
  if (e.image) e.state = State::Ready;
  else fail(url, e, "failed to decode " + what);
}

int ImageCache::pump() {
  if (!pool_ || decoding_ == 0) return 0;
  std::vector<Pool::Done> done;
  pool_->take(&done);
  int n = 0;
  for (Pool::Done& d : done) {
    decoding_--;
    count(d.us);
    asyncDecodes++;
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
