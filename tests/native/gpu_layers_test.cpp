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

// Paint counting (setPaintCounting, the benchmarks' draw counts) paints
// through a counting canvas that stands in for the target: the LayerCache
// works through it as on the target itself (it used to see a canvas without
// pixels, recorder or context and paint every layer and subtree directly,
// so a counted GPU frame never cached anything), with the same pixels.
TEST(gpu_layer_caches_while_counting) {
  auto run = [](bool counting, int* layerDraws, int* layerRenders, int* motionDraws, int* draws) {
    jsui::GpuLayerCaches caches(256u << 20);
    gpu_layer_scene::LayerScene g;
    jsui::setPaintCounting(g.s.doc(), counting);
    std::vector<uint32_t> px(size_t(240) * 160);
    std::vector<std::vector<uint32_t>> frames;
    *layerDraws = *layerRenders = *motionDraws = *draws = 0;
    for (int i = 0; i < 36; i++) {
      g.step(i);
      g.s.rt->tick(0);
      jsui::GpuLayerFrame frame(caches.of(&g.s.rt->main()));
      g.s.rt->main().renderIncremental(px.data(), 240, 160, 240 * 4);
      jsui::PaintStats st = jsui::lastPaintStats(g.s.doc());
      *layerDraws += st.layerDraws;
      *layerRenders += st.layerRenders;
      *motionDraws += st.motionDraws;
      *draws += st.draws;
      frames.push_back(px);
    }
    return frames;
  };
  int ld, lr, md, dr, cld, clr, cmd, cdr;
  auto plain = run(false, &ld, &lr, &md, &dr);
  auto counted = run(true, &cld, &clr, &cmd, &cdr);
  std::printf("  layer draws %d / %d (rendered %d / %d, in motion %d / %d), %d draws counted\n", ld, cld, lr, clr, md,
              cmd, cdr);
  CHECK(ld > 0);
  CHECK_EQ(cld, ld);
  CHECK_EQ(clr, lr);
  CHECK_EQ(cmd, md);
  CHECK_EQ(dr, 0);
  CHECK(cdr > 0);
  for (size_t i = 0; i < plain.size(); i++) CHECK(plain[i] == counted[i]);
}
