#include "core/document.h"

#include <algorithm>
#include <cmath>

#include "text/font_manager.h"
#include "text/text_layout.h"

namespace jsui {

Document::Document() {
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
  uint32_t id = nextId_++;
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
  if (width == width_ && height == height_ && pixelRatio == pixelRatio_) return;
  bool prChanged = pixelRatio != pixelRatio_;
  width_ = width;
  height_ = height;
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
  layoutDirty_ = false;
  // An armed LayoutAnimation compares the frames before and after this pass.
  std::vector<std::pair<Node*, Rect>> before;
  bool animate = armedSet_;
  if (animate) snapshotLayout(root_, &before);
  YGNodeStyleSetWidth(root_->yoga(), width_);
  YGNodeStyleSetHeight(root_->yoga(), height_);
  YGNodeCalculateLayout(root_->yoga(), width_, height_, YGDirectionLTR);
  if (animate) {
    armedSet_ = false;
    startLayoutAnimations(before);
  }
  if (onLayout) reportLayout(root_);
  paintDirty_ = true;
  return true;
}

void Document::reportLayout(Node* node) {
  if (node->yoga() && YGNodeGetHasNewLayout(node->yoga())) {
    YGNodeSetHasNewLayout(node->yoga(), false);
    for (auto& e : node->props()["__events"].asArray()) {
      if (e.asString() == "layout") {
        Rect r = node->layoutRect();
        auto& prev = reportedLayout_[node->id()];
        if (prev.x != r.x || prev.y != r.y || prev.width != r.width || prev.height != r.height) {
          prev = r;
          onLayout(node, r);
        }
        break;
      }
    }
  }
  for (Node* c : node->children()) reportLayout(c);
}

}  // namespace jsui
