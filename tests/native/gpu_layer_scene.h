// The LayerCache on native Graphite (gpu_layers.h), shared by the Vulkan and
// Metal tests: a scene that composites cached opacity layers, raster-caches
// a subtree, draws a scaling card as a motion layer and repaints damage over
// the static backdrop, rendered into a host-owned target with
// JSUI_GPU_INCREMENTAL, with the cache and without (setGpuLayerCaches), each
// frame read back and compared with the CPU raster of the same frame. Also
// through jsui_gpu_purge (textures freed and rendered again) and a texture
// budget too small for any layer.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/gpu_layers.h"
#include "render/painter.h"
#include "render_util.h"
#include "test.h"

namespace gpu_layer_scene {

using jsui::Node;

inline void patch(Node* n, const std::string& json) {
  std::string err;
  jsui::Value p = jsui::Value::parseJson(json, &err);
  n->patchStyle(p.asObject());
}

// 240x160: a gradient backdrop; a glow drifting behind a list of rows (the
// list is raster-cached: damage reaches it every frame, it does not
// change); a panel fading (an opacity layer over children, its texture
// composited at each frame's opacity); a translucent card springing in
// scale (an opacity layer drawn under the change of scale while it moves).
struct LayerScene {
  rtest::Scene s{240, 160};
  Node* glow = nullptr;
  Node* panel = nullptr;
  Node* card = nullptr;
  LayerScene() {
    s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":240,"height":160,
      "experimental_backgroundImage":"linear-gradient(135deg, #102030, #302040)"}})J");
    glow = s.add(R"J({"style":{"position":"absolute","left":10,"top":20,"width":40,"height":40,"borderRadius":20,
      "backgroundColor":"rgba(120,200,255,0.5)"}})J");
    s.add(R"J({"style":{"position":"absolute","left":8,"top":8,"width":96,"height":144,"padding":4},"children":[
      {"style":{"height":24,"marginBottom":4,"borderRadius":6,"backgroundColor":"rgba(255,255,255,0.12)","flexDirection":"row","alignItems":"center"},
       "children":[{"style":{"width":12,"height":12,"margin":6,"borderRadius":6,"backgroundColor":"#f2c35b"}},{"type":"Text","style":{"color":"#fff","fontSize":11},"text":"Sword"}]},
      {"style":{"height":24,"marginBottom":4,"borderRadius":6,"backgroundColor":"rgba(255,255,255,0.12)","flexDirection":"row","alignItems":"center"},
       "children":[{"style":{"width":12,"height":12,"margin":6,"borderRadius":6,"backgroundColor":"#4ade80"}},{"type":"Text","style":{"color":"#fff","fontSize":11},"text":"Shield"}]},
      {"style":{"height":24,"marginBottom":4,"borderRadius":6,"backgroundColor":"rgba(255,255,255,0.12)","flexDirection":"row","alignItems":"center"},
       "children":[{"style":{"width":12,"height":12,"margin":6,"borderRadius":6,"backgroundColor":"#f87171"}},{"type":"Text","style":{"color":"#fff","fontSize":11},"text":"Potion"}]},
      {"style":{"height":24,"borderRadius":6,"backgroundColor":"rgba(255,255,255,0.12)","flexDirection":"row","alignItems":"center"},
       "children":[{"style":{"width":12,"height":12,"margin":6,"borderRadius":6,"backgroundColor":"#a78bfa"}},{"type":"Text","style":{"color":"#fff","fontSize":11},"text":"Scroll"}]}]})J");
    panel = s.add(R"J({"style":{"position":"absolute","left":120,"top":12,"width":108,"height":60,"borderRadius":10,
      "backgroundColor":"#1d2442","borderWidth":1,"borderColor":"rgba(255,255,255,0.3)","opacity":0.4,"padding":8},"children":[
      {"type":"Text","style":{"color":"#ffcc00","fontSize":14},"text":"Paused"},
      {"style":{"marginTop":6,"height":8,"borderRadius":4,"backgroundColor":"#4ade80"}}]})J");
    card = s.add(R"J({"style":{"position":"absolute","left":130,"top":92,"width":88,"height":48,"borderRadius":12,
      "backgroundColor":"#24305e","borderWidth":1,"borderColor":"rgba(255,255,255,0.25)","opacity":0.9,
      "boxShadow":"0 4px 10px rgba(0,0,0,0.5)","flexDirection":"row","alignItems":"center","transformOrigin":"50% 50%"},"children":[
      {"style":{"width":20,"height":20,"margin":8,"borderRadius":10,"backgroundColor":"#f2c35b"}},
      {"type":"Text","style":{"color":"#fff","fontSize":14},"text":"Play"}]})J");
  }
  // Frame `i` of 36: the glow drifts every frame; the panel fades in over
  // frames 4-15; the card springs 1 -> 0.9 -> 1 over frames 18-29; the rest
  // is steady.
  void step(int i) {
    patch(glow, "{\"left\":" + std::to_string(10 + (i % 12) * 3) + ",\"top\":" + std::to_string(20 + (i % 7) * 4) + "}");
    if (i >= 4 && i < 16) patch(panel, "{\"opacity\":" + std::to_string(0.4 + 0.05 * (i - 3)) + "}");
    if (i >= 18 && i < 30) {
      int k = i - 17;  // 1..12
      double sc = k <= 6 ? 1 - 0.0167 * k : 0.9 + 0.0167 * (k - 6);
      if (k == 12) sc = 1;
      patch(card, R"J({"transform":[{"scale":)J" + std::to_string(sc) + "}]}");
    }
  }
  static bool moving(int i) { return i >= 18 && i < 30; }
};

struct Diff {
  double mean = 0;
  double over = 0;  // share of pixels off by more than tol
  int max = 0;
};
inline Diff compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int tol) {
  Diff d;
  uint64_t sum = 0, n = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int m = 0;
    for (int c = 0; c < 4; c++) {
      int v = std::abs(int((a[i] >> (c * 8)) & 255) - int((b[i] >> (c * 8)) & 255));
      sum += uint64_t(v);
      m = std::max(m, v);
    }
    d.max = std::max(d.max, m);
    if (m > tol) n++;
  }
  d.mean = double(sum) / double(a.size() * 4);
  d.over = double(n) / double(a.size());
  return d;
}

// Renders the scene's current frame into the host's target (`incremental`:
// the target holds the last frame) and returns the pixels read back, RGBA8
// premultiplied like the CPU raster's.
using RenderFn = std::function<std::vector<uint32_t>(jsui::Surface&, bool incremental, JsuiRect* dirty)>;
// jsui_gpu_purge(gpu, cacheBytes) on the backend's context.
using PurgeFn = std::function<void(size_t cacheBytes)>;

struct Totals {
  int layerDraws = 0, layerRenders = 0, motionDraws = 0, nodesPainted = 0, incremental = 0;
  double worstAtRest = 0, worstMoving = 0;  // share of pixels off the CPU frame by > 24
  void add(const jsui::PaintStats& s) {
    layerDraws += s.layerDraws;
    layerRenders += s.layerRenders;
    motionDraws += s.motionDraws;
    nodesPainted += s.nodesPainted;
  }
};

// One run of the 36 frames on the GPU (`frames`: each read back) next to
// the CPU raster's (`cpu`). Fails a frame that differs from the CPU one by
// more than the GPU backends do anyway (mean 1 level, 1% of pixels off by
// more than 24), and while the card scales from its texture by more than
// a resampled texture does (3% of the frame over 24: the card's edges and
// glyphs, ~1150 pixels of its ~4200).
inline Totals run(const char* name, const RenderFn& render, std::vector<std::vector<uint32_t>>* frames,
                  std::vector<std::vector<uint32_t>>* cpu, const PurgeFn& purge) {
  LayerScene g, c;
  Totals t;
  for (int i = 0; i < 36; i++) {
    g.step(i);
    c.step(i);
    if (i == 32) purge(0);  // every texture freed: rendered again
    JsuiRect dirty{};
    std::vector<uint32_t> out = render(g.s.rt->main(), i > 0, &dirty);
    jsui::PaintStats st = jsui::lastPaintStats(g.s.doc());
    t.add(st);
    if (i > 0 && dirty.width * dirty.height < 240 * 160) t.incremental++;
    std::vector<uint32_t> ref(size_t(240) * 160);
    c.s.rt->render(ref.data(), 240, 160, 240 * 4);
    Diff d = compare(out, ref, 24);
    const bool moving = LayerScene::moving(i);
    double& worst = moving ? t.worstMoving : t.worstAtRest;
    worst = std::max(worst, d.over);
    bool ok = moving ? d.over <= 0.03 : d.mean <= 1.0 && d.over <= 0.01;
    if (!ok) {
      std::fprintf(stderr, "  %s frame %d: GPU differs from CPU: mean %.3f, %.2f%% off by > 24, max %d (dirty %d,%d %dx%d)\n",
                   name, i, d.mean, d.over * 100, d.max, dirty.x, dirty.y, dirty.width, dirty.height);
      jsui_test::failures()++;
    }
    frames->push_back(std::move(out));
    cpu->push_back(std::move(ref));
  }
  return t;
}

// The whole check for one backend's context.
inline void checkMatchesCpu(const char* backend, const PurgeFn& purge, const RenderFn& render) {
  std::vector<std::vector<uint32_t>> on, off, cpuOn, cpuOff;
  Totals withCache = run(backend, render, &on, &cpuOn, purge);
  jsui::setGpuLayerCaches(false);
  Totals without = run(backend, render, &off, &cpuOff, purge);
  jsui::setGpuLayerCaches(true);
  std::printf("  %s: with the layer cache %d layer draws (%d rendered, %d in motion), %d nodes painted; without %d "
              "layer draws, %d nodes painted; %d incremental frames; off the CPU by > 24: at rest %.3f%% / %.3f%% "
              "(with / without), scaling %.3f%% / %.3f%%\n",
              backend, withCache.layerDraws, withCache.layerRenders, withCache.motionDraws, withCache.nodesPainted,
              without.layerDraws, without.nodesPainted, withCache.incremental, withCache.worstAtRest * 100,
              without.worstAtRest * 100, withCache.worstMoving * 100, without.worstMoving * 100);
  // The cache is installed and used: layers composited from textures
  // rendered once, the card drawn from its texture while it scales, the
  // list from its raster texture (fewer nodes painted).
  CHECK(withCache.layerDraws > 0);
  CHECK(withCache.layerRenders * 2 < withCache.layerDraws);
  CHECK(withCache.motionDraws >= 6);
  CHECK(withCache.nodesPainted < without.nodesPainted);
  CHECK_EQ(without.layerDraws, 0);
  CHECK(withCache.incremental >= 30);
  // At rest the two runs paint the same within the cache's compositing
  // rounding (a texture composited vs. drawn directly).
  for (int i : {2, 3, 16, 17, 31, 35}) {
    Diff d = compare(on[size_t(i)], off[size_t(i)], 24);
    if (d.over > 0.001) {
      std::fprintf(stderr, "  %s frame %d: with the cache differs from without: %.3f%% off by > 24 (max %d)\n", backend, i,
                   d.over * 100, d.max);
      jsui_test::failures()++;
    }
  }

  // A budget too small for any layer's texture: nothing is kept, every
  // layer is rendered where it is drawn, the frames still match.
  purge(16 << 10);
  std::vector<std::vector<uint32_t>> tight, cpuTight;
  Totals small = run("tight budget", render, &tight, &cpuTight, purge);
  purge(256u << 20);
  std::printf("  %s, 16 KB budget: %d layer draws, %d rendered\n", backend, small.layerDraws, small.layerRenders);
  CHECK(small.layerRenders * 2 >= small.layerDraws);
}

inline void checkMatchesCpu(const char* backend, JsuiGpuContext* gpu, const RenderFn& render) {
  checkMatchesCpu(backend, [gpu](size_t bytes) { jsui_gpu_purge(gpu, bytes); }, render);
}

}  // namespace gpu_layer_scene
