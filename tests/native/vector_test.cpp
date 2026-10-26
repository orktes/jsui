// VectorView (render/vector_view.h): the drawing commands of react-native-svg's
// <Svg> and react-native-skia's <Canvas>, painted with core Skia. Checked on
// the CPU raster: viewBox fitting, SVG paint (fill, stroke, gradients in
// bbox units, currentColor, opacity), clip paths and masks, Skia paint
// (style, shaders, path trimming, blend modes), Animated `draw:<k>.<prop>`
// style keys (patchStyle repaints only the view, no re-parse), and the
// commands being parsed once per change.
#include <cstdio>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/painter.h"
#include "render/vector_view.h"
#include "render_util.h"
#include "test.h"

using rtest::Scene;

namespace {

constexpr uint32_t kRed = 0xff0000ff, kGreen = 0xff00ff00, kBlue = 0xffff0000, kWhite = 0xffffffff;

struct VectorScene : Scene {
  VectorScene(int w, int h, float dpr = 1) : Scene(w, h, dpr) {
    add(R"({"style":{"position":"absolute","left":0,"top":0,"right":0,"bottom":0,"backgroundColor":"#ffffff"}})");
  }
  void frame(double now) {
    px.assign(size_t(dw()) * dh(), 0);
    rt->tick(now);
    rt->render(px.data(), dw(), dh(), size_t(dw()) * 4);
  }
  // A VectorView with `commands` (JSON) and a style (JSON object body).
  jsui::Node* view(const std::string& commands, const std::string& style) {
    jsui::Value::Object p;
    p["commands"] = jsui::Value::parseJson(commands);
    p["style"] = jsui::Value::parseJson("{" + style + "}");
    jsui::Node* n = doc().createNode(jsui::NodeType::Vector);
    n->setProps(jsui::Value(std::move(p)));
    doc().root()->appendChild(n);
    return n;
  }
};

}  // namespace

TEST(vector_svg_viewbox_fill_stroke) {
  VectorScene s(100, 100);
  // viewBox 0 0 10 10 into 100 x 100: a circle of radius 40 px at the center,
  // a 1-unit (10 px) blue stroke.
  s.view(R"({"mode":"svg","vb":[0,0,10,10],"c":[
    {"t":"circle","cx":5,"cy":5,"r":3.5,"fill":"#ff0000","stroke":"#0000ff","strokeWidth":1}]})",
         R"("position":"absolute","left":0,"top":0,"width":100,"height":100)");
  s.frame(0);
  s.save("vector_svg_viewbox");
  CHECK_EQ(s.at(50, 50), kRed);
  CHECK_EQ(s.at(50, 12), kBlue);  // on the stroke (r 35 px +- 5)
  CHECK_EQ(s.at(2, 2), kWhite);
  CHECK_EQ(jsui::vector::elementCount(s.doc().root()->children()[1]), size_t(1));
}

TEST(vector_svg_aspect_ratio_and_current_color) {
  VectorScene s(200, 100);
  // A square viewBox in a 200 x 100 view: meet centers it (x 50..150).
  s.view(R"({"mode":"svg","vb":[0,0,10,10],"color":"#00ff00","c":[
    {"t":"rect","x":0,"y":0,"width":10,"height":10,"fill":"currentColor"}]})",
         R"("position":"absolute","left":0,"top":0,"width":200,"height":100)");
  s.frame(0);
  CHECK_EQ(s.at(40, 50), kWhite);
  CHECK_EQ(s.at(60, 50), kGreen);
  CHECK_EQ(s.at(160, 50), kWhite);
  VectorScene t(200, 100);
  t.view(R"({"mode":"svg","vb":[0,0,10,10],"align":"xMinYMin","c":[
    {"t":"rect","x":0,"y":0,"width":10,"height":10,"fill":"#00ff00"}]})",
         R"("position":"absolute","left":0,"top":0,"width":200,"height":100)");
  t.frame(0);
  CHECK_EQ(t.at(10, 50), kGreen);
  CHECK_EQ(t.at(110, 50), kWhite);
}

TEST(vector_svg_gradient_bbox_units) {
  VectorScene s(100, 50);
  s.view(R"({"mode":"svg","c":[{"t":"rect","x":0,"y":0,"width":100,"height":50,
    "fill":{"t":"linear","units":"bbox","x1":0,"y1":0,"x2":1,"y2":0,"stops":[[0,"#ff0000",1],[1,"#0000ff",1]]}}]})",
         R"("position":"absolute","left":0,"top":0,"width":100,"height":50)");
  s.frame(0);
  CHECK(s.r(2, 25) > 240 && s.b(2, 25) < 15);
  CHECK(s.b(97, 25) > 240 && s.r(97, 25) < 15);
  CHECK_NEAR(s.r(50, 25), 127, 6);
}

TEST(vector_svg_clip_and_mask) {
  VectorScene s(100, 100);
  s.view(R"({"mode":"svg","c":[
    {"t":"rect","x":0,"y":0,"width":100,"height":50,"fill":"#ff0000",
     "clip":{"c":[{"t":"circle","cx":50,"cy":50,"r":20}]}},
    {"t":"rect","x":0,"y":50,"width":100,"height":50,"fill":"#0000ff",
     "mask":{"c":[{"t":"rect","x":0,"y":50,"width":50,"height":50,"fill":"white"}]}}]})",
         R"("position":"absolute","left":0,"top":0,"width":100,"height":100)");
  s.frame(0);
  CHECK_EQ(s.at(50, 40), kRed);    // inside the clip circle
  CHECK_EQ(s.at(10, 10), kWhite);  // clipped away
  CHECK_EQ(s.at(25, 75), kBlue);   // white in the mask
  CHECK_EQ(s.at(75, 75), kWhite);  // no mask: transparent
}

TEST(vector_svg_group_opacity_is_one_layer) {
  VectorScene s(100, 50);
  // Two overlapping opaque rects in a 50% group: the overlap is not darker.
  s.view(R"({"mode":"svg","c":[{"t":"g","opacity":0.5,"c":[
    {"t":"rect","x":0,"y":0,"width":60,"height":50,"fill":"#000000"},
    {"t":"rect","x":40,"y":0,"width":60,"height":50,"fill":"#000000"}]}]})",
         R"("position":"absolute","left":0,"top":0,"width":100,"height":50)");
  s.frame(0);
  CHECK_NEAR(s.r(20, 25), 128, 2);
  CHECK_EQ(s.r(50, 25), s.r(20, 25));
}

TEST(vector_skia_paint_shader_trim_blend) {
  VectorScene s(120, 60);
  s.view(R"({"mode":"skia","c":[
    {"t":"rect","x":0,"y":0,"width":60,"height":60,"color":"#ff0000"},
    {"t":"rect","x":60,"y":0,"width":60,"height":60,
     "shader":{"t":"linear","start":[60,0],"end":[120,0],"colors":["#00ff00","#0000ff"]}},
    {"t":"path","d":"M0 30 L120 30","style":"stroke","strokeWidth":10,"color":"#000000","end":0.25},
    {"t":"g","blendMode":"multiply","c":[{"t":"circle","cx":90,"cy":50,"r":5,"color":"#ff0000"}]}]})",
         R"("position":"absolute","left":0,"top":0,"width":120,"height":60)");
  s.frame(0);
  s.save("vector_skia");
  CHECK_EQ(s.at(10, 10), kRed);
  CHECK(s.g(62, 10) > 240 && s.b(62, 10) < 15);
  CHECK(s.b(118, 10) > 240);
  CHECK_EQ(s.at(10, 30), 0xff000000u);  // trimmed stroke: the first quarter
  CHECK_EQ(s.at(50, 30), kRed);          // past the trim end
  // Multiply over the gradient: red x (green..blue) is black.
  CHECK(s.r(90, 50) < 10 && s.g(90, 50) < 10);
}

TEST(vector_draw_keys_patch_repaints_only_the_view) {
  VectorScene s(300, 200);
  jsui::Node* v = s.view(R"({"mode":"svg","c":[{"t":"circle","k":7,"cx":50,"cy":50,"r":10,"fill":"#ff0000"}]})",
                         R"("position":"absolute","left":100,"top":50,"width":100,"height":100)");
  int dw = s.dw(), dh = s.dh();
  std::vector<uint32_t> buf(size_t(dw) * dh, 0);
  s.rt->tick(0);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK_EQ(buf[size_t(100) * dw + 180], kWhite);  // 30 px from the center
  const jsui::Node::Cache* parsed = v->imageCache.get();
  // An Animated write (patchStyle: paint-only, no relayout).
  jsui::Value::Object partial;
  partial["draw:7.r"] = jsui::Value(40.0);
  CHECK(v->patchStyle(partial));
  s.rt->tick(16);
  CHECK(s.rt->needsRedraw());
  SkIRect dirty = s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(SkIRect::MakeXYWH(99, 49, 102, 102).contains(dirty));
  CHECK_EQ(buf[size_t(100) * dw + 180], kRed);
  CHECK(v->imageCache.get() == parsed);  // the commands were not parsed again
  // Unchanged commands in a new props object: still the same parse.
  jsui::Value::Object p = v->props().asObject();
  v->setProps(jsui::Value(p));
  CHECK(v->imageCache.get() == parsed);
  // A color and a transform through draw keys too.
  partial.clear();
  partial["draw:7.fill"] = jsui::Value("#0000ff");
  partial["draw:7.transform"] = jsui::Value::parseJson(R"([{"translateX":-40}])");
  CHECK(v->patchStyle(partial));
  s.rt->tick(32);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK_EQ(buf[size_t(100) * dw + 140], kBlue);
  CHECK_EQ(buf[size_t(100) * dw + 185], kWhite);
}

TEST(vector_text_and_anchor) {
  VectorScene s(200, 60);
  s.view(R"({"mode":"svg","c":[{"t":"text","x":100,"y":40,"text":"MMMM","fontSize":30,"textAnchor":"middle","fill":"#000000"}]})",
         R"("position":"absolute","left":0,"top":0,"width":200,"height":60)");
  s.frame(0);
  int left = s.darkPixels(0, 0, 100, 60), right = s.darkPixels(100, 0, 200, 60);
  CHECK(left > 50 && right > 50);
  CHECK(left * 10 > right * 7 && right * 10 > left * 7);  // centered on x = 100
  CHECK_EQ(s.darkPixels(0, 0, 30, 60), 0);
}
