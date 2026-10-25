// End-to-end interactions: a real @orktes/jsui-react bundle (packages/cli/test/e2e/
// interactions.jsx) driven through the C API. Hover in / out from native
// pointer enter / leave (with delayHoverIn / delayHoverOut and the hovered
// style state), RN press timing (minPressDuration), an endless Animated loop
// that keeps jsui_wants_frame() set without input, and LayoutAnimation
// update / create / delete checked from rendered pixels. Built and run by the
// @orktes/jsui-cli test suite with JSUI_REACT_INTERACTIONS_BUNDLE=<path>; skipped
// without it.
#include <algorithm>
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

struct InteractionsE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::vector<uint8_t> px = std::vector<uint8_t>(400 * 300 * 4);

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<InteractionsE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~InteractionsE2E() {
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
  void post(const char* name) {
    jsui_post_message(rt, name, "null");
  }
  // RGB at (x, y) of a fresh frame, as 0xRRGGBB.
  uint32_t rgb(int x, int y) {
    jsui_render_rgba(rt, px.data(), 400, 300, 400 * 4);
    const uint8_t* p = &px[(size_t(y) * 400 + x) * 4];
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  }
};

bool near(uint32_t a, uint32_t b, int tol = 12) {
  for (int s = 0; s < 24; s += 8) {
    int d = int((a >> s) & 255) - int((b >> s) & 255);
    if (d < -tol || d > tol) return false;
  }
  return true;
}

}  // namespace

TEST(react_interactions_e2e) {
  const char* path = std::getenv("JSUI_REACT_INTERACTIONS_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_INTERACTIONS_BUNDLE, see packages/cli/test)\n");
    return;
  }
  InteractionsE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  t.tick(32);
  CHECK_EQ(t.take(), std::string("ready=true"));
  CHECK(!jsui_wants_frame(t.rt));  // idle: nothing animates

  // Hover: enter / leave the red Pressable; its style follows `hovered`.
  CHECK(near(t.rgb(50, 25), 0xff0000));
  jsui_send_mouse_move(t.rt, 50, 25);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("hover=\"in\""));
  CHECK(near(t.rgb(50, 25), 0x00ff00));
  // Press: quick click -> pressIn, press, then pressOut after 130 ms; the
  // pressed colour stays visible meanwhile.
  jsui_send_mouse_button(t.rt, 50, 25, JSUI_MOUSE_LEFT, 1, 0);
  jsui_send_mouse_button(t.rt, 50, 25, JSUI_MOUSE_LEFT, 0, 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("pressIn=\"H\" press=\"H\""));
  CHECK(near(t.rgb(50, 25), 0x0000ff));
  t.tick(160);
  CHECK_EQ(t.take(), std::string("pressOut=\"H\""));
  CHECK(near(t.rgb(50, 25), 0x00ff00));
  // Out of it and into the delayed one (delayHoverIn 100 ms).
  jsui_send_mouse_move(t.rt, 50, 80);
  t.tick(48);
  CHECK_EQ(t.take(), std::string("hover=\"out\""));
  CHECK(near(t.rgb(50, 25), 0xff0000));
  t.tick(80);
  CHECK_EQ(t.take(), std::string("dhover=\"in\""));
  jsui_send_mouse_leave(t.rt);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(""));  // delayHoverOut 50 ms
  t.tick(64);
  CHECK_EQ(t.take(), std::string("dhover=\"out\""));

  // PanResponder release velocity (px/ms, RN): one move per frame, then the
  // button comes up a frame later without moving. The velocity of the moves
  // survives the release.
  jsui_send_mouse_move(t.rt, 20, 200);
  jsui_send_mouse_button(t.rt, 20, 200, JSUI_MOUSE_LEFT, 1, 0);
  t.tick(16);
  for (int i = 1; i <= 10; i++) {
    jsui_send_mouse_move(t.rt, 20 + 3.f * i, 200 + float(i));
    t.tick(16);
  }
  jsui_send_mouse_button(t.rt, 50, 210, JSUI_MOUSE_LEFT, 0, 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("release={\"dx\":30,\"dy\":10,\"vx\":0.1875,\"vy\":0.0625}"));
  // A flick whose moves and release all arrive within one frame: the host's
  // event times (jsui_set_event_time, e.g. DOM event.timeStamp) give the
  // velocity. 2 px every 4 ms = 0.5 px/ms.
  double t0 = t.now + 1;
  jsui_set_event_time(t.rt, t0);
  jsui_send_mouse_move(t.rt, 20, 200);
  jsui_send_mouse_button(t.rt, 20, 200, JSUI_MOUSE_LEFT, 1, 0);
  for (int i = 1; i <= 4; i++) {
    jsui_set_event_time(t.rt, t0 + 4 * i);
    jsui_send_mouse_move(t.rt, 20 + 2.f * i, 200);
  }
  jsui_set_event_time(t.rt, t0 + 17);
  jsui_send_mouse_button(t.rt, 28, 200, JSUI_MOUSE_LEFT, 0, 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("release={\"dx\":8,\"dy\":0,\"vx\":0.5,\"vy\":0}"));
  jsui_send_mouse_leave(t.rt);
  t.tick(16);
  t.take();

  // An endless Animated.loop keeps the host ticking without any input.
  t.post("loop");
  t.tick(16);
  int frames = 0;
  uint32_t lo = 0xffffff, hi = 0;
  for (int i = 0; i < 120; i++) {
    t.tick(16);
    frames += jsui_wants_frame(t.rt) ? 1 : 0;
    uint32_t c = t.rgb(10, 130) & 0xff;
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  CHECK_EQ(frames, 120);
  CHECK(lo < 40 && hi > 215);  // opacity swept 0..1
  t.post("stop");
  t.tick(32);
  CHECK(!jsui_wants_frame(t.rt));

  // LayoutAnimation update: width 50 -> 150 over 200 ms (linear). measure()
  // reports the final frame at once; the painted box grows.
  CHECK(near(t.rgb(220, 25), 0xffff00));
  CHECK(near(t.rgb(300, 25), 0x000000));
  t.post("grow");
  t.tick(16);
  t.post("measure");
  t.tick(16);
  CHECK_EQ(t.take(), std::string("width=150"));
  CHECK(jsui_wants_frame(t.rt));
  t.tick(64);  // ~96 ms in: ~98 px wide
  CHECK(near(t.rgb(285, 25), 0xffff00));
  CHECK(near(t.rgb(320, 25), 0x000000));
  t.tick(160);
  CHECK(near(t.rgb(340, 25), 0xffff00));
  t.tick(64);
  CHECK_EQ(t.take(), std::string("growDone=true"));

  // create: fades in (opacity, linear 200 ms).
  t.post("add");
  t.tick(16);
  uint32_t early = t.rgb(225, 125);
  t.tick(80);
  uint32_t mid = t.rgb(225, 125);
  t.tick(160);
  uint32_t end = t.rgb(225, 125);
  CHECK((early & 0xff) < 60);
  CHECK((mid & 0xff) > 80 && (mid & 0xff) < 200);
  CHECK(near(end, 0x00ffff));

  // delete: the removed box scales out about its center, then is freed.
  CHECK(near(t.rgb(305, 205), 0xff00ff));
  t.post("remove");
  t.tick(16);
  t.tick(96);
  CHECK(near(t.rgb(305, 205), 0x000000));  // outer ring already gone
  CHECK(near(t.rgb(330, 230), 0xff00ff));  // center still there
  t.tick(160);
  CHECK(near(t.rgb(330, 230), 0x000000));
  CHECK(!jsui_wants_frame(t.rt));
  CHECK(jsui_last_error(t.rt) == nullptr);
}
