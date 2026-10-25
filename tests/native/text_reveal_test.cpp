// Text reveal (the textReveal* style keys behind <Text reveal>): a paragraph
// paints only its first N graphemes / words, laid out as if all of it showed,
// across nested spans, with an optional per-unit fade.
#include <cstdint>
#include <string>

#include "render_util.h"
#include "test.h"
#include "text/text_layout.h"

using namespace rtest;

namespace {

struct Count {
  int white = 0, red = 0, any = 0;
  int maxRedAlpha = 0;
};

Count count(const Scene& s, int x0, int y0, int x1, int y1) {
  Count c;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      int a = s.a(x, y);
      if (a == 0) continue;
      c.any++;
      if (a > 200 && s.r(x, y) > 200 && s.g(x, y) > 200) c.white++;
      if (s.r(x, y) > 40 && s.g(x, y) < 20 && s.b(x, y) < 20) {
        c.red++;
        c.maxRedAlpha = std::max(c.maxRedAlpha, a);
      }
    }
  return c;
}

void reveal(Node* n, const char* json) {
  Value v = Value::parseJson(json);
  n->patchStyle(v.asObject());
}

const char* kSpans = R"J({"type":"Text","style":{"alignSelf":"flex-start","fontSize":28,"color":"white"},
  "children":["Hello ",{"type":"Text","style":{"color":"red"},"text":"world"}]})J";

}  // namespace

TEST(text_reveal_style_keys_are_paint_only) {
  // (The paint key table is binary searched: every key must be found.)
  for (const char* k : {"textReveal", "textRevealFade", "textRevealFraction", "textRevealMode", "fontFeatureSettings",
                        "fontVariationSettings", "fontFamily", "fontSize", "textShadow", "lottieProgress"})
    CHECK(paintStyleGroup(k) != 0);
  CHECK(paintStyleGroup("textReveal") != paintStyleGroup("color"));
  CHECK_EQ(paintStyleGroup("width"), 0u);
}

TEST(text_reveal_nested_spans) {
  Scene s(320, 60);
  Node* t = s.add(kSpans);
  s.render();
  std::vector<uint32_t> full = s.px;
  Count all = count(s, 0, 0, 320, 60);
  CHECK(all.white > 60 && all.red > 60);
  Rect box = t->layoutRect();
  CHECK_EQ(text::revealUnitCount(t), 11);
  uint64_t builds = s.doc().perf.counters.paragraphBuilds;
  uint64_t layouts = gParagraphLayouts;

  // Nothing yet.
  reveal(t, R"({"textReveal":0})");
  s.render();
  CHECK_EQ(count(s, 0, 0, 320, 60).any, 0);

  // "Hello " (6 graphemes, space included): the plain run only, pixel for
  // pixel what the full paragraph paints there.
  reveal(t, R"({"textReveal":6})");
  s.render();
  s.save("reveal_6");
  Count six = count(s, 0, 0, 320, 60);
  CHECK(six.white > 40);
  CHECK_EQ(six.red, 0);
  int differ = 0;
  for (size_t i = 0; i < s.px.size(); i++)
    if (s.px[i] != 0 && s.px[i] != full[i]) differ++;
  CHECK_EQ(differ, 0);

  // Into the red span: "Hello wo".
  reveal(t, R"({"textReveal":8})");
  s.render();
  Count eight = count(s, 0, 0, 320, 60);
  CHECK_EQ(eight.white, six.white);
  CHECK(eight.red > 10 && eight.red < all.red);

  // A fraction rounds up to the unit it is in (7.2 -> 8 units).
  reveal(t, R"({"textReveal":7.2})");
  s.render();
  CHECK_EQ(count(s, 0, 0, 320, 60).red, eight.red);

  // All of it: the same pixels as no reveal.
  reveal(t, R"({"textReveal":11})");
  s.render();
  CHECK(s.px == full);
  reveal(t, R"({"textReveal":1e6})");
  s.render();
  CHECK(s.px == full);

  // Same layout throughout, the paragraph neither re-shaped nor laid out again.
  Rect after = t->layoutRect();
  CHECK_EQ(after.width, box.width);
  CHECK_EQ(after.height, box.height);
  CHECK_EQ(s.doc().perf.counters.paragraphBuilds, builds);
  CHECK_EQ(gParagraphLayouts, layouts);

  // Removing the key shows everything again.
  reveal(t, R"({"textReveal":null})");
  s.render();
  CHECK(s.px == full);
}

TEST(text_reveal_fade_fraction_words) {
  Scene s(320, 60);
  Node* t = s.add(kSpans);
  reveal(t, R"({"textReveal":7})");
  s.render();
  Count seven = count(s, 0, 0, 320, 60);  // "Hello w"
  CHECK(seven.red > 0);

  // Fade over 1 unit: at 7.5 units 0..6 are whole, unit 7 ('o') half there.
  reveal(t, R"({"textReveal":7.5,"textRevealFade":1})");
  s.render();
  s.save("reveal_fade");
  Count half = count(s, 0, 0, 320, 60);
  CHECK(half.red > seven.red);
  CHECK_EQ(half.white, seven.white);
  // The fading glyph's pixels are at most about half opaque.
  reveal(t, R"({"textReveal":8,"textRevealFade":1})");
  s.render();
  Count whole = count(s, 0, 0, 320, 60);
  CHECK(whole.red > seven.red);
  CHECK(whole.maxRedAlpha > 200);
  {
    // Pixels the 'o' adds at 7.5: none fully opaque.
    reveal(t, R"({"textReveal":7.5,"textRevealFade":1})");
    s.render();
    std::vector<uint32_t> h = s.px;
    reveal(t, R"({"textReveal":7})");
    s.render();
    int maxA = 0;
    for (size_t i = 0; i < h.size(); i++)
      if (s.px[i] == 0 && h[i] != 0) maxA = std::max(maxA, int(h[i] >> 24));
    CHECK(maxA > 60 && maxA < 150);
  }

  // revealFraction: 0..1 of the text (11 graphemes: 0.5 -> 5.5 -> 6 units).
  reveal(t, R"({"textReveal":0.5,"textRevealFraction":true,"textRevealFade":null})");
  s.render();
  Count frac = count(s, 0, 0, 320, 60);
  CHECK_EQ(frac.red, 0);
  CHECK(frac.white > 40);
  reveal(t, R"({"textReveal":1})");
  s.render();
  CHECK(count(s, 0, 0, 320, 60).red > 60);

  // Words: "Hello " is the first word (its space with it), "world" the second.
  reveal(t, R"({"textReveal":1,"textRevealFraction":null,"textRevealMode":"words"})");
  CHECK_EQ(text::revealUnitCount(t), 2);
  s.render();
  Count w1 = count(s, 0, 0, 320, 60);
  CHECK(w1.white > 40);
  CHECK_EQ(w1.red, 0);
  reveal(t, R"({"textReveal":2})");
  s.render();
  CHECK(count(s, 0, 0, 320, 60).red > 60);
}

TEST(text_reveal_lines_and_graphemes) {
  // Two lines: revealing into the second shows the first whole.
  Scene s(200, 100);
  Node* t = s.add(R"J({"type":"Text","style":{"width":150,"fontSize":20,"lineHeight":30,"color":"white"},
    "text":"Lorem ipsum dolor sit amet consectetur"})J");
  s.render();
  std::vector<uint32_t> full = s.px;
  Rect box = t->layoutRect();
  CHECK(box.height >= 59);  // wrapped
  reveal(t, R"({"textReveal":24})");
  s.render();
  s.save("reveal_lines");
  int top = 0, topFull = 0, bottom = 0, bottomFull = 0, differ = 0;
  for (int y = 0; y < 100; y++)
    for (int x = 0; x < 200; x++) {
      size_t i = size_t(y) * 200 + x;
      bool on = s.px[i] != 0, onFull = full[i] != 0;
      (y < 30 ? top : bottom) += on;
      (y < 30 ? topFull : bottomFull) += onFull;
      if (on && s.px[i] != full[i]) differ++;
    }
  CHECK_EQ(top, topFull);
  CHECK(bottom > 0 && bottom < bottomFull);
  CHECK_EQ(differ, 0);
  CHECK_EQ(t->layoutRect().height, box.height);

  // Graphemes: e + combining acute is one unit, a ZWJ family emoji one more.
  Node* g = s.add(R"J({"type":"Text","style":{"fontSize":12},"text":"éx👨‍👩‍👧 y"})J");
  s.render();
  CHECK_EQ(text::revealUnitCount(g), 5);
  reveal(g, R"({"textRevealMode":"words"})");
  CHECK_EQ(text::revealUnitCount(g), 2);
}
