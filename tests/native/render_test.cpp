// Painter tests: build trees from JSON, paint, check pixels at key points.
// Set JSUI_TEST_OUT=/some/dir to also write the frames as PNGs.
#include <chrono>
#include <thread>

#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "render/image_cache.h"
#include "render_util.h"
#include "test.h"
#include "text/text_layout.h"

using namespace jsui;
using rtest::Scene;

namespace {

std::string base64(const void* data, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out += t[(v >> 18) & 63];
    out += t[(v >> 12) & 63];
    out += i + 1 < n ? t[(v >> 6) & 63] : '=';
    out += i + 2 < n ? t[v & 63] : '=';
  }
  return out;
}

// A w x h PNG: left half red, right half blue (opaque), as a data: URI.
std::string testPngUri(int w, int h) {
  SkBitmap bm;
  bm.allocN32Pixels(w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) *bm.getAddr32(x, y) = x < w / 2 ? SkPreMultiplyColor(SK_ColorRED) : SkPreMultiplyColor(SK_ColorBLUE);
  SkDynamicMemoryWStream s;
  SkPngEncoder::Encode(&s, bm.pixmap(), {});
  sk_sp<SkData> d = s.detachAsData();
  return "data:image/png;base64," + base64(d->data(), d->size());
}

}  // namespace

TEST(render_background_and_radius) {
  Scene s(100, 60);
  s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":80,"height":40,"backgroundColor":"#3366ff","borderRadius":12}})J");
  s.render();
  s.save("background_radius");
  CHECK_EQ(s.r(50, 30), 0x33);
  CHECK_EQ(s.g(50, 30), 0x66);
  CHECK_EQ(s.b(50, 30), 0xff);
  CHECK_EQ(s.a(50, 30), 255);
  CHECK_EQ(s.a(5, 5), 0);
  // The corner pixel is outside the rounded corner; the edge midpoint is inside.
  CHECK_EQ(s.a(10, 10), 0);
  CHECK_EQ(s.a(10, 30), 255);
  // Antialiased corner: some pixel along the diagonal is partially covered.
  bool partial = false;
  for (int i = 0; i < 6; i++) {
    int a = s.a(10 + i, 10 + i);
    partial |= a > 0 && a < 255;
  }
  CHECK(partial);
}

TEST(render_box_shadow_falloff) {
  Scene s(200, 200);
  s.add(R"J({"style":{"position":"absolute","left":50,"top":50,"width":100,"height":100,"backgroundColor":"white",
            "boxShadow":"0 0 20px rgba(0,0,0,1)"}})J");
  s.render();
  s.save("box_shadow");
  // Inside: white. Right at the edge the shadow is ~50%, 20px out nearly 0.
  CHECK_EQ(s.r(100, 100), 255);
  int atEdge = s.a(151, 100), mid = s.a(160, 100), far = s.a(172, 100);
  CHECK(atEdge > 100 && atEdge < 160);  // Gaussian with sigma 10: ~46% at the edge
  CHECK(mid < atEdge && mid > far);
  CHECK(far < 30);
  CHECK_EQ(s.a(195, 100), 0);
}

TEST(render_shadow_clipped_under_translucent_bg) {
  Scene s(120, 120);
  s.add(R"J({"style":{"position":"absolute","left":20,"top":20,"width":80,"height":80,"backgroundColor":"rgba(255,0,0,0.5)",
            "shadowColor":"#000","shadowOffset":{"width":0,"height":0},"shadowOpacity":1,"shadowRadius":10}})J");
  s.render();
  s.save("shadow_translucent");
  // Center shows only the translucent red, not the shadow beneath.
  CHECK_EQ(s.a(60, 60), 128);
  CHECK_EQ(s.r(60, 60), 128);
  CHECK_EQ(s.g(60, 60), 0);
  CHECK(s.a(102, 60) > 40);  // shadow outside
}

TEST(render_inset_shadow) {
  Scene s(100, 100);
  s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":80,"height":80,"backgroundColor":"white",
            "boxShadow":"inset 0 0 0 10px red"}})J");
  s.render();
  CHECK_EQ(s.r(15, 50), 255);
  CHECK_EQ(s.g(15, 50), 0);
  CHECK_EQ(s.g(50, 50), 255);
}

TEST(render_linear_gradient_midpoint) {
  Scene s(200, 50);
  s.add(R"J({"style":{"width":200,"height":50,"experimental_backgroundImage":"linear-gradient(to right, #000000, #ffffff)"}})J");
  s.render();
  s.save("gradient_linear");
  CHECK(s.r(1, 25) < 5);
  CHECK(s.r(198, 25) > 250);
  CHECK_NEAR(s.r(100, 25), 128, 3);
  // Two stops at the same spot make a hard edge.
  Scene h(100, 10);
  h.add(R"J({"style":{"width":100,"height":10,"backgroundImage":"linear-gradient(90deg, red 50%, blue 50%)"}})J");
  h.render();
  CHECK_EQ(h.r(48, 5), 255);
  CHECK_EQ(h.b(52, 5), 255);
}

TEST(render_linear_gradient_corner_and_repeat) {
  Scene s(100, 100);
  s.add(R"J({"style":{"width":100,"height":100,"backgroundImage":"repeating-linear-gradient(90deg, red 0px, red 10px, blue 10px, blue 20px)"}})J");
  s.render();
  CHECK_EQ(s.r(5, 50), 255);
  CHECK_EQ(s.b(15, 50), 255);
  CHECK_EQ(s.r(45, 50), 255);
  CHECK_EQ(s.b(55, 50), 255);
  Scene c(200, 100);
  c.add(R"J({"style":{"width":200,"height":100,"backgroundImage":"linear-gradient(to bottom right, black 50%, white 50%)"}})J");
  c.render();
  c.save("gradient_corner");
  // With "to bottom right" the 50% line is the other diagonal (top-right to bottom-left).
  CHECK(c.r(20, 20) < 10);
  CHECK(c.r(180, 80) > 245);
  CHECK(c.r(176, 10) < 10);  // the edge crosses y=10 at x=179
  CHECK(c.r(183, 10) > 245);
}

TEST(render_radial_gradient) {
  Scene s(100, 100);
  s.add(R"J({"style":{"width":100,"height":100,"backgroundImage":"radial-gradient(circle closest-side, white, black)"}})J");
  s.render();
  s.save("gradient_radial");
  CHECK(s.r(50, 50) > 250);
  CHECK_NEAR(s.r(75, 50), 128, 4);
  CHECK(s.r(99, 50) < 5);
  CHECK(s.r(2, 2) < 5);
}

TEST(render_borders) {
  Scene s(120, 120);
  s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":100,"height":100,"backgroundColor":"white",
            "borderWidth":10,"borderTopColor":"red","borderRightColor":"lime","borderBottomColor":"blue","borderLeftColor":"black"}})J");
  s.render();
  s.save("borders_mixed");
  CHECK_EQ(s.r(60, 15), 255);
  CHECK_EQ(s.g(105, 60), 255);
  CHECK_EQ(s.b(60, 105), 255);
  CHECK_EQ(s.r(15, 60), 0);
  CHECK_EQ(s.a(15, 60), 255);
  CHECK_EQ(s.g(60, 60), 255);
  CHECK_EQ(s.r(60, 60), 255);
  // Rounded uniform border is a ring: transparent inside if no background.
  Scene r(100, 100);
  r.add(R"J({"style":{"width":100,"height":100,"borderRadius":50,"borderWidth":4,"borderColor":"#000"}})J");
  r.render();
  r.save("border_ring");
  CHECK_EQ(r.a(50, 50), 0);
  CHECK_EQ(r.a(50, 2), 255);
  CHECK_EQ(r.a(2, 2), 0);
}

TEST(render_dashed_border) {
  Scene s(100, 40);
  s.add(R"J({"style":{"width":100,"height":40,"borderWidth":2,"borderColor":"black","borderStyle":"dashed"}})J");
  s.render();
  s.save("border_dashed");
  int on = 0, off = 0;
  for (int x = 0; x < 100; x++) (s.a(x, 0) > 128 ? on : off)++;
  CHECK(on > 30 && off > 30);
  CHECK_EQ(s.a(0, 0), 255);   // dashes start at the corners
  CHECK_EQ(s.a(99, 0), 255);
}

TEST(render_opacity_layer_and_overflow_clip) {
  Scene s(100, 100);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":60,"height":60,"opacity":0.5,"backgroundColor":"red","overflow":"hidden","borderRadius":30},
            "children":[{"style":{"width":100,"height":100,"backgroundColor":"blue","margin":-10}}]})J");
  s.render();
  s.save("opacity_clip");
  // Child painted inside the parent's layer: blue at half alpha, clipped to the circle.
  CHECK_EQ(s.a(30, 30), 128);
  CHECK_EQ(s.b(30, 30), 128);
  CHECK_EQ(s.r(30, 30), 0);
  CHECK_EQ(s.a(2, 2), 0);
  CHECK_EQ(s.a(80, 30), 0);
}

TEST(render_transform_and_zindex) {
  Scene s(100, 100);
  s.add(R"J({"style":{"width":100,"height":100},"children":[
     {"style":{"position":"absolute","left":0,"top":0,"width":50,"height":50,"backgroundColor":"red","zIndex":2}},
     {"style":{"position":"absolute","left":25,"top":25,"width":50,"height":50,"backgroundColor":"blue"}},
     {"style":{"position":"absolute","left":0,"top":60,"width":20,"height":20,"backgroundColor":"lime","transform":[{"translateX":60},{"scale":2}]}}
  ]})J");
  s.render();
  s.save("transform_zindex");
  CHECK_EQ(s.r(40, 40), 255);  // red (zIndex 2) above blue despite document order
  CHECK_EQ(s.b(60, 40), 255);
  // Lime square scaled 2x about its center (10,70) then moved +60: covers x 50..90, y 50..90.
  CHECK_EQ(s.g(85, 85), 255);
  CHECK_EQ(s.g(52, 52), 255);
  CHECK_EQ(s.g(92, 70), 0);
  CHECK_EQ(s.g(10, 70), 0);
  // rotate 90deg keeps a square in place
  Scene r(40, 40);
  r.add(R"J({"style":{"position":"absolute","left":10,"top":0,"width":20,"height":40,"backgroundColor":"black","transform":"rotate(90deg)"}})J");
  r.render();
  CHECK_EQ(r.a(2, 20), 255);
  CHECK_EQ(r.a(20, 2), 0);
}

TEST(render_text_draws_and_measures) {
  Scene s(300, 120);
  Node* t = s.add(R"J({"type":"Text","style":{"alignSelf":"flex-start","fontSize":24,"color":"black"},"text":"Hello JSUI"})J");
  Node* bold = s.add(R"J({"type":"Text","style":{"alignSelf":"flex-start","fontSize":24,"fontWeight":"700","color":"black"},"text":"Hello JSUI"})J");
  s.render();
  s.save("text_basic");
  Rect r = t->layoutRect();
  CHECK(r.width > 100 && r.width < 160);
  CHECK(r.height >= 28 && r.height <= 30);  // Inter normal line height ~1.21em
  Rect rb = bold->layoutRect();
  CHECK(rb.width > r.width);  // bold is wider: the 700 face was picked
  CHECK(s.darkPixels(0, 0, int(r.width), int(r.height)) > 150);
  CHECK_EQ(s.darkPixels(0, 70, 300, 120), 0);
}

TEST(render_text_wraps_lines_and_ellipsis) {
  Scene s(200, 200);
  Node* t = s.add(R"J({"type":"Text","style":{"width":120,"fontSize":16,"lineHeight":20},"text":"The quick brown fox jumps over the lazy dog"})J");
  Node* e = s.add(R"J({"type":"Text","props":{"numberOfLines":1},"style":{"width":120,"fontSize":16},"text":"The quick brown fox jumps over the lazy dog"})J");
  s.render();
  s.save("text_wrap");
  Rect r = t->layoutRect();
  CHECK(r.height >= 60 && r.height <= 80);
  CHECK_NEAR(std::fmod(r.height, 20.f), 0, 0.01);  // whole lines of exactly 20px
  Rect re = e->layoutRect();
  CHECK(re.height > 15 && re.height < 22);
}

TEST(render_text_spans_inherit) {
  Scene s(300, 60);
  s.add(R"J({"type":"Text","style":{"fontSize":20,"color":"red"},"children":["Red ",
      {"type":"Text","style":{"color":"blue","fontWeight":"bold"},"text":"Blue"}]})J");
  s.render();
  s.save("text_spans");
  int red = 0, blue = 0;
  for (int y = 0; y < 30; y++)
    for (int x = 0; x < 300; x++) {
      if (s.a(x, y) > 200 && s.r(x, y) > 200 && s.b(x, y) < 40) red++;
      if (s.a(x, y) > 200 && s.b(x, y) > 200 && s.r(x, y) < 40) blue++;
    }
  CHECK(red > 40);
  CHECK(blue > 40);
}

TEST(render_text_rtl_auto_direction) {
  Scene s(200, 40);
  // Hebrew: dir="auto" makes the paragraph RTL, so start alignment is right.
  Node* t = s.add(R"J({"type":"Text","style":{"width":200,"fontSize":16},"text":"שלום"})J");
  s.render();
  (void)t;
  // Inter has no Hebrew; fallback fonts may be missing on the machine, so
  // only check that nothing was drawn at the far left when glyphs exist.
  int left = s.darkPixels(0, 0, 60, 30, 200), right = s.darkPixels(140, 0, 200, 30, 200);
  CHECK(left == 0 || right > left);
}

TEST(render_image_data_uri_and_resize_modes) {
  std::string uri = testPngUri(40, 20);
  Scene s(200, 100);
  Node* intrinsic = s.add(R"J({"type":"Image","style":{"alignSelf":"flex-start"},"props":{"source":{"uri":")J" + uri + R"J("}}})J");
  s.add(R"J({"type":"Image","props":{"resizeMode":"contain","source":{"uri":")J" + uri +
        R"J("}},"style":{"position":"absolute","left":100,"top":0,"width":100,"height":100,"backgroundColor":"#00ff00"}})J");
  s.add(R"J({"type":"Image","props":{"source":")J" + uri +
        R"J("},"style":{"position":"absolute","left":0,"top":50,"width":50,"height":50,"borderRadius":25,"tintColor":"black"}})J");
  s.render();
  s.save("image");
  Rect r = intrinsic->layoutRect();
  CHECK_NEAR(r.width, 40, 0.01);
  CHECK_NEAR(r.height, 20, 0.01);
  CHECK_EQ(s.r(5, 10), 255);
  CHECK_EQ(s.b(35, 10), 255);
  // contain: 100x50 centered vertically, background visible above/below.
  CHECK_EQ(s.g(150, 10), 255);
  CHECK_EQ(s.r(120, 50), 255);
  CHECK_EQ(s.b(180, 50), 255);
  // cover + tint + round clip: black inside the circle, nothing at the corner.
  CHECK_EQ(s.a(25, 75), 255);
  CHECK_EQ(s.r(25, 75), 0);
  CHECK_EQ(s.a(1, 51), 0);
}

// Large images decode on the pool, not in the layout that asked: that frame
// shows nothing (and wants another), and a later tick publishes the image,
// re-measures the Image and repaints it.
TEST(render_image_decodes_off_the_frame_thread) {
  std::string uri = testPngUri(512, 256);
  Scene s(600, 300);
  ImageCache& images = *s.rt->images;
  CHECK(int64_t(512) * 256 >= images.asyncMinPixels);
  Node* img = s.add(R"J({"type":"Image","style":{"alignSelf":"flex-start"},"props":{"source":{"uri":")J" + uri + R"J("}}})J");
  s.render();
  ImageCache::Info info;
  CHECK(images.peek(uri, &info));
  CHECK(info.state == ImageCache::State::Loading);
  CHECK_EQ(images.pendingDecodes(), 1);
  CHECK(s.rt->wantsFrame());  // a host keeps ticking until it lands
  CHECK_NEAR(img->layoutRect().width, 0, 0.01);
  CHECK_EQ(s.a(5, 10), 0);
  // Asking again while it decodes does not start a second decode.
  images.get(uri);
  CHECK_EQ(images.pendingDecodes(), 1);
  uint32_t gen = images.generation;
  for (int i = 0; i < 5000 && images.pendingDecodes() > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    s.render();
  }
  CHECK_EQ(images.pendingDecodes(), 0);
  CHECK_EQ(images.asyncDecodes, 1u);
  CHECK(images.generation != gen);
  s.render();
  CHECK(images.peek(uri, &info));
  CHECK(info.state == ImageCache::State::Ready);
  CHECK_NEAR(img->layoutRect().width, 512, 0.01);
  CHECK_NEAR(img->layoutRect().height, 256, 0.01);
  CHECK_EQ(s.r(5, 10), 255);
  CHECK_EQ(s.b(500, 10), 255);

  // Cleared while decoding: the stale result is dropped, the next request
  // decodes again.
  images.clear();
  images.get(uri);
  CHECK_EQ(images.pendingDecodes(), 1);
  images.clear();
  images.finishDecodes();
  CHECK_EQ(images.pendingDecodes(), 0);
  CHECK(!images.peek(uri, &info));

  // Below the threshold an image still decodes inline.
  std::string small = testPngUri(40, 20);
  ImageCache::State st;
  CHECK(images.get(small, &st) != nullptr);
  CHECK(st == ImageCache::State::Ready);
}

TEST(render_scrollview_offset_and_clip) {
  Scene s(100, 100);
  Node* sv = s.add(R"J({"type":"ScrollView","style":{"width":100,"height":50},"children":[
      {"style":{"height":50,"backgroundColor":"red"}},{"style":{"height":50,"backgroundColor":"blue"}}]})J");
  sv->scrollY = 50;
  s.render();
  CHECK_EQ(s.b(50, 25), 255);
  CHECK_EQ(s.a(50, 75), 0);  // clipped
}

TEST(render_hidpi_scale) {
  Scene s(50, 50, 2);
  s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":10,"height":10,"backgroundColor":"black"}})J");
  s.render();
  CHECK_EQ(s.a(20, 20), 255);
  CHECK_EQ(s.a(39, 39), 255);
  CHECK_EQ(s.a(41, 41), 0);
}

namespace {
// Applies a style patch like Animated / patchStyle does.
void patch(Node* n, const std::string& json) {
  std::string err;
  Value p = Value::parseJson(json, &err);
  Value::Object style = n->props()["style"].asObject();
  for (auto& [k, v] : p.asObject()) style[k] = v;
  Value::Object props = n->props().asObject();
  props["style"] = Value(style);
  n->setProps(Value(props));
}

using rtest::kExactUnderClip;
using rtest::matchesFull;
using rtest::PixelDiff;
using rtest::pixelDiff;

// Failure details for frames that should match: the repainted bounds and
// the first few differing pixels (RGBA, incremental vs full), so a CI log
// from another platform says where and by how much.
void describeDiff(const std::vector<uint32_t>& inc, const std::vector<uint32_t>& full, int w, const SkIRect& damage) {
  std::fprintf(stderr, "    repainted bounds [%d,%d %dx%d]:", damage.left(), damage.top(), damage.width(), damage.height());
  int shown = 0, worst = 0;
  for (size_t k = 0; k < inc.size(); k++) {
    if (inc[k] == full[k]) continue;
    for (int ch = 0; ch < 4; ch++)
      worst = std::max(worst, std::abs(int((inc[k] >> (ch * 8)) & 255) - int((full[k] >> (ch * 8)) & 255)));
    if (shown++ < 4) std::fprintf(stderr, " (%d,%d) %08x vs %08x", int(k % w), int(k / w), inc[k], full[k]);
  }
  std::fprintf(stderr, "; max diff %d\n", worst);
}
}  // namespace

// Incremental frames (only the damage repainted) must equal full repaints,
// pixel for pixel, across the kinds of change a UI makes.
TEST(render_incremental_matches_full) {
  Scene s(320, 240);
  Node* panel = s.add(R"J({"style":{"position":"absolute","left":20,"top":20,"width":200,"height":160,"borderRadius":16,"overflow":"hidden",
    "backgroundColor":"#1d2442","boxShadow":"0 6px 18px rgba(0,0,0,0.6)"},"children":[
    {"style":{"position":"absolute","left":30,"top":30,"width":12,"height":12,"borderRadius":6,"backgroundColor":"#f2c35b"}},
    {"style":{"position":"absolute","left":96,"top":20,"width":4,"height":60,"borderRadius":2,"backgroundColor":"#ff4d6a","transformOrigin":"50% 100%"}},
    {"type":"Text","style":{"position":"absolute","left":10,"top":120,"color":"#fff","fontSize":14},"text":"Score 100"},
    {"type":"ScrollView","style":{"position":"absolute","left":120,"top":90,"width":70,"height":60,"backgroundColor":"#11162a"},"children":[
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#4ade80"}},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#4fd1ff"}}]}]})J");
  Node* other = s.add(R"J({"style":{"position":"absolute","left":240,"top":30,"width":60,"height":60,"borderRadius":30,"backgroundColor":"#b26bff","opacity":0.8}})J");
  // A fixed-size text whose padding is a percentage of its parent's width,
  // and a clipped column of rows below a counter (rows laid out from Yoga's
  // cache when the counter changes). Their damage (a text's ink bounds reach
  // ~13 px beyond its box) stays clear of the panel's rounded outline: damage
  // touching it repaints the whole panel under a non-rectangular clip, which
  // Skia on macOS arm64 blends a level apart from the full frame on a few
  // edge pixels (see kExactUnderClip; render_incremental_rounded_boxes_*
  // covers that case).
  Node* padBox = s.add(R"J({"style":{"position":"absolute","left":10,"top":200,"width":100,"height":36,"backgroundColor":"#26304f"},"children":[
    {"type":"Text","style":{"width":80,"height":30,"paddingLeft":"20%","color":"#fff","fontSize":12},"text":"Pad"}]})J");
  Node* column = s.add(R"J({"style":{"position":"absolute","left":240,"top":110,"width":70,"height":60,"overflow":"hidden"},"children":[
    {"style":{"flexDirection":"row"},"children":[{"type":"Text","style":{"color":"#fff","fontSize":10},"text":"Frame 1"}]},
    {"style":{"height":12,"backgroundColor":"#4ade80"}},{"style":{"height":12,"backgroundColor":"#4fd1ff"}},
    {"style":{"height":12,"backgroundColor":"#b26bff"}},{"style":{"height":12,"backgroundColor":"#f2c35b"}}]})J");
  Node* needle = panel->children()[1];
  Node* label = panel->children()[2];
  Node* scroll = panel->children()[3];
  std::vector<uint32_t> inc(320 * 240), full(320 * 240);
  int step = 0;
  SkIRect damage;
  auto check = [&](const char* what) {
    s.doc().layout();
    damage = s.rt->renderIncremental(inc.data(), 320, 240, 320 * 4);
    // Painted where updateDamage placed each node, without placing it again.
    CHECK_EQ(lastPaintStats(s.doc()).placements, 0);
    s.rt->render(full.data(), 320, 240, 320 * 4);
    size_t diff = 0;
    for (size_t i = 0; i < inc.size(); i++) diff += inc[i] != full[i];
    if (diff) {
      std::fprintf(stderr, "  step %d (%s): %zu pixels differ\n", step, what, diff);
      describeDiff(inc, full, 320, damage);
    }
    CHECK_EQ(diff, size_t(0));
    // render() painted elsewhere: bring `inc` back in step (a full repaint).
    s.rt->renderIncremental(inc.data(), 320, 240, 320 * 4);
    step++;
  };
  check("first frame");
  for (int i = 0; i < 6; i++) {
    patch(needle, R"J({"transform":[{"rotate":")J" + std::to_string(i * 23) + R"J(deg"}]})J");
    check("rotate");
  }
  patch(panel->children()[0], R"J({"left":60,"top":50})J");
  check("move (layout)");
  patch(panel->children()[0], R"J({"backgroundColor":"#ff0000"})J");
  check("color");
  label->children()[0]->setText("Score 12345");
  check("text");
  scroll->scrollY = 12;
  scroll->markPaintDirty();
  check("scroll");
  patch(other, R"J({"opacity":0.3})J");
  check("opacity");
  patch(other, R"J({"display":"none"})J");
  check("hide");
  panel->removeChild(scroll);
  check("remove");
  // The text keeps its frame; only its padding (from its parent) changes.
  const SkIRect panelBox = SkIRect::MakeLTRB(19, 19, 221, 181);
  patch(padBox, R"J({"width":180})J");
  check("percent padding");
  CHECK(!damage.isEmpty() && !SkIRect::Intersects(damage, panelBox));
  column->children()[0]->children()[0]->children()[0]->setText("Frame 22");
  check("counter over cached rows");
  CHECK(!damage.isEmpty() && !SkIRect::Intersects(damage, panelBox));
  // The counter's row grows: the rows below move.
  patch(column->children()[0], R"J({"height":20})J");
  check("rows move");
  CHECK(!damage.isEmpty() && !SkIRect::Intersects(damage, panelBox));
  // Nothing changed: nothing repainted.
  s.doc().layout();
  SkIRect r = s.rt->renderIncremental(inc.data(), 320, 240, 320 * 4);
  CHECK(r.isEmpty());
}

// updateDamage skips subtrees in which nothing changed: the damage must be
// what walking every node finds, whatever changes deep inside a subtree
// that was skipped, while a box animating in front leaves the rest alone.
TEST(render_skipped_subtrees_damage_like_a_full_walk) {
  const char* tree = R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240},"children":[
    {"style":{"position":"absolute","left":10,"top":10,"width":140,"height":100,"borderRadius":12,"backgroundColor":"#1d2442"},"children":[
      {"type":"Text","style":{"position":"absolute","left":10,"top":8,"color":"#fff","fontSize":12},"text":"Title"},
      {"style":{"position":"absolute","left":10,"top":40,"width":60,"height":20,"borderRadius":4,"backgroundColor":"#4ade80"},"children":[
        {"style":{"position":"absolute","left":4,"top":4,"width":12,"height":12,"borderRadius":6,"backgroundColor":"#f2c35b"}}]}]},
    {"style":{"position":"absolute","left":170,"top":10,"width":140,"height":100,"backgroundColor":"#26304f"},"children":[
      {"style":{"margin":8,"height":20,"backgroundColor":"#ff4d6a"}},
      {"style":{"margin":8,"height":20,"backgroundColor":"#4fd1ff"},"children":[
        {"style":{"width":"50%","height":10,"backgroundColor":"#ffffff"}}]}]},
    {"type":"ScrollView","style":{"position":"absolute","left":10,"top":130,"width":140,"height":60,"backgroundColor":"#11162a"},"children":[
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#4ade80"}},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#4fd1ff"}},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#b26bff"},"children":[
        {"type":"Text","style":{"color":"#fff","fontSize":12},"text":"Row 3"}]},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#f2c35b"}},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#ff4d6a"},"children":[
        {"type":"Text","style":{"color":"#fff","fontSize":12},"text":"Row 5"}]},
      {"style":{"height":30,"margin":4,"borderRadius":5,"backgroundColor":"#4ade80"}}]},
    {"style":{"position":"absolute","left":170,"top":130,"width":140,"height":100,"backgroundColor":"#302040"},"children":[
      {"style":{"position":"absolute","left":10,"top":10,"width":60,"height":30,"backgroundColor":"#b26bff"},"children":[
        {"style":{"position":"absolute","left":5,"top":5,"width":20,"height":20,"backgroundColor":"#ffffff"}}]},
      {"type":"Text","style":{"position":"absolute","left":0,"top":50,"width":120,"height":30,"paddingLeft":"10%","color":"#fff","fontSize":12},"text":"Padded"}]},
    {"style":{"position":"absolute","left":0,"top":200,"width":30,"height":30,"borderRadius":15,"backgroundColor":"#f2c35b"}}]})J";
  Scene a(320, 240), b(320, 240);
  Node* ra = a.add(tree);
  Node* rb = b.add(tree);
  int step = 0, skippedSteps = 0;
  auto frame = [&]() {
    a.doc().layout();
    b.doc().layout();
    SkRegion da = updateDamage(a.doc(), 320, 240);
    int visitedA = lastPaintStats(a.doc()).nodesUpdated;
    setRecordSkipping(false);
    SkRegion db = updateDamage(b.doc(), 320, 240);
    int visitedB = lastPaintStats(b.doc()).nodesUpdated;
    setRecordSkipping(true);
    if (da != db) {
      SkIRect ba = da.getBounds(), bb = db.getBounds();
      std::fprintf(stderr, "  step %d: damage [%d,%d %dx%d] with skips, [%d,%d %dx%d] walking all\n", step, ba.left(),
                   ba.top(), ba.width(), ba.height(), bb.left(), bb.top(), bb.width(), bb.height());
    }
    CHECK(da == db);
    if (visitedA < visitedB) skippedSteps++;
    step++;
  };
  // The same change to both trees (the node found by child indices).
  auto both = [&](std::initializer_list<int> path, auto&& fn) {
    Node* na = ra;
    Node* nb = rb;
    for (int i : path) {
      na = na->children()[size_t(i)];
      nb = nb->children()[size_t(i)];
    }
    fn(na, ra);
    fn(nb, rb);
  };
  for (int i = 0; i < 44; i++) {
    both({4}, [&](Node* n, Node*) { patch(n, R"J({"transform":[{"translateX":)J" + std::to_string(i % 7) + "}]}"); });
    switch (i) {
      case 5: both({0, 1, 0}, [](Node* n, Node*) { patch(n, R"J({"backgroundColor":"#ff0000"})J"); }); break;
      case 8: both({1, 1, 0}, [](Node* n, Node*) { patch(n, R"J({"width":"80%"})J"); }); break;
      case 11: both({0, 0}, [](Node* n, Node*) { n->children()[0]->setText("A longer title"); }); break;
      case 14:
        both({2}, [](Node* n, Node*) {
          n->scrollY = 9;
          n->markBoundsDirty();
        });
        break;
      case 17: both({3, 0}, [](Node* n, Node*) { patch(n, R"J({"opacity":0})J"); }); break;
      case 18: both({3, 0, 0}, [](Node* n, Node*) { patch(n, R"J({"backgroundColor":"#00ff00","left":15})J"); }); break;
      case 21: both({3, 0}, [](Node* n, Node*) { patch(n, R"J({"opacity":1})J"); }); break;
      case 24: both({3}, [](Node* n, Node*) { patch(n, R"J({"width":120})J"); }); break;  // the Text's padding
      case 27:  // a subtree moves to another parent
        both({0, 1}, [](Node* n, Node* root) { root->children()[1]->children()[0]->appendChild(n->children()[0]); });
        break;
      case 30: both({1}, [](Node* n, Node*) { n->removeChild(n->children()[0]); }); break;
      case 33: both({0}, [](Node* n, Node*) { patch(n, R"J({"left":12})J"); }); break;
      default: break;
    }
    if (i >= 35) {
      // Scrolling: rows move out of view and back in (skipped while out).
      both({2}, [&](Node* n, Node*) {
        n->scrollY = float((i - 35) % 3 == 2 ? 20 : 9 + 61 * ((i - 35) % 3)) + 0.5f * float(i % 2);
        n->markBoundsDirty();
      });
    }
    if (i == 38) both({2, 4, 0}, [](Node* n, Node*) { n->children()[0]->setText("Row 5!"); });  // out of view
    frame();
  }
  // In most frames only the glow changed: the other subtrees were skipped.
  CHECK(skippedSteps > 30);
  // Children out of view (rows scrolled out of the list, an invisible box)
  // do not keep their ancestors from being skipped: once the rest settles,
  // only the glow and the two nodes above it are visited (the list, its
  // rows out of view since the scroll, stays skipped too).
  both({3, 0}, [](Node* n, Node*) { patch(n, R"J({"opacity":0})J"); });
  int visited = 0;
  for (int i = 0; i < 6; i++) {
    both({4}, [&](Node* n, Node*) { patch(n, R"J({"transform":[{"translateX":)J" + std::to_string(i) + "}]}"); });
    frame();
    visited = lastPaintStats(a.doc()).nodesUpdated;
  }
  std::printf("  settled: %d nodes visited per frame\n", visited);
  CHECK(visited <= 3);
  // A change inside a subtree skipped for all those frames walks only its
  // path: the clean children on it are skipped again (the rounded box next
  // to the title, its curved outline carried over from where its skipped
  // parent copied it).
  both({0, 0}, [](Node* n, Node*) { n->children()[0]->setText("Title!"); });
  frame();
  visited = lastPaintStats(a.doc()).nodesUpdated;
  std::printf("  deep change: %d nodes visited\n", visited);
  CHECK(visited <= 5);
  both({0, 1}, [](Node* n, Node*) { patch(n, R"J({"top":44})J"); });
  frame();
}

// Damage cutting through rounded boxes at random places (a small box moving
// over them, also translucent and under a scale) must still repaint exactly
// like a full frame. Perf guards: damage inside a rounded box's padding box
// repaints only itself; crossing edges repaints the boxes crossed, never
// the frame.
TEST(render_incremental_rounded_boxes_cut_anywhere) {
  for (float dpr : {1.f, 1.5f}) {
    Scene s(400, 260, dpr);
    s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":130,"height":80,"borderRadius":18,"borderWidth":1.5,
      "borderColor":"#8fa0ff","backgroundColor":"rgba(30,40,80,0.9)","overflow":"hidden"},"children":[
      {"style":{"position":"absolute","left":-10,"top":40,"width":200,"height":30,"backgroundColor":"#f2c35b"}}]})J");
    s.add(R"J({"style":{"position":"absolute","left":150,"top":12.5,"width":120,"height":70,"borderTopLeftRadius":30,
      "borderBottomRightRadius":"40%","experimental_backgroundImage":"linear-gradient(90deg, #ff4d6a, #4fd1ff)"}})J");
    s.add(R"J({"style":{"position":"absolute","left":20,"top":105,"width":110,"height":80,"borderRadius":40,"borderWidth":4,
      "borderColor":"#4ade80","transform":[{"scale":1.035}]}})J");
    s.add(R"J({"style":{"position":"absolute","left":160,"top":100,"width":120,"height":90,"borderRadius":12,"backgroundColor":"#222",
      "boxShadow":"0 4px 10px rgba(0,0,0,0.6)"},"children":[{"style":{"margin":10,"height":50,"borderRadius":25,"backgroundColor":"#b26bff"}}]})J");
    s.add(R"J({"style":{"position":"absolute","left":290,"top":20,"width":90,"height":60,"borderRadius":10,"borderWidth":3,
      "borderStyle":"dashed","borderColor":"#fff","transform":[{"rotate":"12deg"}]}})J");
    s.add(R"J({"style":{"position":"absolute","left":295,"top":110,"width":80,"height":50,"borderRadius":8,"backgroundColor":"#4fd1ff",
      "outlineWidth":2,"outlineColor":"#f2c35b","outlineOffset":3,"boxShadow":"6px 6px 0 rgba(255,0,0,0.5)"}})J");
    s.add(R"J({"style":{"position":"absolute","left":290,"top":180,"width":100,"height":70,"borderRadius":20,"overflow":"hidden",
      "backgroundColor":"#333","opacity":0.85},"children":[{"style":{"position":"absolute","left":50,"top":30,"width":80,"height":60,
      "borderRadius":30,"backgroundColor":"#ff4d6a"}},{"type":"Text","style":{"color":"#fff","fontSize":13,"margin":6},"text":"Clipped"}]})J");
    Node* probe = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":9,"height":7,"backgroundColor":"rgba(255,255,255,0.5)"}})J");
    int dw = s.dw(), dh = s.dh();
    std::vector<uint32_t> inc(size_t(dw) * dh), full(inc.size());
    s.doc().layout();
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    uint32_t seed = 12345;
    auto rnd = [&](float lo, float hi) {
      seed = seed * 1664525u + 1013904223u;
      return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
    };
    size_t bad = 0, differ = 0;
    int worst = 0;
    double probeArea = 0, repainted = 0;
    for (int i = 0; i < 200; i++) {
      float pw = rnd(2, 40), ph = rnd(2, 30);
      probeArea += 2 * pw * ph * dpr * dpr;  // old + new rect
      patch(probe, "{\"left\":" + std::to_string(rnd(-5, 395)) + ",\"top\":" + std::to_string(rnd(-5, 255)) +
                       ",\"width\":" + std::to_string(pw) + ",\"height\":" + std::to_string(ph) + "}");
      s.doc().layout();
      SkIRect damage = s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
      repainted += double(lastPaintStats(s.doc()).repaintedPixels);
      s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
      PixelDiff d = pixelDiff(inc, full);
      bool ok = matchesFull(d, inc.size());
      if (!ok && !bad) {
        std::fprintf(stderr, "  dpr %g step %d: %zu pixels differ\n", dpr, i, d.count);
        describeDiff(inc, full, dw, damage);
      }
      if (!ok) bad += d.count;
      differ = std::max(differ, d.count);
      worst = std::max(worst, d.max);
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);  // full again after render()
    }
    std::printf("  dpr %g: at most %zu pixels per step differ from a full repaint, by at most %d\n", dpr, differ, worst);
    CHECK_EQ(bad, size_t(0));
    double frame = double(dw) * dh;
    std::printf("  dpr %g: repainted %.0f px per step (%.1f%% of the frame) for %.0f px of probe rects\n", dpr,
                repainted / 200, repainted / 200 / frame * 100, probeArea / 200);
    CHECK(repainted / 200 < frame * 0.15);
    // Inside the first box's padding box, clear of its rounded corners:
    // only the probe's old and new rects (and their pixel margins) repaint.
    for (float x : {40.f, 60.f}) {
      patch(probe, "{\"left\":" + std::to_string(x) + ",\"top\":16,\"width\":9,\"height\":7}");
      s.doc().layout();
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    }
    double inside = double(lastPaintStats(s.doc()).repaintedPixels);
    std::printf("  dpr %g: inside a rounded box: repainted %.0f px\n", dpr, inside);
    CHECK(inside <= 2 * (9 + 4) * (7 + 4) * dpr * dpr);
    s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
    CHECK(matchesFull(pixelDiff(inc, full), inc.size()));
  }
}

// Gradient backgrounds and scaled images are blitted from cached raster
// sprites (and a one-draw node's opacity is folded into its draw): the
// frame matches drawing everything directly (a picture canvas never uses
// sprites) up to rounding, incremental frames match full ones (matchesFull), and
// later frames reuse the sprites. Perf guards: no sprite re-rendered while
// only an unrelated probe moves; the folded opacity takes no layer.
// A gradient that keeps moving by sub-pixels (the showcase's drifting glow)
// is resampled from its sprite of phase 0 instead of a sprite per frame or a
// direct draw: no sprite is rendered after the first frames, each frame is
// within a few levels of the same state painted exactly, and once it stops
// the next frame repaints it exactly.
TEST(render_moving_gradient_reuses_its_sprite) {
  const char* back = R"J({"style":{"position":"absolute","left":0,"top":0,"width":380,"height":240,
    "backgroundColor":"#101828"}})J";
  const char* glow = R"J({"style":{"position":"absolute","left":30,"top":20,"width":300,"height":180,"borderRadius":90,
    "opacity":0.7,"experimental_backgroundImage":"radial-gradient(circle at 50% 50%, rgba(242,195,91,0.6) 0%, rgba(79,140,255,0.3) 45%, rgba(5,7,16,0) 70%)"}})J";
  auto drift = [](int i) {
    return "{\"transform\":[{\"translateX\":" + std::to_string(20 * (1 - std::cos(float(i) * 0.15f)) + 0.37f * float(i)) +
           "},{\"translateY\":" + std::to_string(0.21f * float(i)) + "}]}";
  };
  for (float dpr : {1.f, 1.5f}) {
    Scene s(380, 240, dpr);
    s.add(back);
    Node* n = s.add(glow);
    int dw = s.dw(), dh = s.dh();
    std::vector<uint32_t> inc(size_t(dw) * dh), full(inc.size());
    s.doc().layout();
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    int renders = 0, draws = 0, approximate = 0, worst = 0;
    const int frames = 40;
    for (int i = 1; i <= frames; i++) {
      patch(n, drift(i));
      s.doc().layout();
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
      PaintStats st = lastPaintStats(s.doc());
      if (i > 2) renders += st.spriteRenders;  // the first moving frame: the sprite of phase 0
      draws += st.spriteDraws;
      approximate += s.doc().exactRepaintPending();
      Scene e(380, 240, dpr);  // the same state, painted exactly
      e.add(back);
      patch(e.add(glow), drift(i));
      e.render();
      worst = std::max(worst, pixelDiff(inc, e.px).max);
    }
    std::printf("  dpr %g: %d sprites rendered in %d moving frames (%d sprite draws, %d approximate), "
                "at most %d levels from exact\n",
                dpr, renders, frames - 2, draws, approximate, worst);
    CHECK_EQ(renders, 0);
    CHECK(draws >= frames);
    CHECK(approximate >= frames - 2);
    CHECK(worst <= 6);  // its anti-aliased edge, resampled
    // At rest: the next frame repaints it exactly.
    CHECK(s.doc().exactRepaintPending());
    s.doc().layout();
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    CHECK(!s.doc().exactRepaintPending());
    s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
    CHECK(matchesFull(pixelDiff(inc, full), inc.size()));
  }
}

// Raster sprite blits bypass Skia where they paint its pixels (checked at
// first use, see blitSprite): on SSE2 that is always.
TEST(render_sprite_blits_bypass_skia) {
#if defined(__SSE2__) || defined(_M_X64)
  CHECK(fastSpriteBlits());
#endif
  std::printf("  fast sprite blits: %s\n", fastSpriteBlits() ? "on" : "off");
}

TEST(render_sprites_match_direct_and_are_reused) {
  for (float dpr : {1.f, 1.5f}) {
    Scene s(320, 240, dpr);
    s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,
      "experimental_backgroundImage":"linear-gradient(90deg, rgba(5,7,16,0.9) 0%, rgba(40,60,120,0.7) 40%, rgba(5,7,16,0.4) 75%, #203040 100%)"}})J");
    s.add(R"J({"style":{"position":"absolute","left":10,"top":10,"width":160,"height":120,"borderRadius":40,"opacity":0.6,
      "experimental_backgroundImage":"radial-gradient(circle at 50% 50%, rgba(242,195,91,0.9) 0%, rgba(79,140,255,0.5) 45%, rgba(5,7,16,0) 70%)"}})J");
    s.add(R"J({"style":{"position":"absolute","left":165,"top":12,"width":150,"height":120,"borderWidth":4,"borderColor":"#fff",
      "borderRadius":12,"experimental_backgroundImage":"linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c08a2a 100%)"}})J");
    s.add(R"J({"type":"Image","props":{"resizeMode":"cover","source":{"uri":")J" + testPngUri(64, 48) +
          R"J("}},"style":{"position":"absolute","left":20,"top":120,"width":170,"height":110,"borderRadius":18}})J");
    s.add(R"J({"style":{"position":"absolute","left":210,"top":140,"width":100,"height":80,"transform":[{"rotate":"10deg"}],
      "experimental_backgroundImage":"linear-gradient(45deg, red, blue)"}})J");
    Node* probe = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":20,"height":14,"backgroundColor":"rgba(255,255,255,0.5)"}})J");
    s.render();
    s.render();  // the image is decoded by now
    PaintStats first = lastPaintStats(s.doc());
    CHECK(first.spriteDraws >= 4);  // page gradient, folded radial, bordered box, image
    // Direct: the same document painted into a picture (no sprites), played
    // into a raster frame.
    int dw = s.dw(), dh = s.dh();
    SkPictureRecorder rec;
    paintDocument(s.doc(), rec.beginRecording(SkRect::MakeIWH(dw, dh)));
    sk_sp<SkPicture> pic = rec.finishRecordingAsPicture();
    std::vector<uint32_t> direct(size_t(dw) * dh, 0);
    sk_sp<SkSurface> surf = SkSurfaces::WrapPixels(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, kPremul_SkAlphaType), direct.data(), size_t(dw) * 4);
    surf->getCanvas()->drawPicture(pic);
    int worst = 0;
    for (size_t i = 0; i < direct.size(); i++)
      for (int ch = 0; ch < 4; ch++)
        worst = std::max(worst, std::abs(int((direct[i] >> (ch * 8)) & 255) - int((s.px[i] >> (ch * 8)) & 255)));
    std::printf("  dpr %g: sprites vs direct max diff %d, %d sprite draws\n", dpr, worst, first.spriteDraws);
    CHECK(worst <= 4);  // anti-aliased edges: coverage applied before the blit, rounded once more
    // A probe moves over everything: incremental == full, sprites reused.
    setPaintCounting(s.doc(), true);
    std::vector<uint32_t> inc(size_t(dw) * dh), full(inc.size());
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    size_t bad = 0;
    int renders = 0, layers = 0;
    for (int i = 0; i < 40; i++) {
      patch(probe, "{\"left\":" + std::to_string(i * 7.3) + ",\"top\":" + std::to_string(i * 5.1) + "}");
      s.doc().layout();
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
      PaintStats st = lastPaintStats(s.doc());
      renders += st.spriteRenders;
      layers += st.saveLayers;
      s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
      renders += lastPaintStats(s.doc()).spriteRenders;
      PixelDiff d = pixelDiff(inc, full);
      if (!matchesFull(d, inc.size())) {
        if (!bad) std::fprintf(stderr, "  dpr %g step %d: %zu pixels differ, max %d\n", dpr, i, d.count, d.max);
        bad += d.count;
      }
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    }
    CHECK_EQ(bad, size_t(0));
    CHECK_EQ(renders, 0);
    CHECK_EQ(layers, 0);  // the 0.6 opacity is folded into its gradient
  }
}

// A shadowed box springing like a hovered menu button (a sub-pixel
// translation and a scale every frame, a translucent background, so the box
// is cut out of the mask) draws its shadow from cached masks while it moves
// (painter.cpp PaintMotion): after the first frames no mask is blurred
// again (PaintStats::shadowRenders; one per quantized scale at most, where
// every frame rendered one before), each frame is within a few levels of the
// same state painted exactly from scratch, and once it stops the next frame
// repaints it exactly (incremental == full). The same for an inset shadow
// (a glowing inventory slot springing when selected).
TEST(render_moving_shadow_reuses_its_mask) {
  const char* outer = R"J({"style":{"position":"absolute","left":40,"top":30,"width":180,"height":52,"borderRadius":10,
    "backgroundColor":"rgba(26,36,64,0.9)","borderWidth":1,"borderColor":"rgba(148,170,255,0.32)",
    "boxShadow":"0 4px 12px rgba(0,0,0,0.5)"}})J";
  const char* inset = R"J({"style":{"position":"absolute","left":40,"top":30,"width":72,"height":72,"borderRadius":10,
    "backgroundColor":"rgba(10,14,26,0.9)","borderWidth":1.5,"borderColor":"#ffb23f",
    "boxShadow":"inset 0 0 18px rgba(255,178,63,0.5)"}})J";
  // A damped spring towards +14 px and 1.05x (overshooting).
  auto spring = [](int i) {
    float k = 1 - std::exp(-float(i) * 0.12f) * std::cos(float(i) * 0.5f);
    return "{\"transform\":[{\"translateX\":" + std::to_string(14 * k) + "},{\"scale\":" +
           std::to_string(1 + 0.05f * k) + "}]}";
  };
  for (const char* box : {outer, inset})
  for (float dpr : {1.f, 1.5f}) {
    Scene s(320, 140, dpr);
    Node* n = s.add(box);
    int dw = s.dw(), dh = s.dh();
    std::vector<uint32_t> inc(size_t(dw) * dh), full(inc.size());
    s.doc().layout();
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    int renders = 0, draws = 0, approximate = 0, worst = 0;
    size_t differ = 0, worstOver = 0;
    const int frames = 40;
    for (int i = 1; i <= frames; i++) {
      patch(n, spring(i));
      s.doc().layout();
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
      PaintStats st = lastPaintStats(s.doc());
      if (i > 2) renders += st.shadowRenders;  // the first moving frames: the exact mask, then phase 0
      draws += st.shadowDraws;
      approximate += s.doc().exactRepaintPending();
      Scene e(320, 140, dpr);  // the same state, painted exactly
      patch(e.add(box), spring(i));
      e.render();
      PixelDiff d = pixelDiff(inc, e.px);
      size_t over = 0;  // pixels off by more than 4 levels
      for (size_t k = 0; k < inc.size(); k++)
        for (int ch = 0; ch < 4; ch++)
          if (std::abs(int((inc[k] >> (ch * 8)) & 255) - int((e.px[k] >> (ch * 8)) & 255)) > 4) {
            over++;
            break;
          }
      worstOver = std::max(worstOver, over);
      worst = std::max(worst, d.max);
      differ = std::max(differ, d.count);
    }
    std::printf("  %s dpr %g: %d masks blurred in %d moving frames (%d shadow draws, %d approximate), "
                "at most %zu pixels differ from exact (%zu by more than 4 levels), by at most %d\n",
                box == inset ? "inset" : "outer", dpr, renders, frames - 2, draws, approximate, differ, worstOver, worst);
    CHECK(renders <= 2);
    CHECK(draws >= frames);
    CHECK(approximate >= frames - 2);
    CHECK(worst <= 6);
    // At rest: the next frame repaints it exactly.
    CHECK(s.doc().exactRepaintPending());
    s.doc().layout();
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    CHECK(!s.doc().exactRepaintPending());
    s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
    PixelDiff d = pixelDiff(inc, full);
    if (!matchesFull(d, inc.size())) std::fprintf(stderr, "  dpr %g at rest: %zu pixels differ, max %d\n", dpr, d.count, d.max);
    CHECK(matchesFull(d, inc.size()));
  }
}

// Scrolling by whole device pixels moves the viewport's pixels instead of
// repainting them (scroll blit): every frame must still match a full
// repaint, with rounded, bordered, shadowed and clipped cards, images, text
// and gradients crossing the viewport's edges, a sidebar beside it and a
// badge above it. Skia's rasterization is not exactly translation
// invariant (anti-aliased edges, filtered images), so a moved pixel may
// differ from one painted fresh at its new place by a level or two: at 2x on
// every platform, at 1x on macOS arm64 (about 50 pixels per frame, CI).
// Moved pixels are copies, so such differences never add up over frames.
// x86 builds are exact at 1x and checked so (a real blit error, stale or
// misplaced pixels, differs by far more than a few levels anywhere).
// Repaints that do not blit are checked like other damage-only repaints
// (matchesFull).
// Perf guard: blitted frames repaint a fraction of the viewport. Frames that
// cannot blit (a sub-pixel scroll, content changing while it scrolls)
// repaint as before.
TEST(render_scroll_blit_matches_full) {
  for (float dpr : {1.f, 2.f}) {
    const bool exact = dpr == 1 && kExactUnderClip;
    Scene s(400, 300, dpr);
    s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":400,"height":300,"backgroundColor":"#0b1020"},"children":[
      {"style":{"position":"absolute","left":0,"top":0,"width":100,"height":300,"backgroundColor":"#111830","borderRightWidth":1,
        "borderRightColor":"rgba(255,255,255,0.1)"},"children":[{"type":"Text","style":{"color":"#fff","fontSize":20,"margin":10},
        "text":"Collection"}]}]})J");
    std::string cards;
    for (int i = 0; i < 12; i++) {
      if (i) cards += ",";
      cards += R"J({"style":{"width":120,"height":140,"margin":8,"borderRadius":14,"overflow":"hidden","backgroundColor":"#182041",
        "borderWidth":1,"borderColor":"rgba(255,255,255,0.15)","boxShadow":"0 8px 16px rgba(0,0,0,0.6)"},"children":[
        {"type":"Image","props":{"resizeMode":"cover","source":{"uri":")J" + testPngUri(48, 32) + R"J("}},"style":{"width":"100%","height":70}},
        {"type":"Text","style":{"color":"#fff","fontSize":14,"margin":6},"text":"Card )J" + std::to_string(i) + R"J("},
        {"style":{"height":8,"marginHorizontal":6,"borderRadius":4,"overflow":"hidden","backgroundColor":"rgba(255,255,255,0.1)"},
         "children":[{"style":{"width":"60%","height":"100%","experimental_backgroundImage":"linear-gradient(90deg, #ff4d6a, #f2c35b)"}}]}]})J";
    }
    Node* sv = s.add(R"J({"type":"ScrollView","style":{"position":"absolute","left":101,"top":0,"width":299,"height":300},
      "children":[{"style":{"flexDirection":"row","flexWrap":"wrap","padding":6},"children":[)J" + cards + "]}]}");
    Node* badge = s.add(R"J({"style":{"position":"absolute","left":330,"top":250,"width":60,"height":30,"borderRadius":15,
      "backgroundColor":"rgba(242,195,91,0.9)"}})J");
    s.render();
    s.render();  // images decoded
    int dw = s.dw(), dh = s.dh();
    std::vector<uint32_t> inc(size_t(dw) * dh), full(inc.size());
    s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
    int blits = 0, steps = 0;
    double blitRepaint = 0;
    size_t bad = 0, off = 0;
    int worst = 0;
    auto step = [&](float y, bool expectBlit, const char* what) {
      sv->scrollY = y;  // like InputManager::applyScroll
      sv->markBoundsDirty();
      s.doc().markPaintDirty();
      s.doc().layout();
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);
      PaintStats st = lastPaintStats(s.doc());
      bool blitted = st.blitPixels > 0;
      if (blitted != expectBlit) std::fprintf(stderr, "  dpr %g %s to %g: blit %d, expected %d\n", dpr, what, y, blitted, expectBlit);
      CHECK_EQ(blitted, expectBlit);
      if (blitted) {
        blits++;
        blitRepaint += double(st.repaintedPixels);
      }
      s.rt->render(full.data(), dw, dh, size_t(dw) * 4);
      size_t diff = 0;
      int md = 0;
      for (size_t k = 0; k < inc.size(); k++) {
        if (inc[k] == full[k]) continue;
        diff++;
        for (int ch = 0; ch < 4; ch++)
          md = std::max(md, std::abs(int((inc[k] >> (ch * 8)) & 255) - int((full[k] >> (ch * 8)) & 255)));
      }
      if (diff && (!blitted || exact || md > 4))
        std::fprintf(stderr, "  dpr %g %s to %g: %zu pixels differ, max %d\n", dpr, what, y, diff, md);
      if (exact || (!blitted && !matchesFull(PixelDiff{diff, md}, inc.size()))) bad += diff;
      off = std::max(off, diff);
      worst = std::max(worst, md);
      s.rt->renderIncremental(inc.data(), dw, dh, size_t(dw) * 4);  // in step again (full)
      steps++;
    };
    float y = 0;
    for (int i = 0; i < 12; i++) step(y += 7, true, "down");
    for (int i = 0; i < 6; i++) step(y -= 13, true, "up");
    step(y += 0.25f, dpr * 0.25f == std::round(dpr * 0.25f), "sub-pixel");
    y = std::round(y);
    step(y, dpr * 0.25f == std::round(dpr * 0.25f), "back to whole");
    // Content changing while scrolling: no blit, still exact.
    patch(sv->children()[0]->children()[3], R"J({"backgroundColor":"#ff0000"})J");
    step(y += 5, false, "scroll + change");
    // The badge above the viewport moving while scrolling: blit, and the
    // badge repaints where it was and is.
    patch(badge, R"J({"left":300,"top":200})J");
    step(y += 5, true, "scroll + overlay move");
    CHECK_EQ(bad, size_t(0));
    std::printf("  dpr %g: at most %zu pixels per frame differ from a full repaint, by at most %d\n", dpr, off, worst);
    CHECK(off <= size_t(299 * 300 * dpr * dpr * 0.0025));  // a quarter percent of the viewport
    CHECK(worst <= 4);
    double vpArea = 299.0 * 300 * dpr * dpr;
    std::printf("  dpr %g: %d of %d frames blitted, repainting %.0f px of a %.0f px viewport\n", dpr, blits, steps,
                blitRepaint / std::max(1, blits), vpArea);
    CHECK(blitRepaint / std::max(1, blits) < vpArea * 0.6);
  }
}

TEST(render_text_color_change_keeps_the_paragraph) {
  // A color-only change (a hover / state color transition) repaints in the
  // new color without building the paragraph again, pixel for pixel what a
  // paragraph built in that color paints.
  const char* kText = R"J({"type":"Text","style":{"alignSelf":"flex-start","fontSize":24,"color":"%s",
      "textDecorationLine":"underline"},"children":["Hello ",{"type":"Text","style":{"fontWeight":"bold"},"text":"JSUI"}]})J";
  auto json = [&](const char* c) {
    char buf[512];
    std::snprintf(buf, sizeof buf, kText, c);
    return std::string(buf);
  };
  Scene s(300, 60);
  Node* t = s.add(json("#ff0000").c_str());
  s.render();
  uint64_t builds = s.doc().perf.counters.paragraphBuilds;
  t->patchStyle(Value::parseJson(R"({"color":"#2040ff"})").asObject());
  s.render();
  CHECK_EQ(s.doc().perf.counters.paragraphBuilds, builds);
  Scene fresh(300, 60);
  fresh.add(json("#2040ff").c_str());
  fresh.render();
  CHECK(s.px == fresh.px);

  // A span with a color of its own keeps it: the root's change rebuilds.
  Scene sp(300, 60);
  Node* r = sp.add(R"J({"type":"Text","style":{"fontSize":20,"color":"red"},"children":["Red ",
      {"type":"Text","style":{"color":"blue"},"text":"Blue"}]})J");
  sp.render();
  builds = sp.doc().perf.counters.paragraphBuilds;
  r->patchStyle(Value::parseJson(R"({"color":"#00ff00"})").asObject());
  sp.render();
  CHECK_EQ(sp.doc().perf.counters.paragraphBuilds, builds + 1);
  int blue = 0, red = 0;
  for (int y = 0; y < 30; y++)
    for (int x = 0; x < 300; x++) {
      if (sp.a(x, y) > 200 && sp.b(x, y) > 200 && sp.r(x, y) < 40) blue++;
      if (sp.a(x, y) > 200 && sp.r(x, y) > 200 && sp.g(x, y) < 40) red++;
    }
  CHECK(blue > 40);
  CHECK_EQ(red, 0);
}
