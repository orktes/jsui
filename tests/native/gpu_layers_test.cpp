// GpuLayerCaches / GpuLayerFrame (gpu_layers.h), the GPU contexts' layer
// caches, on a raster canvas (the LayerCache works there too): the scene
// the Vulkan and Metal tests render (gpu_layer_scene.h), retained frames
// with the caches and without, through a clear (purge) and a tiny budget.
#include <vector>

#include "gpu_layer_scene.h"
#include "render/gpu_layers.h"
#include "test.h"

TEST(gpu_layer_caches_on_raster) {
  jsui::GpuLayerCaches caches(256u << 20);
  std::vector<uint32_t> px(size_t(240) * 160);
  gpu_layer_scene::checkMatchesCpu(
      "raster",
      [&](size_t bytes) {
        if (bytes) caches.setBudget(bytes);
        caches.clear();
        CHECK_EQ(caches.bytes(), size_t(0));
      },
      [&](jsui::Surface& s, bool incremental, JsuiRect* dirty) {
        if (!incremental) px.assign(px.size(), 0);
        jsui::GpuLayerFrame frame(caches.of(&s));
        SkIRect r = s.renderIncremental(px.data(), 240, 160, 240 * 4);
        *dirty = JsuiRect{r.x(), r.y(), r.width(), r.height()};
        return px;
      });
  // Off: no cache for any surface.
  jsui::setGpuLayerCaches(false);
  CHECK(caches.of(nullptr) == nullptr);
  jsui::setGpuLayerCaches(true);
  CHECK(caches.of(nullptr) != nullptr);
}
