// The retained view tree. One Node per host element created by the React
// reconciler (View, Text, Image, ...) plus RawText leaves for string children.
#pragma once

#include <yoga/Yoga.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/style.h"
#include "core/value.h"

namespace jsui {

class Document;

enum class NodeType : uint8_t {
  View,        // "View"
  Text,        // "Text"   - paragraph root or nested inline span
  RawText,     // string child of a Text
  Image,       // "Image"
  ScrollView,  // "ScrollView" - View with a scrollable content offset
  TextInput,   // "TextInput"
  Lottie,      // "LottieView" - plays a Lottie composition (render/lottie.h)
  Shader,      // "ShaderView" - painted by an SkSL shader (render/shader_view.h)
};

// Parses the element type names used by the reconciler ("View", "Text", ...).
bool nodeTypeFromName(const std::string& name, NodeType* out);
const char* nodeTypeName(NodeType t);

struct Rect {
  float x = 0, y = 0, width = 0, height = 0;
  bool contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

class Node {
 public:
  Node(Document* doc, uint32_t id, NodeType type);
  ~Node();
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  uint32_t id() const { return id_; }
  NodeType type() const { return type_; }
  Document* document() const { return doc_; }
  Node* parent() const { return parent_; }
  const std::vector<Node*>& children() const { return children_; }

  // Tree mutation. A child is detached from its previous parent first.
  void appendChild(Node* child);
  void insertBefore(Node* child, Node* before);  // before == nullptr: append
  void removeChild(Node* child);

  // Replaces all props. `props` is an object; `props["style"]` is the
  // already-flattened style object. Event handlers are not passed here; the
  // JS side keeps them and native only receives flags such as
  // `props["__events"] = ["press", "pointerMove", ...]` used for hit testing.
  void setProps(const Value& props);
  const Value& props() const { return props_; }
  // Merges `partial` into props().style (a null value removes the key), like
  // setProps with the merged style but without copying the props: when only
  // paint keys change, the style object is updated in place (copy-on-write)
  // and just their style groups are re-resolved. Returns false if nothing
  // changed. The animation path (patchStyle, native Animated).
  bool patchStyle(const Value::Object& partial);
  // Facts about props() that hot paths ask for (computed by setProps).
  enum PropFlag : uint8_t {
    kListensLayout = 1,  // __events has "layout" (onLayout)
    kAutoFocus = 2,      // autoFocus / hasTVPreferredFocus
    kModal = 4,          // accessibilityViewIsModal / aria-modal
  };
  uint8_t propFlags() const { return propFlags_; }
  // Re-applies the current props (e.g. after a pixel-ratio change, which
  // affects border-width snapping).
  void restyle();

  // RawText content (UTF-8).
  void setText(std::string text);
  const std::string& text() const { return text_; }

  const ComputedStyle& style() const { return style_; }
  YGNodeRef yoga() const { return yoga_; }

  // True when this Text node is nested in another Text (an inline span).
  bool isInlineText() const;
  // Closest ancestor-or-self Text that is not inline (the paragraph root).
  Node* paragraphRoot();

  // Layout results in logical px. `layoutRect` is relative to the parent's
  // border box origin; `absoluteRect` to the viewport, ignoring transforms
  // but including scroll offsets of ancestors.
  Rect layoutRect() const;
  Rect absoluteRect() const;
  // Where the node is drawn and hit-tested: layoutRect(), or the in-between
  // frame while a LayoutAnimation update moves / resizes it.
  Rect visualRect() const;

  // LayoutAnimation state (core/layout_animation.cpp). Update animations
  // interpolate the frame; create / delete animations fade (`opacity`) or
  // scale (`scaleX` / `scaleY`, about the center) the node.
  struct LayoutAnim {
    enum class Kind : uint8_t { Create, Update, Delete };
    Kind kind = Kind::Update;
    uint8_t curve = 0;         // LayoutAnimCurve
    uint8_t property = 0;      // LayoutAnimProperty (create / delete)
    float springDamping = 0.5f;
    double start = 0, duration = 0;
    Rect from;                 // update: parent-relative frame it starts from
    Rect current;              // update: frame drawn this tick (heads for layoutRect())
    float opacity = 1, scaleX = 1, scaleY = 1;
  };
  std::unique_ptr<LayoutAnim> layoutAnim;
  // Children removed while a LayoutAnimation with a `delete` config was
  // armed: detached from the tree (no layout, no hit testing) but painted
  // below the children until their fade / scale-out ends. Owned by the
  // Document; `ghostOf` is the parent painting this node.
  std::vector<Node*> ghosts;
  Node* ghostOf = nullptr;
  bool destroyWhenDone = false;  // JS destroyed the ghost: free it at the end

  // ScrollView content offset.
  float scrollX = 0, scrollY = 0;
  // Opacity of the overlay scroll indicators (0: hidden). Driven by
  // InputManager (shown while scrolling, then faded out); read by the painter.
  float scrollIndicatorOpacity = 0;

  // Marks layout and paint dirty (text content or style changed).
  void markDirty();
  // Marks only this node's painting dirty (no relayout).
  void markPaintDirty();

  // Opaque per-node cache owned by the renderer / text layout.
  struct Cache {
    virtual ~Cache() = default;
  };
  std::unique_ptr<Cache> textCache;
  std::unique_ptr<Cache> imageCache;
  // Renderer state (recorded picture of this subtree, see render/painter.cpp).
  std::unique_ptr<Cache> paintCache;
  // TextInput editing state (text, selection, caret; see text/text_edit.h).
  std::unique_ptr<Cache> editState;
  // Bumped on every change that affects how this node itself paints (props,
  // text, children list). The painter compares it against its cache.
  uint32_t paintVersion = 0;
  // Like paintVersion, but not bumped by changes that only move or fade the
  // node as a whole (opacity, transform, zIndex: an Animated frame): what it
  // paints into its own layer stays the same, so a cached layer of its
  // subtree can be composited again (painter.cpp, LayerCache).
  uint32_t contentVersion = 0;
  // What props()["__events"] asks of input routing, as InputManager reads
  // it (input.cpp listenMask); recomputed after the props change.
  mutable uint32_t listenMask = 0;
  mutable bool listenMaskValid = false;

  // Hit-test culling (InputManager::hitNode). A clean node caches whether
  // every hittable point of its subtree lies inside its own box, so a
  // pointer outside the box can skip the subtree. Anything that can move a
  // descendant's hittable area (layout, transform, display, pointerEvents,
  // overflow, scroll, children, LayoutAnimation frames) calls
  // markBoundsDirty(), which dirties this node and its ancestors. A clean
  // node's children are always clean, so marking stops at the first dirty
  // ancestor.
  void markBoundsDirty() {
    for (Node* n = this; n && !n->boundsDirty_; n = n->parent_) n->boundsDirty_ = true;
    markPaintTree();
  }
  bool boundsDirty() const { return boundsDirty_; }
  bool boundsContained() const { return boundsContained_; }
  void setBoundsContained(bool c) {
    boundsContained_ = c;
    boundsDirty_ = false;
  }
  // layoutRect() as of the last post-layout pass (Document::afterLayout).
  Rect boundsLayout{-1, -1, -1, -1};
  // Its resolved padding and border (left, top, right, bottom each) as of
  // that pass: with the frame, all of a node's layout the painter reads.
  float edgesLayout[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  // The Yoga passes (Document::layout) that last laid this node out and
  // that last took its layout from Yoga's cache (which leaves its subtree
  // as it was); see Document::afterLayout.
  uint64_t yogaLaidOut = 0, yogaCached = 0;

  // Painter culling (render/painter.cpp, updateRecords): a node is dirty
  // when anything that can change what its subtree paints changed since the
  // painter last visited it (props, text, children, layout, scroll offset or
  // indicators, LayoutAnimation frames, caret). The painter skips a clean
  // subtree whose placement is unchanged. Like the bounds flag, a clean
  // node's descendants are clean, so marking stops at the first dirty
  // ancestor; a ghost's ancestor is the node painting it.
  void markPaintTree() {
    for (Node* n = this; n && !n->paintTreeDirty_; n = n->parent_ ? n->parent_ : n->ghostOf) n->paintTreeDirty_ = true;
  }
  bool paintTreeDirty() const { return paintTreeDirty_; }
  void clearPaintTree() { paintTreeDirty_ = false; }

 private:
  void updatePropFlags();
  // Text / TextInput: invalidates the paragraph after a style change.
  void textStyleChanged(const TextStyle& oldText, bool propsMetrics, bool propsPaint, bool* relayout);
  void attachYoga(Node* child, size_t index);
  void detachYoga(Node* child);

  Document* doc_;
  uint32_t id_;
  NodeType type_;
  Node* parent_ = nullptr;
  std::vector<Node*> children_;
  YGNodeRef yoga_ = nullptr;
  Value props_;
  uint8_t propFlags_ = 0;
  bool boundsDirty_ = true, boundsContained_ = false;
  bool paintTreeDirty_ = true;
  ComputedStyle style_;
  std::string text_;
};

}  // namespace jsui
