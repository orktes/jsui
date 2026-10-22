#include "core/node.h"

#include <algorithm>

#include "core/document.h"

namespace jsui {

bool nodeTypeFromName(const std::string& n, NodeType* out) {
  static const struct {
    const char* name;
    NodeType type;
  } kTypes[] = {{"View", NodeType::View},           {"Text", NodeType::Text},
                {"RawText", NodeType::RawText},     {"Image", NodeType::Image},
                {"ScrollView", NodeType::ScrollView}, {"TextInput", NodeType::TextInput}};
  for (auto& t : kTypes) {
    if (n == t.name) {
      *out = t.type;
      return true;
    }
  }
  return false;
}

const char* nodeTypeName(NodeType t) {
  switch (t) {
    case NodeType::View: return "View";
    case NodeType::Text: return "Text";
    case NodeType::RawText: return "RawText";
    case NodeType::Image: return "Image";
    case NodeType::ScrollView: return "ScrollView";
    case NodeType::TextInput: return "TextInput";
  }
  return "?";
}

namespace {
// Children of Text are laid out by the paragraph, not by Yoga.
bool hasYogaChildren(const Node* n) { return n->type() != NodeType::Text && n->type() != NodeType::RawText; }
}  // namespace

Node::Node(Document* doc, uint32_t id, NodeType type) : doc_(doc), id_(id), type_(type) {
  if (type_ != NodeType::RawText) {
    yoga_ = YGNodeNew();
    YGNodeSetContext(yoga_, this);
    applyStyle(Value(), &style_, yoga_);
  }
}

Node::~Node() {
  if (yoga_) {
    YGNodeRemoveAllChildren(yoga_);
    if (YGNodeRef owner = YGNodeGetOwner(yoga_)) YGNodeRemoveChild(owner, yoga_);
    YGNodeFree(yoga_);
  }
}

void Node::attachYoga(Node* child, size_t index) {
  if (!yoga_ || !child->yoga_ || !hasYogaChildren(this)) return;
  // Yoga index counts only yoga children preceding `index`.
  size_t yi = 0;
  for (size_t i = 0; i < index && i < children_.size(); i++) {
    if (children_[i] != child && children_[i]->yoga_) yi++;
  }
  YGNodeInsertChild(yoga_, child->yoga_, yi);
}

void Node::detachYoga(Node* child) {
  if (yoga_ && child->yoga_ && YGNodeGetOwner(child->yoga_) == yoga_) YGNodeRemoveChild(yoga_, child->yoga_);
}

void Node::appendChild(Node* child) { insertBefore(child, nullptr); }

void Node::insertBefore(Node* child, Node* before) {
  if (child->parent_) child->parent_->removeChild(child);
  auto it = before ? std::find(children_.begin(), children_.end(), before) : children_.end();
  size_t index = size_t(it - children_.begin());
  children_.insert(it, child);
  child->parent_ = this;
  attachYoga(child, index);
  markDirty();
}

void Node::removeChild(Node* child) {
  auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return;
  detachYoga(child);
  children_.erase(it);
  child->parent_ = nullptr;
  markDirty();
}

void Node::setProps(const Value& props) {
  props_ = props;
  if (yoga_) applyStyle(props["style"], &style_, yoga_);
  markDirty();
}

void Node::setText(std::string text) {
  text_ = std::move(text);
  markDirty();
}

bool Node::isInlineText() const {
  return type_ == NodeType::Text && parent_ && parent_->type_ == NodeType::Text;
}

Node* Node::paragraphRoot() {
  Node* n = this;
  while (n && (n->type_ == NodeType::RawText || n->isInlineText())) n = n->parent_;
  return (n && n->type_ == NodeType::Text) ? n : nullptr;
}

Rect Node::layoutRect() const {
  if (!yoga_) return {};
  return {YGNodeLayoutGetLeft(yoga_), YGNodeLayoutGetTop(yoga_), YGNodeLayoutGetWidth(yoga_),
          YGNodeLayoutGetHeight(yoga_)};
}

Rect Node::absoluteRect() const {
  Rect r = layoutRect();
  for (const Node* p = parent_; p; p = p->parent_) {
    Rect pr = p->layoutRect();
    r.x += pr.x - p->scrollX;
    r.y += pr.y - p->scrollY;
  }
  return r;
}

void Node::markDirty() {
  textCache.reset();
  // Text measurement depends on descendants: dirty the paragraph root's measure.
  if (type_ == NodeType::Text || type_ == NodeType::RawText) {
    if (Node* p = paragraphRoot()) {
      p->textCache.reset();
      if (p->yoga_ && YGNodeHasMeasureFunc(p->yoga_)) YGNodeMarkDirty(p->yoga_);
    }
  }
  if (doc_) doc_->markLayoutDirty();
}

}  // namespace jsui
