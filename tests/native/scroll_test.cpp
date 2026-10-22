// ScrollView physics and input: fling deceleration, catching a fling,
// paging / snap points, smooth programmatic / wheel / keyboard / focus
// scrolling, gamepad sticks, scroll events and their order, overlay
// indicators and damage-tracked repaint.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "input/input.h"
#include "js/engine.h"
#include "render_util.h"
#include "test.h"

using namespace jsui;
using rtest::Scene;

namespace {

// A Scene with a JS event logger: every dispatched event type is recorded
// (scrollenddrag with its velocity and target offset). The log is read
// back through a host message.
struct ScrollFixture {
  Scene s;
  double now = 0;
  Value lastLog;
  explicit ScrollFixture(int w = 200, int h = 200) : s(w, h) {
    s.rt->config.on_message = [](void* u, const char* name, const char* json) {
      std::string err;
      if (std::string(name) == "log") static_cast<ScrollFixture*>(u)->lastLog = Value::parseJson(json, &err);
    };
    s.rt->config.message_user = this;
    js("globalThis.log = [];"
       "globalThis.logger = e => {"
       "  let t = e.type;"
       "  if (e.type === 'scrollenddrag') t += '#v=' + e.velocity.y.toFixed(2) + ',t=' + e.targetContentOffset.y.toFixed(0);"
       "  log.push(t); return 0; };"
       "globalThis.__jsui_dispatchEvent = logger;");
  }
  void js(const std::string& src) { CHECK(s.rt->js->eval(src, "scroll_test.js")); }
  InputManager& in() { return *s.rt->input; }
  void tick(double ms = 16) {
    for (double end = now + ms; now < end - 1e-9;) {
      now += std::min(16.0, end - now);
      s.rt->tick(now);
    }
  }
  // Runs until nothing wants frames (bounded).
  void settle(double maxMs = 5000) {
    for (double t = 0; t < maxMs && s.rt->wantsFrame(); t += 16) tick(16);
  }
  // Logged events since the last call: drag / momentum ones only, or all.
  std::string events(bool all = false) {
    js("__jsui_native.postMessage('log', JSON.stringify(log)); log = [];");
    std::string out;
    for (auto& e : lastLog.asArray()) {
      const std::string& t = e.asString();
      bool dragOrMomentum =
          t.rfind("scrollbegindrag", 0) == 0 || t.rfind("scrollenddrag", 0) == 0 || t.rfind("momentum", 0) == 0;
      if (all || dragOrMomentum) out += (out.empty() ? "" : " ") + t;
    }
    return out;
  }
};

// A vertical ScrollView 100x100 at (0, 0) with 1000 px of content, plus
// optional ScrollView props.
Node* addList(ScrollFixture& f, const std::string& props = "{}", const std::string& content = "") {
  std::string kids = content.empty() ? R"({"style":{"height":1000,"backgroundColor":"#336699"}})" : content;
  return f.s.add(R"J({"type":"ScrollView","props":)J" + props +
                 R"J(,"style":{"position":"absolute","left":0,"top":0,"width":100,"height":100},"children":[)J" + kids +
                 "]}");
}

// Drags a touch from (x, y0) to (x, y1) in `steps` moves, one per 16 ms frame.
void drag(ScrollFixture& f, float x, float y0, float y1, int steps, bool release = true) {
  f.in().touch(0, 0, x, y0);
  for (int i = 1; i <= steps; i++) {
    f.tick(16);
    f.in().touch(0, 1, x, y0 + (y1 - y0) * float(i) / float(steps));
  }
  if (release) f.in().touch(0, 2, x, y1);
}

}  // namespace

TEST(scroll_fling_decelerates_and_stops) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  f.events();
  // 20 px per 16 ms frame upwards: content offset velocity 1.25 px/ms.
  drag(f, 50, 90, 30, 3);
  float released = sv->scrollY;
  CHECK_EQ(released, 60.f);  // the content follows the finger from the touch-down point
  // RN order: begin drag, end drag (velocity + projected target), momentum.
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=684 momentumscrollbegin"));
  CHECK(f.s.rt->wantsFrame());
  // Momentum: monotonic, decelerating, ends near the projection.
  std::vector<float> pos{released};
  for (int i = 0; i < 400 && f.in().animating(); i++) {
    f.tick(16);
    pos.push_back(sv->scrollY);
  }
  CHECK(pos.size() > 20);
  for (size_t i = 1; i < pos.size(); i++) CHECK(pos[i] >= pos[i - 1]);
  CHECK(pos[1] - pos[0] > 15);  // the first frame keeps the release speed
  size_t mid = pos.size() / 2;
  CHECK(pos[mid + 1] - pos[mid] < pos[1] - pos[0]);  // slowing down
  CHECK(sv->scrollY > 665 && sv->scrollY < 685);
  CHECK_EQ(f.events(), std::string("momentumscrollend"));
  CHECK(!f.s.rt->wantsFrame());
  CHECK_EQ(sv->scrollIndicatorOpacity, 0.f);  // faded out
}

TEST(scroll_fling_fast_deceleration) {
  ScrollFixture f;
  Node* sv = addList(f, R"({"decelerationRate":"fast"})");
  f.tick(16);
  f.events();
  drag(f, 50, 90, 30, 3);
  f.settle();
  // 'fast' (0.99 / ms) travels ~v / 0.01 = 125 px past the release.
  CHECK(sv->scrollY > 175 && sv->scrollY < 186);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=184 momentumscrollbegin momentumscrollend"));
}

TEST(scroll_fling_stops_at_edge_and_resting_finger_has_no_momentum) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  // Fast fling towards the end clamps there (no overscroll).
  drag(f, 50, 95, 5, 2);
  f.settle();
  CHECK_EQ(sv->scrollY, 900.f);
  f.events();
  // Drag, rest for 200 ms, release: no fling.
  f.in().touch(0, 0, 50, 10);
  f.tick(16);
  f.in().touch(0, 1, 50, 40);
  f.tick(200);
  f.in().touch(0, 2, 50, 40);
  CHECK_EQ(sv->scrollY, 870.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 870.f);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=0.00,t=870"));
}

TEST(scroll_touch_catches_fling) {
  ScrollFixture f;
  Node* sv = addList(f, "{}", R"({"style":{"height":1000},"props":{"__events":["press"]}})");
  f.tick(16);
  drag(f, 50, 90, 30, 3);
  f.tick(100);
  f.events();
  CHECK(f.in().animating());
  // Touch down stops the fling; the pressable under the finger is not pressed.
  CHECK(f.in().touch(1, 0, 50, 50));
  CHECK_EQ(f.events(), std::string("momentumscrollend"));
  float caught = sv->scrollY;
  f.tick(100);
  CHECK_EQ(sv->scrollY, caught);
  f.in().touch(1, 2, 50, 50);
  f.settle();
  CHECK_EQ(sv->scrollY, caught);
  std::string ev = f.events(true);
  CHECK(ev.find("pointerdown") == std::string::npos);
  CHECK(ev.find("click") == std::string::npos);
  // A caught list drags without the slop.
  f.in().touch(1, 0, 50, 50);
  f.tick(16);
  f.in().touch(1, 1, 50, 45);  // 5 px: below the slop, but it is not animating -> nothing
  CHECK_EQ(sv->scrollY, caught);
  f.in().touch(1, 2, 50, 45);
}

TEST(scroll_paging_snaps_by_velocity) {
  ScrollFixture f;
  Node* sv = addList(f, R"({"pagingEnabled":true})");
  f.tick(16);
  f.events();
  // Short quick flick (40 px): next page.
  drag(f, 50, 90, 50, 2);
  CHECK_EQ(sv->scrollY, 40.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 100.f);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=100 momentumscrollbegin momentumscrollend"));
  // Drag back by 38 px with the finger resting before release: nearest page.
  f.in().touch(0, 0, 50, 50);
  f.tick(16);
  f.in().touch(0, 1, 50, 70);
  f.tick(16);
  f.in().touch(0, 1, 50, 88);
  f.tick(300);
  f.in().touch(0, 2, 50, 88);
  f.settle();
  CHECK_EQ(sv->scrollY, 100.f);
  // A fling never moves more than one page.
  drag(f, 50, 95, 5, 2);
  f.settle();
  CHECK_EQ(sv->scrollY, 200.f);
}

TEST(scroll_snap_to_interval) {
  ScrollFixture f;
  Node* a = addList(f, R"({"snapToInterval":80})");
  f.tick(16);
  // Mouse wheel notches go from snap point to snap point.
  CHECK(f.in().mouseWheel(50, 50, 0, 40));
  f.settle();
  CHECK_EQ(a->scrollY, 80.f);
  f.in().mouseWheel(50, 50, 0, 40);
  f.in().mouseWheel(50, 50, 0, 40);  // accumulates during the glide
  f.settle();
  CHECK_EQ(a->scrollY, 240.f);
  // A fling lands on a multiple of the interval (or the end of the content).
  drag(f, 50, 90, 70, 2);  // from 240 to 260, 0.625 px/ms: projects to ~572
  f.settle();
  CHECK_EQ(a->scrollY, 640.f);
  drag(f, 50, 90, 30, 3);
  f.settle();
  CHECK_EQ(a->scrollY, 900.f);
  // Trackpad (pixel-precise) scrolling settles on the nearest snap point.
  f.in().scrollTo(a, 0, 0);
  f.in().mouseWheel(50, 50, 0, 10.5f);
  f.in().mouseWheel(50, 50, 0, 25.5f);
  CHECK_EQ(a->scrollY, 36.f);
  f.settle();
  CHECK_EQ(a->scrollY, 0.f);
  f.in().mouseWheel(50, 50, 0, 45.5f);
  f.settle();
  CHECK_EQ(a->scrollY, 80.f);
}

TEST(scroll_snap_to_offsets) {
  ScrollFixture f;
  Node* b = addList(f, R"({"snapToOffsets":[300,600],"snapToStart":false})");
  f.tick(16);
  // A small fling that ends before the first offset scrolls freely.
  drag(f, 50, 90, 75, 3);
  f.settle();
  CHECK(b->scrollY > 100 && b->scrollY < 300);
  // A fling past the offsets lands on the next snap (the end counts:
  // snapToEnd defaults to true).
  drag(f, 50, 90, 30, 3);
  f.settle();
  CHECK_EQ(b->scrollY, 900.f);
  // Released between offsets with no velocity: the nearest.
  f.in().touch(0, 0, 50, 10);
  f.tick(16);
  f.in().touch(0, 1, 50, 90);  // -> 820
  f.tick(16);
  f.in().touch(0, 1, 50, 99);  // -> 811
  f.tick(300);
  f.in().touch(0, 2, 50, 99);
  f.settle();
  CHECK_EQ(b->scrollY, 900.f);
  f.in().scrollTo(b, 0, 420);
  f.in().touch(0, 0, 50, 50);
  f.tick(16);
  f.in().touch(0, 1, 50, 60);  // -> 410
  f.tick(300);
  f.in().touch(0, 2, 50, 60);
  f.settle();
  CHECK_EQ(b->scrollY, 300.f);
}

TEST(scroll_smooth_scrollto_reaches_target) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  f.events();
  CHECK(f.in().scrollTo(sv, 0, 500, true));
  CHECK_EQ(sv->scrollY, 0.f);  // nothing jumps
  f.tick(160);
  CHECK(sv->scrollY > 50 && sv->scrollY < 450);
  f.settle();
  CHECK_EQ(sv->scrollY, 500.f);
  // Clamped target; an instant scrollTo cancels a running animation.
  f.in().scrollTo(sv, 0, 5000, true);
  f.tick(160);
  CHECK(sv->scrollY > 500 && sv->scrollY < 900);
  f.in().scrollTo(sv, 0, 100);
  CHECK_EQ(sv->scrollY, 100.f);
  f.tick(200);
  CHECK_EQ(sv->scrollY, 100.f);
  // The JS bridge: scrollTo(id, x, y, animated).
  f.js("__jsui_native.scrollTo(" + std::to_string(sv->id()) + ", 0, 300, true);");
  CHECK_EQ(sv->scrollY, 100.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 300.f);
  f.js("__jsui_native.scrollTo(" + std::to_string(sv->id()) + ", 0, 1e9, false);");
  CHECK_EQ(sv->scrollY, 900.f);
  // Programmatic animations are not momentum: scroll events only.
  std::string ev = f.events(true);
  CHECK(ev.find("momentum") == std::string::npos);
  CHECK(ev.find("scroll") != std::string::npos);
}

TEST(scroll_smooth_wheel_and_trackpad) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  // A notch glides over ~150 ms; notches accumulate.
  CHECK(f.in().mouseWheel(50, 50, 0, 100));
  CHECK_EQ(sv->scrollY, 0.f);
  f.tick(48);
  CHECK(sv->scrollY > 30 && sv->scrollY < 100);
  CHECK(f.in().mouseWheel(50, 50, 0, 100));
  f.settle();
  CHECK_EQ(sv->scrollY, 200.f);
  // Trackpad deltas apply immediately and exactly.
  CHECK(f.in().mouseWheel(50, 50, 0, 2.25f));
  CHECK_EQ(sv->scrollY, 202.25f);
  f.in().smoothWheel = false;
  CHECK(f.in().mouseWheel(50, 50, 0, 100));
  CHECK_EQ(sv->scrollY, 302.25f);
  // At the end: not consumed, so the wheel falls through.
  f.in().smoothWheel = true;
  f.in().scrollTo(sv, 0, 900);
  CHECK(!f.in().mouseWheel(50, 50, 0, 100));
}

TEST(scroll_gamepad_sticks) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  // Right stick: continuous, proportional, with a dead zone.
  CHECK(!f.in().gamepadAxis(0, 3, 0.1f));
  f.tick(160);
  CHECK_EQ(sv->scrollY, 0.f);
  CHECK(f.in().gamepadAxis(0, 3, 1.f));
  CHECK(f.s.rt->wantsFrame());
  f.tick(160);
  float full = sv->scrollY;
  CHECK(full > 200 && full < 280);  // 1.6 px/ms
  f.in().gamepadAxis(0, 3, 0.5f);
  f.tick(160);
  float half = sv->scrollY - full;
  CHECK(half > 20 && half < 50);  // quadratic response
  f.in().gamepadAxis(0, 3, -1.f);
  f.tick(80);
  CHECK(sv->scrollY < full + half);
  f.in().gamepadAxis(0, 3, 0.f);
  float stop = sv->scrollY;
  f.tick(100);
  CHECK_EQ(sv->scrollY, stop);
  // preventDefault on the axis event keeps the stick for the app.
  f.js("__jsui_dispatchEvent = e => e.type === 'gamepadaxis' ? 3 : 0;");
  f.in().gamepadAxis(0, 3, 1.f);
  f.tick(100);
  CHECK_EQ(sv->scrollY, stop);
  f.js("__jsui_dispatchEvent = logger;");
  f.in().gamepadAxis(0, 3, 0.f);
  // Left stick scrolls when there is nothing focusable to navigate.
  f.in().gamepadAxis(0, 1, 1.f);
  f.tick(160);
  CHECK(sv->scrollY > stop + 150);
  f.in().gamepadAxis(0, 1, 0.f);
  float left = sv->scrollY;
  f.tick(100);
  CHECK_EQ(sv->scrollY, left);
}

TEST(scroll_gamepad_stick_snaps_on_release) {
  ScrollFixture f;
  Node* sv = addList(f, R"({"pagingEnabled":true})");
  f.tick(16);
  f.in().gamepadAxis(0, 3, 0.6f);
  f.tick(64);
  CHECK(sv->scrollY > 0 && sv->scrollY < 100);
  f.in().gamepadAxis(0, 3, 0.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 100.f);  // continues to the next page in the direction of travel
}

TEST(scroll_focus_navigation_scrolls_into_view_smoothly) {
  ScrollFixture f;
  std::string rows;
  for (int i = 0; i < 10; i++)
    rows += std::string(i ? "," : "") + R"({"style":{"height":50},"props":{"__events":["press"]}})";
  Node* sv = addList(f, "{}", R"({"children":[)" + rows + "]}");
  f.tick(16);
  Node* content = sv->children()[0];
  CHECK_EQ(content->children().size(), size_t(10));
  auto row = [&](int i) { return content->children()[size_t(i)]; };
  InputManager& in = f.in();
  in.focus(row(0));
  auto pad = [&](int button) {
    in.gamepadButton(0, button, true, 1);
    in.gamepadButton(0, button, false, 0);
  };
  // D-pad down x3: glide, keeping 25 px of context (min(32, (100 - 50) / 2)).
  for (int i = 0; i < 3; i++) pad(13);
  CHECK_EQ(in.focused(), row(3));
  CHECK_EQ(sv->scrollY, 0.f);  // animated, not a jump
  f.tick(80);
  CHECK(sv->scrollY > 0 && sv->scrollY < 125);
  f.settle();
  CHECK_EQ(sv->scrollY, 125.f);  // row 3 (150..200) + 25 px below it
  pad(12);
  pad(12);
  f.settle();
  CHECK_EQ(in.focused(), row(1));
  CHECK_EQ(sv->scrollY, 25.f);  // row 1 (50..100) + 25 px above it
  // Focus moved by code after gamepad input glides too.
  in.focus(row(9));
  f.settle();
  CHECK_EQ(sv->scrollY, 400.f);
  // Pointer focus never scrolls (the item is already under the pointer).
  in.scrollTo(sv, 0, 0);
  in.mouseButton(50, 90, 0, true, 0);
  in.mouseButton(50, 90, 0, false, 0);
  CHECK_EQ(in.focused(), row(1));
  f.settle();
  CHECK_EQ(sv->scrollY, 0.f);
}

TEST(scroll_dpad_scrolls_past_last_focusable) {
  ScrollFixture f;
  Node* sv = addList(f, "{}", R"({"style":{"height":1000},"children":[{"style":{"height":40},"props":{"__events":["press"]}}]})");
  f.tick(16);
  InputManager& in = f.in();
  in.gamepadButton(0, 13, true, 1);  // focuses the only item
  in.gamepadButton(0, 13, false, 0);
  CHECK(in.focused() != nullptr);
  in.gamepadButton(0, 13, true, 1);  // nothing below: scroll a line
  in.gamepadButton(0, 13, false, 0);
  f.settle();
  CHECK_EQ(sv->scrollY, InputManager::kLineStep);
}

TEST(scroll_keyboard_paging) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  auto key = [&](const char* k, int mods = 0) {
    bool r = f.in().key(k, k, true, false, mods);
    f.in().key(k, k, false, false, mods);
    f.settle();
    return r;
  };
  CHECK(key("PageDown"));
  CHECK_EQ(sv->scrollY, 87.5f);
  CHECK(key("PageDown"));
  CHECK_EQ(sv->scrollY, 175.f);
  CHECK(key("PageUp"));
  CHECK_EQ(sv->scrollY, 87.5f);
  CHECK(key("End"));
  CHECK_EQ(sv->scrollY, 900.f);
  CHECK(!key("End"));  // already there
  CHECK(key("Home"));
  CHECK_EQ(sv->scrollY, 0.f);
  CHECK(key("ArrowDown"));  // nothing focusable: arrows scroll by a line
  CHECK_EQ(sv->scrollY, 40.f);
  CHECK(key(" "));
  CHECK_EQ(sv->scrollY, 127.5f);
  CHECK(key(" ", JSUI_MOD_SHIFT));
  CHECK_EQ(sv->scrollY, 40.f);
  // Repeated keys accumulate while gliding.
  f.in().key("PageDown", "PageDown", true, false, 0);
  f.in().key("PageDown", "PageDown", true, true, 0);
  f.in().key("PageDown", "PageDown", false, false, 0);
  f.settle();
  CHECK_EQ(sv->scrollY, 215.f);
  // A focused TextInput keeps the keys.
  Node* input = f.s.add(R"({"type":"TextInput","style":{"position":"absolute","left":120,"top":0,"width":60,"height":20}})");
  f.tick(16);
  f.in().focus(input);
  CHECK_EQ(f.in().focused(), input);
  key("PageDown");
  key("End");
  CHECK_EQ(sv->scrollY, 215.f);
}

TEST(scroll_horizontal_and_scroll_enabled) {
  ScrollFixture f;
  Node* sv = f.s.add(R"J({"type":"ScrollView","props":{"horizontal":true},"style":{"position":"absolute","left":0,"top":0,"width":100,"height":100,"flexDirection":"row"},
    "children":[{"style":{"width":1000,"height":100}}]})J");
  f.tick(16);
  // Touch drags move along the ScrollView's axis only.
  f.in().touch(0, 0, 90, 50);
  f.tick(16);
  f.in().touch(0, 1, 60, 40);
  f.tick(300);
  f.in().touch(0, 2, 60, 40);
  CHECK_EQ(sv->scrollX, 30.f);
  CHECK_EQ(sv->scrollY, 0.f);
  f.in().key("ArrowRight", "ArrowRight", true, false, 0);
  f.settle();
  CHECK_EQ(sv->scrollX, 70.f);
  // scrollEnabled=false: user input does nothing, programmatic still works.
  Value::Object p = sv->props().asObject();
  p["scrollEnabled"] = Value(false);
  sv->setProps(Value(p));
  f.tick(16);
  CHECK(!f.in().mouseWheel(50, 50, 0, 100));
  f.in().touch(0, 0, 90, 50);
  f.tick(16);
  f.in().touch(0, 1, 30, 50);
  f.in().touch(0, 2, 30, 50);
  f.in().gamepadAxis(0, 2, 1.f);
  f.tick(100);
  f.in().gamepadAxis(0, 2, 0.f);
  f.settle();
  CHECK_EQ(sv->scrollX, 70.f);
  f.in().scrollTo(sv, 500, 0);
  CHECK_EQ(sv->scrollX, 500.f);
}

TEST(scroll_indicators_fade_and_repaint_only_scrollview) {
  ScrollFixture f(200, 200);
  f.s.add(R"({"style":{"position":"absolute","left":150,"top":150,"width":40,"height":40,"backgroundColor":"#ff0000"}})");
  Node* sv = f.s.add(R"J({"type":"ScrollView","style":{"position":"absolute","left":20,"top":30,"width":100,"height":100,"backgroundColor":"#ffffff"},
    "children":[{"style":{"height":250,"backgroundColor":"#3366cc"}},{"style":{"height":250,"backgroundColor":"#33cc66"}}]})J");
  f.tick(16);
  std::vector<uint32_t> buf(200 * 200), full(200 * 200);
  f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  auto px = [&](int x, int y) { return buf[size_t(y) * 200 + size_t(x)]; };
  uint32_t content = px(100, 40);
  CHECK_EQ(px(116, 40), content);  // no indicator before scrolling
  // A user scroll: only the ScrollView's rect repaints.
  f.in().mouseWheel(50, 50, 0, 30.5f);
  CHECK_EQ(sv->scrollIndicatorOpacity, 1.f);
  f.tick(16);
  SkIRect d = f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  CHECK(!d.isEmpty());
  // (+1 px: anti-aliasing margin of the clip)
  CHECK(d.left() >= 19 && d.top() >= 29 && d.right() <= 121 && d.bottom() <= 131);
  // The thumb: 3 px wide, 2 px from the right edge, over the content.
  CHECK(px(116, 40) != content);
  CHECK_EQ(px(110, 40), content);
  f.s.rt->render(full.data(), 200, 200, 200 * 4);
  CHECK(buf == full);
  f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);  // back in step
  // After the hold the indicator fades: only its strip repaints.
  f.tick(InputManager::kIndicatorHoldMs + 60);
  CHECK(sv->scrollIndicatorOpacity > 0 && sv->scrollIndicatorOpacity < 1);
  d = f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  CHECK(!d.isEmpty());
  CHECK(d.left() >= 110 && d.right() <= 121 && d.top() >= 29 && d.bottom() <= 131);
  f.s.rt->render(full.data(), 200, 200, 200 * 4);
  CHECK(buf == full);
  f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  f.settle();
  CHECK_EQ(sv->scrollIndicatorOpacity, 0.f);
  f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  CHECK_EQ(px(116, 40), px(100, 40));
  // flashScrollIndicators shows them again.
  f.in().flashScrollIndicators(sv);
  f.tick(16);
  f.s.rt->renderIncremental(buf.data(), 200, 200, 200 * 4);
  CHECK(px(116, 40) != px(100, 40));
  // showsVerticalScrollIndicator={false}: nothing painted.
  Value::Object p = sv->props().asObject();
  p["showsVerticalScrollIndicator"] = Value(false);
  sv->setProps(Value(p));
  f.in().flashScrollIndicators(sv);
  f.tick(16);
  f.s.rt->render(full.data(), 200, 200, 200 * 4);
  CHECK_EQ(full[size_t(40) * 200 + 116], full[size_t(40) * 200 + 100]);
}

TEST(scroll_content_shrink_clamps_offset) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  f.in().scrollTo(sv, 0, 800);
  Node* content = sv->children()[0];
  Value::Object p = content->props().asObject();
  std::string err;
  p["style"] = Value::parseJson(R"({"height":300})", &err);
  content->setProps(Value(p));
  f.tick(16);
  CHECK_EQ(sv->scrollY, 200.f);
}
