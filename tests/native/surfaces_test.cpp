// Surfaces (jsui_surface_create) without React: the runtime <-> JS protocol
// (__jsui_runSurface / __jsui_stopSurface, createNode(type, rootTag)), node
// ids unique across surfaces, nodes that cannot cross surfaces, per-surface
// viewports and resize events, auto-sized axes, and a surface destroyed from
// inside a JS callback.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/painter.h"
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
globalThis.__jsui_runSurface = (tag, appKey, propsJson) => {
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
  JsuiRuntime* b = jsui_surface_create(h.rt, "B", R"({"color":"#0000ff"})", 30, 30, 1);
  std::string tagB = std::to_string(jsui_surface_tag(b));
  CHECK(tagA != tagB);
  CHECK(has(h.take(), "run={\"tag\":" + tagB));

  jsui::Runtime* rt = jsui::internalRuntime(h.rt);
  CHECK_EQ(rt->surfaces().size(), size_t(3));
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

  // Viewports are per surface; resize events carry the root tag.
  h.take();
  jsui_set_viewport(a, 40, 12, 2);
  CHECK(has(h.take(), "\"rootTag\":" + tagA));
  float w = 0, hh = 0;
  jsui_surface_get_size(a, &w, &hh);
  CHECK_NEAR(w, 40.f, 0.01f);
  CHECK_NEAR(hh, 12.f, 0.01f);
  jsui_surface_get_size(h.rt, &w, &hh);
  CHECK_NEAR(w, 64.f, 0.01f);

  // Props updates reuse the surface's tree.
  CHECK_EQ(jsui_surface_set_props(a, R"({"color":"#00ff00"})"), 0);
  CHECK(has(h.take(), "\"box\":" + std::to_string(boxA) + ",\"color\":\"#00ff00\""));
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
globalThis.__jsui_runSurface = (tag) => {
  const v = n.createNode('View', tag);
  n.setProps(v, { style: { width: 30, height: 12, backgroundColor: '#ffffff' } });
  n.appendChild(tag, v);
};
)JS";
  CHECK_EQ(jsui_load_bundle_source(h.rt, src, std::strlen(src), "auto.js"), 0);
  // Width from the content, height fixed; then both from the content.
  JsuiRuntime* s = jsui_surface_create(h.rt, "X", nullptr, 0, 20, 1);
  h.tick();
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

// Two surfaces of one runtime repaint incrementally while changes land in
// one, the other or both: the painter skips the unchanged subtrees of each
// document (its own passes, records and damage), and every frame equals the
// same runtime's frame painted with every node walked.
TEST(surfaces_incremental_frames_with_record_skipping) {
  const char* src = R"JS(
const n = __jsui_native;
const dots = {};
globalThis.__jsui_runSurface = (tag, appKey) => {
  const root = n.createNode('View', tag);
  n.setProps(root, { style: { flex: 1, padding: 4, backgroundColor: '#102030' } });
  n.appendChild(tag, root);
  dots[appKey] = [];
  for (let i = 0; i < 6; i++) {
    const row = n.createNode('View', tag);
    n.setProps(row, { style: { height: 8, marginBottom: 2, borderRadius: 3, backgroundColor: '#406080' } });
    n.appendChild(root, row);
    const dot = n.createNode('View', tag);
    n.setProps(dot, { style: { width: 6, height: 6, margin: 1, borderRadius: 3, backgroundColor: '#ffffff' } });
    n.appendChild(row, dot);
    dots[appKey].push(dot);
  }
};
globalThis.__jsui_onHostMessage = (name, json) => {
  const { i, x, c } = JSON.parse(json);
  n.setProps(dots[name][i], { style: { width: 6, height: 6, margin: 1, marginLeft: x, borderRadius: 3,
    backgroundColor: c } });
};
)JS";
  struct Run {
    SurfaceHost h;
    JsuiRuntime* s[2];
    std::vector<uint32_t> px[2];
  } runs[2];  // [0] skips unchanged subtrees, [1] walks every node
  const int W = 40, H = 70;
  for (Run& r : runs) {
    r.s[0] = jsui_surface_create(r.h.rt, "A", nullptr, W, H, 1);
    r.s[1] = jsui_surface_create(r.h.rt, "B", nullptr, W, H, 1);
    CHECK_EQ(jsui_load_bundle_source(r.h.rt, src, std::strlen(src), "rows.js"), 0);
    for (auto& p : r.px) p.assign(size_t(W) * H, 0);
  }
  int skipped = 0, idle = 0, mismatches = 0;
  const char* colors[] = {"#ffffff", "#ff8040", "#40ff80"};
  for (int step = 0; step < 24; step++) {
    // A changes on even steps, B every third, both sometimes, neither on others.
    for (Run& r : runs) {
      char msg[96];
      if (step % 2 == 0) {
        std::snprintf(msg, sizeof msg, R"({"i":%d,"x":%d,"c":"%s"})", step % 6, 1 + step % 5, colors[step % 3]);
        jsui_post_message(r.h.rt, "A", msg);
      }
      if (step % 3 == 0) {
        std::snprintf(msg, sizeof msg, R"({"i":%d,"x":%d,"c":"%s"})", (step / 3) % 6, 1 + step % 4, colors[step % 2]);
        jsui_post_message(r.h.rt, "B", msg);
      }
      r.h.tick();
    }
    for (int k = 0; k < 2; k++) {
      bool dirty = jsui_needs_redraw(runs[0].s[k]);
      if (step > 0 && !dirty) idle++;
      for (int v = 0; v < 2; v++) {
        jsui::setRecordSkipping(v == 0);
        jsui_render_rgba_incremental(runs[v].s[k], runs[v].px[k].data(), W, H, size_t(W) * 4, nullptr);
      }
      jsui::setRecordSkipping(true);
      if (runs[0].px[k] != runs[1].px[k]) mismatches++;
      jsui::Surface* sf = jsui::internalRuntime(runs[0].h.rt)->surfaceByTag(uint32_t(jsui_surface_tag(runs[0].s[k])));
      // 14 nodes per surface; the rows that did not change lately are skipped.
      if (step > 1 && dirty && sf && jsui::lastPaintStats(sf->doc).nodesUpdated < 14) skipped++;
    }
  }
  std::printf("  %d frames skipped subtrees, %d idle surface frames, %d mismatches\n", skipped, idle, mismatches);
  CHECK_EQ(mismatches, 0);
  CHECK(skipped > 10);
  CHECK(idle > 5);
}
