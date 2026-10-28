// The Elements panel for the JS debugger (js/inspector.h): the runtime's view
// tree as CDP's DOM domain (View, Text, Image, ... elements; testID and the
// like as attributes, string children as text nodes), each view's style as
// its inline style (CSS domain; edits apply to the live view), and
// Overlay.highlightNode drawn in the app. Runs on the thread that owns the
// document (Inspector::pumpDom from Runtime::tick); what React renders next
// writes its props back over an edit, as React DOM does in a browser.
#pragma once

#if JSUI_DEBUGGER

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.h"

namespace jsui {

class Document;
class Node;

class DomAgent {
 public:
  using Emit = std::function<void(const char* method, Value params)>;

  void attach(Document* doc);
  // One DOM.* / CSS.* / Overlay.* request: false when it isn't one of
  // these; else *result or *error is set. Events go through `event`.
  bool handle(const std::string& method, const Value& params, Value* result, std::string* error, const Emit& event);
  // Live updates (inserted / removed views, attributes, text) and the
  // highlight following its view.
  void tick(const Emit& event);
  bool enabled() const { return enabled_; }
  std::string documentUrl;

 private:
  struct Known {
    uint32_t parent = 0;
    std::vector<uint32_t> children;
    std::string attrs;
    std::string text;
  };
  Value describe(Node* n, Node* parent, int depth);
  Node* node(const Value& params) const;
  static Value attributes(const Node* n);
  static std::string attributesKey(const Node* n);
  static std::vector<Node*> childrenOf(const Node* n);
  Value styleObject(Node* n);
  void setStyle(Node* n, const Value::Object& next);
  void highlight(Node* n);
  void syncHighlight();
  void diff(const Emit& event, Node* n);

  Document* doc_ = nullptr;
  bool enabled_ = false;
  std::unordered_map<uint32_t, Known> known_;  // what the frontend has, by view id
  Node* highlight_ = nullptr;
  uint32_t highlightTarget_ = 0;
};

}  // namespace jsui

#endif  // JSUI_DEBUGGER
