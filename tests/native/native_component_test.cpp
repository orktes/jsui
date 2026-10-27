// Native components (render/native_component.h, jsui.h "Native components"):
// a host class drawing into its view's content box through the C canvas
// API, binding in either order (mount then register, register then mount),
// the props callback (React changes only, not style), prop readers (plain
// props, `prop:<name>` style keys over them, colors, number arrays, JSON),
// repaints (invalidate, clocked classes, Animated patchStyle), commands,
// events (queued, only those the view lists, through the JS bridge's
// componentevent) and unregistering / unmounting (destroy).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "js/engine.h"
#include "jsui/jsui.h"
#include "jsui/jsui_component.hpp"
#include "render/native_component.h"
#include "render_util.h"
#include "test.h"

using rtest::Scene;

namespace {

constexpr uint32_t kRed = 0xff0000ff, kGreen = 0xff00ff00, kBlue = 0xffff0000, kWhite = 0xffffffff;

// A class whose callbacks record what happened. draw fills the content box
// with the `color` prop and draws a `bar` (0..1 of the width) in blue.
struct Recorder {
  int creates = 0, destroys = 0, draws = 0;
  std::vector<std::string> props, commands;
  int lastView = 0;
  float w = 0, h = 0;
  double bar = -1, scale = 0;
  std::vector<float> nums;
  std::string json;
  std::string text;
  JsuiComponentClass cls() {
    JsuiComponentClass c;
    jsui_component_class_init(&c);
    c.user = this;
    c.create = [](void* user, JsuiRuntime*, int view) -> void* {
      auto* r = static_cast<Recorder*>(user);
      r->creates++;
      r->lastView = view;
      return r;
    };
    c.destroy = [](void* inst, int) { static_cast<Recorder*>(inst)->destroys++; };
    c.props = [](void* inst, int, const char* json) { static_cast<Recorder*>(inst)->props.push_back(json); };
    c.command = [](void* inst, int, const char* name, const char* args) {
      static_cast<Recorder*>(inst)->commands.push_back(std::string(name) + args);
    };
    c.draw = [](void* inst, JsuiCanvas* cv, const JsuiDrawInfo* info) {
      auto* r = static_cast<Recorder*>(inst);
      r->draws++;
      r->w = info->width;
      r->h = info->height;
      JsuiPaint p;
      jsui_paint_init(&p);
      p.color = jsui_prop_color(cv, "color", 0xffffffffu);
      jsui_canvas_draw_rect(cv, 0, 0, info->width, info->height, &p);
      r->bar = jsui_prop_number(cv, "bar", 0);
      r->scale = jsui_prop_number(cv, "scale", -1);
      p.color = 0xff0000ffu;  // blue (ARGB)
      jsui_canvas_draw_rect(cv, 0, 0, float(r->bar) * info->width, info->height, &p);
      float buf[8];
      int n = jsui_prop_numbers(cv, "nums", buf, 8);
      r->nums.assign(buf, buf + std::min(n, 8));
      const char* j = jsui_prop_json(cv, "data");
      r->json = j ? j : "";
      const char* s = jsui_prop_string(cv, "label");
      r->text = s ? s : "";
      // An unbalanced restore must not pop the painter's state.
      jsui_canvas_restore(cv);
    };
    return c;
  }
};

struct CompScene : Scene {
  CompScene(int w, int h, float dpr = 1) : Scene(w, h, dpr) {}
  // Incremental, like a host: only what changed is repainted.
  void frame(double now) {
    if (px.size() != size_t(dw()) * dh()) px.assign(size_t(dw()) * dh(), 0);
    rt->tick(now);
    rt->renderIncremental(px.data(), dw(), dh(), size_t(dw()) * 4);
  }
  jsui::Node* view(const std::string& component, const std::string& props, const std::string& style) {
    jsui::Value::Object p = props.empty() ? jsui::Value::Object() : jsui::Value::parseJson("{" + props + "}").asObject();
    p["component"] = jsui::Value(component);
    p["style"] = jsui::Value::parseJson("{" + style + "}");
    jsui::Node* n = doc().createNode(jsui::NodeType::Component);
    n->setProps(jsui::Value(std::move(p)));
    doc().root()->appendChild(n);
    return n;
  }
  jsui::NativeComponents& comps() { return *rt->components; }
};

}  // namespace

TEST(component_draws_into_the_content_box) {
  CompScene s(100, 60, 2);
  Recorder r;
  CHECK_EQ(s.comps().registerClass("Gauge", r.cls()), 1);
  s.view("Gauge", R"("color":"#00ff00","bar":0.5)",
         R"("position":"absolute","left":10,"top":10,"width":60,"height":30,"padding":5,"backgroundColor":"#ff0000")");
  s.frame(0);
  s.save("component_gauge");
  CHECK_EQ(r.creates, 1);
  CHECK_EQ(r.draws, 1);
  CHECK_NEAR(r.w, 50, 1e-3);  // the content box, logical px
  CHECK_NEAR(r.h, 20, 1e-3);
  // Padding (the View's background) stays red; the content box: blue bar
  // over the left half, green right half (device px = 2x).
  CHECK_EQ(s.at(2 * 12, 2 * 12), kRed);
  CHECK_EQ(s.at(2 * 20, 2 * 25), kBlue);
  CHECK_EQ(s.at(2 * 55, 2 * 25), kGreen);
  CHECK_EQ(s.at(2 * 80, 2 * 25), 0u);  // outside the view
}

TEST(component_binds_in_either_order_and_unbinds) {
  CompScene s(40, 20);
  Recorder r;
  jsui::Node* v = s.view("Late", R"("color":"#00ff00")", R"("width":40,"height":20,"backgroundColor":"#ff0000")");
  s.frame(0);
  CHECK_EQ(s.at(5, 5), kRed);  // only the View part
  CHECK_EQ(r.creates, 0);
  CHECK(!s.comps().has("Late"));
  s.comps().registerClass("Late", r.cls());
  CHECK(s.comps().has("Late"));
  CHECK_EQ(r.creates, 1);
  CHECK_EQ(r.lastView, int(v->id()));
  CHECK_EQ(r.props.size(), size_t(1));  // right after create
  s.frame(16);
  CHECK_EQ(s.at(5, 5), kGreen);
  // Unregister: destroy, then the View part again.
  CHECK(s.comps().unregisterClass("Late"));
  CHECK_EQ(r.destroys, 1);
  s.frame(32);
  CHECK_EQ(s.at(5, 5), kRed);
  // Re-register and unmount: destroy once.
  s.comps().registerClass("Late", r.cls());
  CHECK_EQ(r.creates, 2);
  s.doc().root()->removeChild(v);
  s.doc().destroyNode(v);
  CHECK_EQ(r.destroys, 2);
  s.frame(48);
  CHECK_EQ(r.destroys, 2);
  // A draw callback is required.
  JsuiComponentClass none;
  jsui_component_class_init(&none);
  CHECK_EQ(s.comps().registerClass("None", none), -1);
}

TEST(component_props_callback_and_readers) {
  CompScene s(40, 20);
  Recorder r;
  s.comps().registerClass("P", r.cls());
  jsui::Node* v = s.view("P", R"("bar":0.25,"nums":[1,[2,3]],"data":{"a":[1,2]},"label":"hi","onPress":1)",
                         R"("width":40,"height":20)");
  CHECK_EQ(r.props.size(), size_t(1));
  jsui::Value first = jsui::Value::parseJson(r.props[0]);
  CHECK_EQ(first["bar"].asNumber(), 0.25);
  CHECK(!first.has("style"));
  CHECK(!first.has("component"));
  s.frame(0);
  CHECK_EQ(r.bar, 0.25);
  CHECK_EQ(r.scale, -1);
  CHECK_EQ(r.nums.size(), size_t(3));
  CHECK_EQ(r.nums[2], 3.f);
  CHECK_EQ(r.json, std::string(R"({"a":[1,2]})"));
  CHECK_EQ(r.text, std::string("hi"));
  // A style-only change: no props call. A prop change: one.
  jsui::Value::Object p = v->props().asObject();
  p["style"] = jsui::Value::parseJson(R"({"width":40,"height":20,"opacity":0.5})");
  v->setProps(jsui::Value(p));
  CHECK_EQ(r.props.size(), size_t(1));
  p["bar"] = jsui::Value(0.5);
  v->setProps(jsui::Value(p));
  CHECK_EQ(r.props.size(), size_t(2));
  // Animated props: prop:<name> style keys win over the prop, and
  // patchStyle repaints without a props call.
  int draws = r.draws;
  s.frame(16);
  CHECK(r.draws > draws);
  CHECK(v->patchStyle({{"prop:bar", jsui::Value(0.75)}}));
  s.frame(32);
  CHECK_EQ(r.bar, 0.75);
  CHECK_EQ(r.props.size(), size_t(2));
}

TEST(component_repaints_on_invalidate_and_clock) {
  CompScene s(80, 80);
  Recorder r;
  s.comps().registerClass("Still", r.cls());
  jsui::Node* v = s.view("Still", "", R"("width":40,"height":20)");
  s.frame(0);
  s.frame(16);
  CHECK(!s.rt->needsRedraw());
  CHECK_EQ(r.draws, 1);  // nothing changed: the recording is reused
  CHECK(!s.comps().animating());
  CHECK_EQ(s.comps().invalidate(v->id(), nullptr), 1);
  s.frame(32);
  CHECK_EQ(r.draws, 2);
  CHECK_EQ(s.comps().invalidate(0, "Still"), 1);
  CHECK_EQ(s.comps().invalidate(0, "Other"), 0);
  s.frame(48);
  CHECK_EQ(r.draws, 3);
  // A clocked class repaints every tick and keeps frames coming.
  Recorder c;
  JsuiComponentClass cc = c.cls();
  cc.flags = JSUI_COMPONENT_CLOCKED;
  s.comps().registerClass("Clock", cc);
  s.view("Clock", "", R"("position":"absolute","left":60,"top":60,"width":10,"height":10)");
  s.frame(64);
  int still = r.draws;  // (mounting a sibling may repaint it once)
  s.frame(80);
  s.frame(96);
  CHECK_EQ(c.draws, 3);
  CHECK(s.comps().animating());
  CHECK(s.rt->wantsFrame());
  CHECK_EQ(r.draws, still);  // the other view is still reused
}

TEST(component_commands_and_events) {
  CompScene s(40, 20);
  Recorder r;
  s.comps().registerClass("C", r.cls());
  jsui::Node* v = s.view("C", R"("__componentEvents":["hit"])", R"("width":40,"height":20)");
  CHECK(s.comps().command(v, "zoomTo", jsui::Value::parseJson("[2,{\"x\":1}]")));
  CHECK_EQ(r.commands.size(), size_t(1));
  CHECK_EQ(r.commands[0], std::string(R"(zoomTo[2,{"x":1}])"));
  // Events: only those the view lists, queued until the tick delivers them.
  CHECK_EQ(s.comps().emit(v->id(), "hit", R"({"id":3})"), 1);
  CHECK_EQ(s.comps().emit(v->id(), "miss", "{}"), 0);
  CHECK_EQ(s.comps().emit(99999, "hit", "{}"), -1);
  CHECK_EQ(s.comps().events.size(), size_t(1));
  CHECK(s.comps().animating());
  s.frame(0);  // no JS here: delivery drops them
  // Unbound view: no command.
  jsui::Node* u = s.view("Nope", "", R"("width":4,"height":4)");
  CHECK(!s.comps().command(u, "x", jsui::Value()));
}

// The JS side: componentevent reaches the view's handler; commands and
// hasComponent through __jsui_native.
TEST(component_js_bridge) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 40;
  cfg.height = 20;
  std::vector<std::string> messages;
  cfg.on_message = [](void* u, const char* name, const char* json) {
    static_cast<std::vector<std::string>*>(u)->push_back(std::string(name) + ":" + (json ? json : ""));
  };
  cfg.message_user = &messages;
  JsuiRuntime* rt = jsui_create(&cfg);
  Recorder r;
  JsuiComponentClass cls = r.cls();
  CHECK_EQ(jsui_register_component(rt, "Radar", &cls), 1);
  const char* src = R"(
    const n = __jsui_native;
    const id = n.createNode('NativeComponentView');
    n.setProps(id, {component: 'Radar', __events: ['componentEvent'], __componentEvents: ['contact'],
                    style: {width: 40, height: 20}});
    n.appendChild(n.rootId, id);
    globalThis.__jsui_dispatchEvent = (ev) => {
      if (ev.type === 'componentevent') n.postMessage('event', {name: ev.name, data: ev.data, target: ev.target === id});
    };
    n.postMessage('has', {radar: n.hasComponent('Radar'), other: n.hasComponent('Other'),
                          cmd: n.componentCommand(id, 'ping', [1, 'a'])});
    n.postMessage('id', id);
  )";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "component.js"), 0);
  jsui_tick(rt, 16);
  int id = 0;
  for (const std::string& m : messages)
    if (m.compare(0, 3, "id:") == 0) id = std::atoi(m.c_str() + 3);
  CHECK(id > 0);
  CHECK_EQ(r.creates, 1);
  CHECK_EQ(r.commands.size(), size_t(1));
  if (!r.commands.empty()) CHECK_EQ(r.commands[0], std::string(R"(ping[1,"a"])"));
  bool sawHas = false;
  for (const std::string& m : messages)
    if (m == R"(has:{"radar":true,"other":false,"cmd":true})") sawHas = true;
  CHECK(sawHas);
  CHECK_EQ(jsui_component_emit(rt, id, "contact", R"({"id":7})"), 1);
  CHECK_EQ(jsui_component_emit(rt, id, "other", nullptr), 0);
  jsui_tick(rt, 32);
  bool sawEvent = false;
  for (const std::string& m : messages)
    if (m == R"(event:{"name":"contact","data":{"id":7},"target":true})") sawEvent = true;
  CHECK(sawEvent);
  std::vector<uint32_t> px(40 * 20);
  jsui_render_rgba(rt, px.data(), 40, 20, 40 * 4);
  CHECK_EQ(px[5 * 40 + 30], kWhite);  // the default color prop
  CHECK_EQ(jsui_component_invalidate(rt, id, nullptr), 1);
  CHECK_EQ(jsui_unregister_component(rt, "Radar"), 1);
  CHECK_EQ(jsui_unregister_component(rt, "Radar"), 0);
  CHECK_EQ(r.destroys, 1);
  jsui_destroy(rt);
  CHECK_EQ(r.destroys, 1);
}

// A JsuiPath and the rest of the canvas calls draw where expected.
TEST(component_canvas_calls) {
  CompScene s(60, 60);
  JsuiComponentClass c;
  jsui_component_class_init(&c);
  c.draw = [](void*, JsuiCanvas* cv, const JsuiDrawInfo* info) {
    JsuiPaint p;
    jsui_paint_init(&p);
    p.color = 0xffff0000u;  // red
    JsuiPath* path = jsui_path_from_svg("M0 0 L30 0 L30 30 Z");
    jsui_canvas_draw_path(cv, path, &p);
    jsui_path_destroy(path);
    jsui_canvas_save(cv);
    jsui_canvas_translate(cv, 30, 30);
    p.color = 0xff00ff00u;  // green
    jsui_canvas_draw_circle(cv, 15, 15, 10, &p);
    jsui_canvas_restore(cv);
    // Stroke with a sweep gradient, a line, text, a missing image.
    uint32_t colors[2] = {0xff0000ffu, 0xff0000ffu};
    JsuiGradient g{};
    g.type = JSUI_GRADIENT_SWEEP;
    g.x0 = 10;
    g.y0 = 50;
    g.end = 360;
    g.colors = colors;
    g.count = 2;
    p.gradient = &g;
    jsui_canvas_draw_rect(cv, 0, 40, 20, 20, &p);
    p.gradient = nullptr;
    JsuiFont f{};
    f.size = 10;
    CHECK(jsui_canvas_measure_text(cv, "abc", &f) > 5);
    CHECK(jsui_canvas_draw_text(cv, "abc", 30, 10, &f, &p) > 5);
    CHECK_EQ(jsui_canvas_draw_texture(cv, "nope", 0, 0, 1, 1, 1), 0);
    CHECK(jsui_canvas_skia(cv) != nullptr);
    CHECK(info->width == 60);
  };
  s.comps().registerClass("K", c);
  s.view("K", "", R"("width":60,"height":60)");
  s.frame(0);
  s.save("component_canvas");
  CHECK_EQ(s.at(25, 5), kRed);    // inside the triangle
  CHECK_EQ(s.at(5, 25), 0u);      // below its diagonal
  CHECK_EQ(s.at(45, 45), kGreen); // the translated circle
  CHECK_EQ(s.at(10, 50), kBlue);  // the gradient rect
}

// The C++ helpers (jsui_component.hpp): one instance per view, props,
// commands, emit and invalidate from the instance.
namespace {
struct Dial : jsui::Component {
  static int live;
  std::string lastProps, lastCommand;
  Dial() { live++; }
  ~Dial() override { live--; }
  void props(const char* json) override { lastProps = json; }
  void command(const char* name, const char* args) override {
    lastCommand = std::string(name) + args;
    emit("ack", R"({"ok":true})");
    invalidate();
  }
  void draw(jsui::Canvas& c, const JsuiDrawInfo& info) override {
    c.rect(0, 0, info.width, info.height, jsui::Paint(c.color("color", 0xffffffffu)));
    jsui::Path tri;
    tri.moveTo(0, 0).lineTo(10, 0).lineTo(0, 10).close();
    c.path(tri, jsui::Paint(0xffff0000u));
    c.circle(15, 15, 2, jsui::Paint(0xff0000ffu).stroke(1).round());
    c.text("x", 2, 18, jsui::Font(8).centered(), jsui::Paint(0xff000000u));
  }
};
int Dial::live = 0;
}  // namespace

TEST(component_cpp_helpers) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 20;
  cfg.height = 20;
  JsuiRuntime* rt = jsui_create(&cfg);
  CHECK_EQ(jsui::registerComponent<Dial>(rt, "Dial"), 1);
  jsui::Runtime* r = jsui::internalRuntime(rt);
  jsui::Node* n = r->doc.createNode(jsui::NodeType::Component);
  n->setProps(jsui::Value::parseJson(
      R"({"component":"Dial","color":"#00ff00","__componentEvents":["ack"],"style":{"width":20,"height":20}})"));
  r->doc.root()->appendChild(n);
  CHECK_EQ(Dial::live, 1);
  CHECK(r->components->command(n, "spin", jsui::Value::parseJson("[90]")));
  CHECK_EQ(r->components->events.size(), size_t(1));
  std::vector<uint32_t> px(20 * 20);
  jsui_tick(rt, 16);
  jsui_render_rgba(rt, px.data(), 20, 20, 20 * 4);
  CHECK_EQ(px[2 * 20 + 2], kRed);     // the triangle
  CHECK_EQ(px[18 * 20 + 18], kGreen); // the `color` prop
  r->doc.root()->removeChild(n);
  r->doc.destroyNode(n);
  CHECK_EQ(Dial::live, 0);
  jsui_destroy(rt);
}
