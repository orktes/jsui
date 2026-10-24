// What the engine examples (integrations/raylib, sdl3, sokol) share besides
// the engine glue: command line, the game <-> UI contract of
// integrations/shared/ui/app.jsx (messages, the `game` state, the `entities`
// frame data) and the --selftest input script.
#pragma once

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
};

inline void usage(const char* exe) {
  std::printf(
      "usage: %s [--dev] [--cpu] [--width W] [--height H] [--frames N] [--screenshot out.png] [--selftest]\n"
      "          [bundle.js | http://127.0.0.1:5173/bundle.js]\n"
      "  default bundle: %s (npx jsui build integrations/shared/ui/app.jsx -o ...)\n",
      exe, JSUI_EXAMPLE_BUNDLE);
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
  int clicks = 0, wheel = 0, keys = 0, pad = 0;  // passed through to the game

  // jsui_set_state is a no-op when the text is unchanged, so calling this
  // every frame only costs the formatting.
  void publish(JsuiRuntime* rt) const {
    char json[512];
    std::snprintf(json, sizeof(json),
                  "{\"engine\":\"%s\",\"renderer\":\"%s\",\"fps\":%d,\"entities\":%d,\"paused\":%s,"
                  "\"passed\":{\"clicks\":%d,\"wheel\":%d,\"keys\":%d,\"pad\":%d}}",
                  engine.c_str(), renderer.c_str(), fps, entities, paused ? "true" : "false", clicks, wheel, keys,
                  pad);
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

// --selftest: once the UI has reported where its Spawn button is, sends
// synthetic input straight to the runtime and checks the pass-through
// contract every host relies on:
//   1. a click on empty space (the middle of the screen) is not consumed,
//      and jsui_hit_test says nothing is there;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the host, which adds a unit.
// Prints "selftest: ok" (exit 0) or what failed (exit 1).
class SelfTest {
 public:
  // Feed every UI message (on_message).
  void onMessage(const char* name, const char* json) {
    if (std::strcmp(name, "ui:anchors") == 0) {
      spawnX_ = float(jsonNumber(json, "x") + jsonNumber(json, "width") / 2);
      spawnY_ = float(jsonNumber(json, "y") + jsonNumber(json, "height") / 2);
      anchors_ = true;
    }
  }

  // Call once per frame after jsui_tick. `w`, `h`: viewport (logical px).
  // Returns 0 while running, 1 when passed, -1 when failed.
  int step(JsuiRuntime* rt, float w, float h, int entities) {
    frame_++;
    switch (phase_) {
      case 0:  // wait for the UI and a few frames of layout
        if (anchors_ && frame_ > 30) phase_ = 1;
        else if (frame_ > 900) return fail("the UI never reported ui:anchors");
        return 0;
      case 1: {
        float x = w * 0.5f, y = h * 0.45f;
        if (jsui_hit_test(rt, x, y) != 0) return fail("jsui_hit_test reports UI over the empty middle of the screen");
        jsui_send_mouse_move(rt, x, y);
        int down = jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 1, 0);
        int up = jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 0, 0);
        if (down || up) return fail("a click on empty space was consumed");
        std::printf("selftest: empty-space click passed through to the game\n");
        phase_ = 2;
        return 0;
      }
      case 2: {
        if (!(jsui_hit_test(rt, spawnX_, spawnY_) & JSUI_HIT_POINTER))
          return fail("jsui_hit_test misses the Spawn button");
        before_ = entities;
        jsui_send_mouse_move(rt, spawnX_, spawnY_);
        int down = jsui_send_mouse_button(rt, spawnX_, spawnY_, JSUI_MOUSE_LEFT, 1, 0);
        int up = jsui_send_mouse_button(rt, spawnX_, spawnY_, JSUI_MOUSE_LEFT, 0, 0);
        if (!down || !up) return fail("the click on the Spawn button was not consumed");
        jsui_send_mouse_leave(rt);
        std::printf("selftest: Spawn button click consumed by the UI\n");
        phase_ = 3;
        wait_ = frame_;
        return 0;
      }
      case 3:
        if (entities == before_ + 1) {
          std::printf("selftest: game:spawn reached the game (%d -> %d units)\n", before_, entities);
          phase_ = 4;
          wait_ = frame_;
          return 0;
        }
        if (frame_ - wait_ > 120) return fail("game:spawn never arrived");
        return 0;
      default:
        // A few more frames so the nameplate of the new unit is on screen
        // for --screenshot.
        if (frame_ - wait_ < 20) return 0;
        std::printf("selftest: ok\n");
        return 1;
    }
  }

 private:
  int fail(const char* what) {
    std::fprintf(stderr, "selftest: FAILED: %s\n", what);
    return -1;
  }
  bool anchors_ = false;
  float spawnX_ = 0, spawnY_ = 0;
  int phase_ = 0, before_ = 0;
  long frame_ = 0, wait_ = 0;
};

}  // namespace jsui_example
