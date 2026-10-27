// Selectable Text (<Text selectable>): mouse drag / double / triple /
// shift-click selection, touch long-press, copy and select all, clearing,
// nested spans, hit testing and the cursor.
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
#include "text/text_edit.h"
#include "text/text_layout.h"

using namespace jsui;

namespace {

struct TS {
  JsuiConfig cfg;
  std::vector<std::string> logs;
  std::unique_ptr<Runtime> rt;
  double now = 0;

  TS() {
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.log = [](void* u, JsuiLogLevel, const char* m) { static_cast<TS*>(u)->logs.push_back(m); };
    cfg.log_user = this;
    rt = std::make_unique<Runtime>(cfg, nullptr);
    js("globalThis.__jsui_dispatchEvent = e => {"
       "  if (e.type === 'selectionchange') console.log('sel=' + e.selection.start + ',' + e.selection.end + ':' + e.text);"
       "  if (e.type === 'pointercancel' || e.type === 'click') console.log(e.type);"
       "  return 0; };");
  }
  void js(const std::string& src) { CHECK(rt->js->eval(src, "test.js")); }
  Document& doc() { return rt->doc; }
  InputManager& in() { return *rt->input; }

  Node* make(NodeType t, Node* parent, const std::string& json) {
    Node* n = doc().createNode(t);
    if (!json.empty()) {
      std::string err;
      Value v = Value::parseJson(json, &err);
      CHECK(err.empty());
      n->setProps(v);
    }
    parent->appendChild(n);
    return n;
  }
  void raw(Node* parent, const char* s) {
    Node* r = doc().createNode(NodeType::RawText);
    r->setText(s);
    parent->appendChild(r);
  }
  // A paragraph at (x, y), 300 px wide, 20 px text.
  Node* text(float x, float y, const char* s, bool selectable = true, const std::string& extra = "") {
    std::string json = std::string(R"({"__events":["selectionChange"],"selectable":)") + (selectable ? "true" : "false") +
                       R"(,"style":{"position":"absolute","left":)" + std::to_string(x) + ",\"top\":" +
                       std::to_string(y) + R"(,"width":300,"fontSize":20})" + extra + "}";
    Node* n = make(NodeType::Text, doc().root(), json);
    if (s) raw(n, s);
    doc().layout();
    return n;
  }
  void click(float x, float y, int mods = 0) {
    in().mouseMove(x, y);
    in().mouseButton(x, y, 0, true, mods);
    in().mouseButton(x, y, 0, false, mods);
  }
  void key(const char* k, int mods = 0) {
    in().key(k, k, true, false, mods);
    in().key(k, k, false, false, mods);
  }
  void tick(double ms) {
    now += ms;
    rt->tick(now);
  }
  std::string sel(Node* n) { return std::to_string(n->selAnchor) + "," + std::to_string(n->selFocus); }
  std::string events() {
    std::string s;
    for (auto& l : logs) s += (s.empty() ? "" : " ") + l;
    logs.clear();
    return s;
  }
  // Viewport x of the caret before index i of a paragraph placed at x0
  // (single line: the x where textIndexAt lands on i).
  float xOf(Node* n, float x0, int i, float y) {
    for (float x = 0; x < 300; x += 0.25f)
      if (text::textIndexAt(n, x, y) >= i) return x0 + x;
    return x0 + 300;
  }
};

}  // namespace

TEST(text_selection_mouse_drag_and_multi_click) {
  TS t;
  Node* n = t.text(20, 20, "alpha beta gamma");
  float y = 30;  // inside the line (viewport)
  float ly = 10;  // the same, Text-local
  float xBeta = t.xOf(n, 20, 6, ly), xEnd = t.xOf(n, 20, 16, ly);
  // Drag from "beta" to the end.
  t.in().mouseMove(xBeta, y);
  t.in().mouseButton(xBeta, y, 0, true, 0);
  CHECK_EQ(t.sel(n), std::string("6,6"));
  t.in().mouseMove(xEnd + 40, y + 2);
  CHECK_EQ(t.sel(n), std::string("6,16"));
  t.in().mouseButton(xEnd + 40, y, 0, false, 0);
  t.in().mouseMove(20, y);  // released: moves no longer select
  CHECK_EQ(t.sel(n), std::string("6,16"));
  CHECK(t.events().find("sel=6,16:beta gamma") != std::string::npos);
  // Shift+click extends from the anchor.
  t.tick(600);
  t.click(t.xOf(n, 20, 2, ly), y, JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("6,2"));
  // Double click: a word; triple: the paragraph.
  t.tick(600);
  float xg = t.xOf(n, 20, 13, ly);
  t.click(xg, y);
  t.click(xg, y);
  CHECK_EQ(t.sel(n), std::string("11,16"));
  t.click(xg, y);
  CHECK_EQ(t.sel(n), std::string("0,16"));
  // A single click collapses it.
  t.tick(600);
  t.click(xg, y);
  CHECK_EQ(n->selAnchor, n->selFocus);
}

TEST(text_selection_nested_spans_copy_and_select_all) {
  TS t;
  Node* n = t.text(10, 10, nullptr);
  t.raw(n, "Hello ");
  Node* bold = t.make(NodeType::Text, n, R"({"style":{"fontWeight":"bold"}})");
  t.raw(bold, "bold world");
  t.raw(n, "\nsecond line");
  t.doc().layout();
  CHECK_EQ(edit::toUtf8(text::textContent(n)), std::string("Hello bold world\nsecond line"));
  // A press on the span selects in the root's text.
  float ly = 10;
  float xb = t.xOf(n, 10, 8, ly);
  t.click(xb, 20);
  t.click(xb, 20);
  CHECK_EQ(t.sel(n), std::string("6,10"));
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(t.rt->clipboard, std::string("bold"));
  // Triple click stops at the hard line break.
  t.click(xb, 20);
  CHECK_EQ(t.sel(n), std::string("0,16"));
  t.key("a", JSUI_MOD_META);
  CHECK_EQ(t.sel(n), std::string("0,28"));
  t.key("c", JSUI_MOD_META);
  CHECK_EQ(t.rt->clipboard, std::string("Hello bold world\nsecond line"));
  // The embedder's clipboard when it has one.
  std::string got;
  t.rt->clipboardSet = [](void* u, const char* s) { *static_cast<std::string*>(u) = s; };
  t.rt->clipboardUser = &got;
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(got, std::string("Hello bold world\nsecond line"));
  // An empty selection copies nothing (and leaves the key unconsumed).
  t.tick(600);
  t.click(xb, 20);
  got.clear();
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(got, std::string());
}

TEST(text_selection_clears_elsewhere_and_ignores_plain_text) {
  TS t;
  Node* a = t.text(10, 10, "first paragraph");
  Node* plain = t.text(10, 60, "not selectable", false);
  Node* b = t.text(10, 110, "second paragraph");
  t.click(30, 20);
  t.click(30, 20);
  CHECK(a->selAnchor != a->selFocus);
  t.events();
  // A press on text that is not selectable clears it and selects nothing.
  t.tick(600);
  t.in().mouseMove(30, 70);
  t.in().mouseButton(30, 70, 0, true, 0);
  t.in().mouseMove(200, 70);
  t.in().mouseButton(200, 70, 0, false, 0);
  CHECK_EQ(a->selAnchor, a->selFocus);
  CHECK_EQ(plain->selAnchor, plain->selFocus);
  CHECK_EQ(t.events(), std::string("sel=0,0: click"));
  // Selecting in another paragraph drops the first one's.
  t.click(30, 20);
  t.click(30, 20);
  t.tick(600);
  t.click(30, 120);
  t.click(30, 120);
  CHECK_EQ(a->selAnchor, a->selFocus);
  CHECK(b->selAnchor != b->selFocus);
  // Removing the node forgets it; keys no longer copy.
  t.rt->clipboard.clear();
  t.doc().root()->removeChild(b);
  t.doc().destroyNode(b);
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(t.rt->clipboard, std::string());
}

TEST(text_selection_touch_long_press) {
  TS t;
  Node* n = t.text(20, 20, "alpha beta gamma", true, R"(,"__events":["selectionChange","press"])");
  float ly = 10;
  float xBeta = t.xOf(n, 20, 7, ly), xGamma = t.xOf(n, 20, 13, ly);
  // A tap is a press: no selection.
  t.in().touch(1, 0, xBeta, 30);
  t.tick(100);
  t.in().touch(1, 2, xBeta, 30);
  CHECK_EQ(n->selAnchor, n->selFocus);
  CHECK(t.events().find("click") != std::string::npos);
  // Held: the word under the finger, the press cancelled (no click).
  t.tick(600);
  t.in().touch(2, 0, xBeta, 30);
  CHECK(t.in().animating());  // the host keeps ticking until the long press
  t.tick(200);
  CHECK_EQ(n->selAnchor, n->selFocus);
  t.tick(400);
  CHECK_EQ(t.sel(n), std::string("6,10"));
  // Dragging on extends it by words.
  t.in().touch(2, 1, xGamma, 32);
  CHECK_EQ(t.sel(n), std::string("6,16"));
  t.in().touch(2, 2, xGamma, 32);
  std::string ev = t.events();
  CHECK(ev.find("pointercancel") != std::string::npos);
  CHECK(ev.find("click") == std::string::npos);
  CHECK(ev.find("sel=6,16:beta gamma") != std::string::npos);
  CHECK_EQ(t.sel(n), std::string("6,16"));
  // A finger that moves before the long press does not select.
  t.tick(600);
  t.in().touch(3, 0, 30, 30);  // a tap clears the selection
  CHECK_EQ(n->selAnchor, n->selFocus);
  t.in().touch(3, 1, 60, 30);
  t.tick(600);
  CHECK_EQ(n->selAnchor, n->selFocus);
  t.in().touch(3, 2, 60, 30);
}

TEST(text_selection_hit_and_cursor) {
  TS t;
  Node* n = t.text(10, 10, "selectable");
  Node* plain = t.text(10, 60, "plain", false);
  (void)n;
  (void)plain;
  CHECK_EQ(t.in().capturesAt(20, 20) & JSUI_HIT_POINTER, unsigned(JSUI_HIT_POINTER));
  CHECK_EQ(t.in().capturesAt(20, 70) & JSUI_HIT_POINTER, 0u);
  t.in().mouseMove(20, 20);
  CHECK_EQ(t.rt->main().cursor, std::string("text"));
  t.in().mouseMove(20, 70);
  CHECK_EQ(t.rt->main().cursor, std::string("default"));
}

TEST(text_selection_paints_highlight_incrementally) {
  TS t;
  Node* n = t.text(10, 10, "highlight me", true, R"(,"style":{"position":"absolute","left":10,"top":10,"width":300,"fontSize":20,"backgroundColor":"#ffffff"})");
  constexpr int W = 400, H = 300;
  std::vector<uint32_t> px(W * H), full(W * H);
  t.tick(16);
  t.rt->renderIncremental(px.data(), W, H, W * 4);
  // A double click on the first word: drawn without a relayout, incremental
  // frames equal full repaints.
  t.click(20, 20);
  t.click(20, 20);
  CHECK_EQ(t.sel(n), std::string("0,9"));
  t.tick(16);
  CHECK(t.rt->needsRedraw());
  t.rt->renderIncremental(px.data(), W, H, W * 4);
  t.rt->render(full.data(), W, H, W * 4);
  CHECK(px == full);
  // Default highlight rgba(0, 122, 255, 0.3) over white, between glyphs.
  float lx = 0;
  for (float x = 0; x < 300; x += 0.25f)
    if (text::textIndexAt(n, x, 10) >= 9) {
      lx = x;
      break;
    }
  uint32_t p = full[size_t(10 + 3) * W + size_t(10 + lx - 2)];  // top of the line, left of "me"'s gap
  CHECK_NEAR(int(p & 0xFF), 179, 3);
  CHECK_NEAR(int((p >> 16) & 0xFF), 255, 1);
  // Clearing repaints the white back.
  t.tick(600);
  t.click(200, 200);
  t.tick(16);
  t.rt->renderIncremental(px.data(), W, H, W * 4);
  p = px[size_t(10 + 3) * W + size_t(10 + lx - 2)];
  CHECK_EQ(p & 0xFFFFFF, 0xFFFFFFu);
}
