// End-to-end surfaces: one @orktes/jsui-react bundle (packages/cli/test/e2e/
// surfaces.jsx) showing several root views through jsui_surface_create.
// Surfaces created before and after the bundle loads, their own props,
// sizes, pixels, input and state, props updates that keep state, a
// content-sized surface, a component registered late, destroy (unmount),
// reload (apps start again) and a host-picked main app. Built and run by the
// @orktes/jsui-cli test suite with JSUI_REACT_SURFACES_BUNDLE=<path>; skipped
// without it.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct SurfacesE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  std::string src, path;
  double now = 0;

  bool read(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    src.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    path = p;
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<SurfacesE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    return true;
  }
  bool load() {
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~SurfacesE2E() {
    if (rt) jsui_destroy(rt);
  }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick(rt, now);
    }
  }
  std::string take() {
    std::string s;
    for (auto& [n, j] : messages) s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
  static void click(SurfacesE2E& t, JsuiRuntime* s, float x, float y) {
    jsui_send_mouse_move(s, x, y);
    jsui_send_mouse_button(s, x, y, JSUI_MOUSE_LEFT, 1, 0);
    jsui_send_mouse_button(s, x, y, JSUI_MOUSE_LEFT, 0, 0);
    t.tick(16);
  }
};

// RGBA of one device pixel of a surface rendered at w x h.
uint32_t pixel(JsuiRuntime* s, int w, int h, int x, int y) {
  std::vector<uint32_t> px(size_t(w) * h);
  jsui_render_rgba(s, px.data(), w, h, size_t(w) * 4);
  return px[size_t(y) * w + x];
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST(react_surfaces_e2e) {
  const char* path = std::getenv("JSUI_REACT_SURFACES_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_SURFACES_BUNDLE, see packages/cli/test)\n");
    return;
  }
  SurfacesE2E t;
  CHECK(t.read(path));
  JsuiRuntime* rt = t.rt;
  CHECK_EQ(jsui_surface_tag(rt), 1);

  // A surface created before the bundle exists starts once it has loaded.
  JsuiRuntime* orc = jsui_surface_create(rt, "Nameplate", R"({"name":"orc","hp":50,"color":"#ff0000"})", 120, 40, 1);
  CHECK(orc != nullptr);
  CHECK(jsui_surface_tag(orc) > 1);
  CHECK(jsui_surface_runtime(orc) == rt);
  CHECK(jsui_surface_create(rt, "Nameplate", "[1]", 10, 10, 1) == nullptr);  // props must be an object
  CHECK(jsui_surface_create(rt, nullptr, nullptr, 10, 10, 1) == nullptr);
  CHECK(t.load());
  const char* err = jsui_last_error(rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  std::string m = t.take();
  CHECK(has(m, "main={\"rootTag\":1}"));  // Main auto-started on the main surface
  std::string orcTag = std::to_string(jsui_surface_tag(orc));
  CHECK(has(m, "mount={\"name\":\"orc\",\"rootTag\":" + orcTag + "}"));
  CHECK(has(m, "\"width\":120,\"height\":40"));  // useWindowDimensions is the surface's size

  // A second surface after load, with its own props.
  JsuiRuntime* elf = jsui_surface_create(rt, "Nameplate", R"({"name":"elf","hp":80,"color":"#0000ff"})", 100, 30, 1);
  CHECK(elf != nullptr);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "mount={\"name\":\"elf\""));
  CHECK(!has(m, "\"orc\""));  // the other surfaces did not re-render

  // Each surface paints its own tree into its own target, on transparent.
  CHECK(jsui_needs_redraw(orc));
  CHECK_EQ(pixel(orc, 120, 40, 60, 20), 0xff0000ffu);  // red, RGBA little endian
  CHECK_EQ(pixel(orc, 120, 40, 1, 1), 0u);              // the padding stays transparent
  CHECK(!jsui_needs_redraw(orc));
  CHECK_EQ(pixel(elf, 100, 30, 50, 15), 0xffff0000u);  // blue
  CHECK_EQ(pixel(rt, 400, 300, 200, 150), 0xff202020u);

  // Input goes to the surface it is sent to.
  SurfacesE2E::click(t, orc, 60, 20);
  m = t.take();
  CHECK(has(m, "press={\"name\":\"orc\"}"));
  CHECK(!has(m, "mainPress"));
  CHECK(has(m, "\"presses\":1"));
  SurfacesE2E::click(t, rt, 60, 20);
  m = t.take();
  CHECK(has(m, "mainPress"));
  CHECK(!has(m, "press={"));
  SurfacesE2E::click(t, elf, 50, 15);
  CHECK(has(t.take(), "press={\"name\":\"elf\"}"));

  // New props re-render the same instance (state and mount count kept).
  CHECK_EQ(jsui_surface_set_props(orc, R"({"name":"orc","hp":10,"color":"#ff0000"})"), 0);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "\"hp\":10"));
  CHECK(has(m, "\"presses\":1,\"mounts\":1"));
  CHECK(!has(m, "mount={"));
  CHECK_EQ(jsui_surface_set_props(orc, "nope"), -1);
  // The same props again (a host pushing its state every frame) render nothing.
  CHECK_EQ(jsui_surface_set_props(orc, R"({"name":"orc","hp":10,"color":"#ff0000"})"), 0);
  t.tick(16);
  CHECK(!has(t.take(), "plate="));
  // Updates to several surfaces in one frame land in the same tick.
  CHECK_EQ(jsui_surface_set_props(orc, R"({"name":"orc","hp":9,"color":"#ff0000"})"), 0);
  CHECK_EQ(jsui_surface_set_props(elf, R"({"name":"elf","hp":70,"color":"#0000ff"})"), 0);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "\"name\":\"orc\",\"hp\":9"));
  CHECK(has(m, "\"name\":\"elf\",\"hp\":70"));
  CHECK_EQ(jsui_surface_set_props(orc, R"({"name":"orc","hp":10,"color":"#ff0000"})"), 0);
  t.tick(16);
  t.take();

  // Resizing a surface tells only its components.
  jsui_set_viewport(orc, 150, 50, 1);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "\"width\":150,\"height\":50"));
  CHECK(!has(m, "elf"));

  // Content-sized surface.
  JsuiRuntime* badge = jsui_surface_create(rt, "Badge", R"({"label":"x"})", 0, 0, 2);
  CHECK(badge != nullptr);
  t.tick(16);
  float bw = 0, bh = 0;
  CHECK_EQ(jsui_surface_get_size(badge, &bw, &bh), 0);
  CHECK_NEAR(bw, 64.f, 0.01f);
  CHECK_NEAR(bh, 24.f, 0.01f);
  CHECK_EQ(pixel(badge, 128, 48, 64, 24), 0xff00ff00u);

  // A key registered late starts its surface when it is registered.
  JsuiRuntime* late = jsui_surface_create(rt, "Late", nullptr, 50, 50, 1);
  CHECK(late != nullptr);
  CHECK(!has(t.take(), "late="));
  t.tick(400);
  CHECK(has(t.take(), "late=null"));
  CHECK_EQ(pixel(late, 50, 50, 25, 25), 0xffff0000u);

  // Destroy unmounts the app.
  jsui_surface_destroy(elf);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "unmount={\"name\":\"elf\"}"));
  CHECK(!has(m, "unmount={\"name\":\"orc\"}"));

  // Reload: the surfaces keep their handles and start again.
  jsui_reload(rt);
  t.tick(600);
  m = t.take();
  CHECK(jsui_last_error(rt) == nullptr);
  CHECK(has(m, "main={\"rootTag\":1}"));
  CHECK(has(m, "mount={\"name\":\"orc\""));
  CHECK(has(m, "\"hp\":10"));  // with the props it had
  CHECK(has(m, "late=null"));
  CHECK(!has(m, "elf"));
  CHECK_EQ(pixel(orc, 150, 50, 75, 25), 0xff0000ffu);

  // jsui_destroy frees the runtime and the surface handles still alive.
  (void)badge;
  (void)late;
}

TEST(react_surfaces_e2e_host_picks_main) {
  const char* path = std::getenv("JSUI_REACT_SURFACES_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_SURFACES_BUNDLE, see packages/cli/test)\n");
    return;
  }
  SurfacesE2E t;
  CHECK(t.read(path));
  // The host picks the main surface's app: no auto-start of the first key.
  CHECK_EQ(jsui_surface_run(t.rt, "Nameplate", R"({"name":"hero","hp":100,"color":"#00ff00"})"), 0);
  CHECK(t.load());
  std::string m = t.take();
  CHECK(!has(m, "main="));
  CHECK(has(m, "mount={\"name\":\"hero\",\"rootTag\":1}"));
  CHECK(has(m, "\"width\":400,\"height\":300"));
  CHECK_EQ(pixel(t.rt, 400, 300, 200, 150), 0xff00ff00u);
  // Switching the main surface to another app.
  CHECK_EQ(jsui_surface_run(t.rt, "Main", nullptr), 0);
  t.tick(16);
  m = t.take();
  CHECK(has(m, "unmount={\"name\":\"hero\"}"));
  CHECK(has(m, "main={\"rootTag\":1}"));
  // The main surface cannot be destroyed through the surface API.
  jsui_surface_destroy(t.rt);
  CHECK_EQ(jsui_surface_tag(t.rt), 1);
}

TEST(react_surfaces_e2e_error_isolation) {
  const char* path = std::getenv("JSUI_REACT_SURFACES_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_SURFACES_BUNDLE, see packages/cli/test)\n");
    return;
  }
  SurfacesE2E t;
  CHECK(t.read(path));
  CHECK(t.load());
  JsuiRuntime* orc = jsui_surface_create(t.rt, "Nameplate", R"({"name":"orc","hp":50,"color":"#ff0000"})", 120, 40, 1);
  JsuiRuntime* boom = jsui_surface_create(t.rt, "Boom", R"({"boom":false})", 20, 20, 1);
  t.tick(16);
  CHECK_EQ(pixel(boom, 20, 20, 10, 10), 0xffffffffu);
  t.take();
  // A surface whose app throws unmounts alone; the error is reported.
  CHECK_EQ(jsui_surface_set_props(boom, R"({"boom":true})"), 0);
  CHECK_EQ(jsui_surface_set_props(orc, R"({"name":"orc","hp":40,"color":"#ff0000"})"), 0);
  t.tick(16);
  const char* err = jsui_last_error(t.rt);
  CHECK(err != nullptr && std::string(err).find("boom") != std::string::npos);
  CHECK(has(t.take(), "\"hp\":40"));
  CHECK_EQ(pixel(boom, 20, 20, 10, 10), 0u);
  CHECK_EQ(pixel(orc, 120, 40, 60, 20), 0xff0000ffu);
  SurfacesE2E::click(t, orc, 60, 20);
  CHECK(has(t.take(), "press={\"name\":\"orc\"}"));
  // The main surface keeps running too.
  SurfacesE2E::click(t, t.rt, 60, 20);
  CHECK(has(t.take(), "mainPress"));
}
