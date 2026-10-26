#include "render/gpu_layers.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace jsui {

namespace {
std::atomic<bool> gLayerCaches{true};

LayerCache* active(LayerCache* cache, bool opacityLayers, bool raster, bool backdrop) {
  if (!cache) return nullptr;
  cache->setModes(opacityLayers, raster, backdrop);
  return opacityLayers || raster || backdrop ? cache : nullptr;
}
}  // namespace

bool gpuLayerCachesEnabled() {
  static const bool off = [] {
    const char* v = std::getenv("JSUI_LAYER_CACHE");
    return v && (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0);
  }();
  return gLayerCaches.load(std::memory_order_relaxed) && !off;
}

void setGpuLayerCaches(bool on) { gLayerCaches.store(on, std::memory_order_relaxed); }

GpuLayerCaches::GpuLayerCaches(size_t budgetBytes) : budget_(budgetBytes) {}

void GpuLayerCaches::setBudget(size_t budgetBytes) {
  budget_ = budgetBytes;
  applyBudget();
}

// 3/8 of the context's budget (96 MB of the default 256 MB, the other half
// of which is the image cache's), shared by its surfaces.
void GpuLayerCaches::applyBudget() {
  size_t each = budget_ / 8 * 3 / std::max<size_t>(1, caches_.size());
  for (auto& e : caches_) e.second.cache->setBudget(each);
}

void GpuLayerCaches::clear() {
  for (auto& e : caches_) e.second.cache->clear();
}

size_t GpuLayerCaches::bytes() const {
  size_t n = 0;
  for (const auto& e : caches_) n += e.second.cache->bytes();
  return n;
}

LayerCache* GpuLayerCaches::of(const Surface* surface) {
  frames_++;
  if (!gpuLayerCachesEnabled()) {
    caches_.clear();
    return nullptr;
  }
  auto it = caches_.find(surface);
  if (it == caches_.end()) {
    constexpr uint64_t kMaxIdle = 600;  // frames of the context's other surfaces
    for (auto i = caches_.begin(); i != caches_.end();)
      i = frames_ - i->second.lastUsed > kMaxIdle ? caches_.erase(i) : std::next(i);
    it = caches_.emplace(surface, Entry{std::make_unique<LayerCache>(), 0}).first;
    applyBudget();
  }
  it->second.lastUsed = frames_;
  return it->second.cache.get();
}

GpuLayerFrame::GpuLayerFrame(LayerCache* cache, bool opacityLayers, bool raster, bool backdrop)
    : cache_(cache), scope_(active(cache, opacityLayers, raster, backdrop)) {}

GpuLayerFrame::~GpuLayerFrame() {
  if (cache_) cache_->endFrame();
}

}  // namespace jsui
