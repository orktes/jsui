// Views sliding into a new layout place, the two ways (packages/cli/test/e2e/
// layout_slide.jsx, the jsui-wm switcher's strip in small: cards with a
// native translateX in a horizontal ScrollView): LayoutAnimation's update,
// and a native Animated value set in the commit that moved them
// (useLayoutEffect). Either way the next card slides from its place into the
// dropped one's frame by frame, with no frame at the old place plus the
// offset or off screen: the value lands with its commit's layout. In
// single-threaded, lockstep and JS-thread modes (orktes/jsui#316). Built and run by the
// @orktes/jsui-cli test suite with JSUI_REACT_LAYOUT_SLIDE_BUNDLE=<path>;
// skipped without it.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct SlideE2E {
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::vector<uint8_t> px = std::vector<uint8_t>(400 * 120 * 4);
  std::string messages;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = 400;
    cfg.height = 120;
    cfg.on_message = [](void* u, const char* name, const char*) { static_cast<SlideE2E*>(u)->messages += name; };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    return true;
  }
  ~SlideE2E() {
    if (rt) jsui_destroy(rt);
  }
  // One 16 ms frame. A free-running JS thread gets a moment to commit.
  void frame() {
    now += 16;
    jsui_tick(rt, now);
    if (jsui_test::jsThreadMode() == 1) std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  // The left edge of the green card along y = 60 (-1: not on screen).
  int greenX() {
    jsui_render_rgba(rt, px.data(), 400, 120, 400 * 4);
    for (int x = 0; x < 400; x++) {
      const uint8_t* p = &px[(size_t(60) * 400 + x) * 4];
      if (p[1] > 200 && p[0] < 60 && p[2] < 60) return x;
    }
    return -1;
  }
};

}  // namespace

namespace {

// Drops the first card with `message` and checks the green card slides
// 270 -> 150 over 320 ms: on screen and in between on every frame of it.
void slideCheck(const char* path, const char* message) {
  SlideE2E t;
  CHECK(t.load(path));
  for (int i = 0; i < 30 && t.messages.find("ready") == std::string::npos; i++) t.frame();
  CHECK(jsui_last_error(t.rt) == nullptr);
  CHECK_EQ(t.greenX(), 270);  // red at 150..250, green 20 px after it
  jsui_post_message(t.rt, message, "{\"duration\":320}");
  std::vector<int> xs;
  for (int i = 0; i < 50; i++) {
    t.frame();
    xs.push_back(t.greenX());
  }
  std::string trace;
  for (int x : xs) trace += std::to_string(x) + " ";
  std::printf("  %s, green x per frame: %s\n", message, trace.c_str());
  int between = 0, prev = 270;
  bool monotonic = true, visible = true;
  for (int x : xs) {
    if (x < 0) visible = false;
    if (x > prev) monotonic = false;
    if (x > 152 && x < 268) between++;
    if (x >= 0) prev = x;
  }
  CHECK(visible);
  CHECK(monotonic);
  CHECK(between >= 14);  // ~20 frames of the slide land strictly in between
  CHECK_EQ(xs.back(), 150);
}

}  // namespace

TEST(react_layout_slide_e2e) {
  const char* path = std::getenv("JSUI_REACT_LAYOUT_SLIDE_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_LAYOUT_SLIDE_BUNDLE, see packages/cli/test)\n");
    return;
  }
  slideCheck(path, "remove");        // LayoutAnimation update
  slideCheck(path, "removeNative");  // a native value set in the moving commit
}
