// End-to-end frame data: a real @orktes/jsui-react bundle
// (packages/cli/test/e2e/host_buffers.jsx) driven through the C API. Markers
// follow a jsui_set_buffer buffer every frame through Animated.hostBuffer
// nodes with no React render (only a change in the number of enemies
// re-renders the list), also under jsui_tick_native and in a second surface
// of the same runtime; useHostBuffer re-renders with the new contents.
// Built and run by the @orktes/jsui-cli test suite with
// JSUI_REACT_HOST_BUFFERS_BUNDLE=<path>; skipped without it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/value.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct BuffersE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;

  BuffersE2E() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<BuffersE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
  }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) == 0;
  }
  ~BuffersE2E() { jsui_destroy(rt); }
  void tick() { jsui_tick(rt, now += 16); }
  std::vector<std::string> take(const std::string& name) {
    std::vector<std::string> out;
    for (auto& [n, j] : messages)
      if (n == name) out.push_back(j);
    messages.clear();
    return out;
  }
  jsui::Node* find(const std::string& nativeID, JsuiRuntime* surface = nullptr) {
    jsui::Runtime& r = *jsui::internalRuntime(rt);
    std::function<jsui::Node*(jsui::Node*)> walk = [&](jsui::Node* n) -> jsui::Node* {
      if (n->props()["nativeID"].asString() == nativeID) return n;
      for (jsui::Node* k : n->children())
        if (jsui::Node* f = walk(k)) return f;
      return nullptr;
    };
    for (auto& s : r.surfaces())
      if (!surface || s->handle == surface)
        if (jsui::Node* f = walk(s->doc.root())) return f;
    return nullptr;
  }
  static double transform(jsui::Node* n, size_t i, const char* key) {
    if (!n) return -1e9;
    const auto& a = n->props()["style"]["transform"].asArray();
    return i < a.size() ? a[i][key].asNumber(-1e9) : -1e9;
  }
};

}  // namespace

TEST(react_host_buffers_e2e) {
  const char* path = std::getenv("JSUI_REACT_HOST_BUFFERS_BUNDLE");
  if (!path) {
    std::printf("  (skipped: JSUI_REACT_HOST_BUFFERS_BUNDLE not set)\n");
    return;
  }
  BuffersE2E e;
  // Two enemies before the UI exists.
  std::vector<float> enemies = {10, 20, 100, 50, 60, 50};
  jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
  CHECK(e.load(path));
  e.tick();
  e.tick();
  auto renders = e.take("render");
  CHECK(!renders.empty() && renders.back() == "2");
  jsui::Node* m0 = e.find("m0");
  jsui::Node* m1 = e.find("m1");
  CHECK(m0 && m1);
  CHECK_EQ(BuffersE2E::transform(m0, 0, "translateX"), 10.0);
  CHECK_EQ(BuffersE2E::transform(m1, 1, "translateY"), 60.0);
  CHECK_EQ(BuffersE2E::transform(m1, 2, "scaleX"), 0.5);

  // Every frame new positions: the views follow, React does not render.
  bool follows = true;
  for (int f = 1; f <= 60; f++) {
    enemies[0] = 10.f + float(f);
    enemies[4] = 60.f - float(f) * 0.5f;
    enemies[5] = float(f % 101);
    jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
    e.tick();
    if (BuffersE2E::transform(m0, 0, "translateX") != 10.0 + f) follows = false;
    if (BuffersE2E::transform(m1, 1, "translateY") != double(60.f - float(f) * 0.5f)) follows = false;
    if (std::abs(BuffersE2E::transform(m1, 2, "scaleX") - double(f % 101) / 100) > 1e-9) follows = false;
  }
  CHECK(follows);
  CHECK(e.take("render").empty());

  // Without JS (jsui_tick_native) too.
  enemies[0] = 300;
  jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
  jsui_tick_native(e.rt, e.now += 16);
  CHECK_EQ(BuffersE2E::transform(m0, 0, "translateX"), 300.0);

  // A third enemy: one render of the list, and the new marker follows.
  enemies.insert(enemies.end(), {200, 100, 25});
  jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
  e.tick();
  renders = e.take("render");
  CHECK_EQ(renders.size(), size_t(1));
  if (!renders.empty()) CHECK_EQ(renders[0], std::string("3"));
  jsui::Node* m2 = e.find("m2");
  CHECK_EQ(BuffersE2E::transform(m2, 0, "translateX"), 200.0);
  CHECK_EQ(BuffersE2E::transform(m2, 2, "scaleX"), 0.25);

  // useHostBuffer: a render per change with the new contents.
  int32_t score[2] = {7, 3};
  jsui_set_buffer(e.rt, "score", JSUI_BUFFER_I32, score, 2);
  e.tick();
  auto scores = e.take("score");
  CHECK(!scores.empty() && scores.back() == "[7,3]");

  // A surface of the same runtime follows the same buffer.
  JsuiRuntime* plate = jsui_surface_create(e.rt, "Plate", "{\"i\":2}", 120, 40, 1);
  e.tick();
  e.tick();
  jsui::Node* p = e.find("plate", plate);
  CHECK(p != nullptr);
  CHECK_EQ(BuffersE2E::transform(p, 0, "translateX"), 200.0);
  enemies[6] = 222;
  jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
  e.tick();
  CHECK_EQ(BuffersE2E::transform(p, 0, "translateX"), 222.0);
  CHECK_EQ(BuffersE2E::transform(m2, 0, "translateX"), 222.0);

  // Fewer enemies: markers unmount.
  enemies.resize(3);
  jsui_set_buffer(e.rt, "enemies", JSUI_BUFFER_F32, enemies.data(), enemies.size());
  e.tick();
  CHECK(e.find("m1") == nullptr);
  CHECK(e.find("m0") != nullptr);
}
