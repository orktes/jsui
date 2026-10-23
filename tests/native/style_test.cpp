// Style parser tests: colors, lengths, shadows, gradients, transforms,
// filters, text styles and the Yoga mapping.
#include <yoga/Yoga.h>

#include "core/document.h"
#include "core/style.h"
#include "core/value.h"
#include "test.h"

using namespace jsui;

namespace {
Color col(const char* s) {
  Color c{-1, -1, -1, -1};
  CHECK(parseColor(std::string(s), &c));
  return c;
}
bool near(float a, float b, float eps = 0.002f) { return std::fabs(a - b) <= eps; }
bool colorIs(const Color& c, int r, int g, int b, float a = 1) {
  return near(c.r, r / 255.f) && near(c.g, g / 255.f) && near(c.b, b / 255.f) && near(c.a, a, 0.004f);
}
ComputedStyle styleOf(const char* json, YGNodeRef* yoga = nullptr) {
  static YGNodeRef y = nullptr;
  if (!y) y = YGNodeNew();
  ComputedStyle s;
  applyStyle(Value::parseJson(json), &s, y);
  if (yoga) *yoga = y;
  return s;
}
}  // namespace

TEST(style_colors_hex) {
  CHECK(colorIs(col("#f00"), 255, 0, 0));
  CHECK(colorIs(col("#F008"), 255, 0, 0, 0x88 / 255.f));
  CHECK(colorIs(col("#123456"), 0x12, 0x34, 0x56));
  CHECK(colorIs(col("#12345680"), 0x12, 0x34, 0x56, 128 / 255.f));
  Color c;
  CHECK(!parseColor(std::string("#12345"), &c));
  CHECK(!parseColor(std::string("#ggg"), &c));
  CHECK(!parseColor(std::string("nonsense"), &c));
}

TEST(style_colors_functions) {
  CHECK(colorIs(col("rgb(10, 20, 30)"), 10, 20, 30));
  CHECK(colorIs(col("rgba(10,20,30,0.5)"), 10, 20, 30, 0.5f));
  CHECK(colorIs(col("rgb(10 20 30 / 50%)"), 10, 20, 30, 0.5f));
  CHECK(colorIs(col("rgb(100%, 0%, 50%)"), 255, 0, 128));
  CHECK(colorIs(col("RGBA(0,0,0,.25)"), 0, 0, 0, 0.25f));
  CHECK(colorIs(col("hsl(0, 100%, 50%)"), 255, 0, 0));
  CHECK(colorIs(col("hsl(120deg 100% 25%)"), 0, 128, 0));
  CHECK(colorIs(col("hsla(240, 100%, 50%, 0.3)"), 0, 0, 255, 0.3f));
  CHECK(colorIs(col("hsl(0.5turn, 100%, 50%)"), 0, 255, 255));
}

TEST(style_colors_named) {
  CHECK(colorIs(col("rebeccapurple"), 0x66, 0x33, 0x99));
  CHECK(colorIs(col("White"), 255, 255, 255));
  CHECK(colorIs(col("aliceblue"), 0xf0, 0xf8, 0xff));
  CHECK(colorIs(col("yellowgreen"), 0x9a, 0xcd, 0x32));
  CHECK(colorIs(col("lightgoldenrodyellow"), 0xfa, 0xfa, 0xd2));
  CHECK(col("transparent").transparent());
  // processColor() integers (0xAARRGGBB).
  Color c;
  CHECK(parseColor(Value(double(0x80ff0000u)), &c));
  CHECK(colorIs(c, 255, 0, 0, 128 / 255.f));
}

TEST(style_box_shadow_strings) {
  std::vector<Shadow> v;
  CHECK(parseBoxShadow(Value("0 4px 12px rgba(0,0,0,0.3)"), &v));
  CHECK_EQ(v.size(), size_t(1));
  CHECK_NEAR(v[0].offsetY, 4, 1e-4);
  CHECK_NEAR(v[0].blur, 12, 1e-4);
  CHECK(colorIs(v[0].color, 0, 0, 0, 0.3f));
  CHECK(!v[0].inset);
  CHECK(parseBoxShadow(Value("inset 1px 2px 3px 4px #fff, red 5px 6px, 0 0 0 2px blue inset"), &v));
  CHECK_EQ(v.size(), size_t(3));
  CHECK(v[0].inset);
  CHECK_NEAR(v[0].spread, 4, 1e-4);
  CHECK(colorIs(v[1].color, 255, 0, 0));
  CHECK_NEAR(v[1].offsetX, 5, 1e-4);
  CHECK_NEAR(v[1].blur, 0, 1e-4);
  CHECK(v[2].inset);
  CHECK(colorIs(v[2].color, 0, 0, 255));
  CHECK(parseBoxShadow(Value("none"), &v) && v.empty());
  CHECK(!parseBoxShadow(Value("1px"), &v));
}

TEST(style_box_shadow_objects_and_legacy) {
  std::vector<Shadow> v;
  CHECK(parseBoxShadow(Value::parseJson(R"([{"offsetX":2,"offsetY":"3px","blurRadius":4,"spreadDistance":1,"color":"#00f","inset":true}])"), &v));
  CHECK_EQ(v.size(), size_t(1));
  CHECK(v[0].inset);
  CHECK_NEAR(v[0].offsetY, 3, 1e-4);
  CHECK_NEAR(v[0].spread, 1, 1e-4);
  // react-native-web: shadowOpacity multiplies the color alpha.
  ComputedStyle s = styleOf(R"({"shadowColor":"#000","shadowOffset":{"width":0,"height":2},"shadowOpacity":0.25,"shadowRadius":3.84})");
  CHECK_EQ(s.boxShadows.size(), size_t(1));
  CHECK_NEAR(s.boxShadows[0].offsetY, 2, 1e-4);
  CHECK_NEAR(s.boxShadows[0].blur, 3.84, 1e-4);
  CHECK_NEAR(s.boxShadows[0].color.a, 0.25, 1e-4);
  s = styleOf(R"({"boxShadow":"0 0 2px red","shadowColor":"blue","shadowRadius":2})");
  CHECK_EQ(s.boxShadows.size(), size_t(2));
}

TEST(style_gradients) {
  std::vector<Gradient> g;
  CHECK(parseBackgroundImage(Value("linear-gradient(90deg, red, blue 80%)"), &g));
  CHECK_EQ(g.size(), size_t(1));
  CHECK_NEAR(g[0].angle, 90, 1e-4);
  CHECK_EQ(g[0].stops.size(), size_t(2));
  CHECK(!g[0].stops[0].position.has_value());
  CHECK_NEAR(*g[0].stops[1].position, 0.8, 1e-5);
  CHECK(parseBackgroundImage(Value("linear-gradient(to top left, rgba(0,0,0,0.5) 10px, #fff 50% 70%)"), &g));
  CHECK_EQ(int(g[0].corner), 4);
  CHECK_EQ(g[0].stops.size(), size_t(3));
  CHECK(g[0].stops[0].px);
  CHECK_NEAR(*g[0].stops[0].position, 10, 1e-5);
  CHECK(parseBackgroundImage(Value("linear-gradient(to right, red, blue), radial-gradient(circle at 25% 75%, #fff, #000)"), &g));
  CHECK_EQ(g.size(), size_t(2));
  CHECK_NEAR(g[0].angle, 90, 1e-4);
  CHECK(g[1].kind == Gradient::Kind::Radial);
  CHECK(g[1].circle);
  CHECK_NEAR(g[1].centerX.value, 25, 1e-4);
  CHECK_NEAR(g[1].centerY.value, 75, 1e-4);
  CHECK(parseBackgroundImage(Value("repeating-linear-gradient(0.25turn, red 0 10px, blue 10px 20px)"), &g));
  CHECK(g[0].repeating);
  CHECK_NEAR(g[0].angle, 90, 1e-3);
  CHECK_EQ(g[0].stops.size(), size_t(4));
  CHECK(parseBackgroundImage(Value("radial-gradient(closest-side, red, blue)"), &g));
  CHECK(g[0].extent == Gradient::Extent::ClosestSide);
  CHECK(parseBackgroundImage(Value("radial-gradient(red, blue)"), &g));
  CHECK_EQ(g[0].stops.size(), size_t(2));
  // RN object form.
  CHECK(parseBackgroundImage(Value::parseJson(R"([{"type":"linear-gradient","direction":"to bottom","colorStops":[{"color":"red"},{"color":"blue","positions":["50%"]}]}])"), &g));
  CHECK_NEAR(g[0].angle, 180, 1e-4);
  CHECK_NEAR(*g[0].stops[1].position, 0.5, 1e-5);
  CHECK(!parseBackgroundImage(Value("url(a.png)"), &g));
  // experimental_backgroundImage wins over backgroundImage.
  ComputedStyle s = styleOf(R"J({"backgroundImage":"linear-gradient(red, blue)","experimental_backgroundImage":"linear-gradient(45deg, red, blue)"})J");
  CHECK_EQ(s.backgroundImages.size(), size_t(1));
  CHECK_NEAR(s.backgroundImages[0].angle, 45, 1e-4);
}

TEST(style_transforms) {
  std::vector<TransformOp> t;
  CHECK(parseTransform(Value::parseJson(R"([{"translateX":10},{"translateY":"50%"},{"rotate":"45deg"},{"scale":2},{"skewX":"0.5rad"}])"), &t));
  CHECK_EQ(t.size(), size_t(5));
  CHECK(t[0].kind == TransformOp::Kind::Translate);
  CHECK_NEAR(t[0].a, 10, 1e-5);
  CHECK(t[1].percentB);
  CHECK_NEAR(t[1].b, 50, 1e-5);
  CHECK_NEAR(t[2].a, 45, 1e-4);
  CHECK_NEAR(t[3].a, 2, 1e-5);
  CHECK_NEAR(t[3].b, 2, 1e-5);
  CHECK_NEAR(t[4].a, 28.6479, 1e-3);
  CHECK(parseTransform(Value("translate(10px, 20%) rotate(0.25turn) scaleX(-1) matrix(1, 0, 0, 1, 5, 6)"), &t));
  CHECK_EQ(t.size(), size_t(4));
  CHECK_NEAR(t[0].b, 20, 1e-5);
  CHECK(t[0].percentB);
  CHECK_NEAR(t[1].a, 90, 1e-4);
  CHECK_NEAR(t[2].a, -1, 1e-5);
  CHECK_NEAR(t[2].b, 1, 1e-5);
  CHECK_NEAR(t[3].m[4], 5, 1e-5);
  CHECK(parseTransform(Value::parseJson(R"([{"rotateY":"180deg"}])"), &t));
  CHECK(t[0].flipsBackface);
  CHECK(parseTransform(Value::parseJson(R"([{"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 7,8,0,1]}])"), &t));
  CHECK_NEAR(t[0].m[4], 7, 1e-5);
  CHECK_NEAR(t[0].m[5], 8, 1e-5);
  CHECK(!parseTransform(Value("wobble(3)"), &t));
  ComputedStyle s = styleOf(R"({"transformOrigin":"left bottom"})");
  CHECK_NEAR(s.transformOrigin[0].value, 0, 1e-5);
  CHECK_NEAR(s.transformOrigin[1].value, 100, 1e-5);
  s = styleOf(R"({"transformOrigin":[10, "30%", 0]})");
  CHECK(!s.transformOrigin[0].percent);
  CHECK_NEAR(s.transformOrigin[0].value, 10, 1e-5);
  CHECK(s.transformOrigin[1].percent);
}

TEST(style_filters) {
  std::vector<FilterOp> f;
  CHECK(parseFilter(Value("blur(4px) brightness(150%) grayscale(1) hue-rotate(90deg) drop-shadow(2px 3px 4px rgba(0,0,0,.5))"), &f));
  CHECK_EQ(f.size(), size_t(5));
  CHECK(f[0].kind == FilterOp::Kind::Blur);
  CHECK_NEAR(f[0].amount, 4, 1e-5);
  CHECK_NEAR(f[1].amount, 1.5, 1e-5);
  CHECK_NEAR(f[3].amount, 90, 1e-4);
  CHECK(f[4].kind == FilterOp::Kind::DropShadow);
  CHECK_NEAR(f[4].shadow.blur, 4, 1e-5);
  CHECK(parseFilter(Value::parseJson(R"([{"brightness":0.5},{"saturate":2}])"), &f));
  CHECK_EQ(f.size(), size_t(2));
  ComputedStyle s = styleOf(R"J({"filter":"contrast(2) saturate(0.5)"})J");
  CHECK_NEAR(s.contrast, 2, 1e-5);
  CHECK_NEAR(s.saturate, 0.5, 1e-5);
}

TEST(style_text) {
  ComputedStyle s = styleOf(R"({"color":"red","fontSize":18,"fontWeight":"bold","fontStyle":"italic","lineHeight":24,
    "letterSpacing":1.5,"textAlign":"center","textTransform":"uppercase","textDecorationLine":"underline line-through",
    "textShadowColor":"#000","textShadowOffset":{"width":1,"height":2},"textShadowRadius":3,"fontVariant":["tabular-nums"]})");
  CHECK(s.text.color.has_value() && colorIs(*s.text.color, 255, 0, 0));
  CHECK_NEAR(*s.text.fontSize, 18, 1e-5);
  CHECK_EQ(*s.text.fontWeight, 700);
  CHECK(*s.text.italic);
  CHECK_NEAR(*s.text.lineHeight, 24, 1e-5);
  CHECK(*s.text.textAlign == TextAlign::Center);
  CHECK(*s.text.textTransform == TextTransform::Uppercase);
  CHECK_EQ(int(*s.text.decoration), int(DecorUnderline | DecorLineThrough));
  CHECK_EQ(s.text.textShadows.size(), size_t(1));
  CHECK_NEAR(s.text.textShadows[0].blur, 3, 1e-5);
  CHECK(s.text.fontVariant.has_value());
  CHECK_EQ(*styleOf(R"({"fontWeight":"600"})").text.fontWeight, 600);
  CHECK_EQ(*styleOf(R"({"fontWeight":550})").text.fontWeight, 600);
  CHECK_EQ(*styleOf(R"({"fontWeight":"normal"})").text.fontWeight, 400);
  // No color -> no RN text shadow (react-native-web behaviour).
  CHECK(styleOf(R"({"textShadowOffset":{"width":1,"height":1}})").text.textShadows.empty());
  CHECK(!styleOf(R"({})").text.fontSize.has_value());
}

TEST(style_borders_and_radii) {
  ComputedStyle s = styleOf(R"({"borderWidth":2,"borderTopWidth":4,"borderColor":"red","borderLeftColor":"blue",
     "borderRadius":10,"borderTopRightRadius":"50%","borderBottomStartRadius":3,"borderStyle":"dashed"})");
  CHECK_NEAR(s.borderWidth[0], 4, 1e-5);
  CHECK_NEAR(s.borderWidth[1], 2, 1e-5);
  CHECK(colorIs(s.borderColor[0], 255, 0, 0));
  CHECK(colorIs(s.borderColor[3], 0, 0, 255));
  CHECK_NEAR(s.borderRadius[0].value, 10, 1e-5);
  CHECK(s.borderRadius[1].percent);
  CHECK_NEAR(s.borderRadius[3].value, 3, 1e-5);
  CHECK(s.borderStyle == BorderStyle::Dashed);
  // Default border color is black.
  CHECK(colorIs(styleOf("{}").borderColor[2], 0, 0, 0));
}

TEST(style_yoga_mapping) {
  Document doc;
  doc.setViewport(400, 300, 1);
  Node* row = doc.createNode(NodeType::View);
  row->setProps(Value::parseJson(R"({"style":{"flexDirection":"row","width":"50%","height":100,"paddingHorizontal":10,
     "justifyContent":"space-between","alignItems":"center","gap":5}})"));
  doc.root()->appendChild(row);
  Node* a = doc.createNode(NodeType::View);
  a->setProps(Value::parseJson(R"({"style":{"width":20,"height":"40%","marginStart":3}})"));
  Node* b = doc.createNode(NodeType::View);
  b->setProps(Value::parseJson(R"({"style":{"flex":1,"aspectRatio":2,"maxWidth":60}})"));
  Node* abs = doc.createNode(NodeType::View);
  abs->setProps(Value::parseJson(R"({"style":{"position":"absolute","right":0,"bottom":"10%","width":5,"height":5}})"));
  row->appendChild(a);
  row->appendChild(b);
  row->appendChild(abs);
  doc.layout();
  CHECK_NEAR(row->layoutRect().width, 200, 0.01);
  Rect ra = a->layoutRect();
  CHECK_NEAR(ra.x, 13, 0.01);
  CHECK_NEAR(ra.height, 40, 0.01);
  CHECK_NEAR(ra.y, 30, 0.01);
  Rect rb = b->layoutRect();
  CHECK_NEAR(rb.width, 60, 0.01);
  CHECK_NEAR(rb.height, 30, 0.01);
  Rect rs = abs->layoutRect();
  CHECK_NEAR(rs.x, 195, 0.01);
  CHECK_NEAR(rs.y, 85, 0.01);
  // RN defaults: column, flexShrink 0, alignContent flex-start.
  Node* col = doc.createNode(NodeType::View);
  col->setProps(Value::parseJson(R"({"style":{"height":10}})"));
  doc.root()->appendChild(col);
  CHECK(YGNodeStyleGetFlexDirection(col->yoga()) == YGFlexDirectionColumn);
  CHECK(YGNodeStyleGetAlignContent(col->yoga()) == YGAlignFlexStart);
  CHECK(YGNodeStyleGetPositionType(col->yoga()) == YGPositionTypeRelative);
  // display none
  col->setProps(Value::parseJson(R"({"style":{"height":10,"display":"none"}})"));
  CHECK(!col->style().display);
}

// `flex: N` items are sized by their content while their container is sized
// to fit its content (web: the 0% basis of `flex: N 1 0%` is indefinite then),
// and shrink when that content is wider than the space. Yoga gave them 0.
// The showcase site's stacked "Write it once" list; see cmake/Deps.cmake.
TEST(style_flex_shorthand_in_content_sized_container) {
  Document doc;
  doc.setViewport(354, 600, 1);
  Node* stack = doc.createNode(NodeType::View);
  stack->setProps(Value::parseJson(R"({"style":{"alignItems":"flex-start"}})"));
  doc.root()->appendChild(stack);
  auto point = [&](float contentWidth, Node** inner) {
    Node* col = doc.createNode(NodeType::View);
    col->setProps(Value::parseJson(R"({"style":{"flex":0}})"));
    Node* row = doc.createNode(NodeType::View);
    row->setProps(Value::parseJson(R"({"style":{"flexDirection":"row"}})"));
    Node* check = doc.createNode(NodeType::View);
    check->setProps(Value::parseJson(R"({"style":{"width":20,"height":20}})"));
    *inner = doc.createNode(NodeType::View);
    (*inner)->setProps(Value::parseJson(R"({"style":{"flex":1}})"));
    Node* content = doc.createNode(NodeType::View);
    content->setProps(Value::parseJson("{\"style\":{\"width\":" + std::to_string(contentWidth) + ",\"height\":10}}"));
    (*inner)->appendChild(content);
    row->appendChild(check);
    row->appendChild(*inner);
    col->appendChild(row);
    stack->appendChild(col);
    return col;
  };
  Node *shortInner, *longInner;
  Node* shortCol = point(100, &shortInner);
  Node* longCol = point(600, &longInner);
  doc.layout();
  CHECK_NEAR(shortCol->layoutRect().width, 120, 0.01);
  CHECK_NEAR(shortInner->layoutRect().width, 100, 0.01);
  CHECK_NEAR(longCol->layoutRect().width, 354, 0.01);
  CHECK_NEAR(longInner->layoutRect().width, 334, 0.01);
  CHECK_NEAR(longCol->layoutRect().height, 20, 0.01);
  // `flex: N > 0` maps to flexShrink 1 unless flexShrink is given.
  CHECK_NEAR(YGNodeStyleGetFlexShrink(shortInner->yoga()), 1, 1e-6);
}

TEST(style_paint_only_changes_skip_layout) {
  Document doc;
  doc.setViewport(100, 100, 1);
  Node* v = doc.createNode(NodeType::View);
  v->setProps(Value::parseJson(R"({"style":{"width":50,"height":50,"opacity":1,"transform":[{"translateX":0}]}})"));
  doc.root()->appendChild(v);
  CHECK(doc.layout());
  CHECK(!doc.layout());
  uint32_t pv = v->paintVersion;
  v->setProps(Value::parseJson(R"({"style":{"width":50,"height":50,"opacity":0.5,"transform":[{"translateX":10}],"backgroundColor":"red"}})"));
  CHECK(v->paintVersion != pv);
  CHECK(doc.paintDirty());
  CHECK(!doc.layout());  // no Yoga pass for paint-only changes
  v->setProps(Value::parseJson(R"({"style":{"width":60,"height":50}})"));
  CHECK(doc.layout());
  CHECK_NEAR(v->layoutRect().width, 60, 0.01);
  // Text: color change repaints, fontSize change re-measures.
  Node* t = doc.createNode(NodeType::Text);
  Node* raw = doc.createNode(NodeType::RawText);
  raw->setText("Hello");
  t->appendChild(raw);
  doc.root()->appendChild(t);
  t->setProps(Value::parseJson(R"({"style":{"color":"red"}})"));
  doc.layout();
  t->setProps(Value::parseJson(R"({"style":{"color":"blue"}})"));
  CHECK(!doc.layout());
  t->setProps(Value::parseJson(R"({"style":{"color":"blue","fontSize":30}})"));
  CHECK(doc.layout());
}

// ---------------------------------------------- incremental (paint-only) restyle
namespace {
bool sameLength(const Length& a, const Length& b) { return a.value == b.value && a.percent == b.percent; }
bool sameStyle(const ComputedStyle& a, const ComputedStyle& b) {
  bool ok = a.backgroundColor == b.backgroundColor && a.opacity == b.opacity && a.borderStyle == b.borderStyle &&
            a.overflow == b.overflow && a.zIndex == b.zIndex && a.hasZIndex == b.hasZIndex &&
            a.pointerEvents == b.pointerEvents && a.cursor == b.cursor && a.display == b.display &&
            a.resizeMode == b.resizeMode && a.hasResizeMode == b.hasResizeMode &&
            a.tintColor.has_value() == b.tintColor.has_value() && (!a.tintColor || *a.tintColor == *b.tintColor) &&
            a.text == b.text && textMetricsEqual(a.text, b.text) && a.backfaceHidden == b.backfaceHidden &&
            a.blendMode == b.blendMode && a.outlineWidth == b.outlineWidth && a.outlineOffset == b.outlineOffset &&
            a.outlineColor == b.outlineColor && a.outlineStyle == b.outlineStyle && a.blur == b.blur &&
            a.brightness == b.brightness && a.contrast == b.contrast && a.saturate == b.saturate &&
            a.grayscale == b.grayscale && a.boxShadows == b.boxShadows &&
            a.backgroundImages.size() == b.backgroundImages.size() && a.transform.size() == b.transform.size() &&
            a.filters.size() == b.filters.size();
  for (int i = 0; i < 4; i++)
    ok = ok && a.borderWidth[i] == b.borderWidth[i] && a.borderColor[i] == b.borderColor[i] &&
         sameLength(a.borderRadius[i], b.borderRadius[i]);
  for (int i = 0; i < 2; i++) ok = ok && sameLength(a.transformOrigin[i], b.transformOrigin[i]);
  for (size_t i = 0; ok && i < a.transform.size(); i++)
    ok = a.transform[i].kind == b.transform[i].kind && a.transform[i].a == b.transform[i].a &&
         a.transform[i].b == b.transform[i].b;
  for (size_t i = 0; ok && i < a.backgroundImages.size(); i++)
    ok = a.backgroundImages[i].stops.size() == b.backgroundImages[i].stops.size() &&
         a.backgroundImages[i].angle == b.backgroundImages[i].angle;
  for (size_t i = 0; ok && i < a.filters.size(); i++)
    ok = a.filters[i].kind == b.filters[i].kind && a.filters[i].amount == b.filters[i].amount;
  return ok;
}
}  // namespace

TEST(style_incremental_paint_changes_match_full_restyle) {
  const char* styles[] = {
      R"j({})j",
      R"j({"width":10,"opacity":0.5,"transform":[{"translateX":4},{"scale":1.2}],"backgroundColor":"red"})j",
      R"j({"width":10,"opacity":0.25,"transform":[{"rotate":"45deg"}],"backgroundColor":"#123","borderRadius":6,
          "borderTopLeftRadius":"50%","borderColor":"blue","borderLeftColor":"lime"})j",
      R"j({"width":10,"boxShadow":"0 2px 4px rgba(0,0,0,.5)","shadowColor":"#000","shadowOpacity":0.3,
          "shadowRadius":3,"shadowOffset":{"width":1,"height":2},
          "experimental_backgroundImage":"linear-gradient(90deg, red, blue)","filter":"blur(2px) brightness(1.5)",
          "zIndex":3})j",
      R"j({"width":10,"color":"#eee","fontSize":18,"fontWeight":"700","textShadow":"0 1px 2px black",
          "letterSpacing":1,"textAlign":"center","transformOrigin":"0 0","cursor":"pointer"})j",
      R"j({"width":10,"pointerEvents":"box-none","tintColor":"red","resizeMode":"contain",
          "backfaceVisibility":"hidden","mixBlendMode":"multiply","outlineWidth":2.5,"outlineColor":"red",
          "outlineStyle":"dashed","outlineOffset":1,"borderStyle":"dotted",
          "backgroundImage":"radial-gradient(circle, red, blue)"})j",
      R"j({"width":10,"overflow":"hidden","opacity":0.5})j",
      R"j({"width":20,"opacity":0.5})j",
  };
  YGNodeRef y = YGNodeNew();
  int incremental = 0, full = 0;
  for (const char* from : styles) {
    for (const char* to : styles) {
      Value a = Value::parseJson(from), b = Value::parseJson(to);
      ComputedStyle s, expect;
      applyStyle(a, &s, y);
      bool changed = false;
      if (applyPaintStyleChanges(a, b, &s, 1, &changed)) {
        incremental++;
        applyStyle(b, &expect, y);
        if (!sameStyle(s, expect)) std::fprintf(stderr, "  incremental restyle differs: %s -> %s\n", from, to);
        CHECK(sameStyle(s, expect));
        CHECK_EQ(changed, !styleValueEquals(a, b));
      } else {
        full++;
      }
    }
  }
  // Layout keys (width, overflow) take the full path.
  CHECK(incremental > 20);
  CHECK(full > 10);
  ComputedStyle s;
  bool changed = false;
  CHECK(!applyPaintStyleChanges(Value::parseJson(R"({"width":1})"), Value::parseJson(R"({"width":2})"), &s, 1, &changed));
  CHECK(!applyPaintStyleChanges(Value::parseJson(R"({"opacity":1})"), Value::parseJson(R"({"margin":2})"), &s, 1, &changed));
  CHECK(!applyPaintStyleChanges(Value(), Value::parseJson(R"({"opacity":1})"), &s, 1, &changed));
  // Unknown keys are not assumed to be paint-only.
  CHECK(!applyPaintStyleChanges(Value::parseJson("{}"), Value::parseJson(R"({"someFutureKey":1})"), &s, 1, &changed));
  YGNodeFree(y);
}

TEST(style_incremental_restyle_never_dirties_layout) {
  Document doc;
  Node* n = doc.createNode(NodeType::View);
  doc.root()->appendChild(n);
  n->setProps(Value::parseJson(R"({"style":{"width":50,"height":20,"opacity":1,"backgroundColor":"red"}})"));
  doc.layout();
  uint64_t resolves = doc.perf.counters.styleResolves;
  uint64_t passes = doc.perf.counters.layoutPasses;
  const char* moved = R"({"style":{"width":50,"height":20,"opacity":0.5,"backgroundColor":"red","transform":[{"translateX":3}]}})";
  n->setProps(Value::parseJson(moved));
  CHECK(!doc.layout());  // paint-only: no layout pass
  CHECK_NEAR(n->style().opacity, 0.5, 1e-6);
  CHECK_EQ(n->style().transform.size(), size_t(1));
  CHECK_EQ(doc.perf.counters.styleResolves, resolves + 1);
  // The pointerEvents prop overrides the style and goes away with the prop.
  n->setProps(Value::parseJson(
      R"({"pointerEvents":"none","style":{"width":50,"height":20,"opacity":0.5,"backgroundColor":"red","transform":[{"translateX":3}]}})"));
  CHECK(n->style().pointerEvents == PointerEvents::None);
  n->setProps(Value::parseJson(moved));
  CHECK(n->style().pointerEvents == PointerEvents::Auto);
  CHECK(!doc.layout());
  CHECK_EQ(doc.perf.counters.layoutPasses, passes);
  n->setProps(Value::parseJson(R"({"style":{"width":60,"height":20}})"));
  CHECK(doc.layout());
  CHECK_NEAR(n->layoutRect().width, 60, 1e-6);
  CHECK_NEAR(n->style().opacity, 1, 1e-6);
  CHECK(n->style().transform.empty());
}
