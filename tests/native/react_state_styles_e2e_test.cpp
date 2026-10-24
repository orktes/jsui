// End-to-end state styles: a real @orktes/jsui-react bundle
// (packages/cli/test/e2e/state-styles.jsx) driven through the C API.
// - hoveredStyle eases over the transition, frame by frame, with no JS
//   ticked (jsui_tick_native) and no React render; group styles of the
//   children and the useInteraction value follow the same weight.
// - pressedStyle (instant transition) holds while the button is down, a
//   click shorter than a frame still shows for one frame, keyboard and
//   gamepad activation press too; over a natively animated transform.
// - focusedStyle vs focusVisibleStyle (pointer vs keyboard focus), a layout
//   key (borderWidth) included; group focus within.
// - disabledStyle from mount, no hover on a disabled element.
// - A React re-render with a changed style keeps the active overlays.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_STATE_STYLES_BUNDLE.
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

struct StateE2E {
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
      static_cast<StateE2E*>(u)->messages.emplace_back(name, json ? json : "");
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
  ~StateE2E() {
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
  int renders() {
    post("renders");
    tick();
    auto r = take("renders");
    return r.empty() ? -1 : int(r.back().asNumber());
  }
  jsui::Node* node(const char* name) {
    auto it = ids.find(name);
    return it == ids.end() ? nullptr : runtime().doc.get(it->second);
  }
  const Value& style(const char* name, const char* key) {
    static const Value null;
    jsui::Node* n = node(name);
    return n ? n->props()["style"][key] : null;
  }
  // Channel `i` (0..3) of an rgba() / #rrggbb style color, NaN if absent.
  double channel(const char* name, const char* key, int i) {
    const Value& v = style(name, key);
    if (!v.isString()) return NAN;
    const std::string& s = v.asString();
    if (s.size() == 7 && s[0] == '#') return i == 3 ? 1 : double(std::stoi(s.substr(1 + 2 * i, 2), nullptr, 16));
    double c[4] = {NAN, NAN, NAN, NAN};
    std::sscanf(s.c_str(), "rgba(%lf, %lf, %lf, %lf)", &c[0], &c[1], &c[2], &c[3]);
    return c[i];
  }
  double scale(const char* name) {
    for (const Value& t : style(name, "transform").asArray())
      if (t.has("scale")) return t["scale"].asNumber(NAN);
    return 1;
  }
  double translateX(const char* name) {
    for (const Value& t : style(name, "transform").asArray())
      if (t.has("translateX")) return t["translateX"].asNumber(NAN);
    return 0;
  }
};

}  // namespace

TEST(react_state_styles_e2e) {
  const char* path = std::getenv("JSUI_REACT_STATE_STYLES_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_STATE_STYLES_BUNDLE, see packages/cli/test)\n");
    return;
  }
  StateE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  int renders0 = t.renders();

  // ---- resting: base styles; disabled shows disabledStyle from mount.
  CHECK_NEAR(t.channel("card", "backgroundColor", 0), 32, 1e-6);
  CHECK_NEAR(t.channel("title", "color", 0), 128, 1e-6);
  CHECK_NEAR(t.style("off", "opacity").asNumber(), 0.4, 1e-9);
  CHECK_NEAR(t.style("meter", "opacity").asNumber(-1), 0, 1e-9);

  // ---- hover: 100 ms linear, one 16 ms frame at a time, no JS at all.
  jsui_send_mouse_move(t.rt, 50, 40);
  std::printf("    hover bg:");
  for (int f = 1; f <= 8; f++) {
    t.tickNative();
    double w = std::min(1.0, f * 16 / 100.0);
    double bg = t.channel("card", "backgroundColor", 0), title = t.channel("title", "color", 0);
    double meter = t.style("meter", "opacity").asNumber(-1);
    std::printf(" %.2f", bg);
    if (!(std::fabs(bg - (32 + 32 * w)) < 0.01) || !(std::fabs(title - (128 + 127 * w)) < 0.01) ||
        !(std::fabs(meter - w) < 1e-4)) {
      std::printf("\n    frame %d: bg %g title %g meter %g, want w %g\n", f, bg, title, meter, w);
      CHECK(false);
      break;
    }
  }
  std::printf("\n");
  CHECK_NEAR(t.channel("card", "backgroundColor", 0), 64, 1e-6);

  // ---- a re-render that changes the base style keeps the hover overlay.
  t.post("rerender");
  t.tick();
  CHECK_NEAR(t.style("card", "borderRadius").asNumber(-1), 1, 1e-9);
  CHECK_NEAR(t.channel("card", "backgroundColor", 0), 64, 1e-6);

  // ---- press (instant): scale, group pressed on the child, the
  // interaction value; pointer focus is not focus-visible.
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 1, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 0.9, 1e-9);
  CHECK_NEAR(t.style("dot", "opacity").asNumber(-1), 0.5, 1e-9);
  CHECK_NEAR(t.translateX("meter"), 50, 1e-6);
  for (int f = 0; f < 8; f++) t.tickNative();
  CHECK_NEAR(t.channel("card", "borderColor", 0), 255, 1e-6);  // focusedStyle red
  CHECK_NEAR(t.channel("card", "borderColor", 1), 0, 1e-6);
  CHECK_NEAR(t.style("card", "borderWidth").asNumber(), 2, 1e-9);
  CHECK_NEAR(t.channel("dot", "backgroundColor", 2), 255, 1e-6);  // group focus within
  // Dragging out of the card releases the pressed look, back in restores it.
  jsui_send_mouse_move(t.rt, 300, 250);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 1, 1e-9);
  jsui_send_mouse_move(t.rt, 50, 40);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 0.9, 1e-9);
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 0, 0);
  t.tick();
  CHECK_NEAR(t.scale("card"), 1, 1e-9);
  CHECK(t.style("card", "transform").isNull());  // the base has none: removed again
  CHECK_NEAR(t.translateX("meter"), 0, 1e-6);
  t.take("pressed");  // (Pressable cancels a press dragged out and back in)

  // ---- a click within one frame still shows pressed for that frame.
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 1, 0);
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 0, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 0.9, 1e-9);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 1, 1e-9);
  t.tick();
  CHECK_EQ(t.take("pressed").size(), size_t(1));

  // ---- disabled: no hover overlay; leaving the card fades its hover out.
  jsui_send_mouse_move(t.rt, 50, 140);
  for (int f = 0; f < 8; f++) t.tickNative();
  CHECK(t.style("off", "backgroundColor").isNull());
  CHECK_NEAR(t.channel("card", "backgroundColor", 0), 32, 1e-6);
  CHECK_NEAR(t.channel("title", "color", 0), 128, 1e-6);

  // ---- keyboard focus is focus-visible (green, borderWidth 4: a layout
  // key, so the child moves); Enter presses while held.
  jsui_send_mouse_button(t.rt, 300, 250, JSUI_MOUSE_LEFT, 1, 0);  // blur
  jsui_send_mouse_button(t.rt, 300, 250, JSUI_MOUSE_LEFT, 0, 0);
  t.tick();
  CHECK(t.runtime().doc.get(t.ids["card"]) != nullptr);
  jsui_send_key(t.rt, "Tab", "Tab", 1, 0, 0);
  jsui_send_key(t.rt, "Tab", "Tab", 0, 0, 0);
  for (int f = 0; f < 8; f++) t.tick();
  CHECK_NEAR(t.channel("card", "borderColor", 1), 255, 1e-6);
  CHECK_NEAR(t.channel("card", "borderColor", 0), 0, 1e-6);
  CHECK_NEAR(t.style("card", "borderWidth").asNumber(), 4, 1e-9);
  CHECK_NEAR(t.node("dot")->layoutRect().x, 4, 1e-3);  // inside the 4 px border
  jsui_send_key(t.rt, "Enter", "Enter", 1, 0, 0);
  t.tickNative();
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 0.9, 1e-9);
  jsui_send_key(t.rt, "Enter", "Enter", 0, 0, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 1, 1e-9);
  // Gamepad A likewise.
  jsui_send_gamepad_button(t.rt, 0, 0, 1, 1);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 0.9, 1e-9);
  jsui_send_gamepad_button(t.rt, 0, 0, 0, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("card"), 1, 1e-9);
  t.tick();
  // Blurring eases the layout key back to the base (2 px).
  jsui_send_mouse_button(t.rt, 300, 250, JSUI_MOUSE_LEFT, 1, 0);
  jsui_send_mouse_button(t.rt, 300, 250, JSUI_MOUSE_LEFT, 0, 0);
  for (int f = 0; f < 8; f++) t.tick();
  CHECK_NEAR(t.style("card", "borderWidth").asNumber(), 2, 1e-9);
  CHECK_NEAR(t.channel("card", "borderColor", 0), 0, 1e-6);
  CHECK_NEAR(t.channel("card", "borderColor", 1), 0, 1e-6);
  CHECK_NEAR(t.node("dot")->layoutRect().x, 2, 1e-3);

  // ---- a natively animated transform under pressedStyle: the press wins,
  // writes while pressed land in the base, which shows again on release.
  t.post("grow", "2");
  t.tick();
  CHECK_NEAR(t.scale("knob"), 2, 1e-9);
  jsui_send_mouse_button(t.rt, 220, 120, JSUI_MOUSE_LEFT, 1, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("knob"), 0.5, 1e-9);
  t.post("grow", "3");
  t.tick();
  CHECK_NEAR(t.scale("knob"), 0.5, 1e-9);
  jsui_send_mouse_button(t.rt, 220, 120, JSUI_MOUSE_LEFT, 0, 0);
  t.tickNative();
  CHECK_NEAR(t.scale("knob"), 3, 1e-9);

  // None of it rendered React again (only the explicit re-render did).
  CHECK_EQ(t.renders(), renders0 + 1);
}
