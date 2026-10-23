#include "core/document.h"

#include <algorithm>
#include <cmath>

#include "text/font_manager.h"
#include "text/text_layout.h"

namespace jsui {

Document::Document(uint32_t* sharedNextId, Perf* sharedPerf) : perf(sharedPerf ? *sharedPerf : ownPerf_) {
  if (sharedNextId) idCounter_ = sharedNextId;
  // Layout stays fractional like CSS (no Yoga pixel rounding); the painter
  // snaps box edges to device pixels the way Chrome does.
  yogaConfig_ = YGConfigNew();
  YGConfigSetPointScaleFactor(yogaConfig_, 0);
  root_ = createNode(NodeType::View);
}

Document::~Document() {
  // Free children before parents so Yoga ownership stays consistent.
  if (root_) destroyRecursive(root_);
  // Detached subtrees the reconciler never destroyed: free them top-down too,
  // before the Yoga config they reference.
  while (!nodes_.empty()) {
    Node* n = nodes_.begin()->second.get();
    while (n->parent()) n = n->parent();
    destroyRecursive(n);
  }
  YGConfigFree(yogaConfig_);
}

Node* Document::createNode(NodeType type) {
  uint32_t id = (*idCounter_)++;
  auto node = std::make_unique<Node>(this, id, type);
  Node* raw = node.get();
  nodes_[id] = std::move(node);
  return raw;
}

Node* Document::get(uint32_t id) const {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : it->second.get();
}

void Document::destroyRecursive(Node* node) {
  // Children still fading out (LayoutAnimation delete) go with their parent.
  while (!node->ghosts.empty()) {
    Node* g = node->ghosts.back();
    node->ghosts.pop_back();
    g->ghostOf = nullptr;
    destroyRecursive(g);
  }
  if (node->ghostOf) {
    auto& list = node->ghostOf->ghosts;
    list.erase(std::remove(list.begin(), list.end(), node), list.end());
    node->ghostOf->markPaintDirty();
    node->ghostOf = nullptr;
  }
  while (!node->children().empty()) {
    Node* c = node->children().back();
    node->removeChild(c);
    destroyRecursive(c);
  }
  if (node->parent()) node->parent()->removeChild(node);
  if (onNodeDestroyed) onNodeDestroyed(node);
  if (node->propFlags() & (Node::kAutoFocus | Node::kModal)) setFocusHint(node, false);
  if (node->propFlags() & Node::kListensLayout) layoutListeners--;
  reportedLayout_.erase(node->id());
  nodes_.erase(node->id());
}

void Document::destroyNode(Node* node) {
  if (!node || node == root_) return;
  if (node->ghostOf) {
    // Still fading out: freed when its delete animation ends.
    node->destroyWhenDone = true;
    return;
  }
  destroyRecursive(node);
  markLayoutDirty();
}

void Document::setFocusHint(Node* n, bool on) {
  auto it = std::find(focusHints_.begin(), focusHints_.end(), n);
  if (on && it == focusHints_.end()) focusHints_.push_back(n);
  if (!on && it != focusHints_.end()) focusHints_.erase(it);
}

void Document::queueImageEvents(uint32_t node, bool cached) {
  for (auto& e : imageEvents) {
    if (e.node == node) {
      e = {node, frame, cached};
      return;
    }
  }
  imageEvents.push_back({node, frame, cached});
}

void Document::clear() {
  imageEvents.clear();
  while (!root_->children().empty()) {
    Node* c = root_->children().back();
    destroyNode(c);
  }
  while (!root_->ghosts.empty()) {
    Node* g = root_->ghosts.back();
    root_->ghosts.pop_back();
    g->ghostOf = nullptr;
    destroyRecursive(g);
  }
  root_->layoutAnim.reset();
  animating_.clear();
  armedSet_ = false;
  root_->setProps(Value());
}

namespace {
// Border widths snap to device pixels, so a new pixel ratio restyles.
void restyle(Node* n) {
  n->restyle();
  for (Node* c : n->children()) restyle(c);
}
}  // namespace

void Document::setViewport(float width, float height, float pixelRatio) {
  bool autoW = !(width > 0), autoH = !(height > 0);
  if (autoW == autoWidth_ && autoH == autoHeight_ && (autoW || width == width_) && (autoH || height == height_) &&
      pixelRatio == pixelRatio_)
    return;
  bool prChanged = pixelRatio != pixelRatio_;
  autoWidth_ = autoW;
  autoHeight_ = autoH;
  if (!autoW) width_ = width;
  if (!autoH) height_ = height;
  pixelRatio_ = pixelRatio;
  if (prChanged && root_) restyle(root_);
  markLayoutDirty();
}

bool Document::layout() {
  // A newly registered font changes text metrics everywhere.
  if (fonts && fonts->generation != fontGeneration_) {
    fontGeneration_ = fonts->generation;
    text::invalidateAll(root_);
    layoutDirty_ = paintDirty_ = true;
  }
  if (!layoutDirty_) return false;
  Perf::Scope scope(perf, Phase::Layout);
  perf.counters.layoutPasses++;
  layoutDirty_ = false;
  // An armed LayoutAnimation compares the frames before and after this pass.
  std::vector<std::pair<Node*, Rect>> before;
  bool animate = armedSet_;
  if (animate) snapshotLayout(root_, &before);
  // Auto-sized axes are left unconstrained; the root takes its content's size.
  float w = autoWidth_ ? YGUndefined : width_, h = autoHeight_ ? YGUndefined : height_;
  if (autoWidth_) YGNodeStyleSetWidthAuto(root_->yoga());
  else YGNodeStyleSetWidth(root_->yoga(), w);
  if (autoHeight_) YGNodeStyleSetHeightAuto(root_->yoga());
  else YGNodeStyleSetHeight(root_->yoga(), h);
  YGNodeCalculateLayout(root_->yoga(), w, h, YGDirectionLTR);
  if (autoWidth_) width_ = YGNodeLayoutGetWidth(root_->yoga());
  if (autoHeight_) height_ = YGNodeLayoutGetHeight(root_->yoga());
  if (animate) {
    armedSet_ = false;
    startLayoutAnimations(before);
  }
  afterLayout(root_, onLayout && layoutListeners > 0);
  paintDirty_ = true;
  return true;
}

// Visits only what this pass laid out: Yoga flags every node it lays out
// (hasNewLayout), and a node it did not reach (its parent's layout came from
// the cache) cannot have a flagged descendant, so unflagged subtrees are
// skipped (React Native does the same). A node whose frame moved dirties the
// hit-test bounds of its ancestors; onLayout listeners are told about it.
void Document::afterLayout(Node* node, bool report) {
  if (!node->yoga() || !YGNodeGetHasNewLayout(node->yoga())) return;
  YGNodeSetHasNewLayout(node->yoga(), false);
  Rect r = node->layoutRect();
  const Rect& b = node->boundsLayout;
  if (b.x != r.x || b.y != r.y || b.width != r.width || b.height != r.height) {
    node->boundsLayout = r;
    node->markBoundsDirty();
  }
  if (report && (node->propFlags() & Node::kListensLayout)) {
    auto& prev = reportedLayout_[node->id()];
    if (prev.x != r.x || prev.y != r.y || prev.width != r.width || prev.height != r.height) {
      prev = r;
      onLayout(node, r);
    }
  }
  for (Node* c : node->children()) afterLayout(c, report);
}

}  // namespace jsui
