// Shared by the Graphite backends (gpu_webgpu.cpp, gpu_graphite.cpp): an
// ImageProvider that keeps the GPU copy of each raster image.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "include/core/SkImage.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/ImageProvider.h"
#include "include/gpu/graphite/Recorder.h"

namespace jsui {
namespace graphite_support {

namespace graphite = skgpu::graphite;

constexpr size_t kDefaultCacheBytes = 256u << 20;

// Graphite draws raster SkImages through an ImageProvider; the default one
// uploads them again for every recording. Cache the GPU copy per image id
// (the cached SkImage keeps the raster image's id from being reused), and
// drop entries not used for a while or over budget.
class CachingImageProvider final : public graphite::ImageProvider {
 public:
  sk_sp<SkImage> findOrCreate(graphite::Recorder* recorder, const SkImage* image,
                              SkImage::RequiredProperties props) override {
    if (image->isTextureBacked()) return sk_ref_sp(const_cast<SkImage*>(image));
    uint64_t key = (uint64_t(image->uniqueID()) << 1) | (props.fMipmapped ? 1 : 0);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      it->second.lastUsed = frame_;
      return it->second.gpu;
    }
    sk_sp<SkImage> gpu = SkImages::TextureFromImage(recorder, image, props);
    if (!gpu) return nullptr;
    size_t bytes = image->imageInfo().computeMinByteSize();
    entries_[key] = {sk_ref_sp(const_cast<SkImage*>(image)), gpu, bytes, frame_};
    bytes_ += bytes;
    return gpu;
  }

  // Once per frame: evicts images nobody else holds any more (they cannot be
  // drawn again: shadow masks of shapes that animated away), images unused
  // for kMaxAge frames, and the least recently used ones while over budget.
  void endFrame() {
    frame_++;
    constexpr uint64_t kMaxAge = 600;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.raster->unique() || frame_ - it->second.lastUsed > kMaxAge) {
        bytes_ -= it->second.bytes;
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    while (bytes_ > budget_ && !entries_.empty()) {
      auto oldest = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
      if (oldest->second.lastUsed == frame_ - 1) break;  // everything is in use
      bytes_ -= oldest->second.bytes;
      entries_.erase(oldest);
    }
  }
  void clear() {
    entries_.clear();
    bytes_ = 0;
  }
  void setBudget(size_t bytes) { budget_ = bytes; }

 private:
  struct Entry {
    sk_sp<SkImage> raster, gpu;
    size_t bytes;
    uint64_t lastUsed;
  };
  std::unordered_map<uint64_t, Entry> entries_;
  size_t bytes_ = 0, budget_ = kDefaultCacheBytes / 2;
  uint64_t frame_ = 0;
};

}  // namespace graphite_support
}  // namespace jsui
