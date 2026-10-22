// Painter tests: build trees from JSON, paint, check pixels at key points.
// Set JSUI_TEST_OUT=/some/dir to also write the frames as PNGs.
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
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
