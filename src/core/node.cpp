#include "core/node.h"

#include <algorithm>
#include <optional>

#include "core/document.h"
#include "render/image_node.h"
#include "text/text_edit.h"
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

bool valueEquals(const Value& a, const Value& b) { return styleValueEquals(a, b); }

PointerEvents parsePointerEvents(const Value& v) {
  const std::string& pe = v.asString();
  return pe == "none"       ? PointerEvents::None
         : pe == "box-none" ? PointerEvents::BoxNone
         : pe == "box-only" ? PointerEvents::BoxOnly
                            : PointerEvents::Auto;
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
  if (child->ghostOf && doc_) doc_->reviveGhost(child);  // re-inserted while fading out
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
  // React re-renders often send identical props: nothing to do (and nothing
  // to repaint).
  if (doc_) doc_->perf.counters.setProps++;
  if (yoga_ && valueEquals(props_, props) && props_.isObject()) {
    if (doc_) doc_->perf.counters.setPropsSkipped++;
    return;
  }
  Value old = std::move(props_);
  props_ = props;
  updatePropFlags();
  bool relayout = false;
  if (yoga_) {
    bool isText = type_ == NodeType::Text || type_ == NodeType::TextInput;
    TextStyle oldText;
    if (isText) oldText = style_.text;
    {
      // Only the style groups whose keys changed are re-resolved when no
      // layout key changed (Animated transform / opacity frames, color
      // changes); otherwise the whole style, including Yoga, is re-applied.
      Perf* perf = doc_ ? &doc_->perf : nullptr;
      std::optional<Perf::Scope> scope;
      if (perf) scope.emplace(*perf, Phase::Style);
      float pr = doc_ ? doc_->pixelRatio() : 1;
      bool styled = false;
      if (!applyPaintStyleChanges(old["style"], props_["style"], &style_, pr, &styled)) {
        applyStyle(props_["style"], &style_, yoga_, pr);
        styled = true;
      } else {
        style_.pointerEvents = parsePointerEvents(props_["style"]["pointerEvents"]);
      }
      if (perf && styled) perf->counters.styleResolves++;
    }
    if (props_["pointerEvents"].isString() && !props_["style"].has("pointerEvents"))
      style_.pointerEvents = parsePointerEvents(props_["pointerEvents"]);
    if (YGNodeIsDirty(yoga_) && !isInlineText()) relayout = true;
    if (isText) {
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
        if (doc_) doc_->queueImageEvents(id_, image::settled(this));
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
  if (type_ == NodeType::TextInput) edit::syncProps(this);
}

namespace {
bool truthyProp(const Value& v) { return v.asBool(false) || (v.isNumber() && v.asNumber() != 0); }
}  // namespace

void Node::updatePropFlags() {
  uint8_t f = 0;
  if (props_.isObject()) {
    for (const Value& e : props_["__events"].asArray())
      if (e.asString() == "layout") f |= kListensLayout;
    if (truthyProp(props_["autoFocus"]) || truthyProp(props_["hasTVPreferredFocus"])) f |= kAutoFocus;
    if (truthyProp(props_["accessibilityViewIsModal"]) || truthyProp(props_["aria-modal"])) f |= kModal;
  }
  bool hint = f & (kAutoFocus | kModal), wasHint = propFlags_ & (kAutoFocus | kModal);
  bool listens = f & kListensLayout, listened = propFlags_ & kListensLayout;
  propFlags_ = f;
  if (doc_ && hint != wasHint) doc_->setFocusHint(this, hint);
  if (doc_ && listens != listened) doc_->layoutListeners += listens ? 1 : -1;
}

void Node::restyle() {
  Value p = std::move(props_);
  props_ = Value();
  setProps(p);
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

Rect Node::visualRect() const {
  if (layoutAnim && layoutAnim->kind == LayoutAnim::Kind::Update) return layoutAnim->current;
  return layoutRect();
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
