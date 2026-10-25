// End-to-end: a real @orktes/jsui-react bundle driven through the C API.
//
// The bundle is built from packages/cli/test/e2e/app.jsx by the @orktes/jsui-cli
// test suite, which then runs `jsui-tests react_e2e` with
// JSUI_REACT_E2E_BUNDLE=<path>. Without the variable the test is skipped,
// so the native suite does not depend on Node.
#include <algorithm>
#include <chrono>
#include <cstdint>
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
    cfg.js_thread = jsui_test::jsThreadMode();
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

  // Animated.timing (100 ms, useNativeDriver) on mount, run by the runtime;
  // native ends carry the final value like React Native's.
  t.tick(300);
  CHECK_EQ(t.take(), std::string("dims=[400,300,1] anim={\"finished\":true,\"value\":1}"));

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
  // Native editing: the caret moves, typing inserts at it, Ctrl+A + typing
  // replaces everything; the controlled value follows.
  t.key("ArrowLeft");
  jsui_send_text(t.rt, "X");
  t.tick(16);
  jsui_send_key(t.rt, "a", "KeyA", 1, 0, JSUI_MOD_CTRL);
  jsui_send_key(t.rt, "a", "KeyA", 0, 0, JSUI_MOD_CTRL);
  jsui_send_text(t.rt, "z");
  t.tick(16);
  t.key("Enter");
  CHECK_EQ(t.take(), std::string("text=\"h\xC3\xA9\" text=\"h\" text=\"hy\" text=\"hXy\" text=\"z\" submit=\"z\""));

  CHECK(jsui_last_error(t.rt) == nullptr);
}

// ---------------------------------------------------------------------------
// Virtualized FlatList end to end: packages/cli/test/e2e/list.jsx (10,000
// rows of 30 px, blue even / red odd) in a 400x300 viewport, built by the
// @orktes/jsui-cli suite and passed as JSUI_REACT_LIST_BUNDLE. Checks that only a
// window of rows is mounted, that every on-screen row is painted after
// gamepad focus navigation, wheel scrolling, scrollToIndex and scrollToEnd,
// and prints creation / scrolling timings.

namespace {

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define JSUI_SLOW_BUILD 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define JSUI_SLOW_BUILD 1
#endif
#ifdef JSUI_SLOW_BUILD
constexpr int kSlowFrames = 120;
#else
constexpr int kSlowFrames = 1;
#endif

int statInt(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = s.rfind(k);
  return p == std::string::npos ? -999999 : std::atoi(s.c_str() + p + k.size());
}

struct ListE2E : E2E {
  std::vector<uint8_t> px = std::vector<uint8_t>(400 * 300 * 4);
  std::string stats() {
    take();
    jsui_post_message(rt, "stats", "null");
    tick(16);
    return take();
  }
  void frame() { jsui_render_rgba(rt, px.data(), 400, 300, 400 * 4); }
  // Every row band on screen has its row's colour (no blank spacer showing).
  // React renders the rows in slices on a wall-clock frame budget, so a
  // sanitizer build (several times slower) gets more frames to fill them.
  bool rowsMatch(int offset) {
    for (int extra = 1; extra < kSlowFrames; extra++) {
      if (rowsMatchNow(offset, false)) return true;
      tick(16);
    }
    return rowsMatchNow(offset, true);
  }
  bool rowsMatchNow(int offset, bool report) {
    frame();
    for (int y = 0; y < 300; y += 7) {
      int within = (y + offset) % 30;
      if (within < 2 || within > 27) continue;  // skip anti-aliased edges
      int row = (y + offset) / 30;
      const uint8_t* p = &px[(size_t(y) * 400 + 10) * 4];
      bool odd = row % 2;
      bool ok = odd ? (p[0] > 200 && p[2] < 50) : (p[2] > 200 && p[0] < 50);
      if (!ok) {
        if (report) std::fprintf(stderr, "  row %d at y=%d (offset %d): rgb %d,%d,%d\n", row, y, offset, p[0], p[1], p[2]);
        return false;
      }
    }
    return true;
  }
};

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST(react_list_e2e) {
  const char* path = std::getenv("JSUI_REACT_LIST_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_LIST_BUNDLE, see packages/cli/test)\n");
    return;
  }
  ListE2E t;
  auto t0 = std::chrono::steady_clock::now();
  CHECK(t.load(path));
  double firstFrameMs = msSince(t0);
  if (const char* err = jsui_last_error(t.rt)) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(jsui_last_error(t.rt) == nullptr);
  // Let the render window grow batch by batch (50 ms each) until stable.
  auto t1 = std::chrono::steady_clock::now();
  t.tick(1500);
  double settleMs = msSince(t1);
  std::string s = t.stats();
  int mounted = statInt(s, "mounted"), nodes = statInt(s, "nodes");
  std::printf("  10k rows: load + first frames %.1f ms, window filled in %.1f ms (wall); %d rows mounted, %d host nodes\n",
              firstFrameMs, settleMs, mounted, nodes);
  CHECK(mounted >= 20 && mounted <= 200);
  CHECK(nodes > 0 && nodes < 1000);
  CHECK_EQ(statInt(s, "first"), 0);
  CHECK_EQ(statInt(s, "last"), 9);
  CHECK(t.rowsMatch(0));

  // Gamepad: row 0 has hasTVPreferredFocus; D-pad down walks the list and
  // focus changes glide the focused row into view, keeping 32 px of
  // context below it (src/input/scroll.cpp).
  for (int i = 0; i < 40; i++) t.pad(13);
  std::string msgs = t.take();
  CHECK(msgs.find("focus=40") != std::string::npos);
  CHECK(msgs.find("focus=41") == std::string::npos);
  t.tick(300);
  const int focusOffset = 41 * 30 + 32 - 300;
  CHECK(t.rowsMatch(focusOffset));
  s = t.stats();
  CHECK_EQ(statInt(s, "first"), (focusOffset + 29) / 30);  // fully visible rows
  CHECK_EQ(statInt(s, "last"), (focusOffset + 300) / 30 - 1);
  t.pad(0);
  CHECK(t.take().find("press=40") != std::string::npos);

  // Mouse wheel far past the mounted window (a notch glides for 150 ms).
  jsui_send_mouse_move(t.rt, 200, 150);
  jsui_send_mouse_wheel(t.rt, 200, 150, 0, 6000);
  t.tick(16);
  t.tick(160);
  CHECK(t.rowsMatch(focusOffset + 6000));
  t.tick(500);
  s = t.stats();
  CHECK(statInt(s, "mounted") <= 300);
  CHECK_EQ(statInt(s, "first"), (focusOffset + 6000 + 29) / 30);
  // Focus is still on row 40 (kept mounted although off screen): D-pad down
  // goes to row 41 and scrolls back to it (above the viewport: top edge
  // with 32 px of context).
  t.pad(13);
  CHECK(t.take().find("focus=41") != std::string::npos);
  t.tick(300);
  CHECK(t.rowsMatch(41 * 30 - 32));

  // scrollToIndex / scrollToEnd through the ref (host messages).
  jsui_post_message(t.rt, "scrollToIndex", "{\"index\":5000}");
  t.tick(48);
  CHECK(t.rowsMatch(5000 * 30));
  t.tick(100);
  s = t.stats();
  CHECK_EQ(statInt(s, "first"), 5000);
  jsui_post_message(t.rt, "scrollToEnd", "null");
  t.tick(200);
  CHECK(t.take().find("end=10000") != std::string::npos);
  CHECK(t.rowsMatch(10000 * 30 - 300));
  s = t.stats();
  CHECK_EQ(statInt(s, "last"), 9999);
  CHECK(statInt(s, "nodes") < 1500);

  // Scrolling cost: 300 frames of 40 px wheel steps from the top, each a
  // tick + full repaint, as a game host would do per frame.
  jsui_post_message(t.rt, "scrollToIndex", "{\"index\":0}");
  t.tick(500);
  std::vector<double> frames;
  double inputMs = 0, tickMs = 0, paintMs = 0;
  for (int i = 0; i < 300; i++) {
    auto f0 = std::chrono::steady_clock::now();
    jsui_send_mouse_wheel(t.rt, 200, 150, 0, 40);
    double a = msSince(f0);
    t.now += 16;
    jsui_tick(t.rt, t.now);
    double b = msSince(f0);
    t.frame();
    frames.push_back(msSince(f0));
    inputMs += a, tickMs += b - a, paintMs += frames.back() - b;
  }
  std::printf("  scroll breakdown per frame: wheel+onScroll %.2f ms, tick (timers, React, layout, onLayout) %.2f ms, paint %.2f ms\n",
              inputMs / 300, tickMs / 300, paintMs / 300);
  t.tick(200);  // the last wheel glide
  CHECK(t.rowsMatch(300 * 40));
  double sum = 0;
  for (double f : frames) sum += f;
  std::vector<double> sorted = frames;
  std::sort(sorted.begin(), sorted.end());
  std::printf("  scroll 300 frames x 40 px: avg %.2f ms, p95 %.2f ms, max %.2f ms per frame (wheel + tick + paint 400x300)\n",
              sum / frames.size(), sorted[size_t(frames.size() * 95 / 100)], sorted.back());
  CHECK(jsui_last_error(t.rt) == nullptr);
}
