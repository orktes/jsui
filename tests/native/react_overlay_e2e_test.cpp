// End-to-end overlay layer: a real @orktes/jsui-react bundle
// (packages/cli/test/e2e/overlay.jsx) driven through the C API, with
// jsui_tick_native only (no JS) wherever the runtime alone should react.
// - A hover popout (<Overlay> + Visibility of useGroupState) inside a
//   clipping ScrollView: drawn and hit outside the clip, and the pointer on
//   it keeps the slot's group hovered; a press on it reaches its handler.
// - A badge overlay follows its row while the list scrolls, without JS.
// - An overlay anchored by ref near the bottom edge flips to the top.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_OVERLAY_BUNDLE.
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
#include "input/input.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

struct OverlayE2E {
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
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<OverlayE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    // Refs resolve after the first commit (the ref-anchored overlay
    // renders again with its anchor's id).
    for (int i = 0; i < 4; i++) tick();
    for (const Value& m : take("ids"))
      for (auto& [k, v] : m.asObject()) ids[k] = uint32_t(v.asNumber());
    return !ids.empty();
  }
  ~OverlayE2E() {
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
  bool displayed(const char* name) {
    jsui::Node* n = node(name);
    return n && n->style().display;
  }
  jsui::Rect rect(const char* name) {
    jsui::Node* n = node(name);
    return n ? runtime().input->visualRect(n) : jsui::Rect{-1, -1, -1, -1};
  }
  // Whether the node hit at (x, y) is `name` or inside it (logically).
  bool hits(float x, float y, const char* name) {
    jsui::Node* target = node(name);
    for (jsui::Node* n = runtime().input->hitTest(x, y); n; n = n->parent())
      if (n == target) return true;
    return false;
  }
};

}  // namespace

TEST(react_overlay_e2e) {
  const char* path = std::getenv("JSUI_REACT_OVERLAY_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_OVERLAY_BUNDLE, see packages/cli/test)\n");
    return;
  }
  OverlayE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  int renders0 = t.renders();

  // ---- the popout: hidden until the slot is hovered.
  CHECK(!t.displayed("pop"));
  jsui_send_mouse_move(t.rt, 100, 75);
  t.tickNative();
  CHECK(t.displayed("pop"));
  // Right of the 180-wide slot (x 20..200, y 60..90), centred on it: past
  // the ScrollView's right edge (220), which clips its content.
  jsui::Rect pop = t.rect("pop");
  CHECK_NEAR(pop.x, 200, 0.01);
  CHECK_NEAR(pop.y, 50, 0.01);
  CHECK_NEAR(pop.width, 120, 0.01);
  CHECK(t.hits(300, 75, "popBtn"));
  // The arrow: on the popout's left edge, centred on the slot.
  jsui::Rect arrow = t.rect("arrow");
  CHECK_NEAR(arrow.x + arrow.width / 2, 200, 0.5);
  CHECK_NEAR(arrow.y + arrow.height / 2, 75, 0.5);

  // ---- onto the popout, outside the list: the slot's group stays hovered.
  jsui_send_mouse_move(t.rt, 300, 75);
  t.tickNative();
  t.tickNative();
  CHECK(t.displayed("pop"));
  CHECK(t.runtime().input->stateHovered(t.node("slot")));
  // A press on it reaches its own handler.
  jsui_send_mouse_button(t.rt, 300, 75, JSUI_MOUSE_LEFT, 1, 0);
  t.tick();
  jsui_send_mouse_button(t.rt, 300, 75, JSUI_MOUSE_LEFT, 0, 0);
  t.tick();
  t.tick();
  CHECK_EQ(t.take("popPress").size(), size_t(1));
  CHECK(t.take("slotPress").empty());
  // Away from both: hidden again.
  jsui_send_mouse_move(t.rt, 380, 150);
  t.tickNative();
  CHECK(!t.displayed("pop"));
  CHECK(!t.hits(300, 75, "popBtn"));

  // ---- the badge follows its row as the list scrolls (native only).
  jsui::Rect badge = t.rect("badge");
  CHECK_NEAR(badge.x, 20, 0.01);
  CHECK_NEAR(badge.y, 92, 0.01);
  jsui_send_mouse_move(t.rt, 100, 150);
  jsui_send_mouse_wheel(t.rt, 100, 150, 0, 40);
  t.tick();  // the wheel event goes to JS first (it may preventDefault)
  for (int i = 0; i < 40; i++) t.tickNative();
  jsui::Node* list = t.node("list");
  CHECK(list && list->scrollY > 0);
  if (list) CHECK_NEAR(t.rect("badge").y, 92 - list->scrollY, 0.01);

  // ---- anchored by ref near the bottom edge: flipped above its anchor.
  jsui::Rect flip = t.rect("flip");
  CHECK_NEAR(flip.x, 290, 0.01);
  CHECK_NEAR(flip.y, 270 - 4 - 40, 0.01);

  // Hover, presses aside, scrolling: no re-render for any of it.
  CHECK_EQ(t.renders(), renders0);
}
