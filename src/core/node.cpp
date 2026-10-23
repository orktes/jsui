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
  markBoundsDirty();
  markDirty();
}

void Node::removeChild(Node* child) {
  auto it = std::find(children_.begin(), children_.end(), child);
  if (it == children_.end()) return;
  detachYoga(child);
  children_.erase(it);
  child->parent_ = nullptr;
  markBoundsDirty();
  markDirty();
}

namespace {
// What of a node's own style hit testing depends on (input.cpp hitNode):
// captured before a restyle so bounds are only dirtied when it changed.
// Transforms of up to four functions are compared exactly without
// allocating; longer ones always count as changed.
struct HitShape {
  bool display;
  Overflow overflow;
  PointerEvents pe;
  uint8_t ops;
  Length origin[2];
  TransformOp transform[4];
  explicit HitShape(const ComputedStyle& s)
      : display(s.display), overflow(s.overflow), pe(s.pointerEvents),
        ops(uint8_t(std::min<size_t>(s.transform.size(), 5))), origin{s.transformOrigin[0], s.transformOrigin[1]} {
    for (size_t i = 0; i < ops && i < 4; i++) transform[i] = s.transform[i];
  }
  bool same(const ComputedStyle& s) const {
    if (display != s.display || overflow != s.overflow || pe != s.pointerEvents) return false;
    if (ops > 4 || ops != s.transform.size()) return false;
    if (ops == 0) return true;
    auto lenEq = [](const Length& a, const Length& b) { return a.value == b.value && a.percent == b.percent; };
    if (!lenEq(origin[0], s.transformOrigin[0]) || !lenEq(origin[1], s.transformOrigin[1])) return false;
    for (size_t i = 0; i < ops; i++) {
      const TransformOp &a = transform[i], &b = s.transform[i];
      if (a.kind != b.kind || a.a != b.a || a.b != b.b || a.percentA != b.percentA || a.percentB != b.percentB)
        return false;
      if (a.kind == TransformOp::Kind::Matrix && !std::equal(a.m, a.m + 6, b.m)) return false;
    }
    return true;
  }
};
}  // namespace

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
  HitShape shape(style_);
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
    if (!shape.same(style_)) markBoundsDirty();
    if (isText) {
      bool metrics = false;
      static const char* const kMeasureProps[] = {"numberOfLines", "ellipsizeMode", "value",          "defaultValue",
                                                  "placeholder",   "multiline",     "secureTextEntry"};
      for (const char* k : kMeasureProps)
        if (!valueEquals(old[k], props_[k])) metrics = true;
      textStyleChanged(oldText, metrics, !valueEquals(old["placeholderTextColor"], props_["placeholderTextColor"]),
                       &relayout);
    }
    if (type_ == NodeType::Image) {
      if (!valueEquals(old["source"], props_["source"]) || !valueEquals(old["src"], props_["src"]) ||
          !valueEquals(old["defaultSource"], props_["defaultSource"]) ||
          !valueEquals(old["texture"], props_["texture"])) {
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
  contentVersion++;
  markPaintTree();
  if (!doc_) return;
  if (relayout) doc_->markLayoutDirty();
  else doc_->markPaintDirty();
  if (type_ == NodeType::TextInput) edit::syncProps(this);
}

void Node::textStyleChanged(const TextStyle& oldText, bool propsMetrics, bool propsPaint, bool* relayout) {
  bool metrics = propsMetrics || !textMetricsEqual(oldText, style_.text);
  if (!(metrics || propsPaint || !(oldText == style_.text))) return;
  // The paragraph (owned by the paragraph root) bakes in colors too.
  Node* root = type_ == NodeType::Text ? paragraphRoot() : this;
  if (!root) return;
  root->textCache.reset();
  root->paintVersion++;
  root->contentVersion++;
  root->markPaintTree();
  if (metrics && root->yoga_ && YGNodeHasMeasureFunc(root->yoga_)) {
    YGNodeMarkDirty(root->yoga_);
    *relayout = true;
  }
}

bool Node::patchStyle(const Value::Object& partial) {
  const Value& cur = props_["style"];
  uint32_t groups = 0;
  bool layout = false, any = false;
  for (auto& [k, v] : partial) {
    bool has = cur.has(k);
    if (v.isNull() ? !has : has && valueEquals(cur[k], v)) continue;
    any = true;
    uint32_t g = paintStyleGroup(k);
    if (g) groups |= g;
    else layout = true;
  }
  if (!any) return false;
  if (layout || !yoga_ || !doc_) {
    // A layout key changed: the general path (full restyle, relayout).
    Value::Object props = props_.asObject();
    Value::Object style = cur.asObject();
    for (auto& [k, v] : partial) {
      if (v.isNull()) style.erase(k);
      else style.insert_or_assign(k, v);
    }
    props.insert_or_assign("style", Value(std::move(style)));
    setProps(Value(std::move(props)));
    return true;
  }
  doc_->perf.counters.setProps++;
  Value::Object& style = props_.mutableObject()["style"].mutableObject();
  for (auto& [k, v] : partial) {
    if (v.isNull()) style.erase(k);
    else style.insert_or_assign(k, v);
  }
  bool isText = type_ == NodeType::Text || type_ == NodeType::TextInput;
  TextStyle oldText;
  if (isText) oldText = style_.text;
  HitShape shape(style_);
  {
    Perf::Scope scope(doc_->perf, Phase::Style);
    doc_->perf.counters.styleResolves++;
    applyStyleGroups(props_["style"], groups, &style_, doc_->pixelRatio());
  }
  if (props_["pointerEvents"].isString() && !props_["style"].has("pointerEvents"))
    style_.pointerEvents = parsePointerEvents(props_["pointerEvents"]);
  if (!shape.same(style_)) markBoundsDirty();
  bool relayout = false;
  if (isText) textStyleChanged(oldText, false, false, &relayout);
  paintVersion++;
  if (!styleGroupsMoveOnly(groups)) contentVersion++;
  markPaintTree();
  if (relayout) doc_->markLayoutDirty();
  else doc_->markPaintDirty();
  if (type_ == NodeType::TextInput) edit::syncProps(this);
  return true;
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
  contentVersion++;
  markPaintTree();
  textCache.reset();
  // Text measurement depends on descendants: dirty the paragraph root's measure.
  if (type_ == NodeType::Text || type_ == NodeType::RawText) {
    if (Node* p = paragraphRoot()) {
      p->textCache.reset();
      p->paintVersion++;
      p->contentVersion++;
      p->markPaintTree();
      if (p->yoga_ && YGNodeHasMeasureFunc(p->yoga_)) YGNodeMarkDirty(p->yoga_);
    }
  }
  if (doc_) doc_->markLayoutDirty();
}

void Node::markPaintDirty() {
  paintVersion++;
  contentVersion++;
  markPaintTree();
  if (doc_) doc_->markPaintDirty();
}

}  // namespace jsui
