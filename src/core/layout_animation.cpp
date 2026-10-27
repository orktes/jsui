// LayoutAnimation (React Native's LayoutAnimation.configureNext): the layout
// pass after Document::armLayoutAnimation() animates what changed.
//
// * update: nodes whose parent-relative frame changed move / resize from the
//   frame they were drawn at to their new layout (Node::visualRect(), used by
//   painting and hit testing; measure() and onLayout report the final frame).
//   The target is read live, so a later relayout retargets smoothly.
// * create: nodes that appear (the root of each inserted subtree) fade in
//   (`opacity`) or grow from their center (`scaleX` / `scaleY` / `scaleXY`).
// * delete: children removed by JS while armed become ghosts of their old
//   parent: detached from layout and hit testing, painted below the
//   children at their last frame while they fade / shrink out, then freed
//   (once JS destroyed them, which React always does).
//
// Curves: linear, easeInEaseOut / easeIn / easeOut (CSS cubic-beziers, like
// iOS), keyboard, and spring (damped oscillation with springDamping as the
// damping ratio, settling within the duration).
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/document.h"

namespace jsui {

namespace {

// y of a CSS cubic-bezier(x1, y1, x2, y2) at x.
float cubicBezier(float x1, float y1, float x2, float y2, float x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  auto bx = [&](float t) { return ((1 - 3 * x2 + 3 * x1) * t + (3 * x2 - 6 * x1)) * t * t + 3 * x1 * t; };
  auto by = [&](float t) { return ((1 - 3 * y2 + 3 * y1) * t + (3 * y2 - 6 * y1)) * t * t + 3 * y1 * t; };
  auto dx = [&](float t) { return 3 * (1 - 3 * x2 + 3 * x1) * t * t + 2 * (3 * x2 - 6 * x1) * t + 3 * x1; };
  float t = x;
  for (int i = 0; i < 8; i++) {
    float d = dx(t);
    if (std::fabs(d) < 1e-6f) break;
    float nt = t - (bx(t) - x) / d;
    if (nt < 0 || nt > 1) break;
    t = nt;
  }
  // Bisection polish (also the fallback when Newton left the interval).
  float lo = 0, hi = 1;
  for (int i = 0; i < 20 && std::fabs(bx(t) - x) > 1e-5f; i++) {
    if (bx(t) < x) lo = t;
    else hi = t;
    t = (lo + hi) / 2;
  }
  return by(t);
}

bool finite(const Rect& r) { return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height); }

bool differs(const Rect& a, const Rect& b) {
  const float e = 0.01f;
  return std::fabs(a.x - b.x) > e || std::fabs(a.y - b.y) > e || std::fabs(a.width - b.width) > e ||
         std::fabs(a.height - b.height) > e;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

float layoutAnimProgress(LayoutAnimCurve curve, float springDamping, float t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  switch (curve) {
    case LayoutAnimCurve::Linear: return t;
    case LayoutAnimCurve::EaseInEaseOut: return cubicBezier(0.42f, 0, 0.58f, 1, t);
    case LayoutAnimCurve::EaseIn: return cubicBezier(0.42f, 0, 1, 1, t);
    case LayoutAnimCurve::EaseOut: return cubicBezier(0, 0, 0.58f, 1, t);
    case LayoutAnimCurve::Keyboard: return cubicBezier(0.17f, 0.59f, 0.4f, 0.77f, t);
    case LayoutAnimCurve::Spring: {
      // Damping ratio z; the natural frequency makes the envelope decay to
      // ~0.1 % by the end, like a UIKit spring animation of that duration.
      float z = std::clamp(springDamping, 0.05f, 1.f);
      float w0 = 6.9f / z;
      if (z >= 0.999f) return 1 - (1 + w0 * t) * std::exp(-w0 * t);
      float wd = w0 * std::sqrt(1 - z * z);
      return 1 - std::exp(-z * w0 * t) * (std::cos(wd * t) + (z * w0 / wd) * std::sin(wd * t));
    }
  }
  return t;
}

void Document::snapshotLayout(Node* node, std::vector<std::pair<Node*, Rect>>* out) {
  if (!node->yoga()) return;
  Rect r = node->visualRect();
  // Yoga forgets the layout of a child it detaches (YGNodeRemoveChild /
  // YGNodeRemoveAllChildren), as Node::rebuildYoga does to every child of a
  // node whose render children change: the root's when the first overlay
  // adds the overlay layer, an overlay's parent's when it leaves its Yoga
  // children. Such a node is not new: its frame is the last one afterLayout
  // recorded (boundsLayout; width -1 until the first). Taking it for new made
  // it the root of an inserted subtree, and the create animation faded the
  // whole app in from opacity 0.
  if (!finite(r) && node->boundsLayout.width >= 0) r = node->boundsLayout;
  if (!finite(r)) return;  // never laid out: new
  out->emplace_back(node, r);
  if (node->type() == NodeType::Text || node->type() == NodeType::TextInput) return;  // paragraph content
  for (Node* c : node->children()) snapshotLayout(c, out);
}

void Document::startLayoutAnimations(const std::vector<std::pair<Node*, Rect>>& before) {
  std::unordered_map<Node*, Rect> old;
  old.reserve(before.size());
  for (auto& [n, r] : before) old.emplace(n, r);
  const LayoutAnimConfig cfg = armed_;
  std::vector<Node*> stack(root_->children().rbegin(), root_->children().rend());
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n->yoga()) continue;
    auto it = old.find(n);
    if (it != old.end()) {
      if (cfg.update.enabled && !n->nativeOnly && differs(it->second, n->layoutRect())) startAnim(n, Node::LayoutAnim::Kind::Update, cfg.update, it->second);
      if (n->type() == NodeType::Text || n->type() == NodeType::TextInput) continue;
      for (size_t i = n->children().size(); i-- > 0;) stack.push_back(n->children()[i]);
    } else if (cfg.create.enabled && n->parent() && old.count(n->parent()) && finite(n->layoutRect())) {
      // Root of an inserted subtree; its descendants appear with it.
      startAnim(n, Node::LayoutAnim::Kind::Create, cfg.create, {});
    }
  }
}

void Document::startAnim(Node* n, Node::LayoutAnim::Kind kind, const LayoutAnimSpec& spec, const Rect& from) {
  bool had = n->layoutAnim != nullptr;
  n->layoutAnim = std::make_unique<Node::LayoutAnim>();
  Node::LayoutAnim& a = *n->layoutAnim;
  a.kind = kind;
  a.curve = uint8_t(spec.curve);
  a.property = uint8_t(spec.property);
  a.springDamping = spec.springDamping;
  a.start = now + spec.delay;
  a.duration = spec.duration;
  a.from = from;
  a.current = from;
  if (!had || std::find(animating_.begin(), animating_.end(), n->id()) == animating_.end()) animating_.push_back(n->id());
  bool done = false;
  applyAnim(n, now, &done);  // the first frame already shows the start state
}

void Document::applyAnim(Node* n, double nowMs, bool* done) {
  Node::LayoutAnim& a = *n->layoutAnim;
  double t = a.duration > 0 ? (nowMs - a.start) / a.duration : (nowMs >= a.start ? 1 : 0);
  t = std::clamp(t, 0.0, 1.0);
  *done = t >= 1;
  float e = layoutAnimProgress(LayoutAnimCurve(a.curve), a.springDamping, float(t));
  switch (a.kind) {
    case Node::LayoutAnim::Kind::Update: {
      Rect to = n->layoutRect();
      a.current = {lerp(a.from.x, to.x, e), lerp(a.from.y, to.y, e), std::max(0.f, lerp(a.from.width, to.width, e)),
                   std::max(0.f, lerp(a.from.height, to.height, e))};
      n->markBoundsDirty();
      break;
    }
    case Node::LayoutAnim::Kind::Create:
    case Node::LayoutAnim::Kind::Delete: {
      float v = a.kind == Node::LayoutAnim::Kind::Create ? e : 1 - e;
      a.opacity = 1;
      a.scaleX = a.scaleY = 1;
      switch (LayoutAnimProperty(a.property)) {
        case LayoutAnimProperty::Opacity: a.opacity = std::clamp(v, 0.f, 1.f); break;
        case LayoutAnimProperty::ScaleX: a.scaleX = std::max(0.f, v); break;
        case LayoutAnimProperty::ScaleY: a.scaleY = std::max(0.f, v); break;
        case LayoutAnimProperty::ScaleXY: a.scaleX = a.scaleY = std::max(0.f, v); break;
      }
      break;
    }
  }
  n->markPaintDirty();
}

void Document::finishAnim(Node* n) {
  bool ghost = n->layoutAnim && n->layoutAnim->kind == Node::LayoutAnim::Kind::Delete;
  n->layoutAnim.reset();
  n->markBoundsDirty();
  n->markPaintDirty();
  if (!ghost || !n->ghostOf) return;
  Node* parent = n->ghostOf;
  auto& g = parent->ghosts;
  g.erase(std::remove(g.begin(), g.end(), n), g.end());
  n->ghostOf = nullptr;
  parent->markPaintDirty();
  if (n->destroyWhenDone) destroyRecursive(n);
}

void Document::tickLayoutAnimations(double nowMs) {
  if (animating_.empty()) return;
  Perf::Scope scope(perf, Phase::Layout);
  std::vector<uint32_t> ids;
  ids.swap(animating_);
  for (uint32_t id : ids) {
    Node* n = get(id);
    if (!n || !n->layoutAnim) continue;
    bool done = false;
    applyAnim(n, nowMs, &done);
    if (done) finishAnim(n);
    else animating_.push_back(id);
  }
}

void Document::removeChild(Node* parent, Node* child) {
  bool ghost = armedSet_ && armed_.remove.enabled && child->parent() == parent && child->yoga() && !child->overlay &&
               parent->type() != NodeType::Text && parent->type() != NodeType::TextInput &&
               finite(child->layoutRect()) && child->style().display;
  if (!ghost) {
    parent->removeChild(child);
    return;
  }
  Rect r = child->visualRect();
  parent->removeChild(child);
  // Yoga forgets a detached node's layout: lay the ghost out on its own at
  // its last size so its borders, padding and children keep their geometry.
  YGNodeStyleSetWidth(child->yoga(), r.width);
  YGNodeStyleSetHeight(child->yoga(), r.height);
  YGNodeCalculateLayout(child->yoga(), r.width, r.height, YGDirectionLTR);
  // Input must not keep pointing into the detached subtree (focus, hover).
  if (onNodeDestroyed) {
    std::vector<Node*> stack{child};
    while (!stack.empty()) {
      Node* n = stack.back();
      stack.pop_back();
      onNodeDestroyed(n);
      for (Node* c : n->children()) stack.push_back(c);
    }
  }
  child->ghostOf = parent;
  parent->ghosts.push_back(child);
  parent->markPaintDirty();
  startAnim(child, Node::LayoutAnim::Kind::Delete, armed_.remove, r);
}

void Document::reviveGhost(Node* g) {
  if (!g->ghostOf) return;
  auto& list = g->ghostOf->ghosts;
  list.erase(std::remove(list.begin(), list.end(), g), list.end());
  g->ghostOf->markPaintDirty();
  g->ghostOf = nullptr;
  g->destroyWhenDone = false;
  g->layoutAnim.reset();
  g->restyle();  // undo the fixed ghost size
}

}  // namespace jsui
