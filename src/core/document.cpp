#include "core/document.h"

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
  destroyRecursive(node);
  markLayoutDirty();
}

void Document::clear() {
  while (!root_->children().empty()) {
    Node* c = root_->children().back();
    destroyNode(c);
  }
  root_->setProps(Value());
}

namespace {
// Border widths snap to device pixels, so a new pixel ratio restyles.
void restyle(Node* n) {
  n->setProps(Value(n->props()));
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
  YGNodeStyleSetWidth(root_->yoga(), width_);
  YGNodeStyleSetHeight(root_->yoga(), height_);
  YGNodeCalculateLayout(root_->yoga(), width_, height_, YGDirectionLTR);
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
