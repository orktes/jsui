// The LayerCaches of one GPU context (painter.h: composited opacity layers,
// raster-cached subtrees, the static backdrop), shared by the Graphite
// backends (gpu_webgpu.cpp, gpu_graphite.cpp): one per surface drawing with
// the context, since entries age with the frames of the surface they belong
// to; a surface that stopped drawing here loses its cache after a while.
//
// Their textures are the context's: the owner clears them on
// jsui_gpu_purge, on context loss and before the context goes.
//
// JSUI_LAYER_CACHE=off (or 0) in the environment turns them off on every
// backend, for A/B comparisons: frames then paint every layer offscreen
// and every subtree draw by draw, as without a cache.
#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "render/painter.h"

namespace jsui {

class Surface;

// Whether GPU contexts use LayerCaches (JSUI_LAYER_CACHE, setGpuLayerCaches).
bool gpuLayerCachesEnabled();
// For tests: off makes every GPU context paint without them.
void setGpuLayerCaches(bool on);

class GpuLayerCaches {
 public:
  // `budgetBytes`: the context's texture budget (jsui_gpu_purge's
  // cache_bytes); the caches of all its surfaces keep to 3/8 of it.
  explicit GpuLayerCaches(size_t budgetBytes);
  void setBudget(size_t budgetBytes);
  // Frees every cached texture (purge, context loss, teardown).
  void clear();
  size_t bytes() const;
  // The cache of `surface`, made on its first frame (frees the caches of
  // surfaces idle for a while). Null while the caches are turned off.
  LayerCache* of(const Surface* surface);

 private:
  struct Entry {
    std::unique_ptr<LayerCache> cache;
    uint64_t lastUsed = 0;
  };
  void applyBudget();
  std::unordered_map<const Surface*, Entry> caches_;
  size_t budget_;
  uint64_t frames_ = 0;
};

// One frame's use of a LayerCache: the cache the painter uses while in scope
// (null, or every mode off: none), aged at the end of the frame.
class GpuLayerFrame {
 public:
  GpuLayerFrame(LayerCache* cache, bool opacityLayers = true, bool raster = true, bool backdrop = true);
  ~GpuLayerFrame();
  GpuLayerFrame(const GpuLayerFrame&) = delete;
  GpuLayerFrame& operator=(const GpuLayerFrame&) = delete;

 private:
  LayerCache* cache_;
  LayerCacheScope scope_;
};

}  // namespace jsui
