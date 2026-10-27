// The engine examples' radar: a native component (jsui.h "Native
// components") the HUD shows with requireNativeComponent('Radar')
// (integrations/shared/ui/app.jsx). The game fills a Radar with its units
// every frame; the component draws them with Skia straight from that
// (no JSON, no React render per frame) under a rotating sweep, so it
// is registered JSUI_COMPONENT_CLOCKED and repaints every frame.
//
// Props it reads: `flash` (0..1, an Animated value the HUD pulses with the
// native driver when the selection changes) and `accent` (a color).
// Command `select(x, y)` (view px, from the HUD's onPress) selects the
// nearest blip through Radar::onSelect and answers with a `select` event
// ({index}) the HUD's onSelect receives.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "jsui/jsui.h"
#include "jsui/jsui_component.hpp"

namespace jsui_example {

struct Radar {
  struct Blip {
    float x = 0, y = 0;  // world units from the center (+y: towards the bottom of the radar)
    int hue = 0;         // index into the HUD's palette (entities' hue)
  };
  std::vector<Blip> blips;
  float range = 12;      // world units at the rim
  float heading = 0;     // camera direction, radians (0: +x), drawn as a view cone; NAN: none
  int selected = -1;
  std::function<void(int)> onSelect;  // a blip picked from the UI (select command)
};

namespace radar_detail {

constexpr float kPi = 3.14159265f;
// The HUD palette (examples/theme.js: gold, cyan, purple, green, orange, blue, red).
constexpr uint32_t kHues[] = {0xfff2c35bu, 0xff4fd1ffu, 0xffb26bffu, 0xff4ade80u,
                              0xffff9a3cu, 0xff4f8cffu, 0xffff4d6au};

inline uint32_t withAlpha(uint32_t argb, float a) {
  uint32_t al = uint32_t(std::lround(std::fmax(0.f, std::fmin(1.f, a)) * 255));
  return (argb & 0x00ffffffu) | (al << 24);
}

// Where a blip shows: radar px from the top-left of a view of size `size`.
inline void place(const Radar& r, const Radar::Blip& b, float size, float* x, float* y) {
  float half = size / 2, k = (half - 8) / r.range;
  float px = b.x * k, py = b.y * k;
  float d = std::sqrt(px * px + py * py), max = half - 8;
  if (d > max) {  // off the scale: pinned to the rim
    px *= max / d;
    py *= max / d;
  }
  *x = half + px;
  *y = half + py;
}

inline void draw(void* user, JsuiCanvas* cv, const JsuiDrawInfo* info) {
  const Radar& r = *static_cast<const Radar*>(user);
  jsui::Canvas c(cv);
  float size = std::fmin(info->width, info->height), half = size / 2;
  uint32_t accent = c.color("accent", 0xff4fd1ffu);
  float flash = float(c.number("flash", 0));
  float t = float(info->time_ms / 1000.0);

  // Face: a dark radial gradient, rings and cross hairs.
  uint32_t face[2] = {0xf0122036u, 0xf0060a14u};
  JsuiGradient g{};
  g.type = JSUI_GRADIENT_RADIAL;
  g.x0 = g.y0 = half;
  g.r = half;
  g.colors = face;
  g.count = 2;
  c.circle(half, half, half - 1, jsui::Paint().shader(&g));
  jsui::Paint line = jsui::Paint(withAlpha(accent, 0.22f)).stroke(1);
  for (int i = 1; i <= 3; i++) c.circle(half, half, (half - 8) * i / 3.f, line);
  c.line(half, 8, half, size - 8, line);
  c.line(8, half, size - 8, half, line);

  // The camera's view cone.
  if (!std::isnan(r.heading)) {
    float deg = r.heading * 180 / kPi;
    c.arc(8, 8, size - 16, size - 16, deg - 25, 50, true, jsui::Paint(withAlpha(0xffffffffu, 0.06f)));
  }

  // The sweep: a wedge whose sweep gradient fades out behind its leading edge.
  float sweepDeg = std::fmod(t * 90.f, 360.f);
  c.save();
  c.translate(half, half);
  c.rotate(sweepDeg);
  uint32_t trail[2] = {withAlpha(accent, 0), withAlpha(accent, 0.45f)};
  JsuiGradient sg{};
  sg.type = JSUI_GRADIENT_SWEEP;
  sg.start = 300;
  sg.end = 360;
  sg.colors = trail;
  sg.count = 2;
  float rr = half - 8;
  c.arc(-rr, -rr, 2 * rr, 2 * rr, -60, 60, true, jsui::Paint().shader(&sg));
  c.line(0, 0, rr, 0, jsui::Paint(withAlpha(accent, 0.9f)).stroke(1.5f).round());
  c.restore();

  // Blips: brightest just after the sweep passed them.
  for (size_t i = 0; i < r.blips.size(); i++) {
    float x, y;
    place(r, r.blips[i], size, &x, &y);
    float ang = std::atan2(y - half, x - half) * 180 / kPi;
    float behind = std::fmod(sweepDeg - ang + 720.f, 360.f);  // degrees since the sweep passed
    float lit = 0.35f + 0.65f * std::exp(-behind / 90.f);
    uint32_t col = kHues[r.blips[i].hue % 7];
    c.circle(x, y, 5, jsui::Paint(withAlpha(col, 0.35f * lit)).glow(3));
    c.circle(x, y, 2.6f, jsui::Paint(withAlpha(col, lit)));
    if (int(i) == r.selected) {
      float pulse = 0.5f + 0.5f * std::sin(t * 6);
      c.circle(x, y, 6.5f + 1.5f * pulse + 6 * flash, jsui::Paint(withAlpha(col, 0.9f - 0.5f * flash)).stroke(1.5f));
    }
  }

  // Rim: brighter while `flash` runs.
  c.circle(half, half, half - 1, jsui::Paint(withAlpha(accent, 0.35f + 0.65f * flash)).stroke(1.5f + 2 * flash));
  char label[32];
  std::snprintf(label, sizeof label, "%d CONTACTS", int(r.blips.size()));
  c.text(label, half, size - 14, jsui::Font(9, 700).centered(), jsui::Paint(withAlpha(accent, 0.8f)));
}

struct Instance {
  Radar* radar;
  JsuiRuntime* rt;
  int view;
};

}  // namespace radar_detail

// Registers the "Radar" component class drawing `radar` (kept by the caller
// for as long as the runtime lives).
inline int registerRadar(JsuiRuntime* rt, Radar* radar) {
  using namespace radar_detail;
  JsuiComponentClass cls;
  jsui_component_class_init(&cls);
  cls.user = radar;
  cls.flags = JSUI_COMPONENT_CLOCKED;
  cls.create = [](void* user, JsuiRuntime* rt, int view) -> void* {
    return new Instance{static_cast<Radar*>(user), rt, view};
  };
  cls.destroy = [](void* inst, int) { delete static_cast<Instance*>(inst); };
  cls.draw = [](void* inst, JsuiCanvas* c, const JsuiDrawInfo* info) {
    draw(static_cast<Instance*>(inst)->radar, c, info);
  };
  cls.command = [](void* inst, int, const char* name, const char* args) {
    auto* in = static_cast<Instance*>(inst);
    float px = 0, py = 0, size = 0;
    if (std::strcmp(name, "select") != 0 || std::sscanf(args, "[%f,%f,%f]", &px, &py, &size) != 3) return;
    int best = -1;
    float bestD = 14 * 14;
    for (size_t i = 0; i < in->radar->blips.size(); i++) {
      float x, y;
      place(*in->radar, in->radar->blips[i], size, &x, &y);
      float d = (x - px) * (x - px) + (y - py) * (y - py);
      if (d < bestD) {
        bestD = d;
        best = int(i);
      }
    }
    if (best < 0) return;
    if (in->radar->onSelect) in->radar->onSelect(best);
    char json[32];
    std::snprintf(json, sizeof json, "{\"index\":%d}", best);
    jsui_component_emit(in->rt, in->view, "select", json);
  };
  return jsui_register_component(rt, "Radar", &cls);
}

}  // namespace jsui_example
