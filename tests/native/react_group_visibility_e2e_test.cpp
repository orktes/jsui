// End-to-end group state, Visibility and host-state bindings: a real
// @orktes/jsui-react bundle (packages/cli/test/e2e/group-visibility.jsx)
// driven through the C API, with jsui_tick_native only (no JS) wherever the
// runtime alone should react.
// - A hover popout: Visibility visible={useGroupState().instant.hovered}
//   with a fade, removed from layout while hidden; useGroupState(name)
//   reaches an outer named group.
// - useHostStateBind: jsui_set_state moves a native layout style, sets
//   Animated.Text and toggles a derived Visibility on the next native tick.
// - Visibility visible={bool} with mode="hide".
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_GROUP_VISIBILITY_BUNDLE.
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

struct VisE2E {
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
      static_cast<VisE2E*>(u)->messages.emplace_back(name, json ? json : "");
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
  ~VisE2E() {
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
  std::string text(const char* name) {
    jsui::Node* n = node(name);
    if (!n) return "<none>";
    for (jsui::Node* c : n->children())
      if (c->type() == jsui::NodeType::RawText) return c->text();
    return "<no text>";
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

TEST(react_group_visibility_e2e) {
  const char* path = std::getenv("JSUI_REACT_GROUP_VISIBILITY_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_GROUP_VISIBILITY_BUNDLE, see packages/cli/test)\n");
    return;
  }
  VisE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  int renders0 = t.renders();

  // ---- the popout: hidden (removed) until the card is hovered.
  CHECK_EQ(t.style("tip", "display").asString(), std::string("none"));
  CHECK_NEAR(t.style("tip", "opacity").asNumber(-1), 0, 1e-9);
  jsui_send_mouse_move(t.rt, 50, 40);
  t.tickNative();
  CHECK(t.style("tip", "display").isNull());  // in layout from the first frame of the fade
  CHECK_NEAR(t.style("tip", "opacity").asNumber(-1), 0.16, 1e-6);
  for (int f = 0; f < 7; f++) t.tickNative();
  CHECK(t.style("tip", "opacity").isNull());  // fully shown: back to the base (none)
  CHECK(t.node("tip")->layoutRect().width > 0);
  jsui_send_mouse_move(t.rt, 300, 250);
  t.tickNative();
  CHECK_NEAR(t.style("tip", "opacity").asNumber(-1), 0.84, 1e-6);
  CHECK(t.style("tip", "display").isNull());  // still fading out
  for (int f = 0; f < 7; f++) t.tickNative();
  CHECK_EQ(t.style("tip", "display").asString(), std::string("none"));

  // ---- useGroupState('slot') from inside the card's group.
  CHECK_NEAR(t.style("named", "opacity").asNumber(-1), 0, 1e-9);
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 1, 0);
  t.tickNative();
  CHECK_NEAR(t.style("named", "opacity").asNumber(-1), 1, 1e-9);
  jsui_send_mouse_button(t.rt, 50, 40, JSUI_MOUSE_LEFT, 0, 0);
  t.tickNative();
  CHECK_NEAR(t.style("named", "opacity").asNumber(-1), 0, 1e-9);

  // ---- host state bound natively: no JS between the set and the frame.
  CHECK_EQ(t.style("low", "display").asString(), std::string("none"));
  jsui_set_state(t.rt, "player", R"({"name":"Ada","health":{"current":80,"max":100}})");
  t.tickNative();
  CHECK_NEAR(t.style("bar", "width").asNumber(-1), 80, 1e-9);
  CHECK_NEAR(t.node("bar")->layoutRect().width, 80, 1e-3);
  CHECK_EQ(t.text("name"), std::string("Ada"));
  CHECK_EQ(t.style("low", "display").asString(), std::string("none"));
  jsui_patch_state(t.rt, "player", R"({"health":{"current":20}})");
  t.tickNative();
  CHECK_NEAR(t.style("bar", "width").asNumber(-1), 20, 1e-9);
  CHECK(t.style("low", "display").isNull());  // no transition: at once
  jsui_patch_state(t.rt, "player", R"({"health":{"current":50}})");
  t.tickNative();
  CHECK_EQ(t.style("low", "display").asString(), std::string("none"));
  t.tick();

  // ---- visible={bool}, mode hide: keeps layout, no hits, opacity 0.
  CHECK_EQ(t.style("static", "pointerEvents").asString(), std::string("none"));
  CHECK_NEAR(t.style("static", "opacity").asNumber(-1), 0, 1e-9);
  CHECK(t.style("static", "display").isNull());
  t.post("show");
  t.tick();
  CHECK(t.style("static", "pointerEvents").isNull());
  CHECK(t.style("static", "opacity").isNull());

  CHECK_EQ(t.renders(), renders0 + 1);  // only the "show" re-render
}
