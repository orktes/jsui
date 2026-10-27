// Selectable Text: <Text selectable> selects natively, like a web page's
// text or RN's selectable Text. The selection lives on the paragraph-root
// Text (Node::selAnchor / selFocus, UTF-16 indices into the paragraph's
// text, nested spans included); the painter draws its highlight under the
// glyphs, so drags never cross into JS. One selection at a time: pressing
// anywhere else clears it.
//
//   mouse   press + drag selects characters, a double click words, a triple
//           click the paragraph (between hard line breaks); shift-click
//           extends the selection
//   touch   a long press selects the word under the finger and dragging on
//           extends it by words (the press under it is cancelled); a tap
//           clears it
//   keys    Ctrl / Cmd+C copies (the embedder's clipboard), Ctrl / Cmd+A
//           selects all of the Text
//
// 'selectionchange' ({selection: {start, end}, text}) goes to an
// onSelectionChange on the root.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "input/input.h"
#include "runtime.h"
#include "text/text_edit.h"
#include "text/text_layout.h"

namespace jsui {

namespace {

constexpr double kLongPressMs = 500;  // RN Pressable's delayLongPress

// The paragraph (between hard line breaks) around index i.
void paragraphAt(const std::u16string& s, int i, int* start, int* end) {
  int n = int(s.size());
  i = std::clamp(i, 0, n);
  int a = i;
  while (a > 0 && s[size_t(a - 1)] != u'\n') a--;
  int b = i;
  while (b < n && s[size_t(b)] != u'\n') b++;
  *start = a;
  *end = b;
}

bool selectable(const Node* n) { return n->props()["selectable"].asBool(); }

}  // namespace

Node* InputManager::selectableRoot(Node* hit) const {
  Node* n = hit;
  if (n && n->type() == NodeType::RawText) n = n->parent();
  if (!n || n->type() != NodeType::Text) return nullptr;
  while (n->parent() && n->parent()->type() == NodeType::Text) n = n->parent();
  return selectable(n) ? n : nullptr;
}

void InputManager::selSet(Node* root, int anchor, int focus) {
  uint32_t rootId = root ? root->id() : 0;
  if (selTextId_ && selTextId_ != rootId) {
    uint32_t oldId = selTextId_;
    selTextId_ = 0;
    if (Node* old = alive(oldId); old && old->selAnchor != old->selFocus) {
      old->selAnchor = old->selFocus = 0;
      old->markPaintDirty();
      if (nodeListens(old, "selectionChange")) {
        Value::Object sel;
        sel["start"] = Value(0);
        sel["end"] = Value(0);
        Value::Object ev = baseEvent("selectionchange", old);
        ev["selection"] = Value(std::move(sel));
        ev["text"] = Value("");
        post(std::move(ev));
      }
    }
  }
  root = alive(rootId);  // the handler may have removed it
  if (!root) return;
  selTextId_ = rootId;
  int len = int(text::textContent(root).size());
  anchor = std::clamp(anchor, 0, len);
  focus = std::clamp(focus, 0, len);
  if (anchor == root->selAnchor && focus == root->selFocus) return;
  bool wasEmpty = root->selAnchor == root->selFocus;
  root->selAnchor = anchor;
  root->selFocus = focus;
  if (!wasEmpty || anchor != focus) root->markPaintDirty();
  if (nodeListens(root, "selectionChange")) {
    int a = std::min(anchor, focus), b = std::max(anchor, focus);
    Value::Object sel;
    sel["start"] = Value(a);
    sel["end"] = Value(b);
    Value::Object ev = baseEvent("selectionchange", root);
    ev["selection"] = Value(std::move(sel));
    ev["text"] = Value(edit::toUtf8(text::textContent(root).substr(size_t(a), size_t(b - a))));
    post(std::move(ev));
  }
}

void InputManager::selClear() {
  if (Node* root = alive(selTextId_)) selSet(root, 0, 0);
  selTextId_ = 0;
}

void InputManager::selPointerDown(Node* hit, PointerState& p, int mods) {
  Node* root = selectableRoot(hit);
  if (p.touch) {
    // A tap drops the selection; a press held on selectable text becomes a
    // long-press selection (selTick).
    if (selPointer_ < 0) selClear();
    selPressPointer_ = -1;
    if (root) {
      selPressPointer_ = p.pointerId;
      selPressTextId_ = root->id();
      selPressAt_ = rt_.now;
      selPressX_ = p.x;
      selPressY_ = p.y;
    }
    return;
  }
  if (!root) {
    selClear();
    return;
  }
  uint32_t id = root->id();
  int count = multiClick(id, p);
  float lx = 0, ly = 0;
  if (!toLocal(root, p.x, p.y, &lx, &ly)) return;
  int idx = text::textIndexAt(root, lx, ly);
  const std::u16string& t = text::textContent(root);
  int a = idx, b = idx;
  selPointer_ = p.pointerId;
  if (count == 2) {
    edit::wordAt(t, idx, &a, &b);
    selMode_ = 1;
  } else if (count == 3) {
    paragraphAt(t, idx, &a, &b);
    selMode_ = 2;
  } else {
    selMode_ = 0;
    if ((mods & JSUI_MOD_SHIFT) && selTextId_ == id) a = root->selAnchor;
  }
  selWordStart_ = std::min(a, b);
  selWordEnd_ = std::max(a, b);
  selSet(root, a, b);
}

void InputManager::selPointerDrag(const PointerState& p) {
  Node* root = alive(selTextId_);
  if (!root || !(p.buttons & 1)) {
    selPointer_ = -1;
    return;
  }
  float lx = 0, ly = 0;
  if (!toLocal(root, p.x, p.y, &lx, &ly)) return;
  int idx = text::textIndexAt(root, lx, ly);
  if (selMode_ == 0) {
    selSet(root, root->selAnchor, idx);
    return;
  }
  // By words / paragraphs: the unit the drag started on stays selected.
  int a, b;
  if (selMode_ == 1) edit::wordAt(text::textContent(root), idx, &a, &b);
  else paragraphAt(text::textContent(root), idx, &a, &b);
  if (idx < selWordStart_) selSet(root, selWordEnd_, a);
  else selSet(root, selWordStart_, std::max(b, selWordEnd_));
}

void InputManager::selPointerUp(const PointerState& p) {
  if (p.pointerId == selPointer_) selPointer_ = -1;
  if (p.pointerId == selPressPointer_) selPressPointer_ = -1;
}

bool InputManager::selKey(const std::string& key, int mods) {
  Node* root = alive(selTextId_);
  if (!root || !selectable(root)) return false;
  bool ctrl = (mods & JSUI_MOD_CTRL) != 0, meta = (mods & JSUI_MOD_META) != 0, alt = (mods & JSUI_MOD_ALT) != 0;
  if (!(ctrl || meta) || alt || key.size() != 1) return false;
  char k = char(std::tolower((unsigned char)key[0]));
  const std::u16string& t = text::textContent(root);
  if (k == 'c') {
    int a = std::clamp(std::min(root->selAnchor, root->selFocus), 0, int(t.size()));
    int b = std::clamp(std::max(root->selAnchor, root->selFocus), 0, int(t.size()));
    if (a >= b) return false;
    rt_.setClipboardText(edit::toUtf8(t.substr(size_t(a), size_t(b - a))));
    return true;
  }
  if (k == 'a') {
    selSet(root, 0, int(t.size()));
    return true;
  }
  return false;
}

void InputManager::selTick() {
  if (selPressPointer_ < 0) return;
  auto it = std::find_if(touches_.begin(), touches_.end(),
                         [&](const PointerState& p) { return p.pointerId == selPressPointer_; });
  if (it == touches_.end() || !it->buttons || it->scrollingId || it->jsOwns ||
      std::fabs(it->x - selPressX_) > kTouchSlop || std::fabs(it->y - selPressY_) > kTouchSlop) {
    selPressPointer_ = -1;
    return;
  }
  if (rt_.now - selPressAt_ < kLongPressMs) return;
  selPressPointer_ = -1;
  Node* root = alive(selPressTextId_);
  if (!root || !selectable(root)) return;
  float lx = 0, ly = 0;
  if (!toLocal(root, selPressX_, selPressY_, &lx, &ly)) return;
  int a, b;
  edit::wordAt(text::textContent(root), text::textIndexAt(root, lx, ly), &a, &b);
  // The touch is the selection's now: the press under it is cancelled (no
  // click), and it neither scrolls nor reaches a JS responder.
  PointerState& p = *it;
  p.jsOwns = true;
  p.owned = true;
  p.downId = 0;
  selPointer_ = p.pointerId;
  selMode_ = 1;
  selWordStart_ = a;
  selWordEnd_ = b;
  uint32_t rootId = root->id();
  postPointer("pointercancel", alive(p.captureId), p, 0, p.x, p.y);
  if ((root = alive(rootId))) selSet(root, a, b);
}

}  // namespace jsui
