// Skia Graphite on Metal (jsui_gpu_create_metal / jsui_render_gpu_target):
// frames rendered into an MTLTexture of the system's default device, read
// back and compared with the CPU raster of the same frame.
//
// Needs JSUI_GRAPHITE_METAL (a Skia built with Graphite + Metal:
// scripts/build-skia-desktop.sh) and a Metal device; otherwise the test
// prints SKIP and passes. JSUI_METAL_TEST_REQUIRE=1 turns that skip into a
// failure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gpu_layer_scene.h"
#include "jsui/jsui.h"
#include "render/gpu.h"
#include "test.h"

#if defined(JSUI_GRAPHITE_METAL) && defined(__APPLE__)
#define JSUI_METAL_TESTS 1
extern "C" {
int test_mtl_init(void** device, void** queue);
void* test_mtl_texture(void* device, int w, int h);
int test_mtl_read(void* device, void* queue, void* texture, int w, int h, void* out);
void test_mtl_release(void* obj);
}
#endif

namespace {

void skip(const char* test, const char* why) {
  const char* v = std::getenv("JSUI_METAL_TEST_REQUIRE");
  if (v && *v && std::strcmp(v, "0") != 0) {
    std::fprintf(stderr, "  %s: %s but JSUI_METAL_TEST_REQUIRE is set\n", test, why);
    jsui_test::failures()++;
    return;
  }
  std::printf("SKIP %s: %s\n", test, why);
}

}  // namespace

// The LayerCache on Graphite Metal (gpu_layer_scene.h): cached opacity
// layers, a raster-cached list, a motion layer and the static backdrop in
// incremental frames of a host-owned MTLTexture match the CPU raster, with
// the cache and without, through a purge and under a tiny texture budget.
TEST(metal_graphite_layer_cache_matches_cpu) {
#if !JSUI_METAL_TESTS
  skip("metal_graphite_layer_cache_matches_cpu", "built without JSUI_GRAPHITE_METAL");
#else
  void *device = nullptr, *queue = nullptr;
  if (!test_mtl_init(&device, &queue)) {
    skip("metal_graphite_layer_cache_matches_cpu", "no Metal device");
    return;
  }
  const int W = 240, H = 160;
  void* texture = test_mtl_texture(device, W, H);
  CHECK(texture != nullptr);
  JsuiGpuContext* gpu = texture ? jsui_gpu_create_metal(device, queue) : nullptr;
  CHECK(gpu != nullptr);
  if (gpu) {
    gpu_layer_scene::checkMatchesCpu("Metal", gpu, [&](jsui::Surface& s, bool incremental, JsuiRect* dirty) {
      JsuiGpuTarget t{};
      t.texture = texture;
      t.width = W;
      t.height = H;
      t.flags = incremental ? JSUI_GPU_INCREMENTAL : 0;
      CHECK_EQ(jsui::graphite_gpu::render(s, gpu, t, dirty, nullptr), 0);
      std::vector<uint32_t> out(size_t(W) * H);
      CHECK(test_mtl_read(device, queue, texture, W, H, out.data()));
      return out;
    });
    jsui_gpu_destroy(gpu);
  }
  test_mtl_release(texture);
  test_mtl_release(queue);
  test_mtl_release(device);
#endif
}
