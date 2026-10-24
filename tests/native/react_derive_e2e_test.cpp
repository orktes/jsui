// End-to-end Animated.derive / Animated.clock: a real @orktes/jsui-react
// bundle (packages/cli/test/e2e/derive.jsx, built by jsui build so the
// expressions carry their source and captured scope) driven through the C API.
// - A nameplate follows host frame data through derived values (clamp,
//   branches, captured constants and a captured Animated value): its style
//   equals the same math on this frame's buffer, every frame, with JS ticked
//   and with only jsui_tick_native (no JS at all).
// - Native evaluation == the JS driver's evaluation of the same expressions.
// - The clock node follows the frame clock and keeps the host ticking.
// - A function outside the expression subset cannot go native (clear error)
//   but still runs on the JS driver.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_DERIVE_BUNDLE.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

struct DeriveE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::map<std::string, uint32_t> ids;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<DeriveE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick();
    tick();
    for (const Value& m : take("ids"))
      for (auto& [k, v] : m.asObject()) ids[k] = uint32_t(v.asNumber());
    return !ids.empty();
  }
  ~DeriveE2E() {
    if (rt) jsui_destroy(rt);
  }
  jsui::Runtime& runtime() { return *reinterpret_cast<std::unique_ptr<jsui::Runtime>*>(rt)->get(); }
  void tick() {
    now += 16;
    jsui_tick(rt, now);
  }
  void tickNative() {
    now += 16;
    jsui_tick_native(rt, now);
  }
  void post(const char* name, const char* json = "null") { jsui_post_message(rt, name, json); }
  std::vector<Value> take(const std::string& name) {
    std::vector<Value> out;
    for (auto it = messages.begin(); it != messages.end();) {
      if (it->first == name) {
        out.push_back(Value::parseJson(it->second));
        it = messages.erase(it);
      } else {
        ++it;
      }
    }
    return out;
  }
  double transform(const char* nativeId, const char* key) {
    auto it = ids.find(nativeId);
    jsui::Node* n = it == ids.end() ? nullptr : runtime().doc.get(it->second);
    if (!n) return NAN;
    for (const Value& t : n->props()["style"]["transform"].asArray())
      if (t.has(key)) return t[key].asNumber(NAN);
    return NAN;
  }
};

double clampd(double x, double lo, double hi) { return std::min(std::max(x, lo), hi); }
double smooth(double e0, double e1, double x) {
  double t = clampd((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}

}  // namespace

TEST(react_derive_e2e) {
  const char* path = std::getenv("JSUI_REACT_DERIVE_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_DERIVE_BUNDLE, see packages/cli/test)\n");
    return;
  }
  DeriveE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);

  // ---- nameplate on host frame data: exact every frame, JS or not.
  double offset = 5;
  int checked = 0;
  for (int f = 0; f < 120; f++) {
    float unit[3] = {float(-50 + f * 4.5), float(-30 + f * 3.1), float(f % 130)};
    float cam[1] = {float(20 + f)};
    jsui_set_buffer(t.rt, "unit", JSUI_BUFFER_F32, unit, 3);
    jsui_set_buffer(t.rt, "cam", JSUI_BUFFER_F32, cam, 1);
    if (f == 60) {
      t.post("offset", "-12");  // the captured Animated value, set from JS
      t.tick();
      offset = -12;
    }
    bool native = f % 3 != 0;  // two of three frames without any JS
    if (native) t.tickNative();
    else t.tick();
    double x = clampd((double(unit[0]) - double(cam[0])) * 1.5 + offset, 8, 392);
    double y = unit[1] < 0 ? 8 : std::min(double(unit[1]), 292.0);
    double sx = smooth(0, 100, double(unit[2]));
    double gx = t.transform("plate", "translateX"), gy = t.transform("plate", "translateY"), gs = t.transform("plate", "scaleX");
    if (!(std::fabs(gx - x) <= 1e-9) || !(std::fabs(gy - y) <= 1e-9) || !(std::fabs(gs - sx) <= 1e-9)) {
      std::printf("    frame %d (%s): got (%g, %g, %g), want (%g, %g, %g)\n", f, native ? "native" : "js", gx, gy, gs, x, y, sx);
      CHECK(false);
      break;
    }
    checked++;
  }
  CHECK_EQ(checked, 120);

  // ---- native == JS driver for every expression, over many inputs.
  const char* names[] = {"arith", "logic", "branches", "math", "helpers", "rounding"};
  double worst = 0;
  int pairs = 0;
  for (int i = 0; i < 60; i++) {
    double a = std::fmod(i * 7.3, 120) - 10, b = std::fmod(i * 13.7, 110) - 5;
    char json[128];
    std::snprintf(json, sizeof json, "[%.17g,%.17g,%.17g,%.17g]", a, b, b + 1, a * 0.5);
    t.post("set", json);
    t.tick();
    t.tick();  // the JS driver patches styles on its rAF
    for (const char* n : names) {
      for (int k = 0; k < 2; k++) {
        std::string id = std::string(n) + std::to_string(k);
        double nv = t.transform(id.c_str(), "translateX");
        double jv = t.transform(("js_" + id).c_str(), "translateX");
        double d = std::fabs(nv - jv);
        if (!(d <= 1e-9 * std::max(1.0, std::fabs(jv)))) {  // also NaN
          std::printf("    %s inputs %d: native %.17g js %.17g\n", id.c_str(), i, nv, jv);
          CHECK(false);
        }
        worst = std::max(worst, d);
        pairs++;
      }
    }
  }
  std::printf("    native vs JS driver: %d checks, max diff %.3g\n", pairs, worst);

  // ---- clock: the frame clock in seconds, re-evaluated every frame.
  CHECK(jsui_wants_frame(t.rt));  // a live clock keeps the host ticking
  for (int f = 0; f < 5; f++) {
    t.tickNative();
    CHECK_NEAR(t.transform("clock", "translateX"), std::fmod(t.now, 1000.0), 1e-6);
  }

  // ---- outside the subset: a clear error natively, still fine on the JS driver.
  t.post("notNative");
  t.tick();
  auto nn = t.take("notNative");
  CHECK_EQ(nn.size(), size_t(1));
  if (!nn.empty()) {
    std::printf("    notNative: %s\n", nn[0].asString().c_str());
    CHECK(nn[0].asString().find("cannot run natively") != std::string::npos);
  }
  auto jo = t.take("jsOnly");
  CHECK_EQ(jo.size(), size_t(1));
  if (!jo.empty()) CHECK_EQ(jo[0].asNumber(), 6.0);
}
