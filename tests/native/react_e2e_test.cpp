// End-to-end: a real @jsui/react bundle driven through the C API.
//
// The bundle is built from packages/cli/test/e2e/app.jsx by the @jsui/cli
// test suite, which then runs `jsui-tests react_e2e` with
// JSUI_REACT_E2E_BUNDLE=<path>. Without the variable the test is skipped,
// so the native suite does not depend on Node.
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

struct E2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<E2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~E2E() {
    if (rt) jsui_destroy(rt);
  }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick(rt, now);
    }
  }
  // "name=json" of messages since the last call, space separated.
  std::string take() {
    std::string s;
    for (auto& [n, j] : messages) s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
  void click(float x, float y) {
    jsui_send_mouse_move(rt, x, y);
    jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 1, 0);
    jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 0, 0);
    tick(16);
  }
  void pad(int button) {
    jsui_send_gamepad_button(rt, 0, button, 1, 1);
    jsui_send_gamepad_button(rt, 0, button, 0, 0);
    tick(16);
  }
  void key(const char* k) {
    jsui_send_key(rt, k, k, 1, 0, 0);
    jsui_send_key(rt, k, k, 0, 0, 0);
    tick(16);
  }
};

}  // namespace

TEST(react_e2e) {
  const char* path = std::getenv("JSUI_REACT_E2E_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_E2E_BUNDLE, see packages/cli/test)\n");
    return;
  }
  E2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);

  // Animated.timing (100 ms) on mount, driven by requestAnimationFrame.
  t.tick(300);
  CHECK_EQ(t.take(), std::string("dims=[400,300,1] anim={\"finished\":true}"));

  // Viewport changes reach Dimensions / useWindowDimensions.
  jsui_set_viewport(t.rt, 500, 350, 2);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("dims=[500,350,2]"));
  jsui_set_viewport(t.rt, 400, 300, 1);
  t.tick(16);
  t.take();

  // Host message round trip; renders synchronously.
  jsui_post_message(t.rt, "ping", "{\"n\":1}");
  t.tick(16);
  CHECK_EQ(t.take(), std::string("pong={\"n\":1}"));

  // Mouse click on A (0..100 x 0..50): exactly one press.
  t.click(20, 20);
  CHECK_EQ(t.take(), std::string("press=\"A:mouse\""));

  // Gamepad: A is focused (hasTVPreferredFocus); button 0 presses it.
  t.pad(0);
  CHECK_EQ(t.take(), std::string("press=\"A:gamepad\""));
  // D-pad down moves focus to B; Enter presses it exactly once (the
  // Pressable handles Enter and prevents the native click).
  t.pad(13);
  t.key("Enter");
  CHECK_EQ(t.take(), std::string("press=\"B\""));

  // Long press on B (50..100).
  jsui_send_mouse_move(t.rt, 20, 70);
  jsui_send_mouse_button(t.rt, 20, 70, JSUI_MOUSE_LEFT, 1, 0);
  t.tick(600);
  jsui_send_mouse_button(t.rt, 20, 70, JSUI_MOUSE_LEFT, 0, 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("long=\"B\""));

  // TextInput (100..130): click focuses, text input edits, Enter submits.
  t.click(20, 115);
  jsui_send_text(t.rt, "h\xC3\xA9");
  t.tick(16);
  jsui_send_key(t.rt, "Backspace", "Backspace", 1, 0, 0);
  t.tick(16);
  jsui_send_text(t.rt, "y");
  t.tick(16);
  t.key("Enter");
  CHECK_EQ(t.take(), std::string("text=\"h\xC3\xA9\" text=\"h\" text=\"hy\" submit=\"hy\""));

  CHECK(jsui_last_error(t.rt) == nullptr);
}
