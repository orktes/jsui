// TextInput editing: caret geometry (skparagraph), keyboard / pointer
// selection, clipboard, undo, controlled value and selection, maxLength,
// secureTextEntry, the caret blink and its incremental repaint.
#include <cmath>
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
#include "text/text_edit.h"
#include "text/text_layout.h"

using namespace jsui;

namespace {

struct TI {
  JsuiConfig cfg;
  std::vector<std::string> logs;
  std::unique_ptr<Runtime> rt;
  double now = 0;

  explicit TI(int w = 400, int h = 200) {
    jsui_config_init(&cfg);
    cfg.width = w;
    cfg.height = h;
    cfg.log = [](void* u, JsuiLogLevel, const char* m) { static_cast<TI*>(u)->logs.push_back(m); };
    cfg.log_user = this;
    rt = std::make_unique<Runtime>(cfg, nullptr);
    // Logs change / selectionchange events; prevents types in `prevent`.
    js("globalThis.prevent = {};"
       "globalThis.__jsui_dispatchEvent = e => {"
       "  if (e.type === 'change') console.log('change=' + e.text + '#' + e.eventCount);"
       "  if (e.type === 'selectionchange') console.log('sel=' + e.selection.start + ',' + e.selection.end);"
       "  return prevent[e.type] ? 3 : 0; };");
  }
  void js(const std::string& src) { CHECK(rt->js->eval(src, "test.js")); }
  Document& doc() { return rt->doc; }
  InputManager& in() { return *rt->input; }

  // An absolutely positioned TextInput; `extra` is more JSON props.
  Node* input(float x, float y, float w, float h, const std::string& style = "", const std::string& extra = "") {
    Node* n = doc().createNode(NodeType::TextInput);
    std::string json = R"({"__events":["change","selectionChange"],"style":{"position":"absolute","left":)" +
                       std::to_string(x) + ",\"top\":" + std::to_string(y) + ",\"width\":" + std::to_string(w) +
                       ",\"height\":" + std::to_string(h) + (style.empty() ? "" : "," + style) + "}" +
                       (extra.empty() ? "" : "," + extra) + "}";
    std::string err;
    Value v = Value::parseJson(json, &err);
    CHECK(err.empty());
    n->setProps(v);
    doc().root()->appendChild(n);
    doc().layout();
    return n;
  }
  void setProps(Node* n, const std::string& json) {
    // Merge into the current props (keeps style / __events).
    Value::Object p = n->props().asObject();
    Value patch = Value::parseJson(json);
    for (auto& [k, v] : patch.asObject()) p[k] = v;
    n->setProps(Value(p));
    doc().layout();
  }
  void key(const char* k, int mods = 0) {
    in().key(k, k, true, false, mods);
    in().key(k, k, false, false, mods);
    doc().layout();
  }
  void type(const char* utf8) {
    in().text(utf8);
    doc().layout();
  }
  void click(float x, float y, int mods = 0) {
    in().mouseMove(x, y);
    in().mouseButton(x, y, 0, true, mods);
    in().mouseButton(x, y, 0, false, mods);
  }
  void tick(double ms) {
    now += ms;
    rt->tick(now);
  }
  std::string value(Node* n) { return edit::toUtf8(edit::state(n)->value); }
  std::string sel(Node* n) {
    auto* s = edit::state(n);
    return std::to_string(s->anchor) + "," + std::to_string(s->focus);
  }
  std::string events() {
    std::string s;
    for (auto& l : logs) s += (s.empty() ? "" : " ") + l;
    logs.clear();
    return s;
  }
  float caretX(Node* n, int i) { return text::inputCaretRect(n, i).fLeft; }
};

}  // namespace

TEST(text_input_caret_geometry) {
  TI t;
  Node* n = t.input(10, 10, 300, 30, R"("fontSize":20,"paddingLeft":4)", R"("value":"hello world")");
  // Monotonic carets from the content box start to the text's end.
  float prev = -1;
  for (int i = 0; i <= 11; i++) {
    float x = t.caretX(n, i);
    CHECK(x > prev);
    prev = x;
  }
  CHECK_NEAR(t.caretX(n, 0), 4, 0.5);
  CHECK(t.caretX(n, 11) > 80 && t.caretX(n, 11) < 140);
  SkRect r = text::inputCaretRect(n, 3);
  CHECK(r.height() >= 18 && r.height() <= 30);  // about one line of 20px text
  CHECK(r.fTop >= 0 && r.fBottom <= 30);
  // Clicking at a caret position lands on that index; between two glyphs on
  // the nearer edge.
  for (int i = 0; i <= 11; i++) CHECK_EQ(text::inputIndexAt(n, t.caretX(n, i) + 0.2f, 15), i);
  float mid = (t.caretX(n, 4) * 3 + t.caretX(n, 5)) / 4;  // closer to 4
  CHECK_EQ(text::inputIndexAt(n, mid, 15), 4);
  CHECK_EQ(text::inputIndexAt(n, 290, 15), 11);
  CHECK_EQ(text::inputIndexAt(n, -5, 15), 0);

  // letterSpacing widens every advance (Chrome adds it after each glyph).
  Node* spaced = t.input(10, 50, 300, 30, R"("fontSize":20,"letterSpacing":5)", R"("value":"hello world")");
  CHECK_NEAR((t.caretX(spaced, 11) - t.caretX(spaced, 0)) - (t.caretX(n, 11) - t.caretX(n, 0)), 55, 3);
  // textTransform: indices stay value indices ("ß" upper-cases to "SS").
  Node* upper = t.input(10, 90, 300, 30, R"("fontSize":20,"textTransform":"uppercase")", R"("value":"aßb")");
  CHECK(t.caretX(upper, 2) - t.caretX(upper, 1) > t.caretX(upper, 3) - t.caretX(upper, 2));
  CHECK_EQ(text::inputIndexAt(upper, t.caretX(upper, 3) + 1, 105), 3);

  // RTL text: index 0 is at the right edge, the caret moves left.
  Node* rtl = t.input(10, 130, 300, 30, R"("fontSize":20)", R"("value":"שלום")");
  CHECK(text::inputRtl(rtl));
  CHECK(t.caretX(rtl, 0) > 250);
  CHECK(t.caretX(rtl, 4) < t.caretX(rtl, 2));
  CHECK(t.caretX(rtl, 2) < t.caretX(rtl, 0));
  // Empty: the caret sits at the start edge (placeholder or not).
  Node* empty = t.input(10, 170, 300, 30, R"("fontSize":20,"textAlign":"center")", R"("placeholder":"Name")");
  CHECK_NEAR(t.caretX(empty, 0), text::inputCaretRect(empty, 0).fLeft, 0);
  CHECK(t.caretX(empty, 0) > 100);
}

TEST(text_input_multiline_caret_and_vertical_moves) {
  TI t;
  Node* n = t.input(0, 0, 200, 120, R"("fontSize":16)", R"("multiline":true,"value":"abc\ndefgh\n")");
  SkRect l0 = text::inputCaretRect(n, 1), l1 = text::inputCaretRect(n, 6), l2 = text::inputCaretRect(n, 10);
  CHECK(l1.fTop > l0.fTop + 10);
  CHECK(l2.fTop > l1.fTop + 10);  // the empty last line after a trailing newline
  CHECK_NEAR(l2.fLeft, text::inputCaretRect(n, 0).fLeft, 0.5);
  CHECK_NEAR(text::inputCaretRect(n, 4).fLeft, text::inputCaretRect(n, 0).fLeft, 0.5);  // start of line 2
  t.in().focus(n);
  t.in().setInputSelection(n, 2, 2);
  t.key("ArrowDown");
  CHECK_EQ(t.sel(n), std::string("6,6"));  // same column on the next line
  t.key("End");
  CHECK_EQ(t.sel(n), std::string("9,9"));
  t.key("ArrowUp");
  CHECK_EQ(t.sel(n), std::string("3,3"));  // clamped to the shorter line
  t.key("ArrowDown");
  CHECK_EQ(t.sel(n), std::string("9,9"));  // goal column kept
  t.key("ArrowDown");
  CHECK_EQ(t.sel(n), std::string("10,10"));
  t.key("ArrowUp", JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("10,9"));  // the goal column again
  t.key("Home");
  CHECK_EQ(t.sel(n), std::string("4,4"));
  // Enter inserts a newline in multiline inputs.
  t.key("Enter");
  CHECK_EQ(t.value(n), std::string("abc\n\ndefgh\n"));
  // Wrapping: a long line breaks inside the box.
  t.setProps(n, R"({"value":"one two three four five six seven eight nine ten"})");
  CHECK(text::inputCaretRect(n, 49).fTop > text::inputCaretRect(n, 0).fTop + 10);
  // At a soft wrap the caret shows at the next line's start; End stops
  // before the wrapping space.
  int wrap = -1;
  for (int i = 1; i < 49 && wrap < 0; i++)
    if (text::inputCaretRect(n, i + 1).fTop > text::inputCaretRect(n, 0).fTop + 10) wrap = i + 1;
  CHECK(wrap > 0);
  CHECK_NEAR(text::inputCaretRect(n, wrap).fLeft, text::inputCaretRect(n, 0).fLeft, 0.5);
  CHECK_EQ(text::inputIndexAt(n, 1, text::inputCaretRect(n, wrap).centerY()), wrap);
  t.in().setInputSelection(n, 0, 0);
  t.key("End");
  CHECK_EQ(t.sel(n), std::to_string(wrap - 1) + "," + std::to_string(wrap - 1));
}

TEST(text_input_keyboard_editing) {
  TI t;
  Node* n = t.input(0, 0, 300, 30, R"("fontSize":16)");
  t.in().focus(n);
  t.events();
  t.type("hello");
  CHECK_EQ(t.value(n), std::string("hello"));
  CHECK_EQ(t.events(), std::string("change=hello#1 sel=5,5"));
  t.key("ArrowLeft");
  t.key("ArrowLeft");
  t.type("XY");
  CHECK_EQ(t.value(n), std::string("helXYlo"));
  CHECK_EQ(t.sel(n), std::string("5,5"));
  t.key("ArrowLeft", JSUI_MOD_SHIFT);
  t.key("ArrowLeft", JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("5,3"));
  t.events();
  t.key("Backspace");  // deletes the selection
  CHECK_EQ(t.value(n), std::string("hello"));
  CHECK_EQ(t.events(), std::string("change=hello#3 sel=3,3"));
  t.key("Delete");
  CHECK_EQ(t.value(n), std::string("helo"));
  t.key("Home");
  t.key("Delete");
  CHECK_EQ(t.value(n), std::string("elo"));
  t.key("End");
  t.key("Backspace");
  CHECK_EQ(t.value(n), std::string("el"));
  // Ctrl+A selects all; typing replaces the selection.
  t.key("a", JSUI_MOD_CTRL);
  CHECK_EQ(t.sel(n), std::string("0,2"));
  t.type("new text here");
  CHECK_EQ(t.value(n), std::string("new text here"));
  // Word jumps (Ctrl / Alt) and word deletes.
  t.key("ArrowLeft", JSUI_MOD_CTRL);
  CHECK_EQ(t.sel(n), std::string("9,9"));
  t.key("ArrowLeft", JSUI_MOD_ALT);
  CHECK_EQ(t.sel(n), std::string("4,4"));
  t.key("ArrowRight", JSUI_MOD_CTRL);
  CHECK_EQ(t.sel(n), std::string("8,8"));
  t.key("ArrowRight", JSUI_MOD_CTRL | JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("8,13"));
  t.key("ArrowLeft");  // collapses to the selection start
  CHECK_EQ(t.sel(n), std::string("8,8"));
  t.key("Backspace", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(n), std::string("new  here"));
  // Cmd+Left / Right: line edges; Home with shift extends.
  t.key("ArrowRight", JSUI_MOD_META);
  CHECK_EQ(t.sel(n), std::string("9,9"));
  t.key("Home", JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("9,0"));
  // Code points / clusters: emoji and combining marks are one step.
  t.key("a", JSUI_MOD_CTRL);
  t.type("a\xF0\x9F\x98\x80" "e\xCC\x81");  // a 😀 e + combining acute
  CHECK_EQ(edit::state(n)->value.size(), size_t(5));
  t.key("ArrowLeft");
  CHECK_EQ(t.sel(n), std::string("3,3"));
  t.key("ArrowLeft");
  CHECK_EQ(t.sel(n), std::string("1,1"));
  t.key("End");
  t.key("Backspace");
  CHECK_EQ(t.value(n), std::string("a\xF0\x9F\x98\x80"));
  t.key("Backspace");
  CHECK_EQ(t.value(n), std::string("a"));
  // Single-line inputs drop line breaks and control characters.
  t.type("b\nc\td\x01");
  CHECK_EQ(t.value(n), std::string("abc\td"));
  // Undo / redo (typing coalesces).
  t.key("a", JSUI_MOD_CTRL);
  t.type("x");
  t.type("y");
  t.key("z", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(n), std::string("abc\td"));
  t.key("z", JSUI_MOD_CTRL | JSUI_MOD_SHIFT);
  CHECK_EQ(t.value(n), std::string("xy"));
  t.key("z", JSUI_MOD_META);
  t.key("y", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(n), std::string("xy"));
  // A prevented keydown / textinput is not applied (onKeyPress preventDefault).
  t.js("prevent.keydown = true; prevent.textinput = true;");
  t.key("Backspace");
  t.type("z");
  CHECK_EQ(t.value(n), std::string("xy"));
  t.js("prevent = {};");
  // Enter does nothing natively in a single-line input (JS submits).
  t.key("Enter");
  CHECK_EQ(t.value(n), std::string("xy"));
}

TEST(text_input_pointer_selection) {
  TI t;
  Node* n = t.input(20, 20, 300, 30, R"("fontSize":20)", R"("value":"alpha beta gamma")");
  float y = 35;
  float xBeta = 20 + t.caretX(n, 6), xEnd = 20 + t.caretX(n, 16);
  t.click(xBeta + 0.3f, y);  // focuses and places the caret
  CHECK_EQ(t.in().focused(), n);
  CHECK_EQ(t.sel(n), std::string("6,6"));
  t.tick(600);  // past the double-click window
  // Drag from the end to "beta".
  t.in().mouseMove(xEnd + 30, y);
  t.in().mouseButton(xEnd + 30, y, 0, true, 0);
  t.in().mouseMove(xBeta + 0.3f, y + 3);
  CHECK_EQ(t.sel(n), std::string("16,6"));
  t.in().mouseButton(xBeta + 0.3f, y, 0, false, 0);
  t.in().mouseMove(xBeta - 40, y);  // released: moves no longer select
  CHECK_EQ(t.sel(n), std::string("16,6"));
  // Shift+click extends.
  t.tick(600);
  t.click(20 + t.caretX(n, 2), y, JSUI_MOD_SHIFT);
  CHECK_EQ(t.sel(n), std::string("16,2"));
  // Double click selects a word, triple click everything.
  t.tick(600);
  float xg = 20 + (t.caretX(n, 12) + t.caretX(n, 13)) / 2;
  t.click(xg, y);
  t.click(xg, y);
  CHECK_EQ(t.sel(n), std::string("11,16"));
  t.click(xg, y);
  CHECK_EQ(t.sel(n), std::string("0,16"));
  std::vector<SkRect> rects;
  text::inputSelectionRects(n, 11, 16, &rects);
  CHECK_EQ(rects.size(), size_t(1));
  if (!rects.empty()) CHECK_NEAR(rects[0].fLeft, t.caretX(n, 11), 0.5);
  // selectTextOnFocus: the focusing press selects everything.
  Node* other = t.input(20, 80, 300, 30, R"("fontSize":20)", R"("value":"select me","selectTextOnFocus":true)");
  t.tick(600);
  t.click(40, 95);
  CHECK_EQ(t.in().focused(), other);
  CHECK_EQ(t.sel(other), std::string("0,9"));
  t.tick(600);
  t.click(30, 95);  // already focused: places the caret
  CHECK_EQ(edit::state(other)->collapsed(), true);
}

TEST(text_input_clipboard) {
  TI t;
  Node* n = t.input(0, 0, 300, 30, R"("fontSize":16)", R"("value":"copy paste")");
  t.in().focus(n);
  t.in().setInputSelection(n, 0, 4);
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(t.rt->clipboard, std::string("copy"));  // in-process fallback
  t.key("End");
  t.key("v", JSUI_MOD_META);
  CHECK_EQ(t.value(n), std::string("copy pastecopy"));
  t.in().setInputSelection(n, 4, 10);
  t.key("x", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(n), std::string("copycopy"));
  CHECK_EQ(t.rt->clipboard, std::string(" paste"));
  // Embedder callbacks replace the fallback (C API).
  struct Clip {
    std::string text = "from\r\nhost";
    std::string got;
  } clip;
  t.rt->clipboardGet = [](void* u) { return static_cast<const char*>(static_cast<Clip*>(u)->text.c_str()); };
  t.rt->clipboardSet = [](void* u, const char* s) { static_cast<Clip*>(u)->got = s; };
  t.rt->clipboardUser = &clip;
  t.key("a", JSUI_MOD_CTRL);
  t.key("v", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(n), std::string("fromhost"));  // single line: line break dropped
  t.key("a", JSUI_MOD_CTRL);
  t.key("c", JSUI_MOD_CTRL);
  CHECK_EQ(clip.got, std::string("fromhost"));
  // Passwords are never copied.
  t.setProps(n, R"({"secureTextEntry":true})");
  clip.got.clear();
  t.key("a", JSUI_MOD_CTRL);
  t.key("c", JSUI_MOD_CTRL);
  t.key("x", JSUI_MOD_CTRL);
  CHECK_EQ(clip.got, std::string());
  CHECK_EQ(t.value(n), std::string("fromhost"));
  // JS sees the same clipboard.
  t.js("__jsui_native.setClipboard('js text'); globalThis.clipOut = __jsui_native.getClipboard();");
  CHECK_EQ(clip.got, std::string("js text"));
}

TEST(text_input_controlled_value_and_selection) {
  TI t;
  Node* n = t.input(0, 0, 300, 30, R"("fontSize":16)", R"("value":"ab","mostRecentEventCount":0)");
  t.in().focus(n);
  t.events();
  t.type("c");
  CHECK_EQ(t.events(), std::string("change=abc#1 sel=3,3"));
  // A render that has not seen the edit (count 0) is ignored...
  t.setProps(n, R"({"value":"ab","mostRecentEventCount":0})");
  CHECK_EQ(t.value(n), std::string("abc"));
  // ...one that has replaces the text: the owner transformed it (same
  // length keeps the caret) or rejected it (caret to the end).
  t.setProps(n, R"({"value":"ABC","mostRecentEventCount":1})");
  CHECK_EQ(t.value(n), std::string("ABC"));
  CHECK_EQ(t.sel(n), std::string("3,3"));
  t.key("Home");
  t.type("x");
  t.setProps(n, R"({"value":"ABC","mostRecentEventCount":2})");
  CHECK_EQ(t.value(n), std::string("ABC"));
  CHECK_EQ(t.sel(n), std::string("3,3"));
  // Programmatic value (no count: always adopted).
  Node* m = t.input(0, 40, 300, 30, R"("fontSize":16)", R"("defaultValue":"start")");
  CHECK_EQ(t.value(m), std::string("start"));
  t.setProps(m, R"({"value":"replaced"})");
  CHECK_EQ(t.value(m), std::string("replaced"));
  // Selection from JS: clamped, no event.
  t.events();
  t.in().setInputSelection(m, 2, 99);
  CHECK_EQ(t.sel(m), std::string("2,8"));
  CHECK_EQ(t.events(), std::string());
  t.js("__jsui_native.setTextInputSelection(" + std::to_string(m->id()) + ", 1, 3)");
  CHECK_EQ(t.sel(m), std::string("1,3"));
  // Not editable: selection works, edits don't.
  t.setProps(m, R"({"editable":false,"focusable":true})");
  t.in().focus(m);
  t.type("zz");
  t.key("Backspace");
  CHECK_EQ(t.value(m), std::string("replaced"));
  CHECK(!text::inputCaretVisible(m));
}

TEST(text_input_max_length_and_secure_entry) {
  TI t;
  Node* n = t.input(0, 0, 300, 30, R"("fontSize":16)", R"("maxLength":5)");
  t.in().focus(n);
  t.type("abcdefgh");
  CHECK_EQ(t.value(n), std::string("abcde"));
  t.events();
  t.type("z");  // full: no change event
  CHECK_EQ(t.events(), std::string());
  t.in().setInputSelection(n, 1, 3);
  t.type("XYZW");  // replaces 2, room for 2
  CHECK_EQ(t.value(n), std::string("aXYde"));
  t.key("a", JSUI_MOD_CTRL);
  t.type("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80");  // never splits a surrogate pair
  CHECK_EQ(edit::state(n)->value.size(), size_t(4));

  Node* pw = t.input(0, 40, 300, 30, R"("fontSize":16)", R"("value":"secret word","secureTextEntry":true)");
  // Bullets: every character advances by the same amount.
  float w1 = t.caretX(pw, 1) - t.caretX(pw, 0), w2 = t.caretX(pw, 7) - t.caretX(pw, 6);
  CHECK_NEAR(w1, w2, 0.01);
  t.in().focus(pw);
  t.key("ArrowLeft", JSUI_MOD_CTRL);  // no word boundaries in a password
  CHECK_EQ(t.sel(pw), std::string("0,0"));
  t.key("Delete", JSUI_MOD_CTRL);
  CHECK_EQ(t.value(pw), std::string());
}

TEST(text_input_scrolls_to_caret) {
  TI t;
  Node* n = t.input(0, 0, 100, 30, R"("fontSize":16,"paddingLeft":5,"paddingRight":5)");
  t.in().focus(n);
  t.type("a fairly long line that overflows");
  SkRect end = text::inputCaretRect(n, int(edit::state(n)->value.size()));
  CHECK(end.fRight <= 95.5f && end.fLeft >= 5);
  CHECK(edit::state(n)->scrollX > 50);
  t.key("Home");
  CHECK_NEAR(edit::state(n)->scrollX, 0, 0.01);
  CHECK_NEAR(text::inputCaretRect(n, 0).fLeft, 5, 0.5);
  t.key("End");
  CHECK(edit::state(n)->scrollX > 50);
  t.in().blur(n);  // a blurred input shows its start
  CHECK_NEAR(edit::state(n)->scrollX, 0, 0.01);
}

TEST(text_input_caret_blink_damages_only_the_caret) {
  TI t(200, 100);
  Node* n = t.input(10, 10, 150, 30, R"("fontSize":16,"backgroundColor":"#ffffff","borderRadius":6)",
                    R"("value":"blink")");
  std::vector<uint32_t> px(200 * 100);
  t.tick(16);
  t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  t.in().focus(n);
  t.tick(16);
  CHECK(t.rt->needsRedraw());
  t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  SkRect caret = text::inputCaretRect(n, 5);
  // Non-AA rects fill the pixels whose centers they cover.
  int cx = int(std::floor(10 + caret.fLeft + 0.5f)), cy = int(10 + caret.centerY());
  auto dark = [&](int x, int y) { return (px[size_t(y) * 200 + x] & 0xFF) < 128; };
  CHECK(text::inputCaretVisible(n));
  CHECK(dark(cx, cy));
  // Idle while "on": no repaint.
  t.tick(100);
  CHECK(!t.rt->needsRedraw());
  // 500 ms later the caret turns off: a frame is needed and only a caret-
  // sized rect is repainted.
  t.tick(450);
  CHECK(t.rt->needsRedraw());
  SkIRect r = t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  CHECK(!r.isEmpty());
  CHECK(r.width() <= 6 && r.height() <= caret.height() + 6);
  CHECK(!dark(cx, cy));
  t.tick(500);
  r = t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  CHECK(r.width() <= 6);
  CHECK(dark(cx, cy));
  // Typing restarts the blink with the caret on; moving the caret repaints
  // its old and new places only.
  t.tick(250);
  t.key("ArrowLeft");
  t.tick(16);
  r = t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  float moved = text::inputCaretRect(n, 4).fLeft;
  CHECK(r.width() <= int(caret.fLeft - moved) + 8);
  CHECK(!dark(cx, cy));
  CHECK(dark(int(std::floor(10 + moved + 0.5f)), cy));
  // caretHidden: never drawn.
  t.setProps(n, R"({"caretHidden":true})");
  t.tick(16);
  CHECK(!text::inputCaretVisible(n));
  // Incremental frames equal full repaints.
  std::vector<uint32_t> full(200 * 100);
  t.setProps(n, R"({"caretHidden":false})");
  t.in().setInputSelection(n, 1, 4);
  t.tick(16);
  t.rt->renderIncremental(px.data(), 200, 100, 200 * 4);
  t.rt->render(full.data(), 200, 100, 200 * 4);
  CHECK(px == full);
  // The selection highlight (default rgba(0, 122, 255, 0.3) over white).
  std::vector<SkRect> rects;
  text::inputSelectionRects(n, 1, 4, &rects);
  CHECK_EQ(rects.size(), size_t(1));
  if (!rects.empty()) {
    int hx = int(10 + rects[0].fLeft + 1), hy = int(10 + rects[0].fTop + 1);
    uint32_t p = full[size_t(hy) * 200 + hx];
    CHECK_NEAR(int(p & 0xFF), 179, 3);
    CHECK_NEAR(int((p >> 8) & 0xFF), 215, 3);
    CHECK_NEAR(int((p >> 16) & 0xFF), 255, 1);
  }
}

// The C API: clipboard callbacks and keys through jsui_send_*.
TEST(text_input_c_api_clipboard) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 200;
  cfg.height = 100;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src =
      "const n = __jsui_native; const id = n.createNode('TextInput');"
      "n.setProps(id, {value: 'hello', style: {position: 'absolute', left: 0, top: 0, width: 150, height: 30}});"
      "n.appendChild(n.rootId, id); n.focus(id); n.setTextInputSelection(id, 1, 4);"
      "globalThis.__jsui_dispatchEvent = e => { if (e.type === 'change') n.postMessage('change', e.text); return 0; };"
      "n.setProps(id, {value: 'hello', __events: ['change'], style: {position: 'absolute', left: 0, top: 0, width: 150, height: 30}});";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "clip.js"), 0);
  jsui_tick(rt, 16);
  struct Clip {
    std::string text = "PASTE", got;
  } clip;
  jsui_set_clipboard_callbacks(
      rt, [](void* u) { return static_cast<Clip*>(u)->text.c_str(); },
      [](void* u, const char* s) { static_cast<Clip*>(u)->got = s; }, &clip);
  CHECK(jsui_send_key(rt, "c", "KeyC", 1, 0, JSUI_MOD_CTRL));
  CHECK_EQ(clip.got, std::string("ell"));
  jsui_send_key(rt, "v", "KeyV", 1, 0, JSUI_MOD_CTRL);
  jsui_send_text(rt, "!");
  jsui_tick(rt, 32);
  CHECK(jsui_last_error(rt) == nullptr);
  jsui_destroy(rt);
}

// Late JS answers (Inbound::replyDelayMs; input_test.cpp "late JS answers"):
// with a keydown listener on the TextInput (every @orktes/jsui-react
// TextInput has one) a key edits once its answer is in, and text typed with
// it waits behind it; a prevented key never edits.
TEST(text_input_late_answers) {
  TI t;
  Node* n = t.input(0, 0, 300, 30, R"("fontSize":16)");
  t.setProps(n, R"({"__events":["change","selectionChange","keyDown"]})");
  t.in().focus(n);
  t.type("ab");
  t.tick(16);
  t.events();
  t.rt->inbound.replyDelayMs = 30;
  CHECK(t.in().key("x", "KeyX", true, false, 0));
  CHECK(t.in().text("x"));
  t.in().key("x", "KeyX", false, false, 0);
  t.key("ArrowLeft");
  CHECK_EQ(t.value(n), std::string("ab"));
  CHECK_EQ(t.events(), std::string());
  t.tick(30);
  CHECK_EQ(t.value(n), std::string("abx"));
  CHECK_EQ(t.sel(n), std::string("2,2"));  // the ArrowLeft after it ran too, in order
  CHECK_EQ(t.events(), std::string("change=abx#2 sel=3,3 sel=2,2"));
  // preventDefault() in the late answer: no edit.
  t.js("prevent.keydown = true;");
  t.key("Backspace");
  t.tick(30);
  CHECK_EQ(t.value(n), std::string("abx"));
  t.js("prevent.keydown = false;");
  t.key("Backspace");
  CHECK_EQ(t.value(n), std::string("abx"));
  t.tick(30);
  CHECK_EQ(t.value(n), std::string("ax"));
  // No answer within the deadline: the edit happens anyway.
  t.rt->inbound.replyDelayMs = 1000;
  t.key("Backspace");
  t.tick(99);
  CHECK_EQ(t.value(n), std::string("ax"));
  t.tick(1);
  CHECK_EQ(t.value(n), std::string("x"));
  CHECK(t.events().find("did not answer a key event") != std::string::npos);
  // Without a keydown listener typing never waits.
  t.setProps(n, R"({"__events":["change","selectionChange"]})");
  t.type("yz");
  CHECK_EQ(t.value(n), std::string("yzx"));
}

// A TextInput as @orktes/jsui-react renders it with native editing: its
// keyDown / textInput handlers are passive and Enter is declared when it
// submits, so typing never waits for a late JS answer (input_test.cpp
// "Declared default actions").
TEST(text_input_declared_no_wait) {
  TI t;
  Node* n = t.input(0, 0, 300, 60, R"("fontSize":16)", R"("multiline":true)");
  t.setProps(n, R"({"__events":["change","selectionChange","keyDown","textInput"],"__passive":["keyDown","textInput"]})");
  // An ancestor declaring Enter does not reach into the focused TextInput.
  t.doc().root()->setProps(Value::parseJson(R"({"preventDefaultKeys":["Enter"]})"));
  t.in().focus(n);
  t.tick(16);
  t.events();
  t.rt->inbound.replyDelayMs = 500;  // JS stalled
  t.type("ab");
  t.key("Backspace");
  CHECK(t.in().key("x", "KeyX", true, false, 0));
  t.type("x");
  t.in().key("x", "KeyX", false, false, 0);
  t.key("Enter");  // multiline: a newline
  t.type("c");
  CHECK_EQ(t.value(n), std::string("ax\nc"));
  CHECK(!t.in().animating());
  // Declared on the input itself (submit): no newline, still no wait.
  t.setProps(n, R"({"preventDefaultKeys":["Enter"]})");
  t.key("Enter");
  CHECK_EQ(t.value(n), std::string("ax\nc"));
  CHECK(!t.in().animating());
  // Handlers that may veto (no __passive): keys and text wait again.
  t.setProps(n, R"({"__passive":[]})");
  t.type("d");
  CHECK_EQ(t.value(n), std::string("ax\nc"));
  t.tick(100);
  CHECK_EQ(t.value(n), std::string("ax\ncd"));
}
