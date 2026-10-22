#include "render/image_cache.h"

#include "include/core/SkData.h"
#include "runtime.h"

namespace jsui {

ImageCache::ImageCache(Runtime& rt) : rt_(rt) {}
ImageCache::~ImageCache() = default;

sk_sp<SkImage> ImageCache::get(const std::string& url, State* state) {
  auto it = entries_.find(url);
  if (it == entries_.end()) {
    entries_[url] = Entry{};
    rt_.loader.load(url, [this, url](Resource res) {
      auto& e = entries_[url];
      if (res.ok()) {
        auto data = SkData::MakeWithCopy(res.data.data(), res.data.size());
        e.image = SkImages::DeferredFromEncodedData(data);
        if (e.image) e.image = e.image->makeRasterImage(nullptr);
      }
      e.state = e.image ? State::Ready : State::Failed;
      if (!e.image) rt_.log(JSUI_LOG_WARN, "image failed to load: " + url);
      rt_.doc.markLayoutDirty();
    });
    if (state) *state = State::Loading;
    return nullptr;
  }
  if (state) *state = it->second.state;
  return it->second.image;
}

void ImageCache::put(const std::string& url, sk_sp<SkImage> image) {
  entries_[url] = Entry{image ? State::Ready : State::Failed, std::move(image)};
}

void ImageCache::clear() { entries_.clear(); }

}  // namespace jsui
