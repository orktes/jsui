// InputManager: hit testing, pointer routing, focus and gamepad navigation.
//
// Documents are built directly. Layout geometry is set on the Yoga nodes and
// paint/hit styles on ComputedStyle (through a test-only const_cast) so the
// tests don't depend on the style parser.
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "input/input.h"
#include "js/engine.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using namespace jsui;

namespace {

struct Fixture {
  JsuiConfig cfg;
  std::vector<std::string> logs;
  std::unique_ptr<Runtime> rt;

  explicit Fixture(int w = 400, int h = 300) {
    jsui_config_init(&cfg);
    cfg.width = w;
    cfg.height = h;
    cfg.log = [](void* u, JsuiLogLevel, const char* m) { static_cast<Fixture*>(u)->logs.push_back(m); };
    cfg.log_user = this;
    rt = std::make_unique<Runtime>(cfg, nullptr);
    // Logs every event; returns true (handled / prevented) for types listed
    // in globalThis.prevent.
    js("globalThis.prevent = {}; globalThis.extra = null;"
       "globalThis.__jsui_dispatchEvent = e => {"
       "  console.log(e.type + ':' + e.target + (extra ? extra(e) : ''));"
       "  return !!prevent[e.type]; };");
    logs.clear();
  }

  void js(const std::string& src) { CHECK(rt->js->eval(src, "test.js")); }
  InputManager& in() { return *rt->input; }
  Document& doc() { return rt->doc; }

  Node* add(Node* parent, NodeType type, const char* props = "{}") {
    Node* n = doc().createNode(type);
    n->setProps(Value::parseJson(props));
    parent->appendChild(n);
    return n;
  }
  // Absolutely positioned box (relative to the parent's padding box).
  Node* box(Node* parent, float x, float y, float w, float h, const char* props = "{}",
            NodeType type = NodeType::View) {
    Node* n = add(parent, type, props);
    place(n, x, y, w, h);
    return n;
  }
  void place(Node* n, float x, float y, float w, float h) {
    YGNodeRef y_ = n->yoga();
    YGNodeStyleSetPositionType(y_, YGPositionTypeAbsolute);
    YGNodeStyleSetPosition(y_, YGEdgeLeft, x);
    YGNodeStyleSetPosition(y_, YGEdgeTop, y);
    YGNodeStyleSetWidth(y_, w);
    YGNodeStyleSetHeight(y_, h);
    doc().markLayoutDirty();
  }
  static ComputedStyle& style(Node* n) { return const_cast<ComputedStyle&>(n->style()); }
  void layout() {
    doc().markLayoutDirty();
    doc().layout();
  }
  // Space-joined log since the last call.
  std::string events() {
    std::string s;
    for (auto& l : logs) s += (s.empty() ? "" : " ") + l;
    logs.clear();
    return s;
  }
};

std::string ev(const char* type, Node* n) { return std::string(type) + ":" + std::to_string(n->id()); }
std::string ev(const char* type, Node* a, const char* type2, Node* b) { return ev(type, a) + " " + ev(type2, b); }

TransformOp op(TransformOp::Kind k, float a, float b = 0) {
  TransformOp t{k};
  t.a = a;
  t.b = b;
  return t;
}

}  // namespace

// ------------------------------------------------------------ hit testing

TEST(input_hit_paint_order_and_zindex) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 0, 0, 100, 100);
  Node* b = f.box(root, 50, 50, 100, 100);
  f.layout();
  CHECK_EQ(f.in().hitTest(75, 75), b);  // later sibling paints on top
  CHECK_EQ(f.in().hitTest(25, 25), a);
  CHECK_EQ(f.in().hitTest(300, 250), root);
  CHECK(f.in().hitTest(500, 10) == nullptr);  // outside the viewport
  Fixture::style(a).zIndex = 1;
  Fixture::style(a).hasZIndex = true;
  CHECK_EQ(f.in().hitTest(75, 75), a);
  // Negative zIndex sinks below zIndex-0 siblings; stable for equal values.
  Fixture::style(a).zIndex = -1;
  Node* c = f.box(root, 60, 60, 10, 10);
  f.layout();
  CHECK_EQ(f.in().hitTest(65, 65), c);
  CHECK_EQ(f.in().hitTest(55, 55), b);
}

TEST(input_hit_overflow_and_descendants) {
  Fixture f;
  Node* root = f.doc().root();
  Node* p = f.box(root, 10, 10, 50, 50);
  Node* child = f.box(p, 40, 40, 50, 50);  // pokes out of p
  f.layout();
  float lx, ly;
  CHECK_EQ(f.in().hitTest(80, 80, &lx, &ly), child);  // overflow visible: still hittable
  CHECK_NEAR(lx, 30, 1e-4);
  CHECK_NEAR(ly, 30, 1e-4);
  CHECK_EQ(f.in().hitTest(20, 20), p);
  Fixture::style(p).overflow = Overflow::Hidden;
  CHECK_EQ(f.in().hitTest(80, 80), root);  // clipped
  CHECK_EQ(f.in().hitTest(55, 55), child);
  // Opacity 0 is still hittable (web semantics); display none is not.
  Fixture::style(child).opacity = 0;
  CHECK_EQ(f.in().hitTest(55, 55), child);
  Fixture::style(child).display = false;
  CHECK_EQ(f.in().hitTest(55, 55), p);
}

TEST(input_hit_rounded_corners) {
  Fixture f;
  Node* root = f.doc().root();
  Node* circle = f.box(root, 0, 0, 100, 100);
  for (auto& r : Fixture::style(circle).borderRadius) r = {50, true};
  Node* inner = f.box(circle, 0, 0, 100, 100);
  f.layout();
  CHECK_EQ(f.in().hitTest(50, 50), inner);
  CHECK_EQ(f.in().hitTest(5, 5), inner);  // inner isn't rounded and circle doesn't clip
  Fixture::style(circle).overflow = Overflow::Hidden;
  CHECK_EQ(f.in().hitTest(5, 5), root);   // clipped by the rounded corner
  CHECK_EQ(f.in().hitTest(95, 95), root);
  CHECK_EQ(f.in().hitTest(15, 50), inner);
  // The rounded element itself also misses at its corners.
  Fixture::style(inner).display = false;
  CHECK_EQ(f.in().hitTest(50, 50), circle);
  CHECK_EQ(f.in().hitTest(4, 96), root);
  // Oversized radii are scaled down like CSS (400 on a 100 box == 50).
  for (auto& r : Fixture::style(circle).borderRadius) r = {400, false};
  CHECK_EQ(f.in().hitTest(5, 5), root);
  CHECK_EQ(f.in().hitTest(50, 2), circle);
}

TEST(input_hit_pointer_events) {
  Fixture f;
  Node* root = f.doc().root();
  Node* p = f.box(root, 0, 0, 100, 100);
  Node* c = f.box(p, 0, 0, 50, 50);
  f.layout();
  Fixture::style(p).pointerEvents = PointerEvents::BoxNone;
  CHECK_EQ(f.in().hitTest(25, 25), c);
  CHECK_EQ(f.in().hitTest(75, 75), root);
  Fixture::style(p).pointerEvents = PointerEvents::BoxOnly;
  CHECK_EQ(f.in().hitTest(25, 25), p);
  Fixture::style(p).pointerEvents = PointerEvents::None;
  CHECK_EQ(f.in().hitTest(25, 25), root);
  // Legacy prop form.
  Fixture::style(p).pointerEvents = PointerEvents::Auto;
  p->setProps(Value::parseJson(R"({"pointerEvents":"box-none"})"));
  f.place(p, 0, 0, 100, 100);
  f.layout();
  CHECK_EQ(f.in().hitTest(75, 75), root);
  CHECK_EQ(f.in().hitTest(25, 25), c);
}

TEST(input_hit_transforms) {
  Fixture f;
  Node* root = f.doc().root();
  Node* t = f.box(root, 100, 100, 100, 50);
  f.layout();
  float lx, ly;
  // translate(50px)
  Fixture::style(t).transform = {op(TransformOp::Kind::Translate, 50, 0)};
  CHECK_EQ(f.in().hitTest(120, 120), root);
  CHECK_EQ(f.in().hitTest(220, 120, &lx, &ly), t);
  CHECK_NEAR(lx, 70, 1e-3);
  CHECK_NEAR(ly, 20, 1e-3);
  // translateX(-50%): percent of own width.
  TransformOp pct = op(TransformOp::Kind::Translate, -50, 0);
  pct.percentA = true;
  Fixture::style(t).transform = {pct};
  CHECK_EQ(f.in().hitTest(60, 110), t);
  CHECK_EQ(f.in().hitTest(160, 110), root);
  // rotate(90deg) about the center (150, 125): becomes a 50x100 bar.
  Fixture::style(t).transform = {op(TransformOp::Kind::Rotate, 90)};
  CHECK_EQ(f.in().hitTest(150, 170, &lx, &ly), t);
  CHECK_NEAR(lx, 95, 1e-2);  // local x runs downwards after +90deg
  CHECK_NEAR(ly, 25, 1e-2);
  CHECK_EQ(f.in().hitTest(110, 110), root);
  // scale(2) about the center: 0..200 / 75..175 band.
  Fixture::style(t).transform = {op(TransformOp::Kind::Scale, 2, 2)};
  CHECK_EQ(f.in().hitTest(55, 80, &lx, &ly), t);
  CHECK_NEAR(lx, 2.5, 1e-3);
  CHECK_NEAR(ly, 2.5, 1e-3);
  // transformOrigin top-left + scale: grows right/down only.
  Fixture::style(t).transformOrigin[0] = {0, false};
  Fixture::style(t).transformOrigin[1] = {0, false};
  CHECK_EQ(f.in().hitTest(290, 190), t);
  CHECK_EQ(f.in().hitTest(95, 105), root);
  // Children inherit the parent's transform; scale(0) can't be hit.
  Node* child = f.box(t, 0, 0, 10, 10);
  f.layout();
  CHECK_EQ(f.in().hitTest(115, 115), child);
  Fixture::style(t).transform = {op(TransformOp::Kind::Scale, 0, 0)};
  CHECK_EQ(f.in().hitTest(100, 100), root);
  // matrix() and skew.
  TransformOp m{TransformOp::Kind::Matrix};
  float mm[6] = {1, 0, 0, 1, -100, 0};
  std::memcpy(m.m, mm, sizeof mm);
  Fixture::style(t).transform = {m};
  CHECK_EQ(f.in().hitTest(5, 105), child);
  Fixture::style(t).transform = {op(TransformOp::Kind::SkewX, 45)};
  Fixture::style(t).transformOrigin[0] = {50, true};
  Fixture::style(t).transformOrigin[1] = {50, true};
  // y=100 is 25 above the center: x shifts by -25.
  CHECK_EQ(f.in().hitTest(80, 101), child);
  // visualRect follows the transform.
  Fixture::style(t).transform = {op(TransformOp::Kind::Translate, 10, 20)};
  Rect vr = f.in().visualRect(child);
  CHECK_NEAR(vr.x, 110, 1e-3);
  CHECK_NEAR(vr.y, 120, 1e-3);
}

TEST(input_hit_scroll_offset_and_text) {
  Fixture f;
  Node* root = f.doc().root();
  Node* sv = f.box(root, 0, 0, 100, 100, "{}", NodeType::ScrollView);
  Fixture::style(sv).overflow = Overflow::Scroll;
  Node* content = f.box(sv, 0, 0, 100, 300);
  Node* item = f.box(content, 0, 150, 100, 20);
  Node* text = f.box(content, 0, 0, 100, 20, "{}", NodeType::Text);
  Node* raw = f.doc().createNode(NodeType::RawText);
  raw->setText("hello");
  text->appendChild(raw);
  f.layout();
  CHECK_EQ(f.in().hitTest(10, 10), text);  // RawText hits resolve to the Text
  sv->scrollY = 100;
  float lx, ly;
  CHECK_EQ(f.in().hitTest(10, 60, &lx, &ly), item);
  CHECK_NEAR(ly, 10, 1e-4);
  CHECK_EQ(f.in().hitTest(10, 150), root);  // clipped by the ScrollView
  float tx, ty;
  CHECK(f.in().toLocal(item, 10, 60, &tx, &ty));
  CHECK_NEAR(ty, 10, 1e-4);
  // Inline span hit hook.
  Node* span = f.doc().createNode(NodeType::Text);
  text->appendChild(span);
  f.in().textHitTest = [&](Node* t, float x, float) { return x > 50 ? span : t; };
  sv->scrollY = 0;
  CHECK_EQ(f.in().hitTest(80, 10), span);
  CHECK_EQ(f.in().hitTest(20, 10), text);
}

// ---------------------------------------------------------------- pointer

TEST(input_enter_leave_order) {
  Fixture f;
  Node* root = f.doc().root();
  const char* hov = R"({"__events":["hoverIn","hoverOut"]})";
  Node* a = f.box(root, 0, 0, 100, 100, hov);
  Node* a1 = f.box(a, 10, 10, 50, 50, hov);
  Node* b = f.box(root, 200, 0, 100, 100, hov);
  Node* b1 = f.box(b, 10, 10, 50, 50, hov);
  f.layout();
  CHECK(f.in().mouseMove(20, 20));  // over listeners: UI consumes it
  CHECK_EQ(f.events(), ev("pointerenter", a, "pointerenter", a1));
  f.in().mouseMove(25, 25);
  CHECK_EQ(f.events(), std::string());  // same path: nothing
  f.in().mouseMove(220, 20);
  CHECK_EQ(f.events(), ev("pointerleave", a1, "pointerleave", a) + " " + ev("pointerenter", b, "pointerenter", b1));
  f.in().mouseMove(205, 90);
  CHECK_EQ(f.events(), ev("pointerleave", b1));
  f.in().mouseMove(399, 299);
  CHECK_EQ(f.events(), ev("pointerleave", b));
  CHECK(!f.in().mouseMove(380, 280));  // nothing interactive here
  f.in().mouseMove(20, 20);
  f.events();
  f.in().mouseLeave();
  CHECK_EQ(f.events(), ev("pointerleave", a1, "pointerleave", a));
}

TEST(input_click_target_rules_and_capture) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 0, 0, 200, 100, R"({"__events":["press"]})");
  Node* a1 = f.box(a, 0, 0, 50, 50, R"({"__events":["pointerMove","press"],"focusable":false})");
  Node* a2 = f.box(a, 100, 0, 50, 50, R"({"__events":["press"],"focusable":false})");
  f.layout();
  // Same node: click on it.
  CHECK(f.in().mouseButton(10, 10, 0, true, 0));
  CHECK(f.in().mouseButton(12, 12, 0, false, 0));
  CHECK_EQ(f.events(), ev("pointermove", a1) + " " + ev("pointerdown", a1) + " " + ev("focus", a) + " " +
                           ev("pointermove", a1) + " " + ev("pointerup", a1, "click", a1));
  // Down on a1, up on a2: click goes to the common ancestor; moves and up
  // are captured by a1.
  f.in().mouseButton(10, 10, 0, true, 0);
  f.in().mouseMove(110, 10);
  f.in().mouseButton(110, 10, 0, false, 0);
  CHECK_EQ(f.events(), ev("pointermove", a1) + " " + ev("pointerdown", a1) + " " + ev("pointermove", a1) + " " +
                           ev("pointerup", a1, "click", a));
  // Down inside, up outside every common node but the root.
  f.in().mouseButton(10, 10, 0, true, 0);
  f.in().mouseButton(300, 200, 0, false, 0);
  CHECK_EQ(f.events(), ev("pointermove", a1) + " " + ev("pointerdown", a1) + " " + ev("pointermove", a1) + " " +
                           ev("pointerup", a1, "click", root));
  // Right button: pointerdown/up, no click.
  f.in().mouseButton(110, 10, 2, true, 0);
  f.in().mouseButton(110, 10, 2, false, 0);
  CHECK_EQ(f.events(), ev("pointerdown", a2, "pointerup", a2));
  // Clicking empty space blurs.
  f.in().mouseButton(300, 200, 0, true, 0);
  f.in().mouseButton(300, 200, 0, false, 0);
  CHECK_EQ(f.events(), ev("pointerdown", root) + " " + ev("blur", a) + " " + ev("pointerup", root, "click", root));
  CHECK(f.in().focused() == nullptr);
}

TEST(input_click_common_ancestor_and_localxy) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 20, 30, 200, 100);
  Node* a1 = f.box(a, 0, 0, 50, 50);
  Node* a2 = f.box(a, 100, 0, 50, 50);
  f.layout();
  f.js("extra = e => e.type === 'click' ? '@' + e.localX + ',' + e.localY : '';");
  f.in().mouseButton(30, 40, 0, true, 0);
  f.in().mouseButton(130, 40, 0, false, 0);
  CHECK_EQ(f.events(), ev("pointerdown", a1) + " " + ev("pointerup", a1) + " " + ev("click", a) + "@110,10");
  (void)a2;
}

TEST(input_cursor) {
  Fixture f;
  Node* root = f.doc().root();
  Node* btn = f.box(root, 0, 0, 100, 100, R"({"__events":["press"]})");
  Node* label = f.box(btn, 0, 0, 20, 20);
  Node* field = f.box(root, 200, 0, 100, 30, "{}", NodeType::TextInput);
  Node* custom = f.box(root, 0, 200, 50, 50);
  f.layout();
  Fixture::style(custom).cursor = "grab";
  f.in().mouseMove(5, 5);
  CHECK_EQ(f.rt->cursor, std::string("pointer"));
  Fixture::style(label).cursor = "help";
  f.in().mouseMove(6, 6);
  f.rt->tick(16);
  CHECK_EQ(f.rt->cursor, std::string("help"));
  f.in().mouseMove(210, 10);
  CHECK_EQ(f.rt->cursor, std::string("text"));
  f.in().mouseMove(10, 210);
  CHECK_EQ(f.rt->cursor, std::string("grab"));
  f.in().mouseMove(390, 290);
  CHECK_EQ(f.rt->cursor, std::string("default"));
  (void)field;
}

TEST(input_hover_refresh_after_layout) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 0, 0, 100, 100, R"({"__events":["hoverIn","hoverOut"]})");
  f.layout();
  f.in().mouseMove(50, 50);
  CHECK_EQ(f.events(), ev("pointerenter", a));
  f.place(a, 200, 0, 100, 100);  // the UI moves under a still mouse
  f.rt->tick(16);
  CHECK_EQ(f.events(), ev("pointerleave", a));
  f.rt->tick(32);
  CHECK_EQ(f.events(), std::string());
  f.place(a, 0, 0, 100, 100);
  f.rt->tick(48);
  CHECK_EQ(f.events(), ev("pointerenter", a));
}

TEST(input_wheel_scrolls_scrollview) {
  Fixture f;
  Node* root = f.doc().root();
  Node* sv = f.box(root, 0, 0, 100, 100, "{}", NodeType::ScrollView);
  Fixture::style(sv).overflow = Overflow::Scroll;
  Node* content = f.box(sv, 0, 0, 100, 300);
  f.layout();
  f.in().smoothWheel = false;  // notches jump (smooth wheel: scroll_test.cpp)
  f.js("extra = e => e.type === 'scroll' ? '@' + e.contentOffset.y + '/' + e.contentSize.height + '/' +"
       " e.layoutMeasurement.height : '';");
  CHECK(f.in().mouseWheel(50, 50, 0, 50));
  CHECK_EQ(f.events(), ev("wheel", content) + " " + ev("scroll", sv) + "@50/300/100");
  CHECK_NEAR(sv->scrollY, 50, 1e-4);
  f.in().mouseWheel(50, 50, 0, 1000);  // clamped to content
  CHECK_NEAR(sv->scrollY, 200, 1e-4);
  f.events();
  CHECK(!f.in().mouseWheel(50, 50, 0, 10));  // at the end: no scroll event, not consumed
  CHECK_EQ(f.events(), ev("wheel", content));
  f.js("prevent.wheel = true;");
  f.in().mouseWheel(50, 50, 0, -100);
  CHECK_NEAR(sv->scrollY, 200, 1e-4);  // prevented
  f.js("prevent.wheel = false;");
  f.in().mouseWheel(50, 50, 0, -500);
  CHECK_NEAR(sv->scrollY, 0, 1e-4);
  // Nested: inner horizontal list takes a vertical wheel sideways, the outer
  // vertical one gets it when the inner can't move.
  Node* hs = f.box(content, 0, 0, 100, 40, R"({"horizontal":true})", NodeType::ScrollView);
  f.box(hs, 0, 0, 250, 40);
  f.layout();
  f.events();
  f.in().mouseWheel(10, 10, 0, 30);
  CHECK_NEAR(hs->scrollX, 30, 1e-4);
  CHECK_NEAR(sv->scrollY, 0, 1e-4);
  f.in().mouseWheel(10, 10, 0, 1000);
  CHECK_NEAR(hs->scrollX, 150, 1e-4);
  f.in().mouseWheel(10, 10, 0, 20);
  CHECK_NEAR(sv->scrollY, 20, 1e-4);
}

TEST(input_touch) {
  Fixture f;
  Node* root = f.doc().root();
  Node* sv = f.box(root, 0, 0, 100, 100, "{}", NodeType::ScrollView);
  Node* content = f.box(sv, 0, 0, 100, 300);
  Node* btn = f.box(content, 0, 0, 100, 50, R"({"__events":["press","hoverIn"]})");
  f.layout();
  f.js("extra = e => e.pointerType ? '#' + e.pointerId + e.pointerType : '';");
  CHECK(f.in().touch(3, 0, 10, 10));
  CHECK(f.in().touch(3, 2, 12, 12));
  CHECK_EQ(f.events(), ev("pointerenter", btn) + "#4touch " + ev("pointerdown", btn) + "#4touch " +
                           ev("focus", btn) + " " + ev("pointerup", btn) + "#4touch " + ev("click", btn) +
                           "#4touch " + ev("pointerleave", btn) + "#4touch");
  // A drag past the slop scrolls and cancels the press: no click.
  f.js("extra = null;");
  f.in().touch(0, 0, 10, 40);
  f.in().touch(0, 1, 10, 20);
  f.in().touch(0, 1, 10, 10);
  f.in().touch(0, 2, 10, 10);
  CHECK_NEAR(sv->scrollY, 30, 1e-4);
  CHECK_EQ(f.events(), ev("pointerenter", btn) + " " + ev("pointerdown", btn) + " " + ev("pointercancel", btn) +
                           " " + ev("scrollbegindrag", sv) + " " + ev("scroll", sv) + " " + ev("scroll", sv) + " " +
                           ev("scrollenddrag", sv) + " " + ev("pointerleave", btn));
}

// ------------------------------------------------------------------ focus

TEST(input_tab_order_and_activation) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 0, 0, 50, 20, R"({"__events":["press"]})");
  Node* group = f.box(root, 0, 50, 200, 50);
  Node* b = f.box(group, 0, 0, 50, 20, R"({"focusable":true})");
  f.box(group, 60, 0, 50, 20, R"({"__events":["press"],"focusable":false})");
  f.box(group, 120, 0, 50, 20, R"({"__events":["press"],"disabled":true})");
  Node* c = f.box(root, 0, 200, 50, 20, "{}", NodeType::TextInput);
  f.layout();
  f.js("extra = e => e.type === 'focus' ? (e.focusVisible ? '!' : '') : '';");
  CHECK(f.in().key("Tab", "Tab", true, false, 0));
  CHECK_EQ(f.events(), ev("keydown", root) + " " + ev("focus", a) + "!");
  f.in().key("Tab", "Tab", false, false, 0);
  f.events();
  f.in().key("Tab", "Tab", true, false, 0);
  CHECK_EQ(f.events(), ev("keydown", a) + " " + ev("blur", a) + " " + ev("focus", b) + "!");
  f.in().key("Tab", "Tab", true, false, 0);
  CHECK_EQ(f.in().focused(), c);
  f.in().key("Tab", "Tab", true, false, 0);
  CHECK_EQ(f.in().focused(), a);  // wraps
  f.in().key("Tab", "Tab", true, false, JSUI_MOD_SHIFT);
  CHECK_EQ(f.in().focused(), c);
  f.in().key("Tab", "Tab", true, false, JSUI_MOD_SHIFT);
  CHECK_EQ(f.in().focused(), b);
  f.events();
  // Enter / Space activate; repeats don't; prevented keydown doesn't.
  f.in().key("Enter", "Enter", true, false, 0);
  CHECK_EQ(f.events(), ev("keydown", b, "click", b));
  f.in().key("Enter", "Enter", true, true, 0);
  CHECK_EQ(f.events(), ev("keydown", b));
  f.in().key(" ", "Space", true, false, 0);
  CHECK_EQ(f.events(), ev("keydown", b, "click", b));
  f.js("prevent.keydown = true;");
  f.in().key("Enter", "Enter", true, false, 0);
  f.in().key("Tab", "Tab", true, false, 0);
  CHECK_EQ(f.events(), ev("keydown", b, "keydown", b));
  CHECK_EQ(f.in().focused(), b);
  f.js("prevent.keydown = false;");
  // Text goes to the focused node; TextInput eats Space instead of clicking.
  f.in().focus(c);
  f.events();
  CHECK(f.in().key(" ", "Space", true, false, 0));
  CHECK(f.in().text("\xc3\xa9"));
  f.js("extra = e => e.type === 'textinput' ? '=' + e.text : '';");
  f.in().text("\xe2\x82\xac");
  CHECK_EQ(f.events(), ev("keydown", c) + " " + ev("textinput", c) + " " + ev("textinput", c) + "=\xe2\x82\xac");
  // JS-driven blur.
  f.in().blur(a);
  CHECK_EQ(f.in().focused(), c);
  f.in().blur(c);
  CHECK(f.in().focused() == nullptr);
  CHECK_EQ(f.events(), ev("blur", c));
}

TEST(input_focus_removed_node_and_reentrancy) {
  Fixture f;
  Node* root = f.doc().root();
  Node* a = f.box(root, 0, 0, 50, 20, R"({"__events":["press","hoverOut"]})");
  Node* b = f.box(root, 0, 50, 50, 20, R"({"__events":["press"]})");
  f.layout();
  f.in().focus(a);
  // Destroying the focused node drops focus without events.
  f.doc().destroyNode(a);
  CHECK(f.in().focused() == nullptr);
  f.events();
  // A blur handler that destroys the next target: no crash, nothing focused.
  Node* c = f.box(root, 0, 100, 50, 20, R"({"__events":["press"]})");
  f.layout();
  f.in().focus(b);
  f.js("extra = e => { if (e.type === 'blur') __jsui_native.destroyNode(" + std::to_string(c->id()) +
       "); return ''; };");
  f.in().focus(c);
  CHECK(f.in().focused() == nullptr);
  // Handlers destroying hovered nodes mid-leave.
  Node* p = f.box(root, 200, 0, 100, 100, R"({"__events":["hoverIn","hoverOut"]})");
  Node* q = f.box(p, 0, 0, 50, 50, R"({"__events":["hoverIn","hoverOut"]})");
  f.layout();
  f.in().mouseMove(210, 10);
  f.js("extra = e => { if (e.type === 'pointerleave') __jsui_native.destroyNode(" + std::to_string(p->id()) +
       "); return ''; };");
  f.in().mouseMove(10, 290);
  f.js("extra = null;");
  f.in().mouseButton(10, 290, 0, true, 0);
  f.in().mouseButton(10, 290, 0, false, 0);
  f.rt->tick(16);
  (void)q;
  CHECK(true);
}

namespace {
// 3x3 grid of 80x40 buttons with 20px gaps at (20, 20).
std::vector<Node*> grid(Fixture& f) {
  std::vector<Node*> cells;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) cells.push_back(f.box(f.doc().root(), 20 + c * 100.f, 20 + r * 60.f, 80, 40,
                                                       R"({"__events":["press"]})"));
  f.layout();
  return cells;
}
}  // namespace

TEST(input_spatial_grid) {
  Fixture f;
  auto g = grid(f);
  InputManager& in = f.in();
  CHECK(in.navigate(NavDirection::Right));  // nothing focused: first focusable
  CHECK_EQ(in.focused(), g[0]);
  CHECK(in.navigate(NavDirection::Right));
  CHECK_EQ(in.focused(), g[1]);
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), g[4]);  // straight down, not diagonal
  CHECK(in.navigate(NavDirection::Right));
  CHECK_EQ(in.focused(), g[5]);
  CHECK(!in.navigate(NavDirection::Right));  // edge: stays
  CHECK_EQ(in.focused(), g[5]);
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), g[8]);
  CHECK(!in.navigate(NavDirection::Down));
  CHECK(in.navigate(NavDirection::Left));
  CHECK_EQ(in.focused(), g[7]);
  CHECK(in.navigate(NavDirection::Up));
  CHECK_EQ(in.focused(), g[4]);
  CHECK(in.navigate(NavDirection::Up));
  CHECK_EQ(in.focused(), g[1]);
  CHECK(!in.navigate(NavDirection::Up));
  // Remove the middle column's middle cell: Down from 1 skips to 7.
  Fixture::style(g[4]).display = false;
  f.layout();
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), g[7]);
  // Explicit override wins.
  g[7]->setProps(Value::parseJson("{\"__events\":[\"press\"],\"nextFocusRight\":" + std::to_string(g[0]->id()) + "}"));
  f.place(g[7], 120, 140, 80, 40);
  f.layout();
  CHECK(in.navigate(NavDirection::Right));
  CHECK_EQ(in.focused(), g[0]);
}

TEST(input_spatial_menu_list) {
  Fixture f(800, 600);
  Node* root = f.doc().root();
  // Centered menu of differently sized items + a side panel button.
  std::vector<Node*> items;
  float widths[] = {200, 120, 260, 160};
  for (int i = 0; i < 4; i++)
    items.push_back(f.box(root, 300 - widths[i] / 2, 100 + i * 60.f, widths[i], 40, R"({"__events":["press"]})"));
  Node* side = f.box(root, 600, 170, 100, 100, R"({"__events":["press"]})");
  // A decoy that is closer but diagonal (no overlap with the menu column).
  Node* decoy = f.box(root, 500, 330, 40, 40, R"({"__events":["press"]})");
  f.layout();
  InputManager& in = f.in();
  in.focus(items[0]);
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), items[1]);
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), items[2]);
  CHECK(in.navigate(NavDirection::Down));
  CHECK_EQ(in.focused(), items[3]);  // overlapping item beats the closer diagonal decoy
  CHECK(in.navigate(NavDirection::Up));
  CHECK(in.navigate(NavDirection::Up));
  CHECK_EQ(in.focused(), items[1]);
  CHECK(in.navigate(NavDirection::Right));
  CHECK_EQ(in.focused(), side);
  CHECK(in.navigate(NavDirection::Left));
  CHECK_EQ(in.focused(), items[2]);  // vertically overlapping, nearest
  in.focus(items[0]);
  CHECK(!in.navigate(NavDirection::Up));
  (void)decoy;
}

TEST(input_scroll_focus_into_view) {
  Fixture f;
  Node* root = f.doc().root();
  Node* sv = f.box(root, 0, 0, 100, 100, "{}", NodeType::ScrollView);
  Node* content = f.box(sv, 0, 0, 100, 500);
  std::vector<Node*> rows;
  for (int i = 0; i < 10; i++) rows.push_back(f.box(content, 0, i * 50.f, 100, 50, R"({"__events":["press"]})"));
  f.layout();
  InputManager& in = f.in();
  // Programmatic focus (no key / gamepad input yet) jumps, edge-aligned.
  in.focus(rows[3]);
  CHECK_NEAR(sv->scrollY, 100, 1e-4);  // row 3 (150..200) bottom-aligned
  in.focus(rows[1]);
  CHECK_NEAR(sv->scrollY, 50, 1e-4);  // top-aligned
  in.focus(rows[9]);
  CHECK_NEAR(sv->scrollY, 400, 1e-4);
  // Navigation glides with context around the item.
  in.navigate(NavDirection::Up);
  CHECK_EQ(in.focused(), rows[8]);
  CHECK_NEAR(sv->scrollY, 400, 1e-4);
  for (double t = 16; t <= 400; t += 16) f.rt->tick(t);
  CHECK_NEAR(sv->scrollY, 375, 1e-4);  // row 8 (400..450) with 25 px above
}

TEST(input_gamepad_dpad_and_a) {
  Fixture f;
  auto g = grid(f);
  InputManager& in = f.in();
  f.rt->tick(0);
  f.events();
  CHECK(in.gamepadButton(0, 15, true, 1));  // nothing focused -> first
  CHECK_EQ(in.focused(), g[0]);
  in.gamepadButton(0, 15, false, 0);
  in.gamepadButton(0, 15, true, 1);
  CHECK_EQ(in.focused(), g[1]);
  // Held D-pad repeats: 400 ms, then every 120 ms.
  f.rt->tick(399);
  CHECK_EQ(in.focused(), g[1]);
  f.rt->tick(400);
  CHECK_EQ(in.focused(), g[2]);
  in.gamepadButton(0, 15, false, 0);
  f.rt->tick(2000);
  CHECK_EQ(in.focused(), g[2]);
  f.events();
  CHECK(in.gamepadButton(0, 0, true, 1));
  CHECK_EQ(f.events(), ev("gamepadbutton", g[2], "click", g[2]));
  in.gamepadButton(0, 0, false, 0);
  f.events();
  // B is just an event; prevented D-pad doesn't navigate.
  in.gamepadButton(0, 1, true, 1);
  CHECK_EQ(f.events(), ev("gamepadbutton", g[2]));
  f.js("prevent.gamepadbutton = true;");
  in.gamepadButton(0, 14, true, 1);
  CHECK_EQ(in.focused(), g[2]);
  in.gamepadButton(0, 14, false, 1);
  in.gamepadButton(0, 0, true, 1);
  CHECK_EQ(f.events(), ev("gamepadbutton", g[2]) + " " + ev("gamepadbutton", g[2]) + " " +
                           ev("gamepadbutton", g[2]));
  f.js("prevent.gamepadbutton = false;");
  in.gamepadConnected(1, true, "Xbox");
  f.js("extra = e => e.type === 'gamepadconnected' ? '#' + e.gamepad + e.id + e.connected : '';");
  in.gamepadConnected(1, false, "Xbox");
  CHECK_EQ(f.events(), ev("gamepadconnected", g[2]) + " " + ev("gamepadconnected", g[2]) + "#1Xboxfalse");
}

TEST(input_gamepad_stick_repeat) {
  Fixture f(800, 600);
  Node* root = f.doc().root();
  std::vector<Node*> items;
  for (int i = 0; i < 8; i++) items.push_back(f.box(root, 0, i * 50.f, 100, 40, R"({"__events":["press"]})"));
  f.layout();
  InputManager& in = f.in();
  in.focus(items[0]);
  f.rt->tick(1000);
  auto idx = [&] { return int(std::find(items.begin(), items.end(), in.focused()) - items.begin()); };
  CHECK(!in.gamepadAxis(0, 1, 0.3f));  // inside the deadzone
  CHECK_EQ(idx(), 0);
  CHECK(in.gamepadAxis(0, 1, 0.8f));
  CHECK_EQ(idx(), 1);
  in.gamepadAxis(0, 1, 0.9f);  // same direction: no extra step
  CHECK_EQ(idx(), 1);
  f.rt->tick(1399);
  CHECK_EQ(idx(), 1);
  f.rt->tick(1400);
  CHECK_EQ(idx(), 2);
  f.rt->tick(1519);
  CHECK_EQ(idx(), 2);
  f.rt->tick(1520);
  CHECK_EQ(idx(), 3);
  f.rt->tick(1640);
  CHECK_EQ(idx(), 4);
  f.rt->tick(5000);  // long frame: one step, no burst
  CHECK_EQ(idx(), 5);
  in.gamepadAxis(0, 1, 0.1f);  // released
  f.rt->tick(6000);
  CHECK_EQ(idx(), 5);
  // Up immediately; X dominant axis wins on diagonals.
  in.gamepadAxis(0, 1, -0.7f);
  CHECK_EQ(idx(), 4);
  in.gamepadAxis(0, 0, 0.9f);  // right dominates: nothing to the right
  CHECK_EQ(idx(), 4);
  in.gamepadAxis(0, 1, 0);
  in.gamepadAxis(0, 0, 0);
  CHECK_EQ(idx(), 4);
  // Unchanged values are not re-dispatched.
  f.events();
  in.gamepadAxis(0, 1, 0);
  CHECK_EQ(f.events(), std::string());
  // Prevented axis events don't navigate.
  f.js("prevent.gamepadaxis = true;");
  in.gamepadAxis(0, 1, 1);
  f.rt->tick(7000);
  CHECK_EQ(idx(), 4);
}

TEST(input_autofocus_and_modal_scope) {
  Fixture f;
  Node* root = f.doc().root();
  Node* menuA = f.box(root, 0, 0, 100, 40, R"({"__events":["press"]})");
  Node* menuB = f.box(root, 0, 50, 100, 40, R"({"__events":["press"],"autoFocus":true})");
  f.rt->tick(0);
  CHECK_EQ(f.in().focused(), menuB);
  f.in().focus(menuA);
  f.rt->tick(16);
  CHECK_EQ(f.in().focused(), menuA);  // autoFocus only on appearance
  // A dialog opens: focus moves inside and is trapped there.
  Node* dialog = f.box(root, 150, 0, 200, 200, R"({"accessibilityViewIsModal":true})");
  Node* ok = f.box(dialog, 10, 10, 80, 40, R"({"__events":["press"]})");
  Node* cancel = f.box(dialog, 100, 10, 80, 40, R"({"__events":["press"],"hasTVPreferredFocus":true})");
  f.rt->tick(32);
  CHECK_EQ(f.in().focused(), cancel);
  f.in().navigate(NavDirection::Left);
  CHECK_EQ(f.in().focused(), ok);
  CHECK(!f.in().navigate(NavDirection::Left));  // menu is outside the scope
  f.in().focusNext(false);
  CHECK_EQ(f.in().focused(), cancel);
  f.in().focusNext(false);
  CHECK_EQ(f.in().focused(), ok);
  // Closing it restores focus to where it was.
  f.doc().destroyNode(dialog);
  f.rt->tick(48);
  CHECK_EQ(f.in().focused(), menuA);
  // A modal without a preferred child takes focus on its first focusable;
  // hiding it (display none) also restores.
  Node* d2 = f.box(root, 150, 0, 200, 200, R"({"aria-modal":true})");
  Node* only = f.box(d2, 10, 10, 80, 40, R"({"__events":["press"]})");
  f.rt->tick(64);
  CHECK_EQ(f.in().focused(), only);
  Fixture::style(d2).display = false;
  f.doc().markLayoutDirty();
  f.rt->tick(80);
  CHECK_EQ(f.in().focused(), menuA);
}

TEST(input_arrow_keys_navigate) {
  Fixture f;
  auto g = grid(f);
  f.in().key("ArrowRight", "ArrowRight", true, false, 0);
  CHECK_EQ(f.in().focused(), g[0]);
  f.in().key("ArrowDown", "ArrowDown", true, false, 0);
  CHECK_EQ(f.in().focused(), g[3]);
  f.in().arrowKeyNavigation = false;
  f.in().key("ArrowDown", "ArrowDown", true, false, 0);
  CHECK_EQ(f.in().focused(), g[3]);
}

// ------------------------------------------------------------------ C API

TEST(input_capi_click_roundtrip) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 200;
  cfg.height = 200;
  std::vector<std::string> logs;
  cfg.log = [](void* u, JsuiLogLevel, const char* m) { static_cast<std::vector<std::string>*>(u)->push_back(m); };
  cfg.log_user = &logs;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src =
      "const n = __jsui_native;"
      "const btn = n.createNode('View');"
      "n.setProps(btn, {style: {width: 100, height: 50, margin: 10}, __events: ['press']});"
      "n.appendChild(n.rootId, btn);"
      "globalThis.__jsui_dispatchEvent = e => { console.log(e.type + ':' + (e.target === btn ? 'btn' : e.target)); "
      "  return e.type === 'click'; };";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "input.js"), 0);
  jsui_tick(rt, 0);
  CHECK(jsui_send_mouse_move(rt, 20, 20) != 0);
  CHECK_EQ(std::string(jsui_get_cursor(rt)), std::string("pointer"));
  CHECK(jsui_send_mouse_button(rt, 20, 20, JSUI_MOUSE_LEFT, 1, 0) != 0);
  CHECK(jsui_send_mouse_button(rt, 20, 20, JSUI_MOUSE_LEFT, 0, 0) != 0);
  CHECK(jsui_send_mouse_move(rt, 150, 150) == 0);  // off the button
  CHECK_EQ(std::string(jsui_get_cursor(rt)), std::string("default"));
  CHECK(jsui_send_key(rt, "Tab", "Tab", 1, 0, 0) != 0);
  CHECK(jsui_send_gamepad_button(rt, 0, 0, 1, 1) != 0);
  std::string all;
  for (auto& l : logs) all += l + " ";
  CHECK_EQ(all, std::string("pointerdown:btn focus:btn pointerup:btn click:btn keydown:btn "
                            "gamepadbutton:btn click:btn "));
  jsui_destroy(rt);
}

// ------------------------------------------------------------ performance

TEST(input_perf_hit_test) {
  // A HUD-sized tree: 40 panels x 25 children (1000+ nodes), some zIndex and
  // transforms. Measures hit test + hover tracking per mouse move.
  Fixture f(1920, 1080);
  Node* root = f.doc().root();
  for (int p = 0; p < 40; p++) {
    Node* panel = f.box(root, float(p % 8) * 240, float(p / 8) * 216, 230, 200);
    if (p % 5 == 0) Fixture::style(panel).transform = {op(TransformOp::Kind::Scale, 1.05f, 1.05f)};
    for (int c = 0; c < 25; c++) {
      Node* n = f.box(panel, float(c % 5) * 46, float(c / 5) * 40, 44, 38, R"({"__events":["press","hoverIn"]})");
      if (c % 7 == 0) {
        Fixture::style(n).zIndex = 1;
        Fixture::style(n).hasZIndex = true;
      }
    }
  }
  f.layout();
  f.js("globalThis.__jsui_dispatchEvent = e => false;");
  const int kIters = 20000;
  auto t0 = std::chrono::steady_clock::now();
  int hits = 0;
  for (int i = 0; i < kIters; i++) hits += f.in().hitTest(float((i * 37) % 1920), float((i * 53) % 1080)) != nullptr;
  auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; i++) f.in().mouseMove(float(500 + (i % 7)), float(300 + (i % 5)));  // same node
  auto t2 = std::chrono::steady_clock::now();
  double hitUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
  double moveUs = std::chrono::duration<double, std::micro>(t2 - t1).count() / kIters;
  std::printf("  perf: hitTest %.2f us, mouseMove (no hover change) %.2f us, 1041 nodes\n", hitUs, moveUs);
  CHECK(hits == kIters);
  CHECK(hitUs < 200);  // generous bound for shared CI machines
}
