// Hit testing, pointer / keyboard / gamepad routing and focus navigation.
#include "input/input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "core/document.h"
#include "js/engine.h"
#include "runtime.h"

namespace jsui {

// ------------------------------------------------------------------ Affine

Affine Affine::operator*(const Affine& o) const {
  return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d,
          b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
}

bool Affine::invert(Affine* out) const {
  double det = double(a) * d - double(b) * c;
  if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
  float ia = float(d / det), ib = float(-b / det), ic = float(-c / det), id = float(a / det);
  *out = {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
  return true;
}

Affine transformOpMatrix(const TransformOp& op, float w, float h) {
  constexpr float kDeg = 3.14159265358979f / 180.f;
  switch (op.kind) {
    case TransformOp::Kind::Translate:
      return Affine::translate(op.percentA ? op.a * w / 100.f : op.a, op.percentB ? op.b * h / 100.f : op.b);
    case TransformOp::Kind::Scale: return {op.a, 0, 0, op.b, 0, 0};
    case TransformOp::Kind::Rotate: {
      float s = std::sin(op.a * kDeg), c = std::cos(op.a * kDeg);
      return {c, s, -s, c, 0, 0};
    }
    case TransformOp::Kind::SkewX: return {1, 0, std::tan(op.a * kDeg), 1, 0, 0};
    case TransformOp::Kind::SkewY: return {1, std::tan(op.a * kDeg), 0, 1, 0, 0};
    case TransformOp::Kind::Matrix: return {op.m[0], op.m[1], op.m[2], op.m[3], op.m[4], op.m[5]};
  }
  return {};
}

Affine nodeLocalToParent(const Node* n) {
  Rect r = n->layoutRect();
  const auto& s = n->style();
  if (s.transform.empty()) return Affine::translate(r.x, r.y);
  float ox = s.transformOrigin[0].resolve(r.width), oy = s.transformOrigin[1].resolve(r.height);
  Affine m = Affine::translate(r.x + ox, r.y + oy);
  for (auto& op : s.transform) m = m * transformOpMatrix(op, r.width, r.height);
  return m * Affine::translate(-ox, -oy);
}

// ----------------------------------------------------------------- helpers

namespace {

bool finiteRect(const Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height);
}

bool icontains(std::string_view hay, std::string_view needle) {
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == y;
  });
  return it != hay.end();
}

// What a node listens to, from props.__events (names like "press",
// "pointerMove", "hoverIn", "pointerEnterCapture").
enum Listen : unsigned {
  kPointer = 1,  // any pointer-ish listener: the UI "eats" pointer input here
  kMove = 2,     // wants pointermove
  kHover = 4,    // wants pointerenter / pointerleave
  kPress = 8,    // pressable (focusable by default, "pointer" cursor)
  kWheel = 16,
};

unsigned listenMask(const Node* n) {
  unsigned m = 0;
  for (auto& e : n->props()["__events"].asArray()) {
    std::string_view s = e.asString();
    if (s == "*") return ~0u;
    if (icontains(s, "press") || icontains(s, "click")) m |= kPress | kPointer;
    if (icontains(s, "pointer") || icontains(s, "mouse") || icontains(s, "touch")) m |= kPointer;
    if (icontains(s, "responder") || icontains(s, "drag") || icontains(s, "pan")) m |= kPointer | kMove;
    if (icontains(s, "move")) m |= kMove | kPointer;
    if (icontains(s, "hover") || icontains(s, "enter") || icontains(s, "leave") || icontains(s, "over") ||
        icontains(s, "pointerout") || icontains(s, "mouseout"))
      m |= kHover | kPointer;
    if (icontains(s, "wheel")) m |= kWheel | kPointer;
  }
  return m;
}

PointerEvents pointerEventsOf(const Node* n) {
  PointerEvents pe = n->style().pointerEvents;
  if (pe != PointerEvents::Auto) return pe;
  // Legacy RN prop form (<View pointerEvents="box-none">). Hot path: avoid
  // building a key string per node.
  static const std::string kKey = "pointerEvents";
  const auto& obj = n->props().asObject();
  auto it = obj.find(kKey);
  if (it == obj.end()) return PointerEvents::Auto;
  const std::string& p = it->second.asString();
  if (p.empty() || p == "auto") return PointerEvents::Auto;
  if (p == "none") return PointerEvents::None;
  if (p == "box-none") return PointerEvents::BoxNone;
  if (p == "box-only") return PointerEvents::BoxOnly;
  return PointerEvents::Auto;
}

// Point inside the border box, honoring (CSS-clamped) rounded corners.
bool insideBox(const ComputedStyle& s, float w, float h, float x, float y) {
  if (!(x >= 0 && y >= 0 && x < w && y < h)) return false;
  float rx[4], ry[4];
  bool any = false;
  for (int i = 0; i < 4; i++) {
    rx[i] = std::max(0.f, s.borderRadius[i].resolve(w));
    ry[i] = std::max(0.f, s.borderRadius[i].resolve(h));
    any |= rx[i] > 0 && ry[i] > 0;
  }
  if (!any) return true;
  // CSS: scale all radii down when adjacent ones overlap.
  float k = 1;
  auto fit = [&k](float len, float r1, float r2) {
    if (r1 + r2 > len && r1 + r2 > 0) k = std::min(k, len / (r1 + r2));
  };
  fit(w, rx[0], rx[1]);
  fit(w, rx[3], rx[2]);
  fit(h, ry[0], ry[3]);
  fit(h, ry[1], ry[2]);
  // Corner centers: TL, TR, BR, BL.
  for (int i = 0; i < 4; i++) {
    float a = rx[i] * k, b = ry[i] * k;
    if (a <= 0 || b <= 0) continue;
    float cx = (i == 0 || i == 3) ? a : w - a;
    float cy = (i == 0 || i == 1) ? b : h - b;
    bool inCorner = ((i == 0 || i == 3) ? x < cx : x > cx) && ((i == 0 || i == 1) ? y < cy : y > cy);
    if (inCorner) {
      float dx = (x - cx) / a, dy = (y - cy) / b;
      if (dx * dx + dy * dy > 1) return false;
    }
  }
  return true;
}

int domButtonBit(int button) {
  // DOM `buttons`: 1 primary, 2 secondary, 4 auxiliary (middle).
  switch (button) {
    case 0: return 1;
    case 1: return 4;
    case 2: return 2;
    default: return button < 30 ? 1 << button : 0;
  }
}

Value::Object rectObject(float x, float y, const char* kx, const char* ky) {
  Value::Object o;
  o[kx] = Value(double(x));
  o[ky] = Value(double(y));
  return o;
}

bool truthy(const Value& v) { return v.asBool(false) || (v.isNumber() && v.asNumber() != 0); }

uint32_t idOf(const Node* n) { return n ? n->id() : 0; }

bool scrollEnabled(const Node* n) {
  const Value& v = n->props()["scrollEnabled"];
  return !v.isBool() || v.asBool();
}

}  // namespace

// ---------------------------------------------------------- InputManager

InputManager::InputManager(Runtime& rt) : rt_(rt) {
  mouse_.pointerId = kMousePointerId;
  zOrder_.reserve(256);
  chainBuild_.reserve(64);
}

InputManager::~InputManager() = default;

bool InputManager::attached(const Node* n) const {
  const Node* root = rt_.doc.root();
  for (; n; n = n->parent())
    if (n == root) return true;
  return false;
}

bool InputManager::visible(const Node* n) const {
  const Node* root = rt_.doc.root();
  for (; n; n = n->parent()) {
    if (!n->style().display) return false;
    if (n == root) return true;
  }
  return false;
}

Affine InputManager::localToViewport(const Node* n) const {
  if (!n->parent()) return nodeLocalToParent(n);
  const Node* p = n->parent();
  return localToViewport(p) * Affine::translate(-p->scrollX, -p->scrollY) * nodeLocalToParent(n);
}

bool InputManager::toLocal(const Node* n, float x, float y, float* lx, float* ly) const {
  if (!n || !n->yoga()) {
    // RawText: use its Text's coordinates.
    if (n && n->parent()) return toLocal(n->parent(), x, y, lx, ly);
    return false;
  }
  float px = x, py = y;
  if (const Node* p = n->parent()) {
    if (!toLocal(p, x, y, &px, &py)) return false;
    px += p->scrollX;
    py += p->scrollY;
  }
  if (n->style().transform.empty()) {
    Rect r = n->layoutRect();
    *lx = px - r.x;
    *ly = py - r.y;
    return true;
  }
  Affine inv;
  if (!nodeLocalToParent(n).invert(&inv)) return false;
  inv.apply(px, py, lx, ly);
  return true;
}

Rect InputManager::visualRect(const Node* n) const {
  if (!n || !n->yoga()) return {};
  Rect r = n->layoutRect();
  Affine m = localToViewport(n);
  float xs[4], ys[4];
  m.apply(0, 0, &xs[0], &ys[0]);
  m.apply(r.width, 0, &xs[1], &ys[1]);
  m.apply(r.width, r.height, &xs[2], &ys[2]);
  m.apply(0, r.height, &xs[3], &ys[3]);
  float x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
  float y0 = *std::min_element(ys, ys + 4), y1 = *std::max_element(ys, ys + 4);
  return {x0, y0, x1 - x0, y1 - y0};
}

// ------------------------------------------------------------- hit testing

Node* InputManager::hitTest(float x, float y) {
  float lx, ly;
  return hitTest(x, y, &lx, &ly);
}

Node* InputManager::hitTest(float x, float y, float* localX, float* localY) {
  zOrder_.clear();
  return hitNode(rt_.doc.root(), x, y, localX, localY);
}

// (px, py) is in the parent's content space (parent-local + parent scroll).
Node* InputManager::hitNode(Node* n, float px, float py, float* outX, float* outY) {
  if (!n->yoga()) return nullptr;
  const ComputedStyle& s = n->style();
  if (!s.display) return nullptr;
  PointerEvents pe = pointerEventsOf(n);
  if (pe == PointerEvents::None) return nullptr;
  Rect r = n->layoutRect();
  if (!finiteRect(r)) return nullptr;  // never laid out yet

  float lx, ly;
  if (s.transform.empty()) {
    lx = px - r.x;
    ly = py - r.y;
  } else {
    Affine inv;
    if (!nodeLocalToParent(n).invert(&inv)) return nullptr;  // scale(0): nothing to hit
    inv.apply(px, py, &lx, &ly);
  }
  bool inside = insideBox(s, r.width, r.height, lx, ly);
  if (s.overflow != Overflow::Visible && !inside) return nullptr;

  // Text lays its children out as a paragraph: it is a leaf here.
  const auto& kids = n->children();
  if (pe != PointerEvents::BoxOnly && n->type() != NodeType::Text && !kids.empty()) {
    float cx = lx + n->scrollX, cy = ly + n->scrollY;
    bool zSorted = false;
    for (Node* c : kids)
      if (c->style().zIndex != 0) zSorted = true;
    if (!zSorted) {
      for (size_t i = kids.size(); i-- > 0;)
        if (Node* h = hitNode(kids[i], cx, cy, outX, outY)) return h;
    } else {
      // Paint order: stable sort by zIndex; topmost (last painted) first.
      size_t base = zOrder_.size();
      zOrder_.insert(zOrder_.end(), kids.begin(), kids.end());
      auto byZ = [](Node* a, Node* b) { return a->style().zIndex < b->style().zIndex; };
      if (kids.size() <= 32) {
        // Stable insertion sort: no temp buffer, fast on short sibling lists.
        for (size_t i = base + 1; i < zOrder_.size(); i++)
          for (size_t j = i; j > base && byZ(zOrder_[j], zOrder_[j - 1]); j--) std::swap(zOrder_[j], zOrder_[j - 1]);
      } else {
        std::stable_sort(zOrder_.begin() + std::ptrdiff_t(base), zOrder_.end(), byZ);
      }
      Node* hit = nullptr;
      for (size_t i = zOrder_.size(); i-- > base && !hit;) hit = hitNode(zOrder_[i], cx, cy, outX, outY);
      zOrder_.resize(base);
      if (hit) return hit;
    }
  }
  if (pe == PointerEvents::BoxNone || !inside) return nullptr;
  *outX = lx;
  *outY = ly;
  if (n->type() == NodeType::Text && textHitTest) {
    // Spans have no box of their own: coordinates stay paragraph-local.
    if (Node* span = textHitTest(n, lx, ly)) return span;
  }
  return n;
}

bool InputManager::consumes(const Node* hit, unsigned mask) const {
  for (const Node* n = hit; n; n = n->parent()) {
    if (n->type() == NodeType::TextInput) return true;
    if (listenMask(n) & mask) return true;
  }
  return false;
}

Node* InputManager::commonAncestor(Node* a, Node* b) const {
  if (!a || !b) return nullptr;
  for (Node* x = a; x; x = x->parent())
    for (Node* y = b; y; y = y->parent())
      if (x == y) return x;
  return nullptr;
}

// --------------------------------------------------------------- dispatch

Value::Object InputManager::baseEvent(const char* type, const Node* target) const {
  Value::Object ev;
  ev["type"] = Value(type);
  ev["target"] = Value(double(target ? target->id() : rt_.doc.root()->id()));
  ev["timestamp"] = Value(rt_.now);
  return ev;
}

void InputManager::addModifiers(Value::Object& ev) const {
  ev["shiftKey"] = Value((mods_ & JSUI_MOD_SHIFT) != 0);
  ev["ctrlKey"] = Value((mods_ & JSUI_MOD_CTRL) != 0);
  ev["altKey"] = Value((mods_ & JSUI_MOD_ALT) != 0);
  ev["metaKey"] = Value((mods_ & JSUI_MOD_META) != 0);
}

bool InputManager::dispatch(Value::Object&& ev) {
  if (!rt_.js) return false;
  // Only preventDefault() suppresses the default action; a handler that
  // merely observes the event (flag 1) lets it proceed, like the DOM.
  return (rt_.js->dispatchEvent(Value(std::move(ev))) & JsEngine::kDefaultPrevented) != 0;
}

bool InputManager::dispatchPointer(const char* type, Node* target, const PointerState& p, int button, float x,
                                   float y) {
  if (!target) return false;
  Value::Object ev = baseEvent(type, target);
  float lx = 0, ly = 0;
  toLocal(target, x, y, &lx, &ly);
  ev["x"] = Value(double(x));
  ev["y"] = Value(double(y));
  ev["localX"] = Value(double(lx));
  ev["localY"] = Value(double(ly));
  ev["pointerId"] = Value(p.pointerId);
  ev["pointerType"] = Value(p.touch ? "touch" : "mouse");
  ev["button"] = Value(button);
  ev["buttons"] = Value(p.buttons);
  addModifiers(ev);
  return dispatch(std::move(ev));
}

// ------------------------------------------------------------------ hover

void InputManager::updateHover(PointerState& p, Node* hit) {
  chainBuild_.clear();
  for (Node* n = hit; n; n = n->parent()) chainBuild_.push_back(n->id());
  std::reverse(chainBuild_.begin(), chainBuild_.end());
  if (chainBuild_ == p.chain) return;  // common case: no allocation, no dispatch
  std::vector<uint32_t> old = std::move(p.chain);
  p.chain = chainBuild_;
  if (!p.touch) hovered_ = hit;
  size_t common = 0;
  while (common < old.size() && common < p.chain.size() && old[common] == p.chain[common]) common++;
  std::vector<uint32_t> next = p.chain;  // stable copy: handlers may re-enter
  // DOM order: leave bottom-up on the old path, then enter top-down.
  for (size_t i = old.size(); i-- > common;) {
    Node* n = rt_.doc.get(old[i]);
    if (n && (listenMask(n) & kHover)) dispatchPointer("pointerleave", n, p, -1, p.x, p.y);
  }
  for (size_t i = common; i < next.size(); i++) {
    Node* n = rt_.doc.get(next[i]);
    if (n && (listenMask(n) & kHover)) dispatchPointer("pointerenter", n, p, -1, p.x, p.y);
  }
}

void InputManager::leaveAll(PointerState& p) { updateHover(p, nullptr); }

void InputManager::updateCursor(Node* hit) {
  const char* cursor = "default";
  for (Node* n = hit; n; n = n->parent()) {
    if (!n->style().cursor.empty()) {
      rt_.cursor = n->style().cursor;
      return;
    }
    if (n->type() == NodeType::TextInput) {
      cursor = "text";
      break;
    }
    if (listenMask(n) & kPress) {
      cursor = "pointer";
      break;
    }
  }
  rt_.cursor = cursor;
}

// ---------------------------------------------------------------- pointer
//
// Any dispatch can run JS that destroys nodes, so node pointers are never
// held across a dispatch: they are re-resolved from ids (`alive`).

Node* InputManager::alive(uint32_t id) const { return id ? rt_.doc.get(id) : nullptr; }

bool InputManager::mouseMove(float x, float y) {
  bool moved = !mouseInside_ || x != mouse_.x || y != mouse_.y;
  mouse_.x = x;
  mouse_.y = y;
  mouseInside_ = true;
  uint32_t hitId = idOf(hitTest(x, y));
  updateHover(mouse_, alive(hitId));
  updateCursor(alive(hitId));
  uint32_t targetId = mouse_.captureId && alive(mouse_.captureId) ? mouse_.captureId : hitId;
  bool handled = false;
  // pointermove is the hottest event: only cross into JS when someone on the
  // propagation path listens to moves.
  if (moved && alive(targetId) && consumes(alive(targetId), kMove))
    handled = dispatchPointer("pointermove", alive(targetId), mouse_, -1, x, y);
  return handled || consumes(alive(hitId), kPointer) || consumes(alive(targetId), kPointer);
}

void InputManager::focusFromPointer(Node* hit) {
  // Like the web: pressing a focusable element focuses it, pressing anything
  // else blurs.
  keyboardModality_ = false;
  Node* f = hit;
  while (f && !(isFocusable(f) && visible(f))) f = f->parent();
  if (f) setFocus(f);
  else if (focused_) setFocus(nullptr);
}

bool InputManager::pointerDown(PointerState& p, uint32_t hitId, int button) {
  bool first = p.buttons == 0;
  p.buttons |= domButtonBit(button);
  if (first) {
    p.captureId = hitId;
    p.startX = p.x;
    p.startY = p.y;
  }
  if (button == 0) p.downId = hitId;
  Node* target = alive(p.captureId) ? alive(p.captureId) : alive(hitId);
  bool handled = dispatchPointer("pointerdown", target, p, button, p.x, p.y);
  // Focus follows the press even when JS handled pointerdown: the bridge
  // reports "handled" and "preventDefault" alike, and a Pressable always
  // handles it.
  if (button == 0) focusFromPointer(alive(hitId));
  return handled || consumes(alive(hitId), kPointer);
}

bool InputManager::pointerUp(PointerState& p, uint32_t hitId, int button, bool cancel) {
  p.buttons &= ~domButtonBit(button);
  uint32_t targetId = alive(p.captureId) ? p.captureId : hitId;
  bool handled = false;
  if (cancel) {
    handled = dispatchPointer("pointercancel", alive(targetId), p, button, p.x, p.y);
  } else if (!p.scrollingId) {
    handled = dispatchPointer("pointerup", alive(targetId), p, button, p.x, p.y);
    if (button == 0 && p.downId) {
      // DOM rule: click goes to the nearest common ancestor of the down and
      // up targets. Re-hit: pointerup handlers may have changed the tree.
      Node* down = alive(p.downId);
      Node* up = hitTest(p.x, p.y);
      if (Node* ct = commonAncestor(down, up)) handled |= dispatchPointer("click", ct, p, 0, p.x, p.y);
    }
  }
  if (button == 0) p.downId = 0;
  if (p.buttons == 0 || cancel) {
    p.buttons = 0;
    p.captureId = 0;
    p.scrollingId = 0;
  }
  return handled || consumes(alive(hitId), kPointer) || consumes(alive(targetId), kPointer);
}

bool InputManager::mouseButton(float x, float y, int button, bool down, int mods) {
  mods_ = mods;
  bool consumed = false;
  if (!mouseInside_ || x != mouse_.x || y != mouse_.y) consumed = mouseMove(x, y);
  uint32_t hitId = idOf(hitTest(x, y));
  int bit = domButtonBit(button);
  if (down) {
    if (mouse_.buttons & bit) return consumed || consumes(alive(hitId), kPointer);  // duplicate down
    return pointerDown(mouse_, hitId, button) || consumed;
  }
  if (!(mouse_.buttons & bit)) return consumed || consumes(alive(hitId), kPointer);  // stray up
  bool r = pointerUp(mouse_, hitId, button, false) || consumed;
  // Capture released: the pointer may now be over something else.
  if (mouse_.buttons == 0 && mouseInside_) {
    uint32_t h = idOf(hitTest(x, y));
    updateHover(mouse_, alive(h));
    updateCursor(alive(h));
  }
  return r;
}

bool InputManager::mouseLeave() {
  mouseInside_ = false;
  leaveAll(mouse_);
  hovered_ = nullptr;
  rt_.cursor = "default";
  return false;
}

bool InputManager::mouseWheel(float x, float y, float dx, float dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
  if (!mouseInside_ || x != mouse_.x || y != mouse_.y) mouseMove(x, y);
  Node* target = hitTest(x, y);
  if (!target) return false;  // outside the viewport
  Value::Object ev = baseEvent("wheel", target);
  float lx = 0, ly = 0;
  toLocal(target, x, y, &lx, &ly);
  ev["x"] = Value(double(x));
  ev["y"] = Value(double(y));
  ev["localX"] = Value(double(lx));
  ev["localY"] = Value(double(ly));
  ev["deltaX"] = Value(double(dx));
  ev["deltaY"] = Value(double(dy));
  ev["pointerType"] = Value("mouse");
  addModifiers(ev);
  if (dispatch(std::move(ev))) return true;
  Node* hit = hitTest(x, y);
  // Default action: scroll the nearest ScrollView that can move this way.
  // A vertical wheel over a horizontal list scrolls it sideways (games UX).
  for (Node* n = hit; n; n = n->parent()) {
    if (n->type() != NodeType::ScrollView || !scrollEnabled(n)) continue;
    bool horizontal = n->props()["horizontal"].asBool();
    float d = horizontal ? (dx != 0 ? dx : dy) : dy;
    if (d == 0) continue;
    if (horizontal ? scrollTo(n, n->scrollX + d, n->scrollY) : scrollTo(n, n->scrollX, n->scrollY + d)) return true;
  }
  return consumes(alive(idOf(hit)), kWheel);
}

bool InputManager::touch(int id, int phase, float x, float y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return false;
  // touches_ is only resized here (JS cannot inject input), so the index
  // stays valid across the dispatches below.
  size_t index = 0;
  while (index < touches_.size() && touches_[index].touchId != id) index++;
  if (phase == 0) {
    if (index == touches_.size()) touches_.emplace_back();
    PointerState& p = touches_[index];
    p.touch = true;
    p.touchId = id;
    p.pointerId = id + 1;
    p.x = x;
    p.y = y;
    uint32_t hitId = idOf(hitTest(x, y));
    updateHover(p, alive(hitId));
    if (p.buttons) return consumes(alive(hitId), kPointer);  // duplicate begin
    return pointerDown(p, hitId, 0);
  }
  if (index == touches_.size()) return false;
  PointerState& p = touches_[index];
  p.x = x;
  p.y = y;
  uint32_t hitId = idOf(hitTest(x, y));
  if (phase == 1) {
    if (Node* sv = alive(p.scrollingId)) {
      scrollTo(sv, sv->scrollX - (x - p.startX), sv->scrollY - (y - p.startY));
      p.startX = x;
      p.startY = y;
      return true;
    }
    updateHover(p, alive(hitId));
    uint32_t targetId = alive(p.captureId) ? p.captureId : hitId;
    bool handled = false;
    if (Node* t = alive(targetId); t && consumes(t, kMove)) handled = dispatchPointer("pointermove", t, p, -1, x, y);
    // Drag past the slop inside a ScrollView turns into scrolling (the
    // pressable under the finger gets pointercancel), like touch-action:auto.
    float ddx = x - p.startX, ddy = y - p.startY;
    if (!handled && p.buttons && (std::fabs(ddx) > kTouchSlop || std::fabs(ddy) > kTouchSlop)) {
      Node* from = alive(targetId) ? alive(targetId) : alive(hitId);
      if (Node* sv = scrollableAncestor(from, -ddx, -ddy)) {
        uint32_t svId = sv->id();
        dispatchPointer("pointercancel", alive(targetId), p, 0, x, y);
        p.scrollingId = svId;
        p.downId = 0;
        if ((sv = alive(svId))) scrollTo(sv, sv->scrollX - ddx, sv->scrollY - ddy);
        p.startX = x;
        p.startY = y;
        return true;
      }
    }
    return handled || consumes(alive(hitId), kPointer) || consumes(alive(targetId), kPointer);
  }
  // End / cancel.
  bool wasScrolling = p.scrollingId != 0;
  bool r = pointerUp(p, hitId, 0, phase == 3);
  leaveAll(p);
  touches_.erase(touches_.begin() + std::ptrdiff_t(index));
  return r || wasScrolling;
}

// --------------------------------------------------------------- scrolling

void InputManager::contentSize(const Node* sv, float* w, float* h) const {
  Rect r = sv->layoutRect();
  float right = 0, bottom = 0;
  for (Node* c : sv->children()) {
    if (!c->yoga() || !c->style().display) continue;
    Rect cr = c->layoutRect();
    if (!finiteRect(cr)) continue;
    right = std::max(right, cr.x + cr.width + YGNodeLayoutGetMargin(c->yoga(), YGEdgeRight));
    bottom = std::max(bottom, cr.y + cr.height + YGNodeLayoutGetMargin(c->yoga(), YGEdgeBottom));
  }
  YGNodeRef y = sv->yoga();
  right += YGNodeLayoutGetPadding(y, YGEdgeRight) + YGNodeLayoutGetBorder(y, YGEdgeRight);
  bottom += YGNodeLayoutGetPadding(y, YGEdgeBottom) + YGNodeLayoutGetBorder(y, YGEdgeBottom);
  *w = std::max(right, r.width);
  *h = std::max(bottom, r.height);
}

Node* InputManager::scrollableAncestor(Node* n, float dx, float dy) const {
  for (; n; n = n->parent()) {
    if (n->type() != NodeType::ScrollView) continue;
    if (!scrollEnabled(n)) continue;
    float cw, ch;
    contentSize(n, &cw, &ch);
    Rect r = n->layoutRect();
    bool horizontal = n->props()["horizontal"].asBool();
    if (horizontal && std::fabs(dx) >= std::fabs(dy) && cw > r.width) return n;
    if (!horizontal && std::fabs(dy) >= std::fabs(dx) && ch > r.height) return n;
  }
  return nullptr;
}

bool InputManager::dispatchScroll(Node* sv) {
  float cw, ch;
  contentSize(sv, &cw, &ch);
  Rect r = sv->layoutRect();
  Value::Object ev = baseEvent("scroll", sv);
  ev["contentOffset"] = Value(rectObject(sv->scrollX, sv->scrollY, "x", "y"));
  ev["contentSize"] = Value(rectObject(cw, ch, "width", "height"));
  ev["layoutMeasurement"] = Value(rectObject(r.width, r.height, "width", "height"));
  return dispatch(std::move(ev));
}

bool InputManager::scrollTo(Node* sv, float x, float y) {
  if (!sv || !sv->yoga()) return false;
  float cw, ch;
  contentSize(sv, &cw, &ch);
  Rect r = sv->layoutRect();
  float maxX = std::max(0.f, cw - r.width), maxY = std::max(0.f, ch - r.height);
  if (!std::isfinite(x)) x = sv->scrollX;
  if (!std::isfinite(y)) y = sv->scrollY;
  x = std::clamp(x, 0.f, maxX);
  y = std::clamp(y, 0.f, maxY);
  if (x == sv->scrollX && y == sv->scrollY) return false;
  sv->scrollX = x;
  sv->scrollY = y;
  rt_.doc.markPaintDirty();
  hoverDirty_ = true;
  dispatchScroll(sv);
  return true;
}

void InputManager::scrollIntoView(Node* n) {
  if (!n || !n->yoga()) return;
  uint32_t id = n->id();
  for (Node* p = n->parent(); p; p = p->parent()) {
    if (p->type() != NodeType::ScrollView) continue;
    Node* cur = rt_.doc.get(id);
    if (!cur) return;
    Rect nr = cur->absoluteRect(), pr = p->absoluteRect();
    float sx = p->scrollX, sy = p->scrollY;
    // Align the near edge if the node is before the viewport (or larger than
    // it), the far edge if it is after.
    if (nr.x < pr.x || nr.width > pr.width) sx -= pr.x - nr.x;
    else if (nr.x + nr.width > pr.x + pr.width) sx += (nr.x + nr.width) - (pr.x + pr.width);
    if (nr.y < pr.y || nr.height > pr.height) sy -= pr.y - nr.y;
    else if (nr.y + nr.height > pr.y + pr.height) sy += (nr.y + nr.height) - (pr.y + pr.height);
    uint32_t pid = p->id();
    scrollTo(p, sx, sy);
    p = rt_.doc.get(pid);  // the scroll handler may have destroyed it
    if (!p) return;
  }
}

// ------------------------------------------------------------------ focus

bool InputManager::isFocusable(const Node* n) const {
  if (!n || n->type() == NodeType::RawText) return false;
  const Value& props = n->props();
  if (truthy(props["disabled"])) return false;
  const Value& f = props["focusable"];
  if (f.isBool()) return f.asBool();
  if (n->type() == NodeType::TextInput) return props["editable"].asBool(true) || !props["editable"].isBool();
  return (listenMask(n) & kPress) != 0;
}

void InputManager::focus(Node* n) { setFocus(n); }

void InputManager::blur(Node* n) {
  if (!n || n == focused_) setFocus(nullptr);
}

void InputManager::setFocus(Node* n) {
  if (n && (!attached(n) || n->type() == NodeType::RawText)) return;
  if (n == focused_) return;
  uint32_t oldId = idOf(focused_), newId = idOf(n);
  focused_ = nullptr;
  if (Node* old = alive(oldId)) {
    // DOM order: blur the old element first (nothing is focused meanwhile).
    Value::Object ev = baseEvent("blur", old);
    ev["relatedTarget"] = newId ? Value(double(newId)) : Value();
    dispatch(std::move(ev));
    // The blur handler may have moved focus elsewhere or destroyed `n`.
    if (focused_) return;
  }
  n = alive(newId);
  if (!n || !attached(n)) return;
  focused_ = n;
  scrollIntoView(n);
  n = alive(newId);
  if (!n || focused_ != n) return;
  Value::Object ev = baseEvent("focus", n);
  ev["focusVisible"] = Value(keyboardModality_);
  ev["relatedTarget"] = alive(oldId) ? Value(double(oldId)) : Value();
  dispatch(std::move(ev));
}

void InputManager::nodeRemoved(Node* n) {
  if (focused_ == n) focused_ = nullptr;
  if (hovered_ == n) hovered_ = nullptr;
  autoFocusSeen_.erase(n->id());
}

Node* InputManager::modalScope() const {
  // The last displayed modal subtree in tree order traps focus
  // (accessibilityViewIsModal / aria-modal), like a dialog.
  Node* scope = rt_.doc.root();
  std::vector<Node*> stack{rt_.doc.root()};
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n->yoga() || !n->style().display) continue;
    const Value& p = n->props();
    if (n != rt_.doc.root() && (truthy(p["accessibilityViewIsModal"]) || truthy(p["aria-modal"]))) scope = n;
    if (n->type() == NodeType::Text) continue;
    for (size_t i = n->children().size(); i-- > 0;) stack.push_back(n->children()[i]);
  }
  return scope;
}

void InputManager::collectFocusable(Node* scope, std::vector<Node*>* out) const {
  out->clear();
  if (!scope || !visible(scope)) return;
  std::vector<Node*> stack{scope};
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n->yoga() || !n->style().display) continue;
    if (isFocusable(n)) out->push_back(n);
    for (size_t i = n->children().size(); i-- > 0;) stack.push_back(n->children()[i]);
  }
}

Node* InputManager::preferredFocus(Node* scope) const {
  std::vector<Node*> list;
  collectFocusable(scope, &list);
  for (Node* n : list)
    if (truthy(n->props()["hasTVPreferredFocus"]) || truthy(n->props()["autoFocus"])) return n;
  return list.empty() ? nullptr : list.front();
}

bool InputManager::focusedUsable() const {
  return focused_ && isFocusable(focused_) && visible(focused_);
}

bool InputManager::focusNext(bool backward) {
  keyboardModality_ = true;
  std::vector<Node*> list;
  collectFocusable(modalScope(), &list);
  if (list.empty()) return false;
  auto it = std::find(list.begin(), list.end(), focused_);
  Node* next;
  if (it == list.end()) next = backward ? list.back() : list.front();
  else if (backward) next = it == list.begin() ? list.back() : *(it - 1);
  else next = (it + 1 == list.end()) ? list.front() : *(it + 1);
  setFocus(next);
  return true;
}

bool InputManager::navigate(NavDirection dir) {
  keyboardModality_ = true;
  Node* scope = modalScope();
  bool inScope = false;
  if (focusedUsable())
    for (Node* n = focused_; n; n = n->parent())
      if (n == scope) inScope = true;
  if (!inScope) {
    Node* pref = preferredFocus(scope);
    if (!pref) return false;
    setFocus(pref);
    return focused_ == pref;
  }
  Node* cur = focused_;
  // Explicit overrides (RN nextFocusUp/Down/Left/Right take a node id).
  static const char* kNext[] = {"nextFocusUp", "nextFocusDown", "nextFocusLeft", "nextFocusRight"};
  const Value& explicitNext = cur->props()[kNext[int(dir)]];
  if (explicitNext.isNumber()) {
    Node* t = rt_.doc.get(uint32_t(explicitNext.asNumber()));
    if (t && isFocusable(t) && visible(t)) {
      setFocus(t);
      return true;
    }
  }

  std::vector<Node*> list;
  collectFocusable(scope, &list);
  Rect c = visualRect(cur);
  bool vertical = dir == NavDirection::Up || dir == NavDirection::Down;
  float sign = (dir == NavDirection::Down || dir == NavDirection::Right) ? 1.f : -1.f;
  // Project rects onto the movement axis (main) and the other one (cross).
  auto mainLo = [&](const Rect& r) { return vertical ? r.y : r.x; };
  auto mainHi = [&](const Rect& r) { return vertical ? r.y + r.height : r.x + r.width; };
  auto crossLo = [&](const Rect& r) { return vertical ? r.x : r.y; };
  auto crossHi = [&](const Rect& r) { return vertical ? r.x + r.width : r.y + r.height; };
  constexpr float kEps = 0.5f;

  Node* best = nullptr;
  for (int pass = 0; pass < 2 && !best; pass++) {
    int bestOverlap = 0;
    float bestScore = std::numeric_limits<float>::infinity(), bestCenter = bestScore;
    for (Node* n : list) {
      if (n == cur) continue;
      Rect r = visualRect(n);
      if (!finiteRect(r) || r.width <= 0 || r.height <= 0) continue;
      float gap;
      if (pass == 0) {
        // Strict half-plane beyond the current rect's leading edge.
        gap = sign > 0 ? mainLo(r) - mainHi(c) : mainLo(c) - mainHi(r);
        if (gap < -kEps) continue;
      } else {
        // Overlapping layouts: fall back to centers.
        float d = ((mainLo(r) + mainHi(r)) - (mainLo(c) + mainHi(c))) * 0.5f * sign;
        if (d <= kEps) continue;
        gap = d;
      }
      float overlap = std::min(crossHi(r), crossHi(c)) - std::max(crossLo(r), crossLo(c));
      float crossGap = overlap > 0 ? 0 : -overlap;
      float score = std::max(0.f, gap) + 2 * crossGap;
      float center = std::fabs((crossLo(r) + crossHi(r)) - (crossLo(c) + crossHi(c))) * 0.5f;
      int ov = overlap > 0 ? 1 : 0;
      if (ov > bestOverlap || (ov == bestOverlap && (score < bestScore || (score == bestScore && center < bestCenter)))) {
        best = n;
        bestOverlap = ov;
        bestScore = score;
        bestCenter = center;
      }
    }
  }
  if (!best) return false;
  setFocus(best);
  return focused_ == best;
}

void InputManager::tickFocusScopes() {
  Node* scope = modalScope();
  uint32_t sid = scope->id();
  auto inside = [](const Node* n, const Node* s) {
    for (; n; n = n->parent())
      if (n == s) return true;
    return false;
  };
  // New autoFocus / hasTVPreferredFocus nodes take focus when nothing (or
  // nothing inside the active scope) is focused.
  uint32_t autoId = 0;
  std::vector<Node*> stack{rt_.doc.root()};
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n->yoga() || !n->style().display) continue;
    const Value& p = n->props();
    if ((truthy(p["autoFocus"]) || truthy(p["hasTVPreferredFocus"])) && autoFocusSeen_.insert(n->id()).second &&
        !autoId && inside(n, scope) && isFocusable(n))
      autoId = n->id();
    for (size_t i = n->children().size(); i-- > 0;) stack.push_back(n->children()[i]);
  }

  if (sid != scopeId_) {
    // Modal scopes: entering one remembers where focus was, leaving restores
    // it. Pop every memo whose modal is gone (destroyed or hidden); the last
    // popped is the outermost closed modal, whose memo holds the focus from
    // before it opened.
    uint32_t restoreId = 0;
    while (!scopeStack_.empty() && scopeStack_.back().scopeId != sid) {
      Node* s = alive(scopeStack_.back().scopeId);
      if (s && visible(s)) break;
      restoreId = scopeStack_.back().restoreId;
      scopeStack_.pop_back();
    }
    bool entering = scope != rt_.doc.root() && (scopeStack_.empty() || scopeStack_.back().scopeId != sid);
    if (entering) scopeStack_.push_back({sid, idOf(focused_)});
    scopeId_ = sid;
    if (entering) {
      if (!autoId && focused_ && !inside(focused_, scope)) setFocus(preferredFocus(scope));  // nullptr blurs
    } else if (Node* r = alive(restoreId);
               r && visible(r) && inside(r, scope) && (!focusedUsable() || !inside(focused_, scope))) {
      setFocus(r);
    }
  }
  scope = alive(sid);  // focus handlers run JS
  Node* a = alive(autoId);
  if (scope && a && (!focusedUsable() || !inside(focused_, scope))) setFocus(a);
}

// --------------------------------------------------------------- keyboard

bool InputManager::key(const char* keyName, const char* code, bool down, bool repeat, int mods) {
  mods_ = mods;
  keyboardModality_ = true;
  std::string k = keyName ? keyName : "";
  Value::Object ev = baseEvent(down ? "keydown" : "keyup", focused_);
  ev["key"] = Value(k);
  ev["code"] = Value(code ? code : "");
  ev["repeat"] = Value(repeat);
  addModifiers(ev);
  bool handled = dispatch(std::move(ev));
  if (handled) return true;
  bool inTextInput = focused_ && focused_->type() == NodeType::TextInput;
  if (!down) return inTextInput;
  if (k == "Tab") {
    return focusNext((mods & JSUI_MOD_SHIFT) != 0);
  }
  if ((k == "Enter" || k == " " || k == "Spacebar") && !repeat && focusedUsable() && !inTextInput) {
    Node* f = focused_;
    Rect r = visualRect(f);
    Value::Object click = baseEvent("click", f);
    click["x"] = Value(double(r.x + r.width / 2));
    click["y"] = Value(double(r.y + r.height / 2));
    click["localX"] = Value(double(f->layoutRect().width / 2));
    click["localY"] = Value(double(f->layoutRect().height / 2));
    click["button"] = Value(0);
    click["buttons"] = Value(0);
    click["pointerType"] = Value("keyboard");
    dispatch(std::move(click));
    return true;
  }
  if (arrowKeyNavigation && !inTextInput) {
    int dir = k == "ArrowUp" ? 0 : k == "ArrowDown" ? 1 : k == "ArrowLeft" ? 2 : k == "ArrowRight" ? 3 : -1;
    if (dir >= 0) return navigate(NavDirection(dir));
  }
  return inTextInput;
}

bool InputManager::text(const char* utf8) {
  if (!utf8 || !*utf8) return false;
  Value::Object ev = baseEvent("textinput", focused_);
  ev["text"] = Value(utf8);
  bool handled = dispatch(std::move(ev));
  return handled || (focused_ && focused_->type() == NodeType::TextInput);
}

// ---------------------------------------------------------------- gamepad

bool InputManager::navStep(int dir) {
  if (dir < 0) return false;
  return navigate(NavDirection(dir));
}

void InputManager::padRepeat(Repeat& r, int dir) {
  r.dir = dir;
  r.next = rt_.now + kRepeatDelayMs;
}

bool InputManager::gamepadButton(int pad, int button, bool down, float value) {
  keyboardModality_ = true;
  Pad* ps = (pad >= 0 && pad < int(pads_.size())) ? &pads_[size_t(pad)] : nullptr;
  bool wasDown = ps && button >= 0 && button < 32 && (ps->buttons >> button) & 1u;
  if (ps && button >= 0 && button < 32) {
    if (down) ps->buttons |= 1u << button;
    else ps->buttons &= ~(1u << button);
  }
  Value::Object ev = baseEvent("gamepadbutton", focused_);
  ev["gamepad"] = Value(pad);
  ev["buttonIndex"] = Value(button);
  ev["value"] = Value(double(value));
  ev["pressed"] = Value(down);
  ev["repeat"] = Value(down && wasDown);
  bool handled = dispatch(std::move(ev));
  bool edge = down != wasDown || !ps;
  if (!edge) return handled || focused_ != nullptr;
  // D-pad: 12 up, 13 down, 14 left, 15 right.
  int dir = button == 12 ? 0 : button == 13 ? 1 : button == 14 ? 2 : button == 15 ? 3 : -1;
  if (dir >= 0) {
    if (!down) {
      if (ps && ps->dpad.dir == dir) ps->dpad.dir = -1;
      return handled;
    }
    if (handled) return true;
    if (ps) padRepeat(ps->dpad, dir);
    navStep(dir);
    return true;
  }
  if (button == 0 && down && !handled && focusedUsable()) {
    Node* f = focused_;
    Rect r = visualRect(f);
    Value::Object click = baseEvent("click", f);
    click["x"] = Value(double(r.x + r.width / 2));
    click["y"] = Value(double(r.y + r.height / 2));
    click["localX"] = Value(double(f->layoutRect().width / 2));
    click["localY"] = Value(double(f->layoutRect().height / 2));
    click["button"] = Value(0);
    click["buttons"] = Value(0);
    click["pointerType"] = Value("gamepad");
    click["gamepad"] = Value(pad);
    dispatch(std::move(click));
    return true;
  }
  return handled;
}

bool InputManager::gamepadAxis(int pad, int axis, float value) {
  if (!std::isfinite(value)) return false;
  Pad* ps = (pad >= 0 && pad < int(pads_.size())) ? &pads_[size_t(pad)] : nullptr;
  if (ps && axis >= 0 && axis < 4) {
    if (ps->axes[axis] == value) return false;  // hosts often resend unchanged axes each frame
    ps->axes[axis] = value;
  }
  keyboardModality_ = true;
  Value::Object ev = baseEvent("gamepadaxis", focused_);
  ev["gamepad"] = Value(pad);
  ev["axis"] = Value(axis);
  ev["value"] = Value(double(value));
  bool handled = dispatch(std::move(ev));
  if (!ps || (axis != 0 && axis != 1)) return handled;
  // Left stick -> direction of the dominant axis beyond the deadzone.
  float lx = ps->axes[0], ly = ps->axes[1];
  int dir = -1;
  if (std::fabs(lx) >= std::fabs(ly)) {
    if (std::fabs(lx) > kStickDeadzone) dir = lx > 0 ? 3 : 2;
  } else if (std::fabs(ly) > kStickDeadzone) {
    dir = ly > 0 ? 1 : 0;
  }
  if (handled) {
    ps->stick.dir = -1;
    return true;
  }
  if (dir == ps->stick.dir) return dir >= 0;
  if (dir < 0) {
    ps->stick.dir = -1;
    return false;
  }
  padRepeat(ps->stick, dir);
  navStep(dir);
  return true;
}

void InputManager::gamepadConnected(int pad, bool connected, const char* id) {
  if (pad >= 0 && pad < int(pads_.size()) && !connected) pads_[size_t(pad)] = Pad();
  Value::Object ev = baseEvent("gamepadconnected", focused_);
  ev["gamepad"] = Value(pad);
  ev["connected"] = Value(connected);
  ev["id"] = Value(id ? id : "");
  dispatch(std::move(ev));
}

// ------------------------------------------------------------------- tick

void InputManager::tick(double nowMs) {
  // Held D-pad / stick repeat: first after kRepeatDelayMs, then every
  // kRepeatIntervalMs. A long frame fires once (no bursts).
  for (Pad& p : pads_) {
    for (Repeat* r : {&p.dpad, &p.stick}) {
      if (r->dir >= 0 && nowMs >= r->next) {
        r->next = nowMs + kRepeatIntervalMs;
        navStep(r->dir);
      }
    }
  }
  bool laidOut = rt_.doc.layout();
  if (laidOut) tickFocusScopes();
  // The UI may have moved under a still mouse: refresh enter/leave + cursor.
  if (mouseInside_ && (laidOut || hoverDirty_ || rt_.doc.paintDirty())) {
    hoverDirty_ = false;
    Node* hit = hitTest(mouse_.x, mouse_.y);
    updateHover(mouse_, hit);
    updateCursor(hit);
  }
}

}  // namespace jsui
