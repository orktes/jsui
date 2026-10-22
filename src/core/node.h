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

  // ScrollView content offset.
  float scrollX = 0, scrollY = 0;

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
  // Bumped on every change that affects how this node itself paints (props,
  // text, children list). The painter compares it against its cache.
  uint32_t paintVersion = 0;

 private:
  void attachYoga(Node* child, size_t index);
  void detachYoga(Node* child);

  Document* doc_;
  uint32_t id_;
  NodeType type_;
  Node* parent_ = nullptr;
  std::vector<Node*> children_;
  YGNodeRef yoga_ = nullptr;
  Value props_;
  ComputedStyle style_;
  std::string text_;
};

}  // namespace jsui
