// End-to-end ScrollView: a real @orktes/jsui-react bundle (packages/cli/test/e2e/
// scroll.jsx) driven through the C API. Built and run by the @orktes/jsui-cli test
// suite with JSUI_REACT_E2E_SCROLL_BUNDLE=<path>; skipped without it.
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

struct ScrollE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = 200;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<ScrollE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~ScrollE2E() {
    if (rt) jsui_destroy(rt);
  }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick(rt, now);
    }
  }
  // Ticks until the runtime stops asking for frames.
  void settle() {
    for (int i = 0; i < 400 && jsui_wants_frame(rt); i++) tick(16);
    tick(200);  // trailing throttled onScroll
  }
  // "name=json" of messages since the last call; scroll messages only when
  // `scroll` is set.
  std::string take(bool scroll = false) {
    std::string s;
    for (auto& [n, j] : messages)
      if (scroll || n != "scroll") s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
  std::string lastScroll() {
    std::string last;
    for (auto& [n, j] : messages)
      if (n == "scroll") last = j;
    return last;
  }
};

}  // namespace

TEST(react_e2e_scroll) {
  const char* path = std::getenv("JSUI_REACT_E2E_SCROLL_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_E2E_SCROLL_BUNDLE, see packages/cli/test)\n");
    return;
  }
  ScrollE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);

  // contentOffset applied on mount (one scroll event).
  CHECK_EQ(t.take(true), std::string("scroll=50"));

  // Touch fling: 20 px per frame upwards = 1.25 px/ms.
  jsui_send_touch(t.rt, 0, 0, 100, 90);
  for (float y : {70.f, 50.f, 30.f}) {
    t.tick(16);
    jsui_send_touch(t.rt, 0, 1, 100, y);
  }
  jsui_send_touch(t.rt, 0, 2, 100, 30);
  CHECK(jsui_wants_frame(t.rt));
  t.settle();
  CHECK_EQ(t.lastScroll(), std::string("724"));  // throttled onScroll still ends on the final offset
  CHECK_EQ(t.take(), std::string("begin=50 end=[110,1.25] mbegin=110 mend=724"));
  CHECK(!jsui_wants_frame(t.rt));

  // ref.scrollTo({y, animated: true}): glides there, no momentum events.
  jsui_post_message(t.rt, "scrollTo", "100");
  t.tick(16);
  CHECK(t.lastScroll() != "100");
  t.settle();
  CHECK_EQ(t.lastScroll(), std::string("100"));
  CHECK_EQ(t.take(), std::string());

  // Mouse wheel notch over the list, then the gamepad right stick.
  jsui_send_mouse_move(t.rt, 100, 50);
  CHECK(jsui_send_mouse_wheel(t.rt, 100, 50, 0, 100));
  t.settle();
  CHECK_EQ(t.lastScroll(), std::string("200"));
  t.take();
  jsui_send_gamepad_axis(t.rt, 0, 3, 1.f);
  t.tick(160);
  jsui_send_gamepad_axis(t.rt, 0, 3, 0.f);
  t.settle();
  std::string after = t.lastScroll();
  CHECK(!after.empty() && std::atoi(after.c_str()) > 400);
  t.take();

  // scrollToEnd: content is 20 + 20 * 50 = 1020 px in a 100 px viewport.
  jsui_post_message(t.rt, "scrollToEnd", "null");
  t.settle();
  CHECK_EQ(t.lastScroll(), std::string("920"));
  t.take();

  // Keyboard: Home glides back to the top.
  jsui_send_key(t.rt, "Home", "Home", 1, 0, 0);
  jsui_send_key(t.rt, "Home", "Home", 0, 0, 0);
  t.settle();
  CHECK_EQ(t.lastScroll(), std::string("0"));
  t.take();

  // A vertical drag on a PanResponder inside a ScrollView stays with the
  // PanResponder (onShouldBlockNativeResponder defaults to true, like RN).
  jsui_send_touch(t.rt, 1, 0, 100, 150);
  for (float y : {140.f, 125.f, 110.f}) {
    t.tick(16);
    jsui_send_touch(t.rt, 1, 1, 100, y);
  }
  jsui_send_touch(t.rt, 1, 2, 100, 110);
  t.settle();
  CHECK_EQ(t.take(), std::string("pan=-40"));

  // A responder that does not block native gestures is asked, agrees, and is
  // terminated when the ScrollView starts dragging.
  jsui_send_touch(t.rt, 2, 0, 100, 250);
  for (float y : {240.f, 225.f, 210.f}) {
    t.tick(16);
    jsui_send_touch(t.rt, 2, 1, 100, y);
  }
  jsui_send_touch(t.rt, 2, 2, 100, 210);
  t.settle();
  CHECK_EQ(t.take(), std::string("politeterm=0 outerbegin=0"));

  CHECK(jsui_last_error(t.rt) == nullptr);
}
