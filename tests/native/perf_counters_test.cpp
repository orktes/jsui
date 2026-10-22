// Performance regression guard: the work a frame does, counted rather than
// timed (Document::perf counters, src/core/perf.h), so the checks are exact
// and machine independent. Each test pins down one "this must not scale
// with the tree" property that tools/jsui-bench measures in microseconds.
#include <memory>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "input/input.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using namespace jsui;

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
