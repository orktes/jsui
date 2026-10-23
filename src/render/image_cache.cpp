#include "render/image_cache.h"

#include <cctype>
#include <chrono>

#include "core/document.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "render/image_node.h"
#include "runtime.h"

namespace jsui {

sk_sp<SkImage> decodeImage(const void* data, size_t size) {
  if (!data || !size) return nullptr;
  auto image = SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(data, size));
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

ImageCache::ImageCache(Runtime& rt) : rt_(rt) {}
ImageCache::~ImageCache() = default;

sk_sp<SkImage> ImageCache::decode(const void* data, size_t size) {
  auto t0 = std::chrono::steady_clock::now();
  sk_sp<SkImage> image = decodeImage(data, size);
  PerfCounters& c = rt_.doc.perf.counters;
  c.imageDecodes++;
  c.imageDecodeUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  return image;
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
  bool retry = it != entries_.end() && it->second.state == State::Loading && !rt_.loader.busy();
  if (it == entries_.end() || retry) {
    if (url.rfind("data:", 0) == 0) {
      std::string bytes;
      Entry e;
      bool ok = decodeDataUri(url, &bytes);
      if (ok) e.image = decode(bytes.data(), bytes.size());
      e.bytes = bytes.size();
      e.state = e.image ? State::Ready : State::Failed;
      if (!e.image) {
        e.error = ok ? "failed to decode image data" : "malformed data URI";
        rt_.log(JSUI_LOG_WARN, "image failed to decode: data URI");
      }
      auto& slot = entries_[url] = std::move(e);
      if (state) *state = slot.state;
      return slot.image;
    }
    entries_[url] = Entry{};
    rt_.loader.load(url, [this, url](Resource res) {
      auto& e = entries_[url];
      if (res.ok()) e.image = decode(res.data.data(), res.data.size());
      e.bytes = res.data.size();
      e.state = e.image ? State::Ready : State::Failed;
      if (!e.image) {
        e.error = res.ok() ? "failed to decode image: " + url
                           : "failed to load image: " + url + " (status " + std::to_string(res.status) +
                                 (res.error.empty() ? "" : ", " + res.error) + ")";
        rt_.log(JSUI_LOG_WARN, "image failed to load: " + url);
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
  generation++;
}

}  // namespace jsui
