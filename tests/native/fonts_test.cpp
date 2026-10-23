// Custom fonts: containers (TTF / TTC / WOFF / WOFF2), @font-face style
// overrides, variable fonts, glyph fallback, the C API and the JS bindings.
// Test fonts: tests/native/fonts (see make_fonts.py there).
#include <fstream>
#include <iterator>

#include "render_util.h"
#include "test.h"

using namespace jsui;
using rtest::Scene;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::vector<uint8_t> testFont(const char* name) { return readFile(std::string("tests/native/fonts/") + name); }

// Laid-out width of one absolutely positioned Text.
float textWidth(Scene& s, const std::string& style, const std::string& text) {
  Node* n = s.add(R"J({"type":"Text","style":{"position":"absolute",)J" + style + R"J(},"text":")J" + text + R"J("})J");
  s.render();
  float w = n->layoutRect().width;
  s.doc().root()->removeChild(n);
  return w;
}

}  // namespace

TEST(fonts_containers_decode_to_the_same_face) {
  Scene s(400, 100);
  FontManager& fm = s.rt->fonts;
  auto ttf = testFont("inter-abc.ttf"), woff = testFont("inter-abc.woff"), woff2 = testFont("inter-abc.woff2");
  CHECK(!ttf.empty() && !woff.empty() && !woff2.empty());
  std::string err;
  CHECK_EQ(fm.registerFont("AbcTtf", ttf.data(), ttf.size(), 0, -1, &err), 1);
  CHECK_EQ(fm.registerFont("AbcWoff", woff.data(), woff.size(), 0, -1, &err), 1);
  CHECK_EQ(fm.registerFont("AbcWoff2", woff2.data(), woff2.size(), 0, -1, &err), 1);
  s.rt->fontsChanged();
  float w = textWidth(s, R"("fontSize":40,"fontFamily":"AbcTtf")", "ABC");
  CHECK(w > 40);
  CHECK_NEAR(textWidth(s, R"("fontSize":40,"fontFamily":"AbcWoff")", "ABC"), w, 0.01);
  CHECK_NEAR(textWidth(s, R"("fontSize":40,"fontFamily":"abcwoff2")", "ABC"), w, 0.01);  // case-insensitive
  // Corrupt / foreign data is refused with a reason.
  std::vector<uint8_t> bad = woff2;
  bad.resize(bad.size() / 2);
  CHECK_EQ(fm.registerFont("Bad", bad.data(), bad.size(), 0, -1, &err), 0);
  CHECK(err.find("WOFF2") != std::string::npos);
  const char junk[] = "definitely not a font";
  CHECK_EQ(fm.registerFont("Junk", junk, sizeof junk, 0, -1, &err), 0);
  CHECK(err.find("not a supported font") != std::string::npos);
  CHECK(!fm.hasFamily("Junk"));
}

TEST(fonts_collection_registers_every_face_and_file_family_names) {
  Scene s(400, 100);
  FontManager& fm = s.rt->fonts;
  auto ttc = testFont("inter-abc.ttc");
  CHECK_EQ(fm.registerFont("Coll", ttc.data(), ttc.size(), 0, -1), 2);
  s.rt->fontsChanged();
  float regular = textWidth(s, R"("fontSize":40,"fontFamily":"Coll")", "ABC");
  float bold = textWidth(s, R"("fontSize":40,"fontFamily":"Coll","fontWeight":"bold")", "ABC");
  CHECK(bold > regular + 1);
  // No family: the name inside the file.
  auto var = testFont("jsui-var.ttf");
  CHECK(fm.registerFont("", var.data(), var.size(), 0, -1) > 0);
  CHECK(fm.hasFamily("JsuiVar"));
}

TEST(fonts_style_overrides_pick_faces_like_font_face) {
  Scene s(400, 100);
  FontManager& fm = s.rt->fonts;
  auto ttc = testFont("inter-abc.ttc");
  auto ttf = testFont("inter-abc.ttf");
  CHECK_EQ(fm.registerFont("Coll", ttc.data(), ttc.size(), 0, -1), 2);
  s.rt->fontsChanged();
  float regular = textWidth(s, R"("fontSize":40,"fontFamily":"Coll")", "ABC");
  // The regular file declared bold (@font-face font-weight: 700).
  CHECK_EQ(fm.registerFont("Swap", ttf.data(), ttf.size(), 700, -1), 1);
  s.rt->fontsChanged();
  // Only a "700" face: weight 700 text uses it at its own (regular) width.
  CHECK_NEAR(textWidth(s, R"("fontSize":40,"fontFamily":"Swap","fontWeight":"700")", "ABC"), regular, 0.01);
  // Re-registering the same style replaces the face.
  CHECK_EQ(fm.registerFont("Coll", ttf.data(), ttf.size(), 700, -1), 1);
  s.rt->fontsChanged();
  CHECK_NEAR(textWidth(s, R"("fontSize":40,"fontFamily":"Coll","fontWeight":"700")", "ABC"), regular, 0.01);
  CHECK(fm.unregisterFamily("coll"));
  CHECK(!fm.hasFamily("Coll"));
  CHECK(!fm.unregisterFamily("Coll"));
}

TEST(fonts_variable_weight_instances_and_variation_settings) {
  Scene s(1000, 200);
  FontManager& fm = s.rt->fonts;
  auto var = testFont("jsui-var.ttf");
  // An instance per 100 step of the 100..900 'wght' axis.
  CHECK_EQ(fm.registerFont("Var", var.data(), var.size(), 0, -1), 1);
  s.rt->fontsChanged();
  // "I" advance: 0.2 / 0.5 / 0.9 em at wght 100 / 400 / 900.
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var","fontWeight":"100")", "I"), 20, 0.5);
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var")", "I"), 50, 0.5);
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var","fontWeight":"900")", "I"), 90, 0.5);
  // fontVariationSettings sets axes directly (string and object forms).
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var","fontVariationSettings":"'wght' 650")", "I"), 70, 0.5);
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var","fontVariationSettings":{"wght":250})", "I"), 35, 0.5);
  // Settings without 'wght' keep fontWeight's.
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Var","fontWeight":"900","fontVariationSettings":"'wdth' 100")", "I"), 90, 0.5);
  // A weight override pins a variable font to that instance (800: 0.82 em,
  // interpolated between the 400 and 900 masters).
  CHECK_EQ(fm.registerFont("Pinned", var.data(), var.size(), 800, -1), 1);
  s.rt->fontsChanged();
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Pinned")", "I"), 82, 0.5);
}

TEST(fonts_glyph_fallback_order) {
  Scene s(1000, 200);
  FontManager& fm = s.rt->fonts;
  const std::string pua = "\xf3\xb0\x80\x80";  // U+F0000: only the test fonts have it
  float tofu = textWidth(s, R"("fontSize":100)", pua);
  auto var = testFont("jsui-var.ttf"), puaFont = testFont("jsui-pua.ttf");
  CHECK_EQ(fm.registerFont("", var.data(), var.size(), 0, -1), 1);
  CHECK_EQ(fm.registerFont("", puaFont.data(), puaFont.size(), 0, -1), 1);
  s.rt->fontsChanged();
  // Inter lacks U+F0000: per-glyph fallback finds a registered font that has it
  // (registration order: JsuiVar, 0.3 em).
  float w = textWidth(s, R"("fontSize":100)", pua);
  CHECK(std::fabs(w - tofu) > 1);
  CHECK_NEAR(w, 30, 0.5);
  // The fallback list goes first.
  fm.setFallbackFamilies({"JsuiPua"});
  s.rt->fontsChanged();
  CHECK_NEAR(textWidth(s, R"("fontSize":100)", pua), 90, 0.5);
  // A family list in the style is honoured per glyph too.
  fm.setFallbackFamilies({});
  s.rt->fontsChanged();
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFamily":"Inter, JsuiPua")", pua), 90, 0.5);
  // Default family.
  fm.setDefaultFamily("JsuiPua");
  s.rt->fontsChanged();
  CHECK_NEAR(textWidth(s, R"("fontSize":100)", pua), 90, 0.5);
}

TEST(fonts_feature_settings_parse) {
  Scene s(1000, 200);
  // Inter's tabular figures: every digit the same width.
  float prop1 = textWidth(s, R"("fontSize":100)", "1");
  float tab1 = textWidth(s, R"("fontSize":100,"fontFeatureSettings":"'tnum'")", "1");
  float tab1b = textWidth(s, R"("fontSize":100,"fontFeatureSettings":{"tnum":1})", "1");
  float tab0 = textWidth(s, R"("fontSize":100,"fontFeatureSettings":"'tnum' 1")", "0");
  CHECK(std::fabs(prop1 - tab1) > 0.5);
  CHECK_NEAR(tab1, tab1b, 0.01);
  CHECK_NEAR(tab1, tab0, 0.01);
  CHECK_NEAR(textWidth(s, R"("fontSize":100,"fontFeatureSettings":"'tnum' off")", "1"), prop1, 0.01);
}

TEST(fonts_c_api) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 200;
  cfg.height = 100;
  JsuiRuntime* rt = jsui_create(&cfg);
  CHECK(rt != nullptr);
  auto woff2 = testFont("inter-abc.woff2");
  CHECK_EQ(jsui_register_font(rt, nullptr, woff2.data(), woff2.size(), 0, -1), 0);
  CHECK_EQ(jsui_register_font(rt, "Junk", "xxxx", 4, 0, -1), -1);
  CHECK_EQ(jsui_unregister_font(rt, "Inter"), 0);  // the file's family name is Inter: replaced, then removed
  CHECK_EQ(jsui_unregister_font(rt, "Inter"), -1);
  jsui_set_font_fallbacks(rt, "A, 'B C'");
  jsui_set_font_fallbacks(rt, nullptr);
  jsui_set_default_font_family(rt, "Inter");
  jsui_destroy(rt);
}
