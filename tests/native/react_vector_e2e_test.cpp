// End-to-end react-native-svg / react-native-skia: a real @orktes/jsui-react
// bundle (packages/cli/test/e2e/vector.jsx) driven through the C API.
// - <Svg> and <Canvas> are one VectorView each; their drawing elements are
//   serialized into `commands`.
// - A native timing on an Animated.createAnimatedComponent(Circle) radius
//   and on a Skia Path's `end` runs with jsui_tick_native only (no JS): the
//   runtime writes the draw:<k>.<prop> style keys, the pixels change frame
//   by frame and React never re-renders.
// - A JS-driver opacity on <G> patches the same keys without renders.
// - A state change inside the Svg (a custom component with a hook)
//   re-serializes the commands.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_VECTOR_BUNDLE.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "render/vector_view.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

constexpr int kW = 400, kH = 300;

struct VectorE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  uint32_t svg = 0, canvas = 0;
  std::vector<uint32_t> px;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = kW;
    cfg.height = kH;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<VectorE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick();
    tick();
    for (const Value& m : take("ids")) {
      svg = uint32_t(m["svg"].asNumber());
      canvas = uint32_t(m["canvas"].asNumber());
    }
    return svg && canvas;
  }
  ~VectorE2E() {
    if (rt) jsui_destroy(rt);
  }
  jsui::Runtime& runtime() { return *reinterpret_cast<std::unique_ptr<jsui::Runtime>*>(rt)->get(); }
  void tick() {
    now += 16;
    jsui_tick(rt, now);
  }
  void tickNative() {
    now += 16;
    jsui_tick_native(rt, now);
  }
  void post(const char* name, const char* json = "null") { jsui_post_message(rt, name, json); }
  std::vector<Value> take(const std::string& name) {
    std::vector<Value> out;
    for (auto it = messages.begin(); it != messages.end();) {
      if (it->first == name) {
        out.push_back(Value::parseJson(it->second));
        it = messages.erase(it);
      } else {
        ++it;
      }
    }
    return out;
  }
  jsui::Node* node(uint32_t id) { return runtime().doc.get(id); }
  double renders() {
    post("renders");
    tick();
    auto r = take("renders");
    return r.empty() ? -1 : r[0].asNumber();
  }
  void render() {
    px.assign(size_t(kW) * kH, 0);
    jsui_render_rgba(rt, px.data(), kW, kH, size_t(kW) * 4);
  }
  uint32_t at(int x, int y) const { return px[size_t(y) * kW + x]; }
  bool red(int x, int y) const { return (at(x, y) & 0xffffff) == 0x0000ff; }
  bool black(int x, int y) const { return (at(x, y) & 0xffffff) == 0; }
  // The first key of the node's style starting with `prefix` and ending with `suffix`.
  const Value* drawKey(uint32_t id, const std::string& suffix) {
    jsui::Node* n = node(id);
    if (!n) return nullptr;
    for (auto& [k, v] : n->props()["style"].asObject())
      if (k.compare(0, 5, "draw:") == 0 && k.size() > suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
        return &v;
    return nullptr;
  }
};

}  // namespace

TEST(react_vector_e2e) {
  const char* path = std::getenv("JSUI_REACT_VECTOR_BUNDLE");
  if (!path) {
    std::printf("SKIP react_vector_e2e: JSUI_REACT_VECTOR_BUNDLE not set\n");
    return;
  }
  VectorE2E e;
  CHECK(e.load(path));
  jsui::Node* svg = e.node(e.svg);
  jsui::Node* canvas = e.node(e.canvas);
  CHECK(svg && svg->type() == jsui::NodeType::Vector);
  CHECK(canvas && canvas->type() == jsui::NodeType::Vector);
  if (!svg || !canvas) return;
  CHECK_EQ(svg->props()["commands"]["mode"].asString(), std::string("svg"));
  CHECK_EQ(canvas->props()["commands"]["mode"].asString(), std::string("skia"));
  // Circle + G + its Rect.
  CHECK_EQ(jsui::vector::elementCount(svg), size_t(3));

  e.render();
  // viewBox 0 0 100 50 in 200 x 100: x2. Circle at (50, 50), r 10 px.
  CHECK(e.red(50, 50));
  CHECK(!e.red(50 + 25, 50));
  // The trimmed path: the first 10% (0..20 px) of y = 150.
  CHECK(e.black(10, 150));
  CHECK(!e.black(100, 150));

  double renders = e.renders();
  CHECK(renders > 0);
  // Native timing on the radius (5 -> 20 user units = 10 -> 40 px).
  e.post("grow");
  e.tick();
  const Value* r = e.drawKey(e.svg, ".r");
  CHECK(r != nullptr);
  int grew = 0;
  double last = r ? r->asNumber() : 0;
  for (int i = 0; i < 12; i++) {
    e.tickNative();
    r = e.drawKey(e.svg, ".r");
    double v = r ? r->asNumber() : 0;
    if (v > last) grew++;
    last = v;
  }
  CHECK(grew >= 8);
  e.tick();
  CHECK_EQ(e.take("grown").size(), size_t(1));
  e.render();
  CHECK(e.red(50 + 35, 50));  // radius 40 px now
  // The Skia path's trim end, natively.
  e.post("trim");
  e.tick();
  for (int i = 0; i < 14; i++) e.tickNative();
  e.tick();
  CHECK_EQ(e.take("trimmed").size(), size_t(1));
  e.render();
  CHECK(e.black(190, 150));
  // Neither animation rendered React.
  CHECK_EQ(e.renders(), renders + 0);

  // JS-driver opacity on a group: patched, no render.
  e.render();
  CHECK_EQ(e.at(160, 70), 0xff00ff00u);
  e.post("fade");
  for (int i = 0; i < 8; i++) e.tick();
  CHECK_EQ(e.take("faded").size(), size_t(1));
  e.render();
  CHECK_EQ(e.at(160, 70), 0xffffffffu);
  CHECK_EQ(e.renders(), renders + 0);

  // State inside the Svg: re-serialized (and the animated radius stays).
  e.post("toggle");
  e.tick();
  e.tick();
  CHECK_EQ(jsui::vector::elementCount(svg), size_t(4));
  e.render();
  CHECK_EQ(e.at(160, 20), 0xffff0000u);  // the blue rect
  CHECK(e.red(50 + 35, 50));
}
