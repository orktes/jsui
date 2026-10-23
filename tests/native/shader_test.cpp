// ShaderView (render/shader_view.h): SkSL runtime effects filling a view,
// uniforms from the `uniforms` prop and `uniform:` style keys (whole values
// and components, patchStyle), the built-in resolution / time / pixelRatio,
// the clock (repaints limited to the view, paused), child images from host
// textures, post-processing the view's own painting (`content`), compile
// errors and the JS bridge (__jsui_native.shaderInfo, error events).
#include <cstdio>
#include <string>
#include <vector>

#include "js/engine.h"
#include "jsui/jsui.h"
#include "render/painter.h"
#include "render/shader_view.h"
#include "render_util.h"
#include "test.h"

using rtest::Scene;

namespace {

constexpr uint32_t kRed = 0xff0000ff, kGreen = 0xff00ff00, kBlue = 0xffff0000;

struct ShaderScene : Scene {
  ShaderScene(int w, int h, float dpr = 1) : Scene(w, h, dpr) {}
  void frame(double now) {
    px.assign(size_t(dw()) * dh(), 0);
    rt->tick(now);
    rt->render(px.data(), dw(), dh(), size_t(dw()) * 4);
  }
  // A ShaderView of `sksl` with extra props / style (JSON object bodies).
  jsui::Node* view(const std::string& sksl, const std::string& props, const std::string& style) {
    jsui::Value::Object p = props.empty() ? jsui::Value::Object() : jsui::Value::parseJson("{" + props + "}").asObject();
    p["source"] = jsui::Value(sksl);
    p["style"] = jsui::Value::parseJson("{" + style + "}");
    jsui::Node* n = doc().createNode(jsui::NodeType::Shader);
    n->setProps(jsui::Value(std::move(p)));
    doc().root()->appendChild(n);
    return n;
  }
};

const char kColor[] = R"(layout(color) uniform half4 color; half4 main(float2 p) { return color; })";

}  // namespace

TEST(shader_fills_the_view_with_uniforms) {
  ShaderScene s(100, 50);
  s.view(kColor, R"("uniforms":{"color":"#ff0000"})", R"("width":100,"height":50)");
  s.frame(0);
  s.save("shader_fill");
  CHECK_EQ(s.at(10, 10), kRed);
  CHECK_EQ(s.at(90, 40), kRed);
  // A number array works for a color too.
  ShaderScene t(10, 10);
  t.view(kColor, R"("uniforms":{"color":[0,0,1,1]})", R"("width":10,"height":10)");
  t.frame(0);
  CHECK_EQ(t.at(5, 5), kBlue);
}

// resolution: the padding box in px; coordinates start at its top-left.
TEST(shader_resolution_and_coordinates) {
  ShaderScene s(120, 60, 2);
  s.view(R"(uniform float2 resolution; uniform float pixelRatio;
half4 main(float2 p) {
  // Green left of the middle, blue right of it; red when pixelRatio is wrong.
  if (pixelRatio != 2) return half4(1, 0, 0, 1);
  return p.x < resolution.x / 2 ? half4(0, 1, 0, 1) : half4(0, 0, 1, 1);
})",
         "", R"("position":"absolute","left":10,"top":5,"width":100,"height":50,"borderWidth":10,"borderColor":"#000")");
  s.frame(0);
  // Padding box: x 20..100 (px) => the middle at 60 => device 120.
  CHECK_EQ(s.at(2 * 50, 2 * 30), kGreen);
  CHECK_EQ(s.at(2 * 70, 2 * 30), kBlue);
  CHECK_EQ(s.at(2 * 15, 2 * 30), 0xff000000u);  // the border
}

TEST(shader_style_uniforms_override_and_patch) {
  ShaderScene s(40, 20);
  jsui::Node* v = s.view(R"(uniform float3 rgb; half4 main(float2 p) { return half4(half3(rgb), 1); })",
                         R"("uniforms":{"rgb":[1,0,0]})", R"("width":40,"height":20,"uniform:rgb":[0,1,0])");
  s.frame(0);
  CHECK_EQ(s.at(5, 5), kGreen);
  // Components (Animated values of a vector) and patchStyle (the animation path).
  CHECK(v->patchStyle({{"uniform:rgb", jsui::Value()}, {"uniform:rgb.2", jsui::Value(1.0)}, {"uniform:rgb.0", jsui::Value(0.0)}}));
  s.frame(16);
  CHECK_EQ(s.at(5, 5), kBlue);
}

// `time` runs on the runtime clock: frames are wanted, only the view
// repaints, and `paused` stops it.
TEST(shader_time_clock_repaints_only_the_view) {
  ShaderScene s(300, 200);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":300,"height":200,"backgroundColor":"#202020"}})J");
  jsui::Node* v = s.view(R"(uniform float time; half4 main(float2 p) { return half4(half(fract(time)), 0, 0, 1); })", "",
                         R"("position":"absolute","left":150,"top":100,"width":100,"height":50)");
  int dw = s.dw(), dh = s.dh();
  std::vector<uint32_t> buf(size_t(dw) * dh, 0);
  s.rt->tick(0);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(s.rt->wantsFrame());
  s.rt->tick(250);
  CHECK(s.rt->needsRedraw());
  SkIRect dirty = s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(SkIRect::MakeXYWH(149, 99, 102, 52).contains(dirty));
  int r1 = int(buf[size_t(120) * dw + 200] & 255);
  CHECK_NEAR(r1, 64, 3);  // fract(0.25) * 255
  s.rt->tick(500);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK_NEAR(int(buf[size_t(120) * dw + 200] & 255), 128, 3);
  // Paused: the time stays, nothing animates.
  jsui::Value::Object p = v->props().asObject();
  p["paused"] = jsui::Value(true);
  v->setProps(jsui::Value(p));
  s.rt->tick(600);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(!s.rt->wantsFrame());
  s.rt->tick(900);
  CHECK(!s.rt->needsRedraw());
  // Resumed where it stopped: paused at 0.5 s (clock at 500 ms), resumed
  // with the clock at 900 ms => 0.6 s at 1000 ms.
  p["paused"] = jsui::Value(false);
  v->setProps(jsui::Value(p));
  s.rt->tick(1000);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK_NEAR(int(buf[size_t(120) * dw + 200] & 255), 153, 3);
  // A `time` given by the app: no clock.
  p["uniforms"] = jsui::Value::parseJson(R"({"time":0.5})");
  v->setProps(jsui::Value(p));
  s.rt->tick(1100);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(!s.rt->wantsFrame());
}

// Child shaders from host textures, stretched over the view.
TEST(shader_child_image_from_host_texture) {
  ShaderScene s(40, 20);
  // 2 x 1: red, green.
  uint32_t texels[2] = {kRed, kGreen};
  CHECK_EQ(s.rt->textures.setPixels("tex", texels, 2, 1, 0, 0), 1);
  s.view(R"(uniform shader image; half4 main(float2 p) { return image.eval(p).gbra; })",
         R"("images":{"image":{"texture":"tex","fit":"stretch"}})", R"("width":40,"height":20)");
  s.frame(0);
  // Swizzled: red -> green... (r,g,b) = (g,b,r): red (1,0,0) -> (0,0,1) blue; green -> red.
  CHECK_EQ(s.at(3, 10), kBlue);
  CHECK_EQ(s.at(36, 10), kRed);
  // Missing children are transparent.
  ShaderScene t(10, 10);
  t.view(R"(uniform shader image; half4 main(float2 p) { return image.eval(p) + half4(0, 0, 0, 0); })", "",
         R"("width":10,"height":10,"backgroundColor":"#00ff00")");
  t.frame(0);
  CHECK_EQ(t.at(5, 5), kGreen);
}

// `content`: the view's own painting, post-processed (mirrored here).
TEST(shader_content_post_processing) {
  ShaderScene s(100, 50);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":100,"height":50,"backgroundColor":"#ffffff"}})J");
  jsui::Node* v = s.view(R"(uniform shader content; uniform float2 resolution;
half4 main(float2 p) { return content.eval(float2(resolution.x - p.x, p.y)); })",
                         "", R"("position":"absolute","left":0,"top":0,"width":100,"height":50)");
  jsui::Node* child = s.doc().createNode(jsui::NodeType::View);
  child->setProps(jsui::Value::parseJson(
      R"({"style":{"position":"absolute","left":0,"top":0,"width":30,"height":50,"backgroundColor":"#ff0000"}})"));
  v->appendChild(child);
  s.frame(0);
  s.save("shader_content");
  // The red box on the left shows on the right; the left is transparent
  // (the view had no background), so the white page shows.
  CHECK_EQ(s.at(90, 25), kRed);
  CHECK_EQ(s.at(10, 25), 0xffffffffu);
  // Incremental: the child turns blue and the mirrored pixels follow.
  int dw = s.dw(), dh = s.dh();
  std::vector<uint32_t> buf(size_t(dw) * dh, 0);
  s.rt->tick(16);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  child->patchStyle({{"backgroundColor", jsui::Value("#0000ff")}});
  s.rt->tick(32);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK_EQ(buf[size_t(25) * dw + 90], kBlue);
  CHECK_EQ(buf[size_t(25) * dw + 10], 0xffffffffu);
}

TEST(shader_compile_errors_and_bridge) {
  ShaderScene s(40, 20);
  std::string js = R"J(
var n = __jsui_native;
globalThis.log = [];
globalThis.__jsui_dispatchEvent = function (ev) { log.push(ev.type + ':' + (ev.error ? 'msg' : '')); return 1; };
var info = n.shaderInfo('uniform float a; layout(color) uniform half4 c; uniform shader img; half4 main(float2 p) { return c; }');
if (info.error) throw new Error(info.error);
if (JSON.stringify(info.uniforms.map(function (u) { return [u.name, u.type, u.color]; })) !== '[["a","float",false],["c","float4",true]]')
  throw new Error('uniforms ' + JSON.stringify(info.uniforms));
if (info.children.join() !== 'img') throw new Error('children');
if (!n.shaderInfo('half4 main(float2 p) { return nope; }').error) throw new Error('no error');
var id = n.createNode('ShaderView');
n.setProps(id, { source: 'half4 main(float2 p) { return nope; }', style: { width: 40, height: 20, backgroundColor: '#00ff00' }, __events: ['error'] });
n.appendChild(n.rootId, id);
)J";
  CHECK(s.rt->loadSource(js, "shader.js"));
  s.frame(0);
  s.frame(16);
  CHECK(s.rt->js->eval("if (log.join() !== 'error:msg') throw new Error('log: ' + log.join());", "check.js"));
  CHECK_EQ(s.at(5, 5), kGreen);  // just the background
  CHECK(!s.rt->wantsFrame());
}
