// Performance regression guard: the work a frame does, counted rather than
// timed (Document::perf counters, src/core/perf.h), so the checks are exact
// and machine independent. Each test pins down one "this must not scale
// with the tree" property that tools/jsui-bench measures in microseconds.
// Heap allocations are counted too (operator new is replaced below).
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "animated/native_animated.h"
#include "core/document.h"
#include "core/value.h"
#include "host_buffers.h"
#include "input/input.h"
#include "jsui/jsui.h"
#include "render/painter.h"
#include "runtime.h"
#include "test.h"

using namespace jsui;

namespace {
// operator new calls made by this thread while tCountAllocs is set.
thread_local bool tCountAllocs = false;
thread_local uint64_t tAllocs = 0;
}  // namespace

void* operator new(size_t n) {
  if (tCountAllocs) tAllocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr int kRows = 2000;

// A 2000-row list (row: icon + three texts) under a header with a counter,
// in a clipping container, like tools/jsui-bench/scenarios/list.jsx.
struct ListFixture {
  JsuiConfig cfg;
  std::unique_ptr<Runtime> rt;
  Node* list = nullptr;
  Node* counter = nullptr;  // RawText of the header counter
  std::vector<Node*> rows;
  std::vector<Node*> icons;
  double now = 0;

  ListFixture() {
    jsui_config_init(&cfg);
    cfg.width = 1280;
    cfg.height = 720;
    rt = std::make_unique<Runtime>(cfg, nullptr);
    Node* root = doc().root();
    root->setProps(Value::parseJson(R"({"style":{"flex":1}})"));
    Node* header = add(root, NodeType::View, R"({"style":{"flexDirection":"row","padding":12}})");
    text(header, R"({"style":{"color":"#fff","fontSize":18}})", "Inventory log");
    counter = text(header, R"({"style":{"color":"#f2c35b","fontSize":16}})", "Frame 0");
    list = add(root, NodeType::View, R"({"style":{"flex":1,"overflow":"hidden"}})");
    for (int i = 0; i < kRows; i++) {
      Node* row = add(list, NodeType::View,
                      R"({"style":{"flexDirection":"row","alignItems":"center","height":28,"paddingHorizontal":12,"gap":10}})");
      icons.push_back(add(row, NodeType::View, R"({"style":{"width":18,"height":18,"borderRadius":4,"backgroundColor":"#2a3150"}})"));
      text(row, R"({"style":{"flex":1,"color":"#dde","fontSize":13}})", "Item " + std::to_string(i));
      text(row, R"({"style":{"width":50,"color":"#99a","fontSize":12}})", "Lv " + std::to_string(1 + i % 60));
      text(row, R"({"style":{"width":60,"color":"#f2c35b","fontSize":12}})", std::to_string(i * 37 % 1000) + " g");
      rows.push_back(row);
    }
    frame();
  }

  Document& doc() { return rt->doc; }
  InputManager& in() { return *rt->input; }
  PerfCounters& counters() { return doc().perf.counters; }
  Node* add(Node* parent, NodeType type, const char* props) {
    Node* n = doc().createNode(type);
    n->setProps(Value::parseJson(props));
    parent->appendChild(n);
    return n;
  }
  Node* text(Node* parent, const char* props, const std::string& s) {
    Node* t = add(parent, NodeType::Text, props);
    Node* raw = doc().createNode(NodeType::RawText);
    raw->setText(s);
    t->appendChild(raw);
    return raw;
  }
  Rect extra_rect(Node* n) { return n->absoluteRect(); }
  void frame() {
    now += 1000.0 / 60;
    rt->tick(now);
  }
  // Counters of one frame after `change` (run before the frame).
  template <class F>
  PerfCounters measure(F&& change) {
    counters() = {};
    change();
    frame();
    return counters();
  }
};

}  // namespace

TEST(perf_idle_frame_does_no_work) {
  ListFixture f;
  f.frame();
  PerfCounters c = f.measure([] {});
  CHECK_EQ(c.layoutPasses, 0u);
  CHECK_EQ(c.setProps, 0u);
  CHECK_EQ(c.styleResolves, 0u);
  CHECK_EQ(c.textMeasures, 0u);
  CHECK_EQ(c.paragraphBuilds, 0u);
  CHECK_EQ(c.hitNodes, 0u);  // no mouse
}

TEST(perf_one_text_change_measures_one_paragraph) {
  ListFixture f;
  int n = 0;
  for (int i = 0; i < 3; i++) {
    PerfCounters c = f.measure([&] { f.counter->setText("Frame " + std::to_string(++n)); });
    CHECK_EQ(c.layoutPasses, 1u);
    // The changed paragraph is shaped once and measured at most twice; the
    // 6000 unchanged texts come from Yoga's cache.
    CHECK_EQ(c.paragraphBuilds, 1u);
    CHECK(c.textMeasures <= 2);
    CHECK_EQ(c.styleResolves, 0u);
  }
}

// The post-layout walk and the painter's damage walk (updateDamage) visit
// the paths to what changed, not the 20000 nodes of the list: the counter's
// relayout takes the list's layout from Yoga's cache, which must neither
// walk its rows nor mark it for the painter (a marked list has the painter
// check each of its 2000 rows).
TEST(perf_one_text_change_walks_only_its_path) {
  ListFixture f;
  updateDamage(f.doc(), 1280, 720);  // the first records: every node
  CHECK(lastPaintStats(f.doc()).nodesUpdated > kRows);
  updateDamage(f.doc(), 1280, 720);  // unchanged since: settled (quiet)
  int n = 0;
  for (int i = 0; i < 3; i++) {
    PerfCounters c = f.measure([&] { f.counter->setText("Frame " + std::to_string(++n)); });
    CHECK(c.layoutVisits <= 8);
    updateDamage(f.doc(), 1280, 720);
    CHECK(lastPaintStats(f.doc()).nodesUpdated <= 6);
    CHECK(f.list->paintTreeDirty() == false);
  }
  // A change inside one row walks that row's path; a row inserted at the
  // top moves every row below it (all of them are laid out and placed
  // again, which only a walk of the list finds).
  for (int i = 0; i < 2; i++) {
    PerfCounters c = f.measure([&] {
      Value::Object partial;
      partial["backgroundColor"] = Value(i ? "#6a4aa0" : "#4a5170");
      CHECK(f.icons[5]->patchStyle(partial));
    });
    CHECK_EQ(c.layoutVisits, 0u);
    updateDamage(f.doc(), 1280, 720);
    CHECK(lastPaintStats(f.doc()).nodesUpdated <= 6);
  }
  Node* extra = f.doc().createNode(NodeType::View);
  extra->setProps(Value::parseJson(R"({"style":{"height":28}})"));
  PerfCounters c = f.measure([&] { f.list->insertBefore(extra, f.rows[0]); });
  CHECK(c.layoutVisits > uint64_t(kRows));
  CHECK(c.layoutVisits < uint64_t(kRows) * 2);  // the rows, not their children
  updateDamage(f.doc(), 1280, 720);
  CHECK(lastPaintStats(f.doc()).nodesUpdated > kRows);
}

TEST(perf_paint_only_style_changes_skip_layout) {
  ListFixture f;
  for (int round = 0; round < 2; round++) {
    PerfCounters c = f.measure([&] {
      for (int i = 0; i < 100; i++) {
        Value::Object partial;
        partial["opacity"] = Value(0.5 + 0.1 * round);
        partial["transform"] = Value::parseJson("[{\"translateX\":" + std::to_string(round + 1) + "}]");
        CHECK(f.icons[size_t(i)]->patchStyle(partial));
      }
      // Through setProps too (a React commit of a paint-only change).
      f.rows[0]->setProps(Value::parseJson(
          std::string(R"({"style":{"flexDirection":"row","alignItems":"center","height":28,"paddingHorizontal":12,"gap":10,"backgroundColor":")") +
          (round ? "#111" : "#222") + "\"}}"));
    });
    CHECK_EQ(c.styleResolves, 101u);
    CHECK_EQ(c.layoutPasses, 0u);
    CHECK_EQ(c.textMeasures, 0u);
  }
}

TEST(perf_hit_test_skips_rows_away_from_the_pointer) {
  ListFixture f;
  f.in().hitTest(200, 300);  // first hit computes the cached row bounds
  for (float y : {80.f, 300.f, 700.f}) {
    f.counters() = {};
    Node* hit = f.in().hitTest(200, y);
    CHECK(hit != nullptr);
    // Root, header (+ its 2 texts), list, every row once, and the children
    // of the one row under the pointer: not every node of the tree.
    CHECK(f.counters().hitNodes <= uint64_t(kRows) + 16);
  }
  // Still exact after rows move (a text change relayouts the header only;
  // inserting a row shifts every row below it).
  Node* extra = f.doc().createNode(NodeType::View);
  extra->setProps(Value::parseJson(R"({"style":{"height":28}})"));
  f.list->insertBefore(extra, f.rows[0]);
  f.frame();
  Rect e = f.extra_rect(extra);
  CHECK_EQ(f.in().hitTest(e.x + 5, e.y + 5), extra);
  Rect ic = f.extra_rect(f.icons[0]);
  CHECK_EQ(f.in().hitTest(ic.x + 9, ic.y + 9), f.icons[0]);
  f.counters() = {};
  CHECK(f.in().hitTest(20, 300) != nullptr);
  CHECK(f.counters().hitNodes <= uint64_t(kRows) + 17);
}

// jsui_get_perf: counters without profiling, phase times with it, reset.
TEST(perf_capi_counters_and_phases) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 64;
  cfg.height = 64;
  JsuiRuntime* rt = jsui_create(&cfg);
  // A 1x1 PNG data: URI, decoded when the image is first needed.
  const char* bundle =
      "const n = __jsui_native; const img = n.createNode('Image');"
      "n.setProps(img, {source: {uri: 'data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='},"
      "  style: {width: 32, height: 32}});"
      "n.appendChild(n.rootId, img);";
  CHECK_EQ(jsui_load_bundle_source(rt, bundle, std::strlen(bundle), "perf.js"), 0);
  std::vector<uint8_t> px(64 * 64 * 4);
  jsui_tick(rt, 16);
  jsui_render_rgba(rt, px.data(), 64, 64, 64 * 4);
  JsuiPerf p;
  jsui_get_perf(rt, &p, 1);
  CHECK_EQ(p.image_decodes, 1ull);
  CHECK(p.image_decode_us > 0);
  CHECK(p.bridge_calls >= 3);
  CHECK_EQ(p.paint_us, 0.0);  // profiling is off: no phase times
  CHECK_EQ(p.pipeline_compiles, 0ull);
  // The reset started new totals.
  jsui_get_perf(rt, &p, 0);
  CHECK_EQ(p.image_decodes, 0ull);
  CHECK_EQ(p.bridge_calls, 0ull);

  jsui_set_profiling(rt, 1);
  const char* more =
      "const v = __jsui_native.createNode('View');"
      "__jsui_native.setProps(v, {style: {width: 10, height: 10, backgroundColor: 'red'}});"
      "__jsui_native.appendChild(__jsui_native.rootId, v);";
  CHECK_EQ(jsui_load_bundle_source(rt, more, std::strlen(more), "more.js"), 0);
  jsui_tick(rt, 32);
  jsui_render_rgba(rt, px.data(), 64, 64, 64 * 4);
  jsui_get_perf(rt, &p, 1);
  CHECK(p.js_us > 0);
  CHECK(p.commit_us > 0);
  CHECK(p.paint_us > 0);
  CHECK(p.layout_us > 0);
  CHECK_EQ(p.gpu_submit_us, 0.0);  // a CPU render
  jsui_destroy(rt);
}

// Native Animated writing views every frame (tools/jsui-bench markers-native:
// Animated.hostBuffer values through interpolations into transforms, plus an
// opacity and a color): each write re-resolves only the written groups,
// never relayouts, and in steady state the whole flush allocates nothing
// (no partial style objects, no hash-set nodes, no strings).
TEST(perf_native_animated_writes_allocate_nothing) {
  constexpr int kMarkers = 50;
  Document doc;
  doc.setViewport(800, 600, 1);
  HostBuffers buffers;
  NativeAnimated na(doc);
  na.buffers = &buffers;
  std::vector<Node*> markers, fills;
  auto create = [&](int tag, const std::string& config) { CHECK(na.createNode(tag, Value::parseJson(config))); };
  for (int i = 0; i < kMarkers; i++) {
    Node* marker = doc.createNode(NodeType::View);
    marker->setProps(Value::parseJson(
        R"({"style":{"position":"absolute","width":48,"height":14,"padding":3,"borderRadius":3,"backgroundColor":"#0d1322cc","transform":[{"translateX":0},{"translateY":0}]}})"));
    doc.root()->appendChild(marker);
    Node* fill = doc.createNode(NodeType::View);
    fill->setProps(Value::parseJson(R"({"style":{"flex":1,"backgroundColor":"#ef476f"}})"));
    marker->appendChild(fill);
    markers.push_back(marker);
    fills.push_back(fill);
    int t = 100 + i * 20;
    for (int k = 0; k < 3; k++) {
      create(t + k, R"({"type":"hostBuffer","key":"enemies","index":)" + std::to_string(i * 3 + k) + "}");
    }
    create(t + 3, R"({"type":"interpolation","inputRange":[0,100],"outputs":[[0,1]]})");
    create(t + 4, R"({"type":"interpolation","inputRange":[0,100],"outputType":"color","outputs":[[255,0],[0,200],[0,0],[1,0.5]]})");
    na.connectNodes(t + 2, t + 3);
    na.connectNodes(t + 2, t + 4);
    create(t + 5, R"({"type":"transform","transforms":[{"property":"translateX","nodeTag":)" + std::to_string(t) +
                      R"(},{"property":"translateY","nodeTag":)" + std::to_string(t + 1) + "}]}");
    na.connectNodes(t, t + 5);
    na.connectNodes(t + 1, t + 5);
    create(t + 6, R"({"type":"style","style":{"transform":)" + std::to_string(t + 5) + "}}");
    na.connectNodes(t + 5, t + 6);
    create(t + 7, R"({"type":"props","props":{"style":)" + std::to_string(t + 6) + "}}");
    na.connectNodes(t + 6, t + 7);
    na.connectToView(t + 7, marker->id());
    create(t + 8, R"({"type":"transform","transforms":[{"property":"scaleX","nodeTag":)" + std::to_string(t + 3) + "}]}");
    na.connectNodes(t + 3, t + 8);
    create(t + 9, R"({"type":"style","style":{"transform":)" + std::to_string(t + 8) + R"(,"opacity":)" +
                      std::to_string(t + 3) + R"(,"backgroundColor":)" + std::to_string(t + 4) + "}}");
    na.connectNodes(t + 8, t + 9);
    na.connectNodes(t + 3, t + 9);
    na.connectNodes(t + 4, t + 9);
    create(t + 10, R"({"type":"props","props":{"style":)" + std::to_string(t + 9) + "}}");
    na.connectNodes(t + 9, t + 10);
    na.connectToView(t + 10, fill->id());
  }
  doc.layout();
  std::vector<float> data(kMarkers * 3);
  auto frame = [&](int f) {
    for (int i = 0; i < kMarkers; i++) {
      data[size_t(i * 3)] = float(i * 10 + f % 7);
      data[size_t(i * 3 + 1)] = float(i * 5 + f % 11);
      data[size_t(i * 3 + 2)] = float(50 + 40 * std::sin(f * 0.37 + i));
    }
    buffers.set("enemies", JSUI_BUFFER_F32, data.data(), data.size());
    doc.perf.counters = {};
    tAllocs = 0;
    tCountAllocs = true;
    na.bufferChanged("enemies");
    na.tick(f * 16.0);
    tCountAllocs = false;
    return doc.perf.counters;
  };
  for (int f = 0; f < 40; f++) frame(f);  // warm up: every value range seen
  for (int f = 40; f < 100; f++) {
    PerfCounters c = frame(f);
    CHECK_EQ(c.animatedWrites, uint64_t(2 * kMarkers));
    CHECK_EQ(c.styleResolves, uint64_t(2 * kMarkers));
    CHECK_EQ(tAllocs, uint64_t(0));
  }
  CHECK(!doc.layout());  // paint groups only
  // The views' styles hold the live values next to React's keys, and the
  // computed styles match them.
  const Value& ms = markers[3]->props()["style"];
  CHECK_EQ(ms["transform"].toJson(), std::string(R"([{"translateX":31},{"translateY":15}])"));  // frame 99
  CHECK_EQ(ms["borderRadius"].asNumber(), 3.0);
  CHECK_EQ(markers[3]->style().transform.size(), size_t(2));
  CHECK_EQ(markers[3]->style().transform[0].a, 31.f);
  CHECK_EQ(markers[3]->style().transform[1].b, 15.f);
  const Value& fs = fills[3]->props()["style"];
  CHECK_EQ(fs["transform"].asArray().at(0)["scaleX"].asNumber(), fs["opacity"].asNumber());
  CHECK_EQ(double(fills[3]->style().opacity), double(float(fs["opacity"].asNumber())));
  CHECK(fs["backgroundColor"].asString().rfind("rgba(", 0) == 0);
  CHECK_EQ(fs["flex"].asNumber(), 1.0);
  // An unchanged frame re-resolves and repaints nothing.
  uint32_t version = fills[3]->paintVersion;
  PerfCounters same = frame(99);
  CHECK_EQ(same.styleResolves, uint64_t(0));
  CHECK_EQ(fills[3]->paintVersion, version);
  CHECK_EQ(tAllocs, uint64_t(0));
}
