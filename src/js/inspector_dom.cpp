// The Elements panel: the runtime's view tree as CDP's DOM, CSS and Overlay
// domains (see inspector_dom.h).
#include "js/inspector_dom.h"

#if JSUI_DEBUGGER

#include <yoga/Yoga.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/document.h"
#include "core/node.h"

namespace jsui {

namespace {

constexpr uint32_t kDocumentId = 1;
// CDP node ids: the view's id + 1 (the document is 1).
uint32_t cdpId(const Node* n) { return n->id() + 1; }

// The props shown as attributes (style is shown as `style`, the rest of the
// props are callbacks and internals).
const char* const kAttributes[] = {"testID", "nativeID", "accessibilityLabel", "accessibilityRole", "accessible",
                                   "pointerEvents", "placeholder", "value", "numberOfLines", "resizeMode"};

std::string formatValue(const Value& v) {
  if (v.isString()) return v.asString();
  if (v.isNumber()) {
    double d = v.asNumber();
    char buf[32];
    if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
    else std::snprintf(buf, sizeof buf, "%g", d);
    return buf;
  }
  if (v.isBool()) return v.asBool() ? "true" : "false";
  return v.toJson();
}

// "12" -> 12, "true" -> true, "[...]" / "{...}" -> JSON, quoted -> the text, else the text.
Value parseValue(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t b = s.find_first_not_of(" \t");
  s = b == std::string::npos ? std::string() : s.substr(b);
  if (s.empty()) return Value();
  if (s == "true" || s == "false") return Value(s == "true");
  if ((s.front() == '[' || s.front() == '{') || (s.front() == '"' && s.back() == '"')) {
    Value j = Value::parseJson(s);
    if (!j.isNull()) return j;
  }
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') return Value(s.substr(1, s.size() - 2));
  char* end = nullptr;
  double d = std::strtod(s.c_str(), &end);
  if (end && *end == '\0') return Value(d);
  if (end && std::string(end) == "px") return Value(d);
  return Value(s);
}

// The view's style as one object (React may pass an array of them).
void flattenStyle(const Value& v, Value::Object& out) {
  if (v.isArray()) {
    for (const Value& e : v.asArray()) flattenStyle(e, out);
  } else if (v.isObject()) {
    for (auto& [k, x] : v.asObject()) out.insert_or_assign(k, x);
  }
}

Value::Object styleOf(const Node* n) {
  Value::Object out;
  if (n->props().isObject()) flattenStyle(n->props()["style"], out);
  return out;
}

std::string cssText(const Value::Object& style) {
  std::string t;
  for (auto& [k, v] : style) {
    if (!t.empty()) t += ' ';
    t += k + ": " + formatValue(v) + ";";
  }
  return t;
}

// "a: 1; b: red" -> {a: 1, b: "red"}. Commented out (DevTools' unticked
// properties: /* a: 1; */) is left out.
Value::Object parseCss(const std::string& text) {
  std::string t;
  for (size_t i = 0; i < text.size(); i++) {
    if (text.compare(i, 2, "/*") == 0) {
      size_t e = text.find("*/", i + 2);
      if (e == std::string::npos) break;
      i = e + 1;
      continue;
    }
    t += text[i];
  }
  Value::Object out;
  size_t i = 0;
  while (i < t.size()) {
    // A value may hold ';' inside brackets or quotes (JSON).
    size_t e = i;
    int depth = 0;
    char quote = 0;
    for (; e < t.size(); e++) {
      char c = t[e];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '[' || c == '{') {
        depth++;
      } else if (c == ']' || c == '}') {
        depth--;
      } else if (c == ';' && depth <= 0) {
        break;
      }
    }
    std::string decl = t.substr(i, e - i);
    i = e + 1;
    size_t colon = decl.find(':');
    if (colon == std::string::npos) continue;
    std::string name = decl.substr(0, colon);
    name.erase(0, name.find_first_not_of(" \t\n\r"));
    name.erase(name.find_last_not_of(" \t\n\r") + 1);
    if (name.empty()) continue;
    Value v = parseValue(decl.substr(colon + 1));
    if (!v.isNull()) out.insert_or_assign(name, std::move(v));
  }
  return out;
}

Value range(int start, int end) {
  return Value(Value::Object{{"startLine", Value(0)}, {"startColumn", Value(start)}, {"endLine", Value(0)}, {"endColumn", Value(end)}});
}

Value quad(float x, float y, float w, float h) {
  return Value(Value::Array{Value(double(x)), Value(double(y)), Value(double(x + w)), Value(double(y)),
                            Value(double(x + w)), Value(double(y + h)), Value(double(x)), Value(double(y + h))});
}

bool visible(const Node* n) { return n && !n->nativeOnly; }

}  // namespace

std::string DomAgent::attributesKey(const Node* n) {
  std::string key;
  for (const Value& a : attributes(n).asArray()) key += a.asString() + '\x1f';
  return key;
}

Value DomAgent::attributes(const Node* n) {
  Value::Array a;
  if (n->props().isObject()) {
    for (const char* k : kAttributes) {
      const Value& v = n->props()[k];
      if (v.isNull()) continue;
      a.push_back(Value(k));
      a.push_back(Value(formatValue(v)));
    }
    if (n->type() == NodeType::Image) {
      const Value& src = n->props()["source"];
      std::string uri = src.isObject() ? src["uri"].asString() : src.isArray() && !src.asArray().empty() ? src.asArray()[0]["uri"].asString() : "";
      if (!uri.empty()) {
        a.push_back(Value("source"));
        a.push_back(Value(uri));
      }
    }
  }
  Value::Object style = styleOf(n);
  if (!style.empty()) {
    a.push_back(Value("style"));
    a.push_back(Value(cssText(style)));
  }
  return Value(std::move(a));
}

std::vector<Node*> DomAgent::childrenOf(const Node* n) {
  std::vector<Node*> out;
  for (Node* c : n->logicalChildren())
    if (visible(c)) out.push_back(c);
  return out;
}

Value DomAgent::describe(Node* n, Node* parent, int depth) {
  Known& k = known_[n->id()];
  k.parent = parent ? parent->id() : 0;
  if (n->type() == NodeType::RawText) {
    k.text = n->text();
    k.children.clear();
    Value::Object o{{"nodeId", Value(double(cdpId(n)))},
                    {"backendNodeId", Value(double(cdpId(n)))},
                    {"nodeType", Value(3)},
                    {"nodeName", Value("#text")},
                    {"localName", Value("")},
                    {"nodeValue", Value(n->text())}};
    if (parent) o["parentId"] = Value(double(cdpId(parent)));
    return Value(std::move(o));
  }
  std::vector<Node*> kids = childrenOf(n);
  k.children.clear();
  for (Node* c : kids) k.children.push_back(c->id());
  k.attrs = attributesKey(n);
  const char* name = n == doc_->root() ? "AppRoot" : nodeTypeName(n->type());
  Value::Object o{{"nodeId", Value(double(cdpId(n)))},
                  {"backendNodeId", Value(double(cdpId(n)))},
                  {"nodeType", Value(1)},
                  {"nodeName", Value(name)},
                  {"localName", Value(name)},
                  {"nodeValue", Value("")},
                  {"childNodeCount", Value(double(kids.size()))},
                  {"attributes", attributes(n)}};
  o["parentId"] = Value(double(parent ? cdpId(parent) : kDocumentId));
  if (depth != 0) {
    Value::Array children;
    for (Node* c : kids) children.push_back(describe(c, n, depth < 0 ? -1 : depth - 1));
    o["children"] = Value(std::move(children));
  }
  return Value(std::move(o));
}

Node* DomAgent::node(const Value& params) const {
  double id = params["nodeId"].asNumber(params["backendNodeId"].asNumber(0));
  if (id <= kDocumentId) return nullptr;
  return doc_ ? doc_->get(uint32_t(id) - 1) : nullptr;
}

Value DomAgent::styleObject(Node* n) {
  Value::Object style = styleOf(n);
  std::string text = cssText(style);
  Value::Array props;
  int at = 0;
  for (auto& [k, v] : style) {
    std::string decl = k + ": " + formatValue(v) + ";";
    props.push_back(Value(Value::Object{{"name", Value(k)},
                                        {"value", Value(formatValue(v))},
                                        {"text", Value(decl)},
                                        {"range", range(at, at + int(decl.size()))},
                                        {"implicit", Value(false)},
                                        {"disabled", Value(false)}}));
    at += int(decl.size()) + 1;
  }
  return Value(Value::Object{{"styleSheetId", Value("inline:" + std::to_string(cdpId(n)))},
                             {"cssProperties", Value(std::move(props))},
                             {"shorthandEntries", Value(Value::Array{})},
                             {"cssText", Value(text)},
                             {"range", range(0, int(text.size()))}});
}

void DomAgent::setStyle(Node* n, const Value::Object& next) {
  Value::Object patch = next;
  for (auto& [k, v] : styleOf(n))
    if (!next.count(k)) patch[k] = Value();  // removed
  n->patchStyle(patch);
  doc_->markPaintDirty();
}

void DomAgent::highlight(Node* n) {
  highlightTarget_ = n ? n->id() : 0;
  syncHighlight();
}

// The highlight is a view of the runtime's own (nativeOnly) above everything:
// the regular paint and damage tracking draw it.
void DomAgent::syncHighlight() {
  if (!doc_) return;
  Node* target = highlightTarget_ ? doc_->get(highlightTarget_) : nullptr;
  if (!target && !highlight_) return;
  if (!highlight_) {
    highlight_ = doc_->createNode(NodeType::View);
    highlight_->nativeOnly = true;
    highlight_->setProps(Value(Value::Object{
        {"pointerEvents", Value("none")},
        {"style", Value(Value::Object{{"position", Value("absolute")},
                                      {"backgroundColor", Value("rgba(111,168,220,0.55)")},
                                      {"borderColor", Value("rgba(30,110,200,0.9)")},
                                      {"borderWidth", Value(1)},
                                      {"zIndex", Value(100000)}})}}));
  }
  std::vector<Node*> extras = doc_->root()->renderExtras();
  bool listed = std::find(extras.begin(), extras.end(), highlight_) != extras.end();
  Rect r = target ? target->absoluteRect() : Rect{};
  Value::Object s{{"left", Value(double(r.x))},
                  {"top", Value(double(r.y))},
                  {"width", Value(double(target ? r.width : 0))},
                  {"height", Value(double(target ? r.height : 0))},
                  {"opacity", Value(target ? 1 : 0)}};
  highlight_->patchStyle(s);
  if (!listed && target) {
    extras.push_back(highlight_);
    doc_->root()->setRenderExtras(extras);
  }
  doc_->markPaintDirty();
}

void DomAgent::attach(Document* doc) {
  if (doc == doc_) return;
  doc_ = doc;
  known_.clear();
  highlight_ = nullptr;
  highlightTarget_ = 0;
}

// Live updates: what changed since the frontend last saw each node.
void DomAgent::diff(const Emit& event, Node* n) {
  auto it = known_.find(n->id());
  if (it == known_.end()) return;
  if (n->type() == NodeType::RawText) {
    if (it->second.text != n->text()) {
      it->second.text = n->text();
      event("DOM.characterDataModified",
            Value(Value::Object{{"nodeId", Value(double(cdpId(n)))}, {"characterData", Value(n->text())}}));
    }
    return;
  }
  std::string attrs = attributesKey(n);
  if (attrs != it->second.attrs) {
    it->second.attrs = attrs;
    const Value a = attributes(n);
    const Value::Array& list = a.asArray();
    for (size_t i = 0; i + 1 < list.size(); i += 2)
      event("DOM.attributeModified", Value(Value::Object{{"nodeId", Value(double(cdpId(n)))},
                                                         {"name", list[i]},
                                                         {"value", list[i + 1]}}));
  }
  std::vector<Node*> kids = childrenOf(n);
  std::vector<uint32_t> now;
  for (Node* c : kids) now.push_back(c->id());
  if (now != it->second.children) {
    std::vector<uint32_t> before = it->second.children;
    for (uint32_t id : before)
      if (std::find(now.begin(), now.end(), id) == now.end()) {
        event("DOM.childNodeRemoved", Value(Value::Object{{"parentNodeId", Value(double(cdpId(n)))}, {"nodeId", Value(double(id + 1))}}));
        known_.erase(id);
      }
    uint32_t prev = 0;
    for (Node* c : kids) {
      if (std::find(before.begin(), before.end(), c->id()) == before.end())
        event("DOM.childNodeInserted", Value(Value::Object{{"parentNodeId", Value(double(cdpId(n)))},
                                                           {"previousNodeId", Value(double(prev ? prev + 1 : 0))},
                                                           {"node", describe(c, n, -1)}}));
      prev = c->id();
    }
    known_[n->id()].children = now;
  }
  for (Node* c : kids) diff(event, c);
}

void DomAgent::tick(const Emit& event) {
  if (!doc_ || !enabled_ || known_.empty()) return;
  diff(event, doc_->root());
  if (highlightTarget_) syncHighlight();  // follows its view as it moves
}

bool DomAgent::handle(const std::string& method, const Value& params, Value* result, std::string* error, const Emit& event) {
  auto ok = [&](Value v = Value(Value::Object{})) {
    *result = std::move(v);
    return true;
  };
  auto fail = [&](const char* why) {
    *error = why;
    return true;
  };
  if (!doc_) return fail("No document");
  const std::string domain = method.substr(0, method.find('.'));
  if (method == "DOM.enable" || method == "CSS.enable" || method == "Overlay.enable") {
    enabled_ = true;
    return ok();
  }
  if (method == "DOM.disable") {
    enabled_ = false;
    known_.clear();
    return ok();
  }
  if (method == "DOM.getDocument") {
    enabled_ = true;
    known_.clear();
    Value root = describe(doc_->root(), nullptr, -1);
    return ok(Value(Value::Object{{"root", Value(Value::Object{{"nodeId", Value(double(kDocumentId))},
                                                               {"backendNodeId", Value(double(kDocumentId))},
                                                               {"nodeType", Value(9)},
                                                               {"nodeName", Value("#document")},
                                                               {"localName", Value("")},
                                                               {"nodeValue", Value("")},
                                                               {"childNodeCount", Value(1)},
                                                               {"children", Value(Value::Array{std::move(root)})},
                                                               {"documentURL", Value(documentUrl)},
                                                               {"baseURL", Value(documentUrl)},
                                                               {"xmlVersion", Value("")}})}}));
  }
  if (method == "DOM.requestChildNodes") {
    Node* n = node(params);
    if (n) {
      Value::Array nodes;
      for (Node* c : childrenOf(n)) nodes.push_back(describe(c, n, -1));
      event("DOM.setChildNodes", Value(Value::Object{{"parentId", Value(double(cdpId(n)))}, {"nodes", Value(std::move(nodes))}}));
    }
    return ok();
  }
  if (method == "DOM.describeNode") {
    Node* n = node(params);
    if (!n) return fail("No node with given id found");
    return ok(Value(Value::Object{{"node", describe(n, n->parent(), 1)}}));
  }
  if (method == "DOM.pushNodesByBackendIdsToFrontend") {
    Value::Array ids;
    for (const Value& v : params["backendNodeIds"].asArray()) ids.push_back(v);
    return ok(Value(Value::Object{{"nodeIds", Value(std::move(ids))}}));
  }
  if (method == "DOM.getBoxModel" || method == "DOM.getContentQuads") {
    Node* n = node(params);
    if (!n || n->type() == NodeType::RawText) return fail("Could not compute box model.");
    Rect r = n->absoluteRect();
    if (method == "DOM.getContentQuads") return ok(Value(Value::Object{{"quads", Value(Value::Array{quad(r.x, r.y, r.width, r.height)})}}));
    YGNodeRef y = n->yoga();
    auto edge = [&](float (*get)(YGNodeConstRef, YGEdge), YGEdge e) { return y ? get(y, e) : 0.f; };
    float bl = edge(YGNodeLayoutGetBorder, YGEdgeLeft), bt = edge(YGNodeLayoutGetBorder, YGEdgeTop);
    float br = edge(YGNodeLayoutGetBorder, YGEdgeRight), bb = edge(YGNodeLayoutGetBorder, YGEdgeBottom);
    float pl = edge(YGNodeLayoutGetPadding, YGEdgeLeft), pt = edge(YGNodeLayoutGetPadding, YGEdgeTop);
    float pr = edge(YGNodeLayoutGetPadding, YGEdgeRight), pb = edge(YGNodeLayoutGetPadding, YGEdgeBottom);
    float ml = edge(YGNodeLayoutGetMargin, YGEdgeLeft), mt = edge(YGNodeLayoutGetMargin, YGEdgeTop);
    float mr = edge(YGNodeLayoutGetMargin, YGEdgeRight), mb = edge(YGNodeLayoutGetMargin, YGEdgeBottom);
    Value model = Value(Value::Object{
        {"border", quad(r.x, r.y, r.width, r.height)},
        {"padding", quad(r.x + bl, r.y + bt, r.width - bl - br, r.height - bt - bb)},
        {"content", quad(r.x + bl + pl, r.y + bt + pt, r.width - bl - br - pl - pr, r.height - bt - bb - pt - pb)},
        {"margin", quad(r.x - ml, r.y - mt, r.width + ml + mr, r.height + mt + mb)},
        {"width", Value(double(r.width))},
        {"height", Value(double(r.height))}});
    return ok(Value(Value::Object{{"model", std::move(model)}}));
  }
  if (method == "DOM.getNodeForLocation") {
    float x = float(params["x"].asNumber()), y = float(params["y"].asNumber());
    Node* hit = nullptr;
    std::vector<Node*> stack{doc_->root()};
    while (!stack.empty()) {  // the deepest, last-painted view under the point
      Node* n = stack.back();
      stack.pop_back();
      if (n->type() != NodeType::RawText && n->absoluteRect().contains(x, y)) hit = n;
      for (Node* c : childrenOf(n)) stack.insert(stack.begin(), c);
    }
    if (!hit) return fail("No node found at given location");
    return ok(Value(Value::Object{{"backendNodeId", Value(double(cdpId(hit)))}, {"nodeId", Value(double(cdpId(hit)))}, {"frameId", Value("F1")}}));
  }
  if (method == "DOM.setAttributeValue" || method == "DOM.setAttributesAsText" || method == "DOM.removeAttribute") {
    Node* n = node(params);
    if (!n || n->type() == NodeType::RawText) return fail("No node with given id found");
    std::string name = params["name"].asString(), value = params["value"].asString();
    bool remove = method == "DOM.removeAttribute";
    if (method == "DOM.setAttributesAsText") {
      // name="value" (DevTools' attribute editor); empty text removes it.
      std::string text = params["text"].asString();
      size_t eq = text.find('=');
      if (text.find_first_not_of(" \t") == std::string::npos) {
        remove = true;
      } else if (eq != std::string::npos) {
        name = text.substr(0, eq);
        name.erase(name.find_last_not_of(" \t") + 1);
        name.erase(0, name.find_first_not_of(" \t"));
        value = text.substr(eq + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
      } else {
        name = text;
        value = "";
      }
    }
    if (name == "style") {
      setStyle(n, remove ? Value::Object{} : parseCss(value));
      return ok();
    }
    Value props = n->props().isObject() ? n->props() : Value(Value::Object{});
    if (remove) props.mutableObject().erase(name);
    else props.mutableObject().insert_or_assign(name, parseValue(value).isNull() ? Value(value) : parseValue(value));
    n->setProps(props);
    doc_->markPaintDirty();
    return ok();
  }
  if (method == "DOM.setNodeValue") {
    Node* n = node(params);
    if (!n || n->type() != NodeType::RawText) return fail("Can only set the text of a text node");
    n->setText(params["value"].asString());
    doc_->markPaintDirty();
    return ok();
  }
  if (method == "DOM.highlightNode" || method == "Overlay.highlightNode") {
    highlight(node(params));
    return ok();
  }
  if (method == "DOM.hideHighlight" || method == "Overlay.hideHighlight") {
    highlight(nullptr);
    return ok();
  }
  if (method == "CSS.getMatchedStylesForNode") {
    Node* n = node(params);
    if (!n || n->type() == NodeType::RawText) return fail("No node with given id found");
    return ok(Value(Value::Object{{"inlineStyle", styleObject(n)},
                                  {"matchedCSSRules", Value(Value::Array{})},
                                  {"pseudoElements", Value(Value::Array{})},
                                  {"inherited", Value(Value::Array{})},
                                  {"cssKeyframesRules", Value(Value::Array{})}}));
  }
  if (method == "CSS.getInlineStylesForNode") {
    Node* n = node(params);
    if (!n || n->type() == NodeType::RawText) return fail("No node with given id found");
    return ok(Value(Value::Object{{"inlineStyle", styleObject(n)}}));
  }
  if (method == "CSS.getComputedStyleForNode") {
    Node* n = node(params);
    if (!n) return fail("No node with given id found");
    Value::Array list;
    Rect r = n->absoluteRect();
    auto add = [&](const std::string& k, const std::string& v) {
      list.push_back(Value(Value::Object{{"name", Value(k)}, {"value", Value(v)}}));
    };
    add("x", formatValue(Value(double(r.x))));
    add("y", formatValue(Value(double(r.y))));
    add("layoutWidth", formatValue(Value(double(r.width))));
    add("layoutHeight", formatValue(Value(double(r.height))));
    for (auto& [k, v] : styleOf(n)) add(k, formatValue(v));
    return ok(Value(Value::Object{{"computedStyle", Value(std::move(list))}}));
  }
  if (method == "CSS.setStyleTexts") {
    Value::Array styles;
    for (const Value& e : params["edits"].asArray()) {
      const std::string& sheet = e["styleSheetId"].asString();
      if (sheet.rfind("inline:", 0) != 0) return fail("Only a view's own style can be edited");
      Node* n = doc_->get(uint32_t(std::strtoul(sheet.c_str() + 7, nullptr, 10)) - 1);
      if (!n) return fail("No node with given id found");
      setStyle(n, parseCss(e["text"].asString()));
      styles.push_back(styleObject(n));
    }
    return ok(Value(Value::Object{{"styles", Value(std::move(styles))}}));
  }
  if (method == "CSS.getPlatformFontsForNode") return ok(Value(Value::Object{{"fonts", Value(Value::Array{})}}));
  if (method == "CSS.takeComputedStyleUpdates") return ok(Value(Value::Object{{"nodeIds", Value(Value::Array{})}}));
  if (method == "CSS.getBackgroundColors") return ok(Value(Value::Object{{"backgroundColors", Value(Value::Array{})}}));
  if (method == "DOM.resolveNode" || method == "DOM.getOuterHTML" || method == "DOM.setOuterHTML" ||
      method == "DOM.querySelector" || method == "DOM.querySelectorAll" || method == "DOM.performSearch")
    return fail("Not supported by a JSUI runtime");
  (void)domain;
  return ok();  // the rest (enable / disable / track... / overlay settings): nothing to do
}

}  // namespace jsui

#endif  // JSUI_DEBUGGER
