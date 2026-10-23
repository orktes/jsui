// Surfaces (jsui_surface_create) without React: the runtime <-> JS protocol
// (__jsui_runSurfaces / __jsui_stopSurface, createNode(type, rootTag)), node
// ids unique across surfaces, nodes that cannot cross surfaces, per-surface
// viewports and resize events, starts / props / resizes batched per tick and
// unchanged props skipped, auto-sized axes, and a surface destroyed from
// inside a JS callback.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct SurfaceHost {
  JsuiRuntime* rt = nullptr;
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* destroyOnMessage = nullptr;
  double t = 0;

  SurfaceHost() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 64;
    cfg.height = 64;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      auto* h = static_cast<SurfaceHost*>(u);
      h->messages.emplace_back(name, json);
      if (std::string(name) == "destroy" && h->destroyOnMessage) {
        jsui_surface_destroy(h->destroyOnMessage);
        h->destroyOnMessage = nullptr;
      }
    };
    cfg.message_user = this;
    cfg.log = [](void*, JsuiLogLevel, const char*) {};
    rt = jsui_create(&cfg);
  }
  ~SurfaceHost() { jsui_destroy(rt); }
  void tick(int n = 1) {
    for (int i = 0; i < n; i++) jsui_tick(rt, t += 16);
  }
  std::string take() {
    std::string s;
    for (auto& [n, j] : messages) s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
};

// A minimal AppRegistry: each surface gets a box filling it, colored by its
// props; resize events and stops are posted back.
const char* kBundle = R"JS(
const n = __jsui_native;
const boxes = (globalThis.boxes = {});
globalThis.__jsui_runSurfaces = (list) => {
  n.postMessage('batch', list.length);
  for (const [tag, appKey, propsJson] of list) run(tag, appKey, propsJson);
};
const run = (tag, appKey, propsJson) => {
  const props = JSON.parse(propsJson);
  let box = boxes[tag];
  if (!box) {
    box = n.createNode('View', tag);
    n.appendChild(tag, box);
    boxes[tag] = box;
  }
  n.setProps(box, { style: { flex: 1, backgroundColor: props.color } });
  n.postMessage('run', { tag, appKey, box, color: props.color });
};
globalThis.__jsui_stopSurface = (tag) => {
  n.removeChild(tag, boxes[tag]);
  n.destroyNode(boxes[tag]);
  delete boxes[tag];
  n.postMessage('stop', tag);
};
globalThis.__jsui_dispatchEvent = (e) => {
  if (e.type === 'resize') n.postMessage('resize', e);
};
globalThis.crossSurface = (a, b) => n.appendChild(boxes[a], boxes[b]);
)JS";

uint32_t pixel(JsuiRuntime* s, int w, int h, int x, int y) {
  std::vector<uint32_t> px(size_t(w) * h);
  jsui_render_rgba(s, px.data(), w, h, size_t(w) * 4);
  return px[size_t(y) * w + x];
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST(surfaces_protocol) {
  SurfaceHost h;
  JsuiRuntime* a = jsui_surface_create(h.rt, "A", R"({"color":"#ff0000"})", 20, 10, 1);
  CHECK(a != nullptr);
  CHECK_EQ(jsui_load_bundle_source(h.rt, kBundle, std::strlen(kBundle), "surfaces.js"), 0);
  std::string tagA = std::to_string(jsui_surface_tag(a));
  CHECK(has(h.take(), "run={\"tag\":" + tagA + ",\"appKey\":\"A\""));
  // Surfaces created after load start on the next tick, in one batch.
  JsuiRuntime* b = jsui_surface_create(h.rt, "B", R"({"color":"#0000ff"})", 30, 30, 1);
  JsuiRuntime* c = jsui_surface_create(h.rt, "C", R"({"color":"#00ff00"})", 30, 30, 1);
  std::string tagB = std::to_string(jsui_surface_tag(b));
  CHECK(tagA != tagB);
  CHECK(h.take().empty());
  h.tick();
  std::string m = h.take();
  CHECK(has(m, "batch=2 run={\"tag\":" + tagB));
  CHECK(has(m, "\"appKey\":\"C\""));
  jsui_surface_destroy(c);
  h.take();

  jsui::Runtime* rt = jsui::internalRuntime(h.rt);
  CHECK_EQ(rt->surfaces().size(), size_t(3));  // main, A, B
  // Node ids are unique across the documents, and each box lives in its surface's.
  jsui::Surface* sa = rt->surfaceByTag(uint32_t(jsui_surface_tag(a)));
  jsui::Surface* sb = rt->surfaceByTag(uint32_t(jsui_surface_tag(b)));
  CHECK(sa && sb);
  CHECK_EQ(sa->doc.root()->children().size(), size_t(1));
  CHECK_EQ(sb->doc.root()->children().size(), size_t(1));
  uint32_t boxA = sa->doc.root()->children()[0]->id(), boxB = sb->doc.root()->children()[0]->id();
  CHECK(boxA != boxB);
  CHECK(sa->doc.get(boxB) == nullptr);
  CHECK(rt->findNode(boxB) != nullptr);
  CHECK(rt->doc.root()->children().empty());

  // A node cannot move into another surface's tree.
  std::string cross = "crossSurface(" + tagA + ", " + tagB + ")";
  jsui_load_bundle_source(h.rt, cross.data(), cross.size(), "cross.js");
  CHECK_EQ(sb->doc.root()->children().size(), size_t(1));
  CHECK(sa->doc.root()->children()[0]->children().empty());

  // Pixels and redraw flags are per surface.
  h.tick();
  CHECK(jsui_needs_redraw(a));
  CHECK_EQ(pixel(a, 20, 10, 10, 5), 0xff0000ffu);
  CHECK(!jsui_needs_redraw(a));
  CHECK(jsui_needs_redraw(b));
  CHECK_EQ(pixel(b, 30, 30, 15, 15), 0xffff0000u);
  CHECK_EQ(pixel(h.rt, 64, 64, 32, 32), 0u);

  // Viewports are per surface; resize events carry the root tag and reach JS
  // on the next tick (the last size wins).
  h.take();
  jsui_set_viewport(a, 30, 12, 2);
  jsui_set_viewport(a, 40, 12, 2);
  CHECK(h.take().empty());
  h.tick();
  m = h.take();
  CHECK_EQ(m, "resize={\"height\":12,\"rootTag\":" + tagA + ",\"scale\":2,\"type\":\"resize\",\"width\":40}");
  float w = 0, hh = 0;
  jsui_surface_get_size(a, &w, &hh);
  CHECK_NEAR(w, 40.f, 0.01f);
  CHECK_NEAR(hh, 12.f, 0.01f);
  jsui_surface_get_size(h.rt, &w, &hh);
  CHECK_NEAR(w, 64.f, 0.01f);

  // Props updates reuse the surface's tree; they apply on the next tick, the
  // last one wins, and the same props again are not delivered.
  CHECK_EQ(jsui_surface_set_props(a, R"({"color":"#ffff00"})"), 0);
  CHECK_EQ(jsui_surface_set_props(a, R"({"color":"#00ff00"})"), 0);
  CHECK_EQ(jsui_surface_set_props(b, R"({"color":"#0000ff"})"), 0);  // unchanged
  CHECK(h.take().empty());
  CHECK(jsui_wants_frame(h.rt));  // hosts that sleep when idle tick to deliver them
  h.tick();
  CHECK(!jsui_wants_frame(h.rt));
  m = h.take();
  CHECK(has(m, "batch=1 run={\"tag\":" + tagA + ",\"appKey\":\"A\",\"box\":" + std::to_string(boxA) +
                   ",\"color\":\"#00ff00\""));
  CHECK(!has(m, "ffff00"));
  CHECK_EQ(jsui_surface_set_props(a, R"({"color":"#00ff00"})"), 0);
  h.tick();
  CHECK(h.take().empty());
  CHECK_EQ(jsui_surface_set_props(a, "[1]"), -1);
  CHECK_EQ(pixel(a, 80, 24, 40, 12), 0xff00ff00u);

  // Destroy from inside a JS callback: unmounted now, freed on the next tick.
  h.destroyOnMessage = b;
  const char* destroy = "__jsui_native.postMessage('destroy', null)";
  jsui_load_bundle_source(h.rt, destroy, std::strlen(destroy), "destroy.js");
  CHECK(has(h.take(), "stop=" + tagB));
  CHECK_EQ(rt->surfaces().size(), size_t(2));
  CHECK(rt->findNode(boxB) == nullptr);
  h.tick();
  CHECK(jsui_last_error(h.rt) == nullptr);

  jsui_surface_destroy(a);
  CHECK(has(h.take(), "stop=" + tagA));
  CHECK_EQ(rt->surfaces().size(), size_t(1));
}

TEST(surfaces_auto_size) {
  SurfaceHost h;
  const char* src = R"JS(
const n = __jsui_native;
globalThis.__jsui_runSurfaces = (list) => list.forEach(([tag]) => {
  const v = n.createNode('View', tag);
  n.setProps(v, { style: { width: 30, height: 12, backgroundColor: '#ffffff' } });
  n.appendChild(tag, v);
});
)JS";
  CHECK_EQ(jsui_load_bundle_source(h.rt, src, std::strlen(src), "auto.js"), 0);
  // Width from the content, height fixed; then both from the content.
  JsuiRuntime* s = jsui_surface_create(h.rt, "X", nullptr, 0, 20, 1);
  h.tick(2);
  float w = 0, hh = 0;
  jsui_surface_get_size(s, &w, &hh);
  CHECK_NEAR(w, 30.f, 0.01f);
  CHECK_NEAR(hh, 20.f, 0.01f);
  jsui_set_viewport(s, 0, 0, 1);
  jsui_surface_get_size(s, &w, &hh);
  CHECK_NEAR(w, 30.f, 0.01f);
  CHECK_NEAR(hh, 12.f, 0.01f);
  CHECK_EQ(pixel(s, 30, 12, 29, 11), 0xffffffffu);
  jsui_set_viewport(s, 50, 50, 1);
  jsui_surface_get_size(s, &w, &hh);
  CHECK_NEAR(w, 50.f, 0.01f);
  CHECK_NEAR(hh, 50.f, 0.01f);
  // Freed with the runtime.
}
