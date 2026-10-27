// ScrollView physics and input: fling deceleration, catching a fling,
// paging / snap points, smooth programmatic / wheel / keyboard / focus
// scrolling, gamepad sticks, scroll events and their order, overlay
// indicators and damage-tracked repaint.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "animated/native_animated.h"
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
  CHECK_EQ(released, 52.f);  // the finger's travel minus the 8 px slop
  // RN order: begin drag, end drag (velocity + projected target), momentum.
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=676 momentumscrollbegin"));
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
  CHECK(sv->scrollY > 655 && sv->scrollY < 677);
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
  CHECK(sv->scrollY > 167 && sv->scrollY < 178);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=176 momentumscrollbegin momentumscrollend"));
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
  CHECK_EQ(sv->scrollY, 878.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 878.f);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=0.00,t=878"));
}

// Regression: a finger held pulling against an edge jitters by a pixel or
// two (iPhone touch digitizers). The clamp used to eat every move into the
// edge and apply every move away from it, so the content shook a few pixels
// until the finger scrolled away. The banked overshoot absorbs the jitter.
TEST(scroll_drag_jitter_against_edge_does_not_shake) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  auto move = [&](float y) {
    f.tick(16);
    f.in().touch(0, 1, 50, y);
  };
  // Top: pull down 40 px past the edge, then jitter.
  f.in().touch(0, 0, 50, 20);
  move(60);
  CHECK_EQ(sv->scrollY, 0.f);
  for (float y : {58.f, 61.f, 57.f, 60.f, 59.f, 62.f, 58.f}) {
    move(y);
    CHECK_EQ(sv->scrollY, 0.f);
  }
  move(80);  // pulls further: the bank stays at kDragEdgeSlack
  // Reversing moves the content once the slack is used up: 40 - 12 = 28.
  move(40);
  CHECK_EQ(sv->scrollY, 28.f);
  // Inside the range the content follows the finger 1:1 again.
  move(45);
  CHECK_EQ(sv->scrollY, 23.f);
  f.in().touch(0, 2, 50, 45);
  f.settle();

  // Bottom, same thing the other way.
  f.in().scrollTo(sv, 0, 900);
  f.tick(16);
  f.in().touch(0, 0, 50, 80);
  move(40);
  CHECK_EQ(sv->scrollY, 900.f);
  for (float y : {42.f, 38.f, 43.f, 40.f}) {
    move(y);
    CHECK_EQ(sv->scrollY, 900.f);
  }
  move(60);  // bank 10 after the jitter, then 20 px down: 900 + 10 - 20
  CHECK_EQ(sv->scrollY, 890.f);
  f.in().touch(0, 2, 50, 60);
  f.settle();
}

// The content does not jump by the slop when a drag is recognized: it
// moves by the finger's travel past it.
TEST(scroll_drag_starts_without_slop_jump) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  f.in().touch(0, 0, 50, 90);
  float y = 90;
  std::vector<float> pos;
  for (int i = 0; i < 8; i++) {
    f.tick(16);
    y -= 3;
    f.in().touch(0, 1, 50, y);
    pos.push_back(sv->scrollY);
  }
  f.in().touch(0, 2, 50, y);
  // 3, 6 px: inside the slop. 9 px: 1 past it. Then 3 px a move.
  CHECK_EQ(pos[0], 0.f);
  CHECK_EQ(pos[1], 0.f);
  CHECK_EQ(pos[2], 1.f);
  for (size_t i = 3; i < pos.size(); i++) CHECK_EQ(pos[i] - pos[i - 1], 3.f);
  f.settle();
}

// Touch samples that don't line up with frames: a 120 Hz digitizer whose
// events reach a 60 Hz frame loop late by a varying amount (a worker, a
// busy main thread, a sample landing on either side of vsync). Applied as
// they arrive, the content moves two samples on one frame and none or three
// on the next. With event times the drag is resampled at the frame time and
// every frame moves the same distance.
TEST(scroll_drag_resampled_at_frame_time) {
  auto run = [](bool stamped) {
    ScrollFixture f;
    Node* sv = addList(f);
    f.tick(16);
    const double frame = 1000.0 / 60, sample = 1000.0 / 120;
    const float speed = 0.6f;  // px/ms, finger moving up
    // Delivery delays (ms) cycling through 0..12: some samples miss the frame.
    const double delays[] = {0.5, 9, 3, 11.5, 1, 7, 12, 2};
    double t0 = f.now + 1;
    struct Ev {
      double t, at;
      float y;
    };
    std::vector<Ev> evs;
    for (int i = 0; i < 60; i++) {
      double t = t0 + i * sample;
      evs.push_back({t, t + delays[i % 8], float(95 - speed * (t - t0))});
    }
    std::vector<float> steps;
    size_t next = 0;
    float last = sv->scrollY;
    bool down = false;
    for (double now = f.now + frame; next < evs.size(); now += frame) {
      for (; next < evs.size() && evs[next].at <= now; next++) {
        if (stamped) f.in().setEventTime(evs[next].t);
        if (!down) f.in().touch(0, 0, 50, evs[next].y);
        else f.in().touch(0, 1, 50, evs[next].y);
        down = true;
      }
      f.now = now;
      f.s.rt->tick(now);
      if (sv->scrollY > 0) steps.push_back(sv->scrollY - last);
      last = sv->scrollY;
    }
    f.in().touch(0, 3, 50, evs.back().y);
    // Steady state: skip the drag's first frames and its last (the samples
    // stop there, so its resample time has no newer sample to bracket it).
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 2; i + 1 < steps.size(); i++) {
      lo = std::min(lo, steps[i]);
      hi = std::max(hi, steps[i]);
    }
    return std::make_pair(lo, hi);
  };
  auto [lo, hi] = run(true);
  // 0.6 px/ms * 16.7 ms = 10 px a frame (device pixels: +-1).
  CHECK(lo >= 9 && hi <= 11);
  auto [ulo, uhi] = run(false);
  CHECK(uhi - ulo >= 5);  // unstamped events move the content as they come
}

// The release velocity is the finger's speed when it lifts (least-squares
// quadratic), not the average over the window: a flick that speeds up
// hands the fling its final speed.
TEST(scroll_release_velocity_follows_accelerating_flick) {
  ScrollFixture f;
  addList(f);
  f.tick(16);
  f.events();
  // y = 95 - 0.01 t^2 over 96 ms, samples every 8 ms: 2 px/ms at the end.
  double t0 = f.now + 1;
  f.in().setEventTime(t0);
  f.in().touch(0, 0, 50, 95);
  for (int i = 1; i <= 12; i++) {
    double t = 8.0 * i;
    f.in().setEventTime(t0 + t);
    f.in().touch(0, 1, 50, float(95 - 0.01 * t * t));
  }
  f.in().setEventTime(t0 + 97);
  f.in().touch(0, 2, 50, float(95 - 0.01 * 96 * 96));
  std::string ev = f.events();
  size_t at = ev.find("#v=");
  CHECK(at != std::string::npos);
  double v = std::atof(ev.c_str() + at + 3);
  CHECK(v > 1.85 && v < 2.05);  // the window's average would be ~1.3
  f.settle();
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
  // Short quick flick (40 px, 32 past the slop): next page.
  drag(f, 50, 90, 50, 2);
  CHECK_EQ(sv->scrollY, 32.f);
  f.settle();
  CHECK_EQ(sv->scrollY, 100.f);
  CHECK_EQ(f.events(), std::string("scrollbegindrag scrollenddrag#v=1.25,t=100 momentumscrollbegin momentumscrollend"));
  // Drag back by 30 px with the finger resting before release: nearest page.
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
  f.in().touch(0, 1, 50, 90);  // -> 828
  f.tick(16);
  f.in().touch(0, 1, 50, 99);  // -> 819
  f.tick(300);
  f.in().touch(0, 2, 50, 99);
  f.settle();
  CHECK_EQ(b->scrollY, 900.f);
  f.in().scrollTo(b, 0, 420);
  f.in().touch(0, 0, 50, 50);
  f.tick(16);
  f.in().touch(0, 1, 50, 60);  // -> 418
  f.tick(300);
  f.in().touch(0, 2, 50, 60);
  f.settle();
  CHECK_EQ(b->scrollY, 300.f);
}

TEST(scroll_smooth_scrollto_reaches_target) {
  ScrollFixture f;
  Node* sv = addList(f, R"({"__events":["scroll"]})");
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

TEST(scroll_wheel_steps_glide_whatever_their_size) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  // macOS Chrome / AppKit: a slow notch of a mouse wheel is 4 px (0.1 line,
  // accelerated); the host says it is a wheel step, so it glides too.
  CHECK(f.in().mouseWheel(50, 50, 0, 4.000244f, JSUI_WHEEL_STEPS));
  CHECK_EQ(sv->scrollY, 0.f);
  f.tick(16);
  CHECK(sv->scrollY > 0 && sv->scrollY < 4);
  f.settle();
  CHECK(std::fabs(sv->scrollY - 4.000244f) < 1e-3f);
  // Pixel-precise input moves at once, even a whole 100.
  CHECK(f.in().mouseWheel(50, 50, 0, 100, JSUI_WHEEL_PRECISE));
  CHECK(std::fabs(sv->scrollY - 104.000244f) < 1e-3f);
}

// A fast spin (a notch every 33 ms, or the accelerated px of a macOS wheel
// every frame) moves at an even speed: a new step keeps the glide's speed
// instead of restarting its ease-out (that surged and sagged every notch).
TEST(scroll_wheel_fast_spin_moves_evenly) {
  for (int interval : {16, 32}) {
    ScrollFixture f(200, 200);
    Node* sv = addList(f, "{}", R"({"style":{"height":20000}})");
    f.tick(16);
    std::vector<float> steps;
    float last = 0;
    for (int frame = 0; frame < 40; frame++) {
      if (frame * 16 % interval == 0) CHECK(f.in().mouseWheel(50, 50, 0, interval == 16 ? 30 : 40, JSUI_WHEEL_STEPS));
      f.tick(16);
      steps.push_back(sv->scrollY - last);
      last = sv->scrollY;
    }
    // Steady state (after the ramp up): per-frame motion within 10 % of its
    // mean (restarting the ease-out per step swung it by 25-50 %), and
    // never backwards.
    float lo = 1e9f, hi = 0, sum = 0;
    for (size_t i = 20; i < steps.size(); i++) {
      lo = std::min(lo, steps[i]);
      hi = std::max(hi, steps[i]);
      sum += steps[i];
    }
    float mean = sum / float(steps.size() - 20);
    CHECK(mean > 15 && mean < 35);
    CHECK((hi - lo) / mean < 0.1f);
    for (float st : steps) CHECK(st >= 0);
    // Stops where the steps add up to, without overshooting.
    f.settle();
    int n = 0;
    for (int frame = 0; frame < 40; frame++) n += frame * 16 % interval == 0;
    CHECK(std::fabs(sv->scrollY - float(n) * (interval == 16 ? 30 : 40)) < 0.5f);
  }
}

// Turning the wheel back mid-glide heads back at once (no slow reversal).
TEST(scroll_wheel_reverses_without_lag) {
  ScrollFixture f;
  Node* sv = addList(f);
  f.tick(16);
  for (int i = 0; i < 3; i++) {
    CHECK(f.in().mouseWheel(50, 50, 0, 100, JSUI_WHEEL_STEPS));
    f.tick(16);
  }
  float before = sv->scrollY;
  CHECK(before > 60);
  CHECK(f.in().mouseWheel(50, 50, 0, -250, JSUI_WHEEL_STEPS));  // target 50
  f.tick(16);
  CHECK(sv->scrollY < before);  // already heading back in the next frame
  f.settle();
  CHECK_EQ(sv->scrollY, 50.f);
}

// Logs 'scroll' events as scroll@offset/timestamp (and pointer enter /
// leave with their target) instead of the fixture's plain types.
void logScrollOffsets(ScrollFixture& f) {
  f.js("globalThis.__jsui_dispatchEvent = e => {"
       "  if (e.type === 'scroll') log.push('scroll@' + e.contentOffset.y + '/' + e.timestamp);"
       "  else if (e.type === 'pointerenter' || e.type === 'pointerleave') log.push(e.type + ':' + e.target);"
       "  else log.push(e.type);"
       "  return 0; };");
}

TEST(scroll_event_throttle_sends_leading_and_trailing_events) {
  ScrollFixture f;
  // Trackpad deltas (applied at once); RN scrollEventThrottle 50 ms.
  Node* sv = addList(f, R"({"__events":["scroll"],"scrollEventThrottle":50})");
  f.tick(16);
  logScrollOffsets(f);
  f.events(true);
  // The first change goes out at once; later ones within 50 ms are held
  // back and the latest goes out (stamped with its own time) once due.
  for (int i = 0; i < 4; i++) {
    CHECK(f.in().mouseWheel(50, 50, 0, 10.5f));  // at t = 16, 32, 48, 64
    f.tick(16);
  }
  CHECK_EQ(sv->scrollY, 42.f);
  CHECK_EQ(f.events(true), std::string("scroll@10.5/16 scroll@42/64"));
  // Nothing more without changes; a later change waits for the interval.
  f.tick(48);
  CHECK_EQ(f.events(true), std::string());
  CHECK(f.in().mouseWheel(50, 50, 0, 8));  // t = 128: 64 ms after the last one
  CHECK_EQ(f.events(true), std::string("scroll@50/128"));
  CHECK(f.in().mouseWheel(50, 50, 0, 8));
  CHECK(f.s.rt->wantsFrame());
  f.tick(64);
  CHECK_EQ(f.events(true), std::string("scroll@58/128"));
  // Up to 16 ms: at most one per frame, the rest at the next tick.
  sv->setProps(Value::parseJson(R"({"__events":["scroll"],"scrollEventThrottle":16,)"
                                R"("style":{"position":"absolute","left":0,"top":0,"width":100,"height":100}})"));
  f.tick(16);
  f.events(true);
  CHECK(f.in().mouseWheel(50, 50, 0, 1));
  CHECK(f.in().mouseWheel(50, 50, 0, 1));
  CHECK(f.in().mouseWheel(50, 50, 0, 1));
  CHECK_EQ(f.events(true), std::string("scroll@59/208"));
  f.tick(16);
  CHECK_EQ(f.events(true), std::string("scroll@61/208"));
  // 0 / unset: every change.
  sv->setProps(Value::parseJson(R"({"__events":["scroll"],)"
                                R"("style":{"position":"absolute","left":0,"top":0,"width":100,"height":100}})"));
  f.tick(16);
  CHECK(f.in().mouseWheel(50, 50, 0, 1));
  CHECK(f.in().mouseWheel(50, 50, 0, 1));
  CHECK_EQ(f.events(true), std::string("scroll@62/240 scroll@63/240"));
}

TEST(scroll_events_reach_native_animated_mappings_without_listeners) {
  ScrollFixture f;
  Node* sv = addList(f, R"({"scrollEventThrottle":50})");
  NativeAnimated& na = *f.s.rt->animated;
  CHECK(na.createNode(1, Value::parseJson(R"({"type":"value","value":0})")));
  na.addEventMapping(sv->id(), "scroll", {"contentOffset", "y"}, 1);
  f.tick(16);
  f.events(true);
  // Animated.event(..., {useNativeDriver: true}) without a JS listener: the
  // mapping follows every offset (no throttle), JS hears nothing.
  for (int i = 1; i <= 3; i++) {
    CHECK(f.in().mouseWheel(50, 50, 0, 5));
    CHECK_EQ(na.getValue(1), 5.0 * i);
  }
  f.tick(100);
  CHECK_EQ(f.events(true), std::string());
}

TEST(scroll_hover_catches_up_when_wheel_scrolling_pauses) {
  ScrollFixture f;
  // Rows of 20 px that track hover, under a still mouse at y = 50.
  std::string rows;
  for (int i = 0; i < 50; i++)
    rows += std::string(i ? "," : "") + R"({"props":{"__events":["hoverIn","hoverOut"]},"style":{"height":20}})";
  Node* sv = addList(f, "{}", R"({"style":{"height":1000},"children":[)" + rows + "]}");
  Node* content = sv->children()[0];
  f.tick(16);
  logScrollOffsets(f);
  f.in().mouseMove(50, 50);
  auto row = [&](int i) { return std::to_string(content->children()[size_t(i)]->id()); };
  CHECK_EQ(f.events(true), "pointerenter:" + row(2));
  // Rows pass under the cursor while the list scrolls: no enter / leave per
  // row (each would re-render it), only once the scrolling pauses.
  for (int i = 0; i < 10; i++) {
    f.in().mouseWheel(50, 50, 0, 10.5f);
    f.tick(16);
  }
  CHECK_EQ(sv->scrollY, 105.f);
  CHECK_EQ(f.events(true), std::string());
  CHECK(f.s.rt->wantsFrame());
  f.tick(48);
  CHECK_EQ(f.events(true), std::string());
  f.tick(64);
  CHECK_EQ(f.events(true), "pointerleave:" + row(2) + " pointerenter:" + row(7));  // (50 + 105) / 20
  // A mouse move updates it at once, scrolling or not.
  f.in().mouseWheel(50, 50, 0, 10.5f);
  f.events(true);
  f.in().mouseMove(50, 90);
  CHECK_EQ(f.events(true), "pointerleave:" + row(7) + " pointerenter:" + row(10));  // (90 + 115.5) / 20
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
  CHECK_EQ(sv->scrollX, 22.f);  // 30 px of travel less the 8 px touch slop
  CHECK_EQ(sv->scrollY, 0.f);
  f.in().key("ArrowRight", "ArrowRight", true, false, 0);
  f.settle();
  CHECK_EQ(sv->scrollX, 62.f);
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
  CHECK_EQ(sv->scrollX, 62.f);
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
