// End-to-end native typewriter and controllable clocks: a real
// @orktes/jsui-react bundle (packages/cli/test/e2e/reveal.jsx) driven
// through the C API.
// - Animated.Text reveal={value} with a native timing: the reveal advances
//   one grapheme per frame with only jsui_tick_native (no JS), the
//   paragraph is neither re-shaped nor laid out again, the painted pixels
//   grow frame by frame (the red span shows only once the reveal reaches
//   it) and end equal to the paragraph without a reveal; React never
//   re-renders.
// - revealMode="words" with revealFade: a word fades in.
// - Animated.clock({paused: true}): resume / pause / reset / setSpeed are
//   single commands; while paused it holds and does not keep the host
//   ticking; Animated.clock({running: useHostStateBind(...)}) follows host
//   state natively.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_REVEAL_BUNDLE.
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
#include "core/perf.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

constexpr int kW = 400, kH = 300;

struct RevealE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::map<std::string, uint32_t> ids;
  std::vector<uint32_t> px;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = kW;
    cfg.height = kH;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<RevealE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick();
    tick();
    for (const Value& m : take("ids"))
      for (auto& [k, v] : m.asObject()) ids[k] = uint32_t(v.asNumber());
    return ids.size() >= 4;
  }
  ~RevealE2E() {
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
  jsui::Node* node(const char* name) {
    auto it = ids.find(name);
    return it == ids.end() ? nullptr : runtime().doc.get(it->second);
  }
  double style(const char* name, const char* key) {
    jsui::Node* n = node(name);
    return n ? n->props()["style"][key].asNumber(NAN) : NAN;
  }
  double translateX(const char* name) {
    jsui::Node* n = node(name);
    if (!n) return NAN;
    for (const Value& t : n->props()["style"]["transform"].asArray())
      if (t.has("translateX")) return t["translateX"].asNumber(NAN);
    return NAN;
  }
  double renders() {
    post("renders");
    tick();
    auto r = take("renders");
    return r.empty() ? -1 : r[0].asNumber();
  }
  void render() {
    px.assign(size_t(kW) * kH, 0);
    jsui_render_rgba(rt, px.data(), kW, kH, size_t(kW) * 4);
  }
  // Text pixels (on black) in the line's band: bright ones and red ones.
  void count(int y0, int y1, int* bright, int* red) {
    *bright = *red = 0;
    for (int y = y0; y < y1; y++)
      for (int x = 0; x < kW; x++) {
        uint32_t p = px[size_t(y) * kW + x];
        int r = int(p & 255), g = int((p >> 8) & 255);
        if (r > 60 && g > 60) ++*bright;
        else if (r > 60 && g < 30) ++*red;
      }
  }
};

}  // namespace

TEST(react_reveal_e2e) {
  const char* path = std::getenv("JSUI_REACT_REVEAL_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_REVEAL_BUNDLE, see packages/cli/test)\n");
    return;
  }
  RevealE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  double r0 = t.renders();

  // ---- typewriter: nothing yet, then one grapheme per native frame.
  t.render();
  int bright = 0, red = 0;
  t.count(10, 55, &bright, &red);
  CHECK_EQ(bright + red, 0);
  jsui::Node* line = t.node("line");
  CHECK(line != nullptr);
  if (!line) return;
  jsui::Rect box = line->layoutRect();
  uint64_t builds = t.runtime().doc.perf.counters.paragraphBuilds;
  uint64_t layouts = jsui::gParagraphLayouts;
  t.post("type");
  t.tick();  // JS starts the native timing at this frame's time
  std::vector<double> reveals;
  std::vector<int> brights, reds;
  for (int f = 0; f < 14; f++) {
    t.tickNative();
    t.render();
    t.count(10, 55, &bright, &red);
    reveals.push_back(t.style("line", "textReveal"));
    brights.push_back(bright);
    reds.push_back(red);
  }
  std::printf("    reveal:");
  for (size_t i = 0; i < reveals.size(); i++) std::printf(" %g(%d/%d)", reveals[i], brights[i], reds[i]);
  std::printf("\n");
  bool monotonic = true;
  for (size_t i = 1; i < reveals.size(); i++) {
    monotonic = monotonic && reveals[i] >= reveals[i - 1] && brights[i] + reds[i] >= brights[i - 1] + reds[i - 1];
    if (reveals[i] < 12) CHECK(reveals[i] > reveals[i - 1]);  // moves every frame
  }
  CHECK(monotonic);
  CHECK_EQ(reveals.back(), 12.0);
  // The red span ("world": graphemes 7..11) shows only once the reveal is past 6.
  for (size_t i = 0; i < reveals.size(); i++) {
    if (reveals[i] <= 6) CHECK_EQ(reds[i], 0);
    if (reveals[i] >= 8) CHECK(reds[i] > 0);
  }
  CHECK(brights[0] > 0 && brights[0] < brights.back());
  // Laid out once with all of it: same box, no reshaping, no relayout.
  jsui::Rect after = line->layoutRect();
  CHECK_EQ(after.width, box.width);
  CHECK_EQ(after.height, box.height);
  CHECK_EQ(t.runtime().doc.perf.counters.paragraphBuilds, builds);
  CHECK_EQ(jsui::gParagraphLayouts, layouts);
  CHECK(t.take("typed").empty());  // no JS ran
  t.tick();
  auto typed = t.take("typed");
  CHECK_EQ(typed.size(), size_t(1));
  if (!typed.empty()) CHECK(typed[0].asBool());
  // Fully revealed == the paragraph painted without a reveal.
  t.render();
  std::vector<uint32_t> revealed = t.px;
  line->patchStyle(Value::parseJson(R"({"textReveal":null})").asObject());
  t.render();
  CHECK(t.px == revealed);

  // ---- words with a fade: "one " whole, "two " half there, "three" not.
  t.post("fade", "1.5");
  t.tick();
  CHECK_EQ(t.style("words", "textReveal"), 1.5);
  t.render();
  int maxA = 0, xMax = 0;
  for (int y = 60; y < 105; y++)
    for (int x = 0; x < kW; x++) {
      uint32_t p = t.px[size_t(y) * kW + x];
      int r = int(p & 255);
      if (r > 0) xMax = std::max(xMax, x);
      maxA = std::max(maxA, r);
    }
  CHECK(maxA > 200);
  jsui::Node* words = t.node("words");
  CHECK(words && xMax < 10 + words->layoutRect().width * 0.75);

  // ---- controllable clock: paused at 0, then commands.
  CHECK_EQ(t.translateX("clock"), 0.0);
  t.post("clock", R"({"op":"resume"})");
  t.tick();
  double x0 = t.translateX("clock");
  for (int f = 0; f < 5; f++) t.tickNative();
  CHECK_NEAR(t.translateX("clock") - x0, 0.08, 1e-9);
  t.post("clock", R"({"op":"speed","speed":2})");
  t.tick();
  double x1 = t.translateX("clock");
  for (int f = 0; f < 5; f++) t.tickNative();
  CHECK_NEAR(t.translateX("clock") - x1, 0.16, 1e-9);
  t.post("clock", R"({"op":"pause"})");
  t.tick();
  auto paused = t.take("clockPaused");
  CHECK(!paused.empty() && paused.back().asBool());
  double held = t.translateX("clock");
  for (int f = 0; f < 5; f++) t.tickNative();
  CHECK_EQ(t.translateX("clock"), held);
  // Paused, and the gated clock held by host state that is not set: nothing
  // keeps the host ticking.
  t.tick();
  CHECK(!jsui_wants_frame(t.rt));
  t.post("clock", R"({"op":"reset","t":100})");
  t.tick();
  CHECK_EQ(t.translateX("clock"), 100.0);
  t.tickNative();
  CHECK_EQ(t.translateX("clock"), 100.0);  // still paused
  t.post("clock", R"({"op":"resume"})");
  t.tick();
  double x2 = t.translateX("clock");
  t.tickNative();
  CHECK_NEAR(t.translateX("clock") - x2, 0.032, 1e-9);
  CHECK(jsui_wants_frame(t.rt));
  t.post("clock", R"({"op":"pause"})");
  t.tick();

  // ---- running: useHostStateBind('game', 'running'), natively.
  double g0 = t.translateX("gated");
  CHECK_EQ(g0, 0.0);
  jsui_set_state(t.rt, "game", R"({"running":true})");
  t.tickNative();
  t.tickNative();
  t.tickNative();
  double g1 = t.translateX("gated");
  CHECK(g1 > 0.03 && g1 < 0.05);  // 2-3 frames of 16 ms
  CHECK(jsui_wants_frame(t.rt));
  jsui_set_state(t.rt, "game", R"({"running":false})");
  t.tickNative();
  double g2 = t.translateX("gated");
  CHECK_NEAR(g2 - g1, 0.016, 1e-9);  // held at the frame that saw it
  for (int f = 0; f < 5; f++) t.tickNative();
  CHECK_EQ(t.translateX("gated"), g2);
  t.tick();
  CHECK(!jsui_wants_frame(t.rt));
  jsui_set_state(t.rt, "game", R"({"running":true})");
  t.tickNative();
  CHECK_EQ(t.translateX("gated"), g2);  // on again from this frame
  t.tickNative();
  CHECK_NEAR(t.translateX("gated") - g2, 0.016, 1e-9);

  // React rendered nothing through all of it.
  CHECK_EQ(t.renders(), r0 + 0);
}
