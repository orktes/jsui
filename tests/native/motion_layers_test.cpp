// Motion layers (painter.cpp, motionTransform): a cached layer or
// raster-cached subtree that keeps scaling or rotating is drawn from its
// texture under the change of matrix, and repainted exactly at rest. The
// LayerCache runs on GPU canvases; it works the same on a raster one, which
// these tests paint into.
#include <cmath>
#include <string>
#include <vector>

#include "render_util.h"
#include "test.h"

using namespace jsui;
using rtest::Scene;

namespace {

// As Animated / patchStyle does (an animation frame: the node's content
// version stays, see Node::contentVersion).
void patch(Node* n, const std::string& json) {
  std::string err;
  Value p = Value::parseJson(json, &err);
  n->patchStyle(p.asObject());
}

// A backdrop and a button-like card (rounded, shadowed, bordered, with text
// and an icon), `extra` added to the card's style.
std::string cardScene(const std::string& extra) {
  return R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,
    "experimental_backgroundImage":"linear-gradient(135deg, #0b1020, #24305e)"},"children":[
    {"style":{"position":"absolute","left":90,"top":80,"width":140,"height":72,"borderRadius":14,"backgroundColor":"#1d2442",
      "borderWidth":1,"borderColor":"rgba(255,255,255,0.25)","boxShadow":"0 6px 16px rgba(0,0,0,0.6)","flexDirection":"row",
      "alignItems":"center",)J" +
         extra + R"J("transformOrigin":"50% 50%"},"children":[
      {"style":{"width":24,"height":24,"margin":12,"borderRadius":12,"backgroundColor":"#f2c35b"}},
      {"type":"Text","style":{"color":"#fff","fontSize":18},"text":"Play"},
      {"style":{"width":10,"height":10,"marginLeft":10,"borderRadius":2,"backgroundColor":"#4ade80"}}]}]})J";
}

struct Pixels {
  size_t over = 0;  // pixels off by more than `tol` levels
  int max = 0;
};
Pixels compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int tol) {
  Pixels p;
  for (size_t k = 0; k < a.size(); k++) {
    int d = 0;
    for (int ch = 0; ch < 4; ch++)
      d = std::max(d, std::abs(int((a[k] >> (ch * 8)) & 255) - int((b[k] >> (ch * 8)) & 255)));
    p.max = std::max(p.max, d);
    if (d > tol) p.over++;
  }
  return p;
}

// One scene painted incrementally into a retained buffer, with or without a
// LayerCache and with motion layers on or off.
struct Run {
  Scene s{320, 240};
  Node* card = nullptr;
  std::unique_ptr<LayerCache> cache;
  bool motion = true;
  std::vector<uint32_t> px = std::vector<uint32_t>(320 * 240, 0);
  PaintStats last, sum;
  Run(const std::string& extra, bool layers, bool motionLayers) : motion(motionLayers) {
    if (layers) cache = std::make_unique<LayerCache>();
    card = s.add(cardScene(extra))->children()[0];
  }
  void frame() {
    LayerCacheScope scope(cache.get());
    setMotionLayers(motion);
    s.doc().layout();
    s.rt->renderIncremental(px.data(), 320, 240, 320 * 4);
    setMotionLayers(true);
    if (cache) cache->endFrame();
    last = lastPaintStats(s.doc());
    sum.layerDraws += last.layerDraws;
    sum.layerRenders += last.layerRenders;
    sum.motionDraws += last.motionDraws;
    sum.nodesPainted += last.nodesPainted;
  }
};

// Scales `extra`'s card from 1 down to 0.86 and up to 1.08, then rests:
// while it moves the motion layer is within `tolerance` of a direct paint,
// at rest it paints what the cache paints without motion layers (to the
// bit) and a direct paint up to the cache's rounding.
void scaleAndRest(const std::string& extra, const char* what, bool raster) {
  Run on(extra, true, true), off(extra, true, false), plain(extra, false, true);
  Run* runs[] = {&on, &off, &plain};
  auto all = [&](const std::string& style) {
    for (Run* r : runs) {
      if (!style.empty()) patch(r->card, style);
      r->frame();
    }
  };
  // At rest, damaged twice (the raster cache takes subtrees that damage
  // reaches in consecutive frames).
  all("");
  all("");
  std::vector<float> scales;
  for (int i = 1; i <= 7; i++) scales.push_back(1 - 0.02f * float(i));
  for (int i = 1; i <= 11; i++) scales.push_back(0.86f + 0.02f * float(i));
  size_t worstOver = 0;
  int worstMax = 0;
  for (float sc : scales) {
    all(R"J({"transform":[{"scale":)J" + std::to_string(sc) + "}]}");
    Pixels d = compare(on.px, plain.px, 48);
    worstOver = std::max(worstOver, d.over);
    worstMax = std::max(worstMax, d.max);
  }
  std::printf("  %s: %d motion draws, %d layer renders and %d nodes painted (%d and %d without motion layers); while "
              "scaling at most %zu pixels off a direct paint by more than 48 levels (max %d)\n",
              what, on.sum.motionDraws, on.sum.layerRenders, on.sum.nodesPainted, off.sum.layerRenders,
              off.sum.nodesPainted, worstOver, worstMax);
  CHECK(on.sum.motionDraws >= int(scales.size()) - 4);
  CHECK_EQ(off.sum.motionDraws, 0);
  // Without motion layers an opacity layer gets a new texture every frame
  // and a raster-cached subtree is painted node by node.
  if (raster) CHECK(on.sum.nodesPainted * 2 <= off.sum.nodesPainted);
  else CHECK(on.sum.layerRenders * 3 <= off.sum.layerRenders);
  // Resampled within 5/4 of its scale: the card is ~140x72 px; its edges
  // and glyphs (a few hundred pixels) differ, nothing by a whole colour.
  CHECK(worstOver <= 300);
  // Rest: the next frame repaints it exactly, and nothing after.
  all("");
  all("");
  CHECK_EQ(on.last.motionDraws, 0);
  Pixels same = compare(on.px, off.px, 0);
  if (same.over) std::fprintf(stderr, "  %s at rest: %zu pixels differ from the cache without motion layers (max %d)\n",
                              what, same.over, same.max);
  CHECK_EQ(same.over, size_t(0));
  Pixels direct = compare(on.px, plain.px, 2);
  if (direct.over) std::fprintf(stderr, "  %s at rest: %zu pixels off a direct paint (max %d)\n", what, direct.over,
                                direct.max);
  CHECK_EQ(direct.over, size_t(0));
  // A full repaint of the same frame equals the retained one.
  std::vector<uint32_t> full(320 * 240);
  {
    LayerCacheScope scope(on.cache.get());
    on.s.rt->render(full.data(), 320, 240, 320 * 4);
  }
  CHECK_EQ(compare(on.px, full, 0).over, size_t(0));
}

}  // namespace

// An opaque subtree (raster-cached) springing in scale.
TEST(motion_layers_raster_subtree_scaling) { scaleAndRest("", "raster subtree", true); }

// A translucent one (a cached opacity layer) springing in scale.
TEST(motion_layers_opacity_layer_scaling) { scaleAndRest(R"J("opacity":0.85,)J", "opacity layer", false); }

// Rotating: drawn rotated from its texture while it turns.
TEST(motion_layers_rotation) {
  Run on("", true, true), plain("", false, true);
  for (Run* r : {&on, &plain}) {
    r->frame();
    r->frame();
  }
  for (int deg = 2; deg <= 30; deg += 2)
    for (Run* r : {&on, &plain}) {
      patch(r->card, R"J({"transform":[{"rotate":")J" + std::to_string(deg) + R"J(deg"}]})J");
      r->frame();
    }
  CHECK(on.sum.motionDraws > 0);
  for (Run* r : {&on, &plain}) {
    r->frame();
    r->frame();
  }
  Pixels direct = compare(on.px, plain.px, 2);
  CHECK_EQ(direct.over, size_t(0));
}

// A one-off jump in scale is no motion: rendered again at the new scale.
TEST(motion_layers_not_for_one_off_changes) {
  Run on("", true, true);
  on.frame();
  on.frame();
  patch(on.card, R"J({"transform":[{"scale":0.9}]})J");
  on.frame();
  CHECK_EQ(on.last.motionDraws, 0);
  on.frame();
  CHECK_EQ(on.last.motionDraws, 0);
}

// A pop-in from 0.3 to 1: a new texture each time the scale leaves 1/2 to
// 5/4 of the last one's, a few in all, and exact at rest.
TEST(motion_layers_pop_in_renders_a_few_textures) {
  Run on("", true, true), plain("", false, true);
  for (Run* r : {&on, &plain}) {
    patch(r->card, R"J({"transform":[{"scale":0.3}]})J");
    r->frame();
    r->frame();
  }
  const int frames = 24;
  for (int i = 1; i <= frames; i++)
    for (Run* r : {&on, &plain}) {
      patch(r->card, R"J({"transform":[{"scale":)J" + std::to_string(0.3 + 0.7 * i / frames) + "}]}");
      r->frame();
    }
  std::printf("  pop-in: %d motion draws, %d layer renders in %d frames\n", on.sum.motionDraws, on.sum.layerRenders,
              frames);
  CHECK(on.sum.motionDraws >= frames / 2);
  CHECK(on.sum.layerRenders <= 8);
  for (Run* r : {&on, &plain}) {
    r->frame();
    r->frame();
  }
  CHECK_EQ(compare(on.px, plain.px, 2).over, size_t(0));
}

// Content changing while it scales (a counter ticking in a springing
// button): no motion layer, painted as it is.
TEST(motion_layers_not_while_the_content_changes) {
  Run on("", true, true), plain("", false, true);
  for (Run* r : {&on, &plain}) {
    r->frame();
    r->frame();
  }
  for (int i = 1; i <= 8; i++)
    for (Run* r : {&on, &plain}) {
      patch(r->card, R"J({"transform":[{"scale":)J" + std::to_string(1 - 0.02 * i) + "}]}");
      r->card->children()[1]->children()[0]->setText("Play " + std::to_string(i));
      r->frame();
    }
  CHECK_EQ(on.sum.motionDraws, 0);
  CHECK_EQ(compare(on.px, plain.px, 2).over, size_t(0));
}
