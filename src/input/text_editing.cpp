// TextInput editing: keys, text, pointer selection, clipboard, undo and the
// caret blink. The text lives in the node's edit state (text/text_edit.h);
// geometry comes from the paragraph (text/text_layout.h).
#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/document.h"
#include "input/input.h"
#include "js/engine.h"
#include "runtime.h"
#include "text/text_edit.h"
#include "text/text_layout.h"

namespace jsui {

namespace {

constexpr double kBlinkMs = 500;           // caret on / off phase (Chrome)
constexpr double kMultiClickMs = 500;      // double / triple click window
constexpr float kMultiClickSlop = 4;       // px
constexpr size_t kUndoLimit = 100;

}  // namespace

// props.__events contains `token` exactly (case-insensitive) or "*".
bool nodeListens(const Node* n, const char* token) {
  for (auto& e : n->props()["__events"].asArray()) {
    const std::string& s = e.asString();
    if (s == "*") return true;
    size_t len = std::char_traits<char>::length(token);
    if (s.size() != len) continue;
    bool eq = true;
    for (size_t i = 0; i < len && eq; i++)
      eq = std::tolower((unsigned char)s[i]) == std::tolower((unsigned char)token[i]);
    if (eq) return true;
  }
  return false;
}

namespace {

bool secure(const Node* n) { return n->props()["secureTextEntry"].asBool(); }
bool multiline(const Node* n) { return n->props()["multiline"].asBool(); }

void resetBlink(edit::EditState* s, Runtime& rt) {
  s->caretOn = true;
  s->blinkBase = rt.now;
}

}  // namespace

bool InputManager::editable(const Node* n) const {
  const Value& p = n->props();
  if (p["editable"].isBool() && !p["editable"].asBool()) return false;
  return !p["readOnly"].asBool();
}

// ------------------------------------------------------------- events

void InputManager::editNotify(uint32_t id, bool text, bool selection) {
  Node* n = alive(id);
  if (text && n && nodeListens(n, "change")) {
    edit::EditState* s = edit::state(n);
    Value::Object ev = baseEvent("change", n);
    ev["text"] = Value(edit::toUtf8(s->value));
    ev["eventCount"] = Value(s->eventCount);
    post(std::move(ev));
  }
  n = alive(id);  // the change handler may have removed it
  if (selection && n && nodeListens(n, "selectionChange")) {
    edit::EditState* s = edit::state(n);
    Value::Object sel;
    sel["start"] = Value(s->start());
    sel["end"] = Value(s->end());
    Value::Object ev = baseEvent("selectionchange", n);
    ev["selection"] = Value(std::move(sel));
    post(std::move(ev));
  }
}

// ---------------------------------------------------------- mutations

void InputManager::editSelect(Node* n, int anchor, int focus, bool keepGoal) {
  edit::EditState* s = edit::state(n);
  int len = int(s->value.size());
  anchor = std::clamp(anchor, 0, len);
  focus = std::clamp(focus, 0, len);
  resetBlink(s, rt_);
  n->markPaintTree();
  doc_.markPaintDirty();
  if (!keepGoal) s->hasGoalX = false;
  s->coalesceTyping = false;
  if (anchor == s->anchor && focus == s->focus) return;
  bool wasCollapsed = s->collapsed();
  s->anchor = anchor;
  s->focus = focus;
  bool scrolled = text::inputScrollToCaret(n);
  // A moving caret only damages its old and new rects (see the painter);
  // a selection or scroll change repaints the input.
  if (scrolled || !wasCollapsed || !s->collapsed()) n->markPaintDirty();
  editNotify(n->id(), false, true);
}

void InputManager::setInputSelection(Node* n, int start, int end) {
  if (!n || n->type() != NodeType::TextInput) return;
  edit::EditState* s = edit::state(n);
  int len = int(s->value.size());
  start = std::clamp(start, 0, len);
  end = std::clamp(end, 0, len);
  if (start == s->anchor && end == s->focus) return;
  s->anchor = start;
  s->focus = end;
  s->hasGoalX = false;
  s->coalesceTyping = false;
  resetBlink(s, rt_);
  text::inputScrollToCaret(n);
  n->markPaintDirty();
}

std::u16string InputManager::editFilter(Node* n, const std::u16string& in) const {
  bool multi = multiline(n);
  std::u16string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    char16_t c = in[i];
    if (c == u'\r') {
      if (i + 1 < in.size() && in[i + 1] == u'\n') continue;  // CRLF -> LF
      c = u'\n';
    }
    if (c == u'\n') {
      if (multi) out.push_back(c);  // single line inputs drop line breaks
      continue;
    }
    if ((c < 0x20 && c != u'\t') || c == 0x7F) continue;
    out.push_back(c);
  }
  return out;
}

bool InputManager::editReplace(Node* n, int start, int end, std::u16string ins, EditKind kind) {
  edit::EditState* s = edit::state(n);
  int len = int(s->value.size());
  start = std::clamp(start, 0, len);
  end = std::clamp(end, start, len);
  const Value& ml = n->props()["maxLength"];
  if (ml.isNumber() && ml.asNumber() >= 0) {
    long room = long(ml.asNumber()) - (len - (end - start));
    if (room <= 0) {
      ins.clear();
    } else if (long(ins.size()) > room) {
      size_t keep = size_t(room);
      if (ins[keep - 1] >= 0xD800 && ins[keep - 1] <= 0xDBFF) keep--;  // don't split a pair
      ins.resize(keep);
    }
  }
  if (ins.empty() && start == end) return false;
  bool coalesce = kind == EditKind::Typing && s->coalesceTyping && start == end && start == s->focus;
  if (!coalesce) {
    s->undo.push_back({s->value, s->anchor, s->focus});
    if (s->undo.size() > kUndoLimit) s->undo.erase(s->undo.begin());
  }
  s->redo.clear();
  std::u16string v = s->value.substr(0, size_t(start)) + ins + s->value.substr(size_t(end));
  s->value = std::move(v);
  s->anchor = s->focus = start + int(ins.size());
  s->eventCount++;
  s->hasGoalX = false;
  edit::contentChanged(n);
  s->coalesceTyping = kind == EditKind::Typing;  // contentChanged() keeps the state
  resetBlink(s, rt_);
  text::inputScrollToCaret(n);
  editNotify(n->id(), true, true);
  return true;
}

bool InputManager::editUndo(Node* n, bool redo) {
  edit::EditState* s = edit::state(n);
  auto& from = redo ? s->redo : s->undo;
  auto& to = redo ? s->undo : s->redo;
  if (from.empty()) return false;
  edit::UndoEntry e = std::move(from.back());
  from.pop_back();
  to.push_back({s->value, s->anchor, s->focus});
  s->value = std::move(e.value);
  int len = int(s->value.size());
  s->anchor = std::clamp(e.anchor, 0, len);
  s->focus = std::clamp(e.focus, 0, len);
  s->eventCount++;
  s->coalesceTyping = false;
  s->hasGoalX = false;
  edit::contentChanged(n);
  resetBlink(s, rt_);
  text::inputScrollToCaret(n);
  editNotify(n->id(), true, true);
  return true;
}

// ---------------------------------------------------------------- keys

bool InputManager::editKey(Node* n, const std::string& key, int mods) {
  edit::EditState* s = edit::state(n);
  const std::u16string& v = s->value;
  int len = int(v.size());
  bool shift = (mods & JSUI_MOD_SHIFT) != 0;
  bool ctrl = (mods & JSUI_MOD_CTRL) != 0, alt = (mods & JSUI_MOD_ALT) != 0, meta = (mods & JSUI_MOD_META) != 0;
  bool byWord = ctrl || alt;              // Ctrl+Arrow (Windows / Linux), Alt+Arrow (macOS)
  bool byLine = meta && !ctrl;            // Cmd+Arrow (macOS)
  bool canEdit = editable(n);
  uint32_t id = n->id();

  std::string k = key;
  if (k.size() == 1) k[0] = char(std::tolower((unsigned char)k[0]));

  // Shortcuts: Ctrl (Windows / Linux) or Cmd (macOS).
  if ((ctrl || meta) && !alt && k.size() == 1) {
    switch (k[0]) {
      case 'a': editSelect(n, 0, len); return true;
      case 'c':
      case 'x': {
        if (s->collapsed() || secure(n)) return true;  // no copying passwords
        rt_.setClipboardText(edit::toUtf8(v.substr(size_t(s->start()), size_t(s->end() - s->start()))));
        if (k[0] == 'x' && canEdit) {
          n = alive(id);  // the embedder callback cannot touch the tree, but be safe
          if (n) editReplace(n, edit::state(n)->start(), edit::state(n)->end(), u"", EditKind::Other);
        }
        return true;
      }
      case 'v': {
        if (!canEdit) return true;
        std::u16string t = editFilter(n, edit::toUtf16(rt_.clipboardText()));
        n = alive(id);
        if (!n) return true;
        s = edit::state(n);
        if (!t.empty()) editReplace(n, s->start(), s->end(), std::move(t), EditKind::Other);
        return true;
      }
      case 'z':
        if (canEdit) editUndo(n, shift);
        return true;
      case 'y':
        if (canEdit && ctrl) editUndo(n, true);
        return true;
      default: break;
    }
  }

  bool rtl = text::inputRtl(n);
  if (key == "ArrowLeft" || key == "ArrowRight") {
    bool forward = (key == "ArrowRight") != rtl;
    int target;
    if (!shift && !s->collapsed() && !byWord && !byLine) {
      target = forward ? s->end() : s->start();  // collapse the selection
    } else if (byLine) {
      target = text::inputLineEdge(n, s->focus, forward);
    } else if (byWord) {
      if (secure(n)) target = forward ? len : 0;
      else target = forward ? edit::nextWordEnd(v, s->focus) : edit::prevWordStart(v, s->focus);
    } else {
      target = forward ? edit::nextBoundary(v, s->focus) : edit::prevBoundary(v, s->focus);
    }
    editSelect(n, shift ? s->anchor : target, target);
    return true;
  }
  if (key == "ArrowUp" || key == "ArrowDown") {
    int dir = key == "ArrowUp" ? -1 : 1;
    int from = s->focus;
    if (!shift && !s->collapsed()) from = dir < 0 ? s->start() : s->end();
    int target = byLine ? (dir < 0 ? 0 : len) : text::inputVerticalMove(n, from, dir, &s->goalX, s->hasGoalX);
    s->hasGoalX = !byLine;
    editSelect(n, shift ? s->anchor : target, target, true);
    return true;
  }
  if (key == "Home" || key == "End") {
    bool end = key == "End";
    int target = ctrl ? (end ? len : 0) : text::inputLineEdge(n, s->focus, end);
    editSelect(n, shift ? s->anchor : target, target);
    return true;
  }
  if (key == "PageUp" || key == "PageDown") {
    int target = key == "PageUp" ? 0 : len;
    editSelect(n, shift ? s->anchor : target, target);
    return true;
  }
  if (key == "Backspace" || key == "Delete") {
    if (!canEdit) return true;
    if (!s->collapsed()) {
      editReplace(n, s->start(), s->end(), u"", EditKind::Delete);
      return true;
    }
    bool back = key == "Backspace";
    int other;
    if (byLine) other = text::inputLineEdge(n, s->focus, !back);
    else if (byWord) other = secure(n) ? (back ? 0 : len) : back ? edit::prevWordStart(v, s->focus) : edit::nextWordEnd(v, s->focus);
    else other = back ? edit::prevBoundary(v, s->focus) : edit::nextBoundary(v, s->focus);
    editReplace(n, std::min(other, s->focus), std::max(other, s->focus), u"", EditKind::Delete);
    return true;
  }
  if (key == "Enter") {
    // JS (the TextInput component) prevents Enter when it submits; reaching
    // here means a newline in a multiline input.
    if (canEdit && multiline(n)) editReplace(n, s->start(), s->end(), u"\n", EditKind::Other);
    return true;
  }
  return false;
}

bool InputManager::editText(Node* n, const char* utf8) {
  if (!editable(n)) return true;
  std::u16string t = editFilter(n, edit::toUtf16(utf8));
  if (t.empty()) return true;
  edit::EditState* s = edit::state(n);
  editReplace(n, s->start(), s->end(), std::move(t), EditKind::Typing);
  return true;
}

// ------------------------------------------------------------- pointer

int InputManager::multiClick(uint32_t id, const PointerState& p) {
  double now = rt_.now;
  if (id == lastClickId_ && now - lastClickTime_ <= kMultiClickMs && std::fabs(p.x - lastClickX_) <= kMultiClickSlop &&
      std::fabs(p.y - lastClickY_) <= kMultiClickSlop)
    clickCount_ = clickCount_ >= 3 ? 1 : clickCount_ + 1;
  else
    clickCount_ = 1;
  lastClickId_ = id;
  lastClickTime_ = now;
  lastClickX_ = p.x;
  lastClickY_ = p.y;
  return clickCount_;
}

void InputManager::editPointerDown(Node* n, const PointerState& p, int mods) {
  if (selectedOnFocusId_ == n->id()) {
    selectedOnFocusId_ = 0;
    dragInputId_ = 0;
    return;
  }
  float lx = 0, ly = 0;
  if (!toLocal(n, p.x, p.y, &lx, &ly)) return;
  edit::EditState* s = edit::state(n);
  multiClick(n->id(), p);
  int idx = text::inputIndexAt(n, lx, ly);
  dragInputId_ = p.touch ? 0 : n->id();
  if (clickCount_ == 2) {
    int a, b;
    if (secure(n)) a = 0, b = int(s->value.size());
    else edit::wordAt(s->value, idx, &a, &b);
    dragMode_ = 1;
    dragWordStart_ = a;
    dragWordEnd_ = b;
    editSelect(n, a, b);
  } else if (clickCount_ == 3) {
    dragMode_ = 2;
    editSelect(n, 0, int(s->value.size()));
  } else {
    dragMode_ = 0;
    editSelect(n, (mods & JSUI_MOD_SHIFT) ? s->anchor : idx, idx);
  }
}

void InputManager::editPointerDrag(const PointerState& p) {
  Node* n = alive(dragInputId_);
  if (!n || focused_ != n || !(p.buttons & 1)) {
    dragInputId_ = 0;
    return;
  }
  float lx = 0, ly = 0;
  if (!toLocal(n, p.x, p.y, &lx, &ly)) return;
  int idx = text::inputIndexAt(n, lx, ly);
  edit::EditState* s = edit::state(n);
  if (dragMode_ == 0) {
    editSelect(n, s->anchor, idx);
  } else if (dragMode_ == 1) {
    int a, b;
    edit::wordAt(s->value, idx, &a, &b);
    if (secure(n)) a = 0, b = int(s->value.size());
    if (idx < dragWordStart_) editSelect(n, dragWordEnd_, a);
    else editSelect(n, dragWordStart_, std::max(b, dragWordEnd_));
  }
}

// --------------------------------------------------------------- focus

void InputManager::editFocus(Node* n, bool focused) {
  if (!n || n->type() != NodeType::TextInput) return;
  edit::EditState* s = edit::state(n);
  s->focused = focused;
  s->coalesceTyping = false;
  s->hasGoalX = false;
  resetBlink(s, rt_);
  if (!focused) {
    // Like Chrome: a blurred input shows its start again.
    s->scrollX = s->scrollY = 0;
    dragInputId_ = 0;
  } else if (n->props()["selectTextOnFocus"].asBool()) {
    s->anchor = 0;
    s->focus = int(s->value.size());
    selectedOnFocusId_ = n->id();
    text::inputScrollToCaret(n);
  } else {
    text::inputScrollToCaret(n);
  }
  n->markPaintDirty();
}

// ---------------------------------------------------------------- tick

void InputManager::editTick() {
  Node* n = focused_;
  if (!n || n->type() != NodeType::TextInput) return;
  edit::EditState* s = edit::peek(n);
  if (!s || !s->focused || !s->collapsed()) return;
  double t = rt_.now - s->blinkBase;
  bool on = t < 0 || std::fmod(t, 2 * kBlinkMs) < kBlinkMs;
  if (on != s->caretOn) {
    s->caretOn = on;
    n->markPaintTree();
    doc_.markPaintDirty();  // the painter damages just the caret rect
  }
}

}  // namespace jsui
