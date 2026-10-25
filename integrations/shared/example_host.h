// What the engine examples (integrations/raylib, sdl3, sokol) share besides
// the engine glue: command line, the game <-> UI contract of
// integrations/shared/ui/app.jsx (messages, the `game` state, the `entities`
// frame data), click-vs-drag selection, a ray-vs-box test for picking units
// and the --selftest input script.
//
// JS runs on its own thread (JsuiConfig.js_thread = JSUI_JS_THREAD_ON, as in
// React Native) unless --js-thread off / lockstep says otherwise: the game's
// frames never wait for the HUD's JS, and the nameplates (host buffer ->
// native Animated) follow the units every frame even while JS is busy. The
// examples tick every frame, so they need no on_js_ready wake-up.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "jsui/jsui.h"

#ifndef JSUI_EXAMPLE_BUNDLE
#define JSUI_EXAMPLE_BUNDLE "integrations/shared/ui/dist/bundle.js"
#endif

namespace jsui_example {

struct Options {
  std::string bundle = JSUI_EXAMPLE_BUNDLE;  // path or http:// URL (jsui dev)
  bool dev = false;                          // poll the dev server, reload on change
  bool cpu = false;                          // CPU raster + texture upload instead of the GPU path
  int width = 1280, height = 720;            // window size (logical px)
  long frames = -1;                          // run N frames, then exit
  std::string screenshot;                    // write the last frame here (with --frames)
  bool selftest = false;                     // scripted input check (see SelfTest), then exit
  int jsThread = JSUI_JS_THREAD_ON;          // --js-thread on|off|lockstep (JsuiConfig.js_thread)
};

inline void usage(const char* exe) {
  std::printf(
      "usage: %s [--dev] [--cpu] [--width W] [--height H] [--frames N] [--screenshot out.png] [--selftest]\n"
      "          [--js-thread on|off|lockstep] [bundle.js | http://127.0.0.1:5173/bundle.js]\n"
      "  default bundle: %s (npx jsui build integrations/shared/ui/app.jsx -o ...)\n"
      "  --js-thread: on (default) runs the UI's JS on its own thread; off on the game's thread\n",
      exe, JSUI_EXAMPLE_BUNDLE);
}

inline const char* jsThreadName(int mode) {
  return mode == JSUI_JS_THREAD_ON ? "on" : mode == JSUI_JS_THREAD_LOCKSTEP ? "lockstep" : "off";
}

inline bool parseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--dev") o->dev = true;
    else if (a == "--cpu") o->cpu = true;
    else if (a == "--selftest") o->selftest = true;
    else if (a == "--width") o->width = std::atoi(value());
    else if (a == "--height") o->height = std::atoi(value());
    else if (a == "--frames") o->frames = std::atol(value());
    else if (a == "--screenshot") o->screenshot = value();
    else if (a == "--js-thread") {
      std::string m = value();
      if (m == "on") o->jsThread = JSUI_JS_THREAD_ON;
      else if (m == "off") o->jsThread = JSUI_JS_THREAD_OFF;
      else if (m == "lockstep") o->jsThread = JSUI_JS_THREAD_LOCKSTEP;
      else {
        std::fprintf(stderr, "--js-thread takes on, off or lockstep\n");
        return false;
      }
    }
    else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      usage(argv[0]);
      return false;
    } else {
      o->bundle = a;
    }
  }
  if (!o->screenshot.empty() && o->frames < 0 && !o->selftest) o->frames = 120;
  return o->width > 0 && o->height > 0;
}

// The number after "key": in a flat JSON object (messages of app.jsx are
// tiny and fixed; a real game would use its JSON library).
inline double jsonNumber(const char* json, const char* key, double fallback = 0) {
  if (!json) return fallback;
  std::string k = std::string("\"") + key + "\":";
  const char* p = std::strstr(json, k.c_str());
  return p ? std::atof(p + k.size()) : fallback;
}
inline bool jsonBool(const char* json, const char* key) {
  if (!json) return false;
  std::string k = std::string("\"") + key + "\":true";
  return std::strstr(json, k.c_str()) != nullptr;
}

// What the game hands to the UI's `game` state, and what reached the game
// because the UI did not consume it.
struct HudState {
  std::string engine, renderer;
  int fps = 0;
  int entities = 0;
  bool paused = false;
  int selected = -1;                             // index of the selected unit, -1 for none
  int clicks = 0, wheel = 0, keys = 0, pad = 0;  // passed through to the game

  // jsui_set_state is a no-op when the text is unchanged, so calling this
  // every frame only costs the formatting.
  void publish(JsuiRuntime* rt) const {
    char json[512];
    std::snprintf(json, sizeof(json),
                  "{\"engine\":\"%s\",\"renderer\":\"%s\",\"fps\":%d,\"entities\":%d,\"paused\":%s,"
                  "\"selected\":%d,\"passed\":{\"clicks\":%d,\"wheel\":%d,\"keys\":%d,\"pad\":%d}}",
                  engine.c_str(), renderer.c_str(), fps, entities, paused ? "true" : "false", selected, clicks, wheel,
                  keys, pad);
    jsui_set_state(rt, "game", json);
  }
};

// Frames per second over the last 60 frames, skipping the first 10 (window
// creation, bundle load, first pipeline compiles), so short --frames /
// --selftest runs show the real rate too. The shown value changes at most
// every 250 ms, so the HUD does not re-render for it every frame.
struct FpsCounter {
  static constexpr int kWindow = 60, kWarmup = 10;
  double times[kWindow] = {};
  long seen = 0;
  double shownAt = -1;
  int fps = 0;
  void frame(double nowMs) {
    if (seen++ < kWarmup) return;
    long n = seen - kWarmup;  // frames in the ring, up to kWindow
    times[(n - 1) % kWindow] = nowMs;
    if (n < 20) return;
    double oldest = times[n < kWindow ? 0 : n % kWindow];
    long count = n < kWindow ? n : kWindow;
    if (nowMs <= oldest || (shownAt >= 0 && nowMs - shownAt < 250)) return;
    fps = int(std::lround((count - 1) * 1000.0 / (nowMs - oldest)));
    shownAt = nowMs;
  }
};

// Selection by click: a left press the UI passed on to the game, released
// within a few px of where it went down, is a click (pick the unit under it,
// or clear the selection on empty ground); anything longer is a camera drag.
struct ClickTracker {
  static constexpr float kSlop = 4.f;  // logical px
  bool down = false;
  float x0 = 0, y0 = 0, travel = 0;
  void press(float x, float y) {
    down = true;
    x0 = x;
    y0 = y;
    travel = 0;
  }
  void move(float x, float y) {
    if (down) travel = std::fmax(travel, std::hypot(x - x0, y - y0));
  }
  // On the left release: whether the press was a click.
  bool release(float x, float y) {
    move(x, y);
    bool click = down && travel <= kSlop;
    down = false;
    return click;
  }
};

// Ray (origin o, direction d) against a box of half extent h around c,
// turned by `yaw` radians around the vertical axis (y). Returns the distance
// along d to the first hit, or a negative number for a miss.
inline float rayBox(const float o[3], const float d[3], const float c[3], const float h[3], float yaw = 0) {
  // Into the box's frame: translate, then rotate by -yaw around y.
  float cs = std::cos(yaw), sn = std::sin(yaw);
  float px = o[0] - c[0], py = o[1] - c[1], pz = o[2] - c[2];
  float lo[3] = {px * cs + pz * sn, py, -px * sn + pz * cs};
  float ld[3] = {d[0] * cs + d[2] * sn, d[1], -d[0] * sn + d[2] * cs};
  float tmin = -1e30f, tmax = 1e30f;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(ld[a]) < 1e-8f) {
      if (lo[a] < -h[a] || lo[a] > h[a]) return -1;
      continue;
    }
    float t1 = (-h[a] - lo[a]) / ld[a], t2 = (h[a] - lo[a]) / ld[a];
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::fmax(tmin, t1);
    tmax = std::fmin(tmax, t2);
    if (tmin > tmax) return -1;
  }
  if (tmax < 0) return -1;
  return tmin >= 0 ? tmin : tmax;
}

// What --selftest drives in the host: its own input handling, as real
// events reach it, and its picking.
class SelfTestHost {
 public:
  virtual ~SelfTestHost() = default;
  // A left click at (x, y) (logical px): pointer move, press, release, through
  // the same code the host runs for real mouse events.
  virtual void click(float x, float y) = 0;
  // The unit a click at (x, y) selects, or -1.
  virtual int pick(float x, float y) = 0;
  // Unit `i`'s centre on screen (logical px); false when it is off screen.
  virtual bool unitOnScreen(int i, float* x, float* y) = 0;
};

// --selftest: once the UI has reported where its Spawn button is, stalls JS
// for a moment and then clicks through the host's input handling
// (SelfTestHost::click), checking:
//   0. with JS on its own thread, a JS stall (jsui_debug_stall_js) holds no
//      host frame: the frames go on meanwhile and JS catches up after it
//      (without the thread the stall holds the host, as expected);
// and the pass-through contract every host relies on:
//   1. a click on empty space (the middle of the screen) is not consumed:
//      jsui_hit_test says nothing is there and the game counts the click;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the host, which adds a unit;
//   3. a click on a unit selects it, and the UI reports the selection back
//      (ui:selected); a click on empty ground clears it; another click
//      selects again (so --screenshot shows a selected unit).
// Prints "selftest: ok" (exit 0) or what failed (exit 1).
class SelfTest {
 public:
  // Feed every UI message (on_message).
  void onMessage(const char* name, const char* json) {
    if (std::strcmp(name, "ui:anchors") == 0) {
      spawnX_ = float(jsonNumber(json, "x") + jsonNumber(json, "width") / 2);
      spawnY_ = float(jsonNumber(json, "y") + jsonNumber(json, "height") / 2);
      anchors_ = true;
    } else if (std::strcmp(name, "ui:selected") == 0) {
      reported_ = int(jsonNumber(json, "index", -1));
      reports_++;
    }
  }

  // Call once per frame after jsui_tick. `w`, `h`: viewport (logical px);
  // `hud`: the game's state (units, selection, pass-through counters).
  // Returns 0 while running, 1 when passed, -1 when failed.
  int step(JsuiRuntime* rt, SelfTestHost& host, float w, float h, const HudState& hud) {
    frame_++;
    switch (phase_) {
      case 0:  // wait for the UI and a few frames of layout
        if (anchors_ && frame_ > 30) phase_ = kStall;
        else if (frame_ > 900) return fail("the UI never reported ui:anchors");
        return 0;
      case kStall: {
        int r = stallStep(rt);
        if (r > 0) phase_ = 1;
        return r < 0 ? -1 : 0;
      }
      case 1: {
        float x = w * 0.5f, y = h * 0.45f;
        if (jsui_hit_test(rt, x, y) != 0) return fail("jsui_hit_test reports UI over the empty middle of the screen");
        int clicks = hud.clicks;
        host.click(x, y);
        if (hud.clicks != clicks + 1) return fail("a click on empty space did not reach the game");
        std::printf("selftest: empty-space click passed through to the game\n");
        phase_ = 2;
        return 0;
      }
      case 2: {
        if (!(jsui_hit_test(rt, spawnX_, spawnY_) & JSUI_HIT_POINTER))
          return fail("jsui_hit_test misses the Spawn button");
        before_ = hud.entities;
        int clicks = hud.clicks;
        host.click(spawnX_, spawnY_);
        if (hud.clicks != clicks) return fail("the click on the Spawn button was passed to the game");
        std::printf("selftest: Spawn button click consumed by the UI\n");
        phase_ = 3;
        wait_ = frame_;
        return 0;
      }
      case 3:
        if (hud.entities == before_ + 1) {
          std::printf("selftest: game:spawn reached the game (%d -> %d units)\n", before_, hud.entities);
          phase_ = 4;
          return 0;
        }
        if (frame_ - wait_ > 120) return fail("game:spawn never arrived");
        return 0;
      case 4: {  // select a unit, clear, select again
        float x = 0, y = 0;
        int want = -1;
        if (round_ == 1) {
          if (!emptyGround(rt, host, w, h, &x, &y)) return fail("no empty ground to click");
        } else {
          want = clickableUnit(rt, host, hud.entities, w, h, &x, &y);
          if (want < 0) return fail("no unit on screen to click");
        }
        host.click(x, y);
        if (hud.selected != want) {
          std::fprintf(stderr, "selftest: clicked (%.0f, %.0f): selected %d, expected %d\n", x, y, hud.selected, want);
          return fail(want < 0 ? "a click on empty ground did not clear the selection" : "a click on a unit did not select it");
        }
        expect_ = want;
        reportsBefore_ = reports_;
        phase_ = 5;
        wait_ = frame_;
        return 0;
      }
      case 5:
        if (reports_ > reportsBefore_ && reported_ == expect_) {
          if (expect_ >= 0) std::printf("selftest: a click on unit %d selected it; the UI reports it\n", expect_);
          else std::printf("selftest: a click on empty ground cleared the selection; the UI reports it\n");
          if (++round_ < 3) {
            phase_ = 4;
          } else {
            phase_ = 6;
            wait_ = frame_;
          }
          return 0;
        }
        if (frame_ - wait_ > 120) return fail("the UI never reported the selection (ui:selected)");
        return 0;
      default:
        // A few more frames so the selection is on screen for --screenshot.
        if (frame_ - wait_ < 20) return 0;
        std::printf("selftest: ok\n");
        return 1;
    }
  }

 private:
  static constexpr int kStall = 100;        // the stall phase
  static constexpr double kStallMs = 300;  // how long JS is kept busy
  using Clock = std::chrono::steady_clock;
  static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

  // One JS stall of kStallMs, stepped once per host frame: 1 when done, -1
  // when it failed. With a JS thread, posting it returns at once, no gap
  // between two host frames comes near the stall while it runs, and
  // jsui_sync sees JS idle again after it. Off or in lockstep the post
  // itself holds the host for the stall.
  int stallStep(JsuiRuntime* rt) {
    Clock::time_point now = Clock::now();
    if (stallFrames_ == 0) {
      int mode = jsui_js_thread(rt);
      jsui_debug_stall_js(rt, kStallMs);
      double held = ms(Clock::now() - now);
      if (mode != JSUI_JS_THREAD_ON) {
        if (held < kStallMs * 0.9) return fail("jsui_debug_stall_js did not keep JS busy");
        std::printf("selftest: JS thread %s: a %.0f ms JS stall held the game for %.0f ms\n", jsThreadName(mode),
                    kStallMs, held);
        return 1;
      }
      if (held > kStallMs / 4) return fail("posting a JS stall waited for the JS thread");
      stallStart_ = lastFrame_ = Clock::now();
      stallFrames_ = 1;
      return 0;
    }
    double gap = ms(now - lastFrame_);
    lastFrame_ = now;
    maxGap_ = std::max(maxGap_, gap);
    if (ms(now - stallStart_) < kStallMs) {
      if (gap > kStallMs / 2) return fail("a game frame waited for the busy JS thread");
      stallFrames_++;
      return 0;
    }
    if (stallFrames_ < 3) return fail("too few game frames while JS was busy");
    if (jsui_sync(rt, 2000) != 1) return fail("JS did not come back from its stall");
    std::printf("selftest: JS thread on: %d game frames during a %.0f ms JS stall, the longest %.1f ms\n",
                stallFrames_, kStallMs, maxGap_);
    return 1;
  }

  // A unit whose centre is on screen, away from the edges, not under the UI,
  // and picked there (not hidden behind another one). The newest first.
  int clickableUnit(JsuiRuntime* rt, SelfTestHost& host, int count, float w, float h, float* x, float* y) {
    for (int i = count - 1; i >= 0; i--) {
      if (!host.unitOnScreen(i, x, y)) continue;
      if (*x < 40 || *y < 40 || *x > w - 40 || *y > h - 40) continue;
      if (jsui_hit_test(rt, *x, *y) != 0) continue;
      if (host.pick(*x, *y) == i) return i;
    }
    return -1;
  }
  // A point with neither UI nor a unit under it.
  bool emptyGround(JsuiRuntime* rt, SelfTestHost& host, float w, float h, float* x, float* y) {
    for (int gy = 2; gy < 10; gy++)
      for (int gx = 1; gx < 16; gx++) {
        *x = w * gx / 16.f;
        *y = h * gy / 12.f;
        if (jsui_hit_test(rt, *x, *y) == 0 && host.pick(*x, *y) < 0) return true;
      }
    return false;
  }
  int fail(const char* what) {
    std::fprintf(stderr, "selftest: FAILED: %s\n", what);
    return -1;
  }
  bool anchors_ = false;
  float spawnX_ = 0, spawnY_ = 0;
  int phase_ = 0, before_ = 0, round_ = 0, expect_ = -1;
  int reported_ = -1, reports_ = 0, reportsBefore_ = 0;
  long frame_ = 0, wait_ = 0;
  int stallFrames_ = 0;
  double maxGap_ = 0;
  Clock::time_point stallStart_, lastFrame_;
};

}  // namespace jsui_example
