#include "core/node.h"

#include <algorithm>

#include "core/document.h"
#include "render/image_node.h"
#include "text/text_layout.h"

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

bool valueEquals(const Value& a, const Value& b) {
  if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
  if (a.isBool()) return b.isBool() && a.asBool() == b.asBool();
  if (a.isNumber()) return b.isNumber() && a.asNumber() == b.asNumber();
  if (a.isString()) return b.isString() && a.asString() == b.asString();
  if (a.isArray()) {
    if (!b.isArray() || a.asArray().size() != b.asArray().size()) return false;
    for (size_t i = 0; i < a.asArray().size(); i++)
      if (!valueEquals(a.asArray()[i], b.asArray()[i])) return false;
    return true;
  }
  if (a.isObject()) {
    if (!b.isObject() || a.asObject().size() != b.asObject().size()) return false;
    auto ib = b.asObject().begin();
    for (auto& [k, v] : a.asObject()) {
      if (k != ib->first || !valueEquals(v, ib->second)) return false;
      ++ib;
    }
    return true;
  }
  return false;
}
}  // namespace

Node::Node(Document* doc, uint32_t id, NodeType type) : doc_(doc), id_(id), type_(type) {
  if (type_ != NodeType::RawText) {
    yoga_ = doc_ && doc_->yogaConfig() ? YGNodeNewWithConfig(doc_->yogaConfig()) : YGNodeNew();
    YGNodeSetContext(yoga_, this);
    applyStyle(Value(), &style_, yoga_);
    text::installMeasure(this);
    image::installMeasure(this);
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
  // Yoga forbids children on measured leaves (e.g. an Image with children).
  if (YGNodeHasMeasureFunc(yoga_)) YGNodeSetMeasureFunc(yoga_, nullptr);
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

// Layout is only invalidated when something layout-affecting changed: Yoga's
// setters ignore no-op writes, so applyStyle() dirties the Yoga node only on
// a real layout change, and text / image measurement is compared separately.
// Paint-only updates (opacity, transform, colors, shadows, ...) - e.g. every
// frame of an Animated transform - therefore never re-run Yoga.
void Node::setProps(const Value& props) {
  Value old = std::move(props_);
  props_ = props;
  bool relayout = false;
  if (yoga_) {
    TextStyle oldText = std::move(style_.text);
    applyStyle(props_["style"], &style_, yoga_, doc_ ? doc_->pixelRatio() : 1);
    if (props_["pointerEvents"].isString() && !props_["style"].has("pointerEvents")) {
      const std::string& pe = props_["pointerEvents"].asString();
      style_.pointerEvents = pe == "none"       ? PointerEvents::None
                             : pe == "box-none" ? PointerEvents::BoxNone
                             : pe == "box-only" ? PointerEvents::BoxOnly
                                                : PointerEvents::Auto;
    }
    if (YGNodeIsDirty(yoga_) && !isInlineText()) relayout = true;
    if (type_ == NodeType::Text || type_ == NodeType::TextInput) {
      bool metrics = !textMetricsEqual(oldText, style_.text);
      static const char* const kMeasureProps[] = {"numberOfLines", "ellipsizeMode", "value",          "defaultValue",
                                                  "placeholder",   "multiline",     "secureTextEntry"};
      for (const char* k : kMeasureProps)
        if (!valueEquals(old[k], props_[k])) metrics = true;
      bool paint = metrics || !(oldText == style_.text) ||
                   !valueEquals(old["placeholderTextColor"], props_["placeholderTextColor"]);
      if (paint) {
        // The paragraph (owned by the paragraph root) bakes in colors too.
        Node* root = type_ == NodeType::Text ? paragraphRoot() : this;
        if (root) {
          root->textCache.reset();
          root->paintVersion++;
          if (metrics && root->yoga_ && YGNodeHasMeasureFunc(root->yoga_)) {
            YGNodeMarkDirty(root->yoga_);
            relayout = true;
          }
        }
      }
    }
    if (type_ == NodeType::Image) {
      if (!valueEquals(old["source"], props_["source"]) || !valueEquals(old["src"], props_["src"]) ||
          !valueEquals(old["defaultSource"], props_["defaultSource"])) {
        imageCache.reset();
        image::request(this);
        if (YGNodeHasMeasureFunc(yoga_)) {
          YGNodeMarkDirty(yoga_);
          relayout = true;
        }
      }
    }
  }
  paintVersion++;
  if (!doc_) return;
  if (relayout) doc_->markLayoutDirty();
  else doc_->markPaintDirty();
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
  paintVersion++;
  textCache.reset();
  // Text measurement depends on descendants: dirty the paragraph root's measure.
  if (type_ == NodeType::Text || type_ == NodeType::RawText) {
    if (Node* p = paragraphRoot()) {
      p->textCache.reset();
      p->paintVersion++;
      if (p->yoga_ && YGNodeHasMeasureFunc(p->yoga_)) YGNodeMarkDirty(p->yoga_);
    }
  }
  if (doc_) doc_->markLayoutDirty();
}

void Node::markPaintDirty() {
  paintVersion++;
  if (doc_) doc_->markPaintDirty();
}

}  // namespace jsui
