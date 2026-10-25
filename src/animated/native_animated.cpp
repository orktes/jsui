#include "animated/native_animated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/document.h"
#include "core/node.h"
#include "core/number.h"
#include "core/state_style.h"

namespace jsui {

std::string formatAnimatedNumber(double v) { return formatNumber(v); }

namespace {

std::vector<double> numbers(const Value& v) {
  std::vector<double> out;
  for (const Value& e : v.asArray()) out.push_back(e.asNumber());
  return out;
}

double jsRound(double v) { return std::floor(v + 0.5); }

// Linear interpolation in uniformly spaced samples of a curve on [0, 1];
// outside, the end segments are extended.
double sampleCurve(const std::vector<double>& s, double t) {
  size_t n = s.size();
  if (n == 0) return t;
  if (n == 1) return s[0];
  double f = t * double(n - 1);
  size_t i = f <= 0 ? 0 : std::min(size_t(f), n - 2);
  double frac = f - double(i);
  return s[i] + (s[i + 1] - s[i]) * frac;
}

}  // namespace

NativeAnimated::ANode* NativeAnimated::node(int tag) {
  auto it = nodes_.find(tag);
  return it == nodes_.end() ? nullptr : &it->second;
}

bool NativeAnimated::createNode(int tag, const Value& c, std::string* error) {
  const std::string& type = c["type"].asString();
  ANode n;
  n.tag = tag;
  if (type == "value") {
    n.type = Type::Value;
    n.value = c["value"].asNumber();
    n.offset = c["offset"].asNumber();
  } else if (type == "interpolation") {
    n.type = Type::Interpolation;
    n.inputRange = numbers(c["inputRange"]);
    for (const Value& ch : c["outputs"].asArray()) n.outputs.push_back(numbers(ch));
    const std::string& kind = c["outputType"].asString();
    n.outKind = kind == "color" ? OutKind::Color : kind == "string" ? OutKind::String : OutKind::Number;
    for (const Value& p : c["parts"].asArray()) n.parts.push_back(p.asString());
    n.easing = numbers(c["easing"]);
    auto ex = [](const Value& v) {
      const std::string& s = v.asString();
      return s == "clamp" ? Extrapolate::Clamp : s == "identity" ? Extrapolate::Identity : Extrapolate::Extend;
    };
    n.left = ex(c["extrapolateLeft"]);
    n.right = ex(c["extrapolateRight"]);
    bool ok = n.inputRange.size() >= 2 && !n.outputs.empty();
    for (auto& o : n.outputs) ok = ok && o.size() == n.inputRange.size();
    if (!ok) {
      if (error) *error = "interpolation: inputRange / outputs mismatch";
      return false;
    }
  } else if (type == "addition" || type == "subtraction" || type == "multiplication" || type == "division") {
    n.type = type == "addition"         ? Type::Addition
             : type == "subtraction"    ? Type::Subtraction
             : type == "multiplication" ? Type::Multiplication
                                        : Type::Division;
    for (const Value& t : c["input"].asArray()) n.inputs.push_back(int(t.asNumber()));
  } else if (type == "modulus") {
    n.type = Type::Modulus;
    n.inputs.push_back(int(c["input"].asNumber()));
    n.modulus = c["modulus"].asNumber(1);
  } else if (type == "diffclamp") {
    n.type = Type::DiffClamp;
    n.inputs.push_back(int(c["input"].asNumber()));
    n.min = c["min"].asNumber();
    n.max = c["max"].asNumber();
    if (c["last"].isNumber() && c["value"].isNumber()) {  // continue the JS node's state
      n.diffInit = true;
      n.lastInput = c["last"].asNumber();
      n.clamped = c["value"].asNumber();
    }
  } else if (type == "color") {
    n.type = Type::Color;
    for (const char* k : {"r", "g", "b", "a"}) n.inputs.push_back(int(c[k].asNumber()));
  } else if (type == "hostBuffer") {
    n.type = Type::HostBuffer;
    n.bufferKey = c["key"].asString();
    double index = c["index"].asNumber();
    n.bufferIndex = index >= 0 && index < 4294967296.0 ? size_t(index) : size_t(-1);
  } else if (type == "hostState") {
    n.type = Type::HostState;
    n.bufferKey = c["key"].asString();
    for (const Value& p : c["path"].asArray())
      n.statePath.push_back(p.isNumber() ? std::to_string(int64_t(p.asNumber())) : p.asString());
  } else if (type == "format") {
    n.type = Type::Format;
    n.inputs.push_back(int(c["input"].asNumber()));
    n.decimals = c["decimals"].isNumber() ? std::clamp(int(c["decimals"].asNumber()), -1, 20) : -1;
    n.minDigits = std::clamp(int(c["minDigits"].asNumber()), 0, 40);
    n.grouping = c["grouping"].asString();
    n.prefix = c["prefix"].asString();
    n.suffix = c["suffix"].asString();
    n.timePattern = c["time"].asString();
    n.outKind = OutKind::String;
  } else if (type == "clock") {
    n.type = Type::Clock;
  } else if (type == "interaction") {
    n.type = Type::Interaction;
    n.interactionKey = c["key"].asString();
    n.instant = c["instant"].asBool();
    const std::string& st = c["state"].asString();
    n.slot = st == "pressed" ? kPressed : st == "focused" ? kFocused : st == "focusVisible" ? kFocusVisible
             : st == "disabled" ? kDisabled : kHovered;
  } else if (type == "expression") {
    n.type = Type::Expression;
    for (const Value& t : c["input"].asArray()) n.inputs.push_back(int(t.asNumber()));
    if (!compileExpression(n, c, error)) return false;
  } else if (type == "transform") {
    n.type = Type::Transform;
    for (const Value& t : c["transforms"].asArray()) {
      TransformEntry e;
      e.property = t["property"].asString();
      if (t["nodeTag"].isNumber()) e.tag = int(t["nodeTag"].asNumber());
      else e.staticValue = t["value"];
      n.transforms.push_back(std::move(e));
    }
  } else if (type == "style") {
    n.type = Type::Style;
    for (auto& [k, v] : c["style"].asObject())
      if (v.isNumber()) n.style.push_back({k, int(v.asNumber()), paintStyleGroup(k)});
  } else if (type == "props") {
    n.type = Type::Props;
    n.styleTag = int(c["props"]["style"].asNumber());
    n.textTag = int(c["props"]["text"].asNumber());
    n.visibleTag = int(c["props"]["visible"].asNumber());
  } else {
    if (error) *error = "unsupported animated node type '" + type + "'";
    return false;
  }
  if (nodes_.count(tag)) dropNode(tag);
  if (n.type == Type::HostBuffer) bufferNodes_[n.bufferKey].push_back(tag);
  if (n.type == Type::HostState) stateNodes_[n.bufferKey].push_back(tag);
  if (n.type == Type::Props) propsNodes_.push_back(tag);
  if (n.type == Type::Clock) clockNodes_.push_back(tag);
  if (n.type == Type::Interaction) interactionNodes_.push_back(tag);
  nodes_.emplace(tag, std::move(n));
  return true;
}

void NativeAnimated::connectNodes(int parent, int child) {
  ANode* p = node(parent);
  ANode* c = node(child);
  if (!p || !c) return;
  if (std::find(p->children.begin(), p->children.end(), child) == p->children.end()) p->children.push_back(child);
  if (std::find(c->parents.begin(), c->parents.end(), parent) == c->parents.end()) c->parents.push_back(parent);
  markDirty(child);
}

void NativeAnimated::disconnectNodes(int parent, int child) {
  if (ANode* p = node(parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), child), p->children.end());
  if (ANode* c = node(child)) c->parents.erase(std::remove(c->parents.begin(), c->parents.end(), parent), c->parents.end());
}

void NativeAnimated::connectToView(int propsTag, uint32_t viewId) {
  ANode* p = node(propsTag);
  if (!p || p->type != Type::Props) return;
  p->view = viewId;
  p->viewWritten = false;  // written at the next flush
  recheckViews_ = true;
}

void NativeAnimated::disconnectFromView(int propsTag, uint32_t viewId) {
  ANode* p = node(propsTag);
  if (p && p->view == viewId) p->view = 0;
}

void NativeAnimated::dropNode(int tag) {
  ANode* n = node(tag);
  if (!n) return;
  for (int p : n->parents)
    if (ANode* pn = node(p)) pn->children.erase(std::remove(pn->children.begin(), pn->children.end(), tag), pn->children.end());
  for (int c : n->children)
    if (ANode* cn = node(c)) cn->parents.erase(std::remove(cn->parents.begin(), cn->parents.end(), tag), cn->parents.end());
  stopAnimationsOf(tag, true);
  mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& m) { return m.tag == tag; }),
                  mappings_.end());
  listening_.erase(tag);
  if (n->queued) dirtyValues_.erase(std::remove(dirtyValues_.begin(), dirtyValues_.end(), tag), dirtyValues_.end());
  if (n->notifyQueued) dirtyNotify_.erase(std::remove(dirtyNotify_.begin(), dirtyNotify_.end(), tag), dirtyNotify_.end());
  if (n->type == Type::HostBuffer) {
    auto it = bufferNodes_.find(n->bufferKey);
    if (it != bufferNodes_.end()) {
      it->second.erase(std::remove(it->second.begin(), it->second.end(), tag), it->second.end());
      if (it->second.empty()) bufferNodes_.erase(it);
    }
  }
  if (n->type == Type::HostState) {
    auto it = stateNodes_.find(n->bufferKey);
    if (it != stateNodes_.end()) {
      it->second.erase(std::remove(it->second.begin(), it->second.end(), tag), it->second.end());
      if (it->second.empty()) stateNodes_.erase(it);
    }
  }
  if (n->type == Type::Props) propsNodes_.erase(std::remove(propsNodes_.begin(), propsNodes_.end(), tag), propsNodes_.end());
  if (n->type == Type::Clock) clockNodes_.erase(std::remove(clockNodes_.begin(), clockNodes_.end(), tag), clockNodes_.end());
  if (n->type == Type::Interaction)
    interactionNodes_.erase(std::remove(interactionNodes_.begin(), interactionNodes_.end(), tag), interactionNodes_.end());
  nodes_.erase(tag);
}

void NativeAnimated::bufferChanged(const std::string& key) {
  auto it = bufferNodes_.find(key);
  if (it == bufferNodes_.end()) return;
  for (int tag : it->second) markDirty(tag);
}

void NativeAnimated::stateChanged(const std::string& key) {
  auto it = stateNodes_.find(key);
  if (it == stateNodes_.end()) return;
  for (int tag : it->second) markDirty(tag);
}

void NativeAnimated::clear() {
  nodes_.clear();
  stateNodes_.clear();
  propsNodes_.clear();
  animations_.clear();
  mappings_.clear();
  listening_.clear();
  dirtyValues_.clear();
  dirtyNotify_.clear();
  bufferNodes_.clear();
  clockNodes_.clear();
  interactionNodes_.clear();
  groups_.clear();
  leafDone_.clear();
  leafGroup_.clear();
  recheckViews_ = true;
  ends.clear();
  updates.clear();
}

void NativeAnimated::viewDestroyed(uint32_t viewId) {
  for (int tag : propsNodes_)
    if (ANode* n = node(tag); n && n->view == viewId) n->view = 0;
  mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& m) { return m.view == viewId; }),
                  mappings_.end());
}

// ------------------------------------------------------------------ values

void NativeAnimated::markDirty(int tag) {
  if (ANode* n = node(tag)) markDirty(*n);
}

void NativeAnimated::markDirty(ANode& n) {
  if (n.queued) return;
  n.queued = true;
  dirtyValues_.push_back(n.tag);
}

void NativeAnimated::markNotify(ANode& n) {
  if (n.notifyQueued) return;
  n.notifyQueued = true;
  dirtyNotify_.push_back(n.tag);
}

void NativeAnimated::setValue(int tag, double v) {
  ANode* n = node(tag);
  if (!n || n->type != Type::Value) return;
  stopAnimationsOf(tag, true);
  n->value = v;
  markDirty(tag);
  flush();
}

void NativeAnimated::setOffset(int tag, double offset) {
  ANode* n = node(tag);
  if (!n || n->type != Type::Value) return;
  n->offset = offset;
  markDirty(tag);
  flush();
}

void NativeAnimated::flattenOffset(int tag) {
  ANode* n = node(tag);
  if (!n || n->type != Type::Value) return;
  n->value += n->offset;
  n->offset = 0;
}

void NativeAnimated::extractOffset(int tag) {
  ANode* n = node(tag);
  if (!n || n->type != Type::Value) return;
  n->offset += n->value;
  n->value = 0;
}

double NativeAnimated::getValue(int tag) const {
  auto it = nodes_.find(tag);
  return it == nodes_.end() ? 0 : it->second.value;
}

void NativeAnimated::snapshotValues(std::vector<std::pair<int, double>>* out) const {
  out->clear();
  for (auto& [tag, n] : nodes_)
    if (n.type == Type::Value) out->emplace_back(tag, n.value);
}

void NativeAnimated::snapshotAnimations(std::vector<std::pair<int, double>>* out) const {
  out->clear();
  for (const Anim& a : animations_)
    if (a.id >= 0) out->emplace_back(a.id, a.kind == Anim::Kind::Spring ? a.lastVelocity : 0);
  for (const auto& g : groups_)
    if (!g->done) out->emplace_back(g->id, 0);
}

double NativeAnimated::nodeOutput(int tag) {
  gen_++;
  return evalNum(tag);
}

Value NativeAnimated::nodeStyleValue(int tag) {
  gen_++;
  return evalOut(tag);
}

// -------------------------------------------------------------- evaluation

double NativeAnimated::interpolate(ANode& n, size_t ch, double input) {
  const auto& in = n.inputRange;
  const auto& out = n.outputs[ch];
  size_t i = 1;
  for (; i < in.size() - 1; ++i)
    if (in[i] >= input) break;
  i--;
  double inMin = in[i], inMax = in[i + 1], outMin = out[i], outMax = out[i + 1];
  double result = input;
  if (result < inMin) {
    if (n.left == Extrapolate::Identity) return result;
    if (n.left == Extrapolate::Clamp) result = inMin;
  }
  if (result > inMax) {
    if (n.right == Extrapolate::Identity) return result;
    if (n.right == Extrapolate::Clamp) result = inMax;
  }
  if (outMin == outMax) return outMin;
  if (inMin == inMax) return input <= inMin ? outMin : outMax;
  if (std::isinf(inMin) && inMin < 0) result = -result;
  else if (std::isinf(inMax) && inMax > 0) result = result - inMin;
  else result = (result - inMin) / (inMax - inMin);
  if (!n.easing.empty()) result = sampleCurve(n.easing, result);
  if (std::isinf(outMin) && outMin < 0) result = -result;
  else if (std::isinf(outMax) && outMax > 0) result = result + outMin;
  else result = result * (outMax - outMin) + outMin;
  return result;
}

void NativeAnimated::setRgba(ANode& n, double r, double g, double b, double a) {
  std::string& s = n.str;
  s.assign("rgba(");
  s += formatAnimatedNumber(r);
  s += ", ";
  s += formatAnimatedNumber(g);
  s += ", ";
  s += formatAnimatedNumber(b);
  s += ", ";
  s += formatAnimatedNumber(a);
  s += ')';
  n.out.setString(s);
}

void NativeAnimated::evaluate(ANode& n) {
  if (n.evalGen == gen_) return;
  n.evalGen = gen_;  // set first: a cycle evaluates to the previous output
  auto in = [&](size_t i) { return i < n.inputs.size() ? evalNum(n.inputs[i]) : 0.0; };
  switch (n.type) {
    case Type::Value:
      n.num = n.value + n.offset;
      n.out = Value(n.num);
      break;
    case Type::HostBuffer: {
      if (!n.bufferEntry || !buffers || n.bufferGen != buffers->generation()) {
        n.bufferEntry = buffers ? buffers->find(n.bufferKey) : nullptr;
        n.bufferGen = n.bufferEntry ? buffers->generation() : 0;
      }
      n.num = n.bufferEntry ? n.bufferEntry->at(n.bufferIndex) : 0;
      if (n.num != n.num) n.num = 0;  // NaN
      n.out = Value(n.num);
      break;
    }
    case Type::HostState: {
      // The store's lock while `j` points into it (JS may write on its thread).
      std::unique_lock<std::recursive_mutex> stateLock;
      if (state) stateLock = state->lock();
      const Json* j = state ? state->value(n.bufferKey) : nullptr;
      for (size_t i = 0; j && i < n.statePath.size(); i++) {
        const std::string& seg = n.statePath[i];
        if (j->kind() == Json::Kind::Array) {
          char* end = nullptr;
          unsigned long k = std::strtoul(seg.c_str(), &end, 10);
          j = end != seg.c_str() && *end == 0 && k < j->items().size() ? &j->items()[k] : nullptr;
        } else {
          j = j->find(seg);
        }
      }
      n.num = 0;
      if (j && j->kind() == Json::Kind::Number) n.num = std::strtod(j->text().c_str(), nullptr);
      else if (j && j->kind() == Json::Kind::Bool) n.num = j->boolean() ? 1 : 0;
      if (n.num != n.num) n.num = 0;
      if (j && j->kind() == Json::Kind::String) n.out = Value(j->text());  // text; num stays 0
      else n.out = Value(n.num);
      break;
    }
    case Type::Clock:
      n.num = clockMs_ / 1000;
      n.out = Value(n.num);
      break;
    case Type::Interaction:
      n.num = interactionWeight(n);
      n.out = Value(n.num);
      break;
    case Type::Expression:
      n.num = runExpression(n);
      n.out = Value(n.num);
      break;
    case Type::Format:
      n.num = in(0);
      formatNumber(n, n.num);
      n.out.setString(n.str);
      break;
    case Type::Interpolation: {
      double x = n.parents.empty() ? 0 : evalNum(n.parents[0]);
      if (n.outKind == OutKind::Number) {
        n.num = interpolate(n, 0, x);
        n.out = Value(n.num);
      } else if (n.outKind == OutKind::Color && n.outputs.size() >= 4) {
        auto ch = [&](size_t c) { return std::clamp(interpolate(n, c, x), 0.0, 255.0); };
        double r = jsRound(ch(0)), g = jsRound(ch(1)), b = jsRound(ch(2));
        double a = std::clamp(interpolate(n, 3, x), 0.0, 1.0);
        a = jsRound(a * 1000) / 1000;
        n.num = r;
        setRgba(n, r, g, b, a);
      } else {
        std::string& s = n.str;
        s.clear();
        for (size_t i = 0; i < n.parts.size(); i++) {
          s += n.parts[i];
          if (i < n.outputs.size() && i + 1 < n.parts.size()) {
            double v = interpolate(n, i, x);
            if (i == 0) n.num = v;
            s += formatAnimatedNumber(jsRound(v * 1e5) / 1e5);
          }
        }
        n.out.setString(s);
      }
      break;
    }
    case Type::Addition: n.num = in(0) + in(1); n.out = Value(n.num); break;
    case Type::Subtraction: n.num = in(0) - in(1); n.out = Value(n.num); break;
    case Type::Multiplication: n.num = in(0) * in(1); n.out = Value(n.num); break;
    case Type::Division: {
      double b = in(1);
      n.num = b == 0 ? 0 : in(0) / b;  // RN's native division yields 0 (and warns in JS)
      n.out = Value(n.num);
      break;
    }
    case Type::Modulus: {
      double m = n.modulus;
      n.num = std::fmod(std::fmod(in(0), m) + m, m);
      n.out = Value(n.num);
      break;
    }
    case Type::DiffClamp: {
      double cur = in(0);
      if (!n.diffInit) {
        n.diffInit = true;
        n.lastInput = cur;
        n.clamped = cur;
      }
      double diff = cur - n.lastInput;
      n.lastInput = cur;
      n.clamped = std::min(std::max(n.clamped + diff, n.min), n.max);
      n.num = n.clamped;
      n.out = Value(n.num);
      break;
    }
    case Type::Color: {
      double r = jsRound(in(0)), g = jsRound(in(1)), b = jsRound(in(2)), a = in(3);
      n.num = r;
      setRgba(n, r, g, b, a);
      break;
    }
    case Type::Transform: {
      // [{property: value}, ...], built once and then updated in place.
      if (!n.out.isArray() || n.out.asArray().size() != n.transforms.size()) {
        Value::Array arr;
        for (auto& t : n.transforms) arr.emplace_back(Value::Object{{t.property, Value()}});
        n.out = Value(std::move(arr));
      }
      // Only this case writes a transform node's `out`, so every entry is
      // still the one-key object built for its property.
      Value::Array& arr = n.out.mutableArray();
      for (size_t i = 0; i < arr.size(); i++) {
        const TransformEntry& t = n.transforms[i];
        arr[i].mutableObject().begin()->second.assignReusing(t.tag ? evalOut(t.tag) : t.staticValue);
      }
      break;
    }
    case Type::Style: {
      Value::Object o;
      for (auto& e : n.style) o[e.key] = evalOut(e.tag);
      n.out = Value(std::move(o));
      break;
    }
    case Type::Props:
      n.out = n.styleTag ? evalOut(n.styleTag) : Value(Value::Object{});
      break;
  }
}

double NativeAnimated::evalNum(int tag) {
  ANode* n = node(tag);
  if (!n) return 0;
  evaluate(*n);
  return n->num;
}

const Value& NativeAnimated::evalOut(int tag) {
  static const Value null;
  ANode* n = node(tag);
  if (!n) return null;
  evaluate(*n);
  return n->out;
}

Node* NativeAnimated::viewOf(const ANode& p) {
  return p.view ? (findView ? findView(p.view) : doc_.get(p.view)) : nullptr;
}

void NativeAnimated::applyProps(ANode& p) {
  Node* view = viewOf(p);
  if (!view) return;
  doc_.perf.counters.animatedWrites++;
  // patchStyle skips unchanged values and, for the paint keys the JS side
  // lets through, updates the style in place and re-resolves just those
  // keys' style groups: no relayout, only the node's rects repaint.
  ANode* style = p.styleTag ? node(p.styleTag) : nullptr;
  if (style && style->type == Type::Style) {
    // The usual graph (props -> style -> values / transform): the style
    // node's inputs go straight into the view, keyed by the strings and
    // paint groups resolved when the node was created, without building a
    // partial style object.
    writes_.clear();
    for (const StyleEntry& e : style->style) writes_.push_back({&e.key, e.group, &evalOut(e.tag)});
    view->patchStyle(writes_.data(), writes_.size());
  } else if (!p.textTag && !p.visibleTag) {
    view->patchStyle(evalOut(p.tag).asObject());
  }
  if (p.visibleTag) view->setHidden(evalNum(p.visibleTag) == 0);
  if (p.textTag) {
    const Value& text = evalOut(p.textTag);
    std::string num;
    if (!text.isString()) num = formatAnimatedNumber(text.asNumber());
    const std::string& str = text.isString() ? text.asString() : num;
    for (Node* c : view->children())
      if (c->type() == NodeType::RawText) {
        if (c->text() != str) c->setText(str);
        break;
      }
  }
  p.viewPaintVersion = view->paintVersion;
  p.viewWritten = true;
}

void NativeAnimated::flush() {
  if (nodes_.empty()) return;
  gen_++;
  uint64_t visit = ++visitGen_;
  // Props nodes downstream of changed values.
  std::vector<int>& stack = stack_;
  std::vector<ANode*>& props = props_;
  props.clear();
  stack.assign(dirtyValues_.begin(), dirtyValues_.end());
  for (int t : dirtyValues_)
    if (ANode* n = node(t)) n->queued = false;
  dirtyValues_.clear();
  while (!stack.empty()) {
    int t = stack.back();
    stack.pop_back();
    ANode* n = node(t);
    if (!n || n->visitGen == visit) continue;
    n->visitGen = visit;
    if (n->type == Type::Props) props.push_back(n);
    for (int c : n->children) stack.push_back(c);
  }
  // ... and props whose view React re-propped since our last write (the
  // reconciler sends the value it rendered with; ours is the live one).
  // Only setProps (or a patchStyle not made here) changes a view's style,
  // so without one since the last check (and no new connection) there is
  // nothing to look for.
  if (recheckViews_ || propsEpoch_ != doc_.perf.propsEpoch) {
    recheckViews_ = false;
    for (int tag : propsNodes_) {
      ANode* n = node(tag);
      if (!n || !n->view || n->visitGen == visit) continue;
      Node* v = viewOf(*n);
      if (v && (!n->viewWritten || v->paintVersion != n->viewPaintVersion)) props.push_back(n);
      else if (!v && !n->viewWritten) recheckViews_ = true;  // its view may come later
    }
  }
  for (ANode* p : props) applyProps(*p);
  // Our own writes may go through setProps / patchStyle (a layout key, a
  // props node without a style node): not React's.
  propsEpoch_ = doc_.perf.propsEpoch;
}

// --------------------------------------------------------------- animations

bool NativeAnimated::parseAnim(Anim& a, const Value& c, std::string* error) {
  a.delay = std::max(0.0, c["delay"].asNumber());
  a.iterations = c["iterations"].isNumber() ? int(c["iterations"].asNumber()) : 1;
  const std::string& type = c["type"].asString();
  if (type == "frames") {
    a.kind = Anim::Kind::Frames;
    a.frames = numbers(c["frames"]);
    a.to = c["toValue"].asNumber();
    a.duration = c["duration"].isNumber() ? c["duration"].asNumber()
                                          : double(std::max<size_t>(a.frames.size(), 1) - 1) * (1000.0 / 60.0);
    if (a.frames.empty()) a.frames = {0, 1};
  } else if (type == "spring") {
    a.kind = Anim::Kind::Spring;
    a.k = c["stiffness"].asNumber(100);
    a.c = c["damping"].asNumber(10);
    a.m = c["mass"].asNumber(1);
    a.v0 = c["initialVelocity"].asNumber();
    a.to = c["toValue"].asNumber();
    a.overshootClamping = c["overshootClamping"].asBool();
    a.restD = c["restDisplacementThreshold"].asNumber(0.001);
    a.restS = c["restSpeedThreshold"].asNumber(0.001);
  } else if (type == "decay") {
    a.kind = Anim::Kind::Decay;
    a.velocity = c["velocity"].asNumber();
    a.deceleration = c["deceleration"].asNumber(0.998);
  } else if (type == "clock") {
    a.kind = Anim::Kind::Clock;
    a.speed = c["speed"].asNumber(1);
    if (a.speed != a.speed) a.speed = 0;
    a.runningTag = c["running"].isNumber() ? int(c["running"].asNumber()) : 0;
    a.iterations = 1;
  } else {
    if (error) *error = "unsupported animation type '" + type + "'";
    return false;
  }
  return true;
}

bool NativeAnimated::startAnimation(int id, int tag, const Value& c, double nowMs, std::string* error) {
  if (c["type"].asString() == "group") {
    auto g = std::make_unique<Group>();
    g->id = id;
    if (!parseStep(g->root, c["root"], error)) return false;
    Group& ref = *g;
    groups_.push_back(std::move(g));
    runStep(ref, ref.root, nowMs, [this, id](bool finished, double) { endGroup(id, finished); });
    purgeGroups();
    flush();
    return true;
  }
  ANode* n = node(tag);
  if (!n || n->type != Type::Value) {
    if (error) *error = "startAnimation: no animated value node " + std::to_string(tag);
    return false;
  }
  Anim a;
  a.id = id;
  a.tag = tag;
  if (!parseAnim(a, c, error)) return false;
  if (a.iterations == 0) {
    ends.push_back({id, true, n->value});
    return true;
  }
  stopAnimationsOf(tag, true);
  a.from = n->value;
  beginIteration(a, nowMs);
  if (a.kind == Anim::Kind::Clock && a.runningTag) {
    gen_++;
    a.held = evalNum(a.runningTag) == 0;  // gated from the start
  }
  animations_.push_back(std::move(a));
  // A zero-length timing applies at once; its end is still reported (with
  // the other ends, after the tick), so a sequence or loop it is part of
  // goes on: Animated.loop(sequence([timing, timing({duration: 0})])).
  Anim& back = animations_.back();
  if (back.kind == Anim::Kind::Frames && back.duration <= 0 && back.delay <= 0) {
    if (step(back, nowMs)) {
      animations_.pop_back();
      ends.push_back({id, true, n->value});
      flush();
    }
  }
  return true;
}

// ------------------------------------------------------------------ groups

bool NativeAnimated::parseStep(Step& s, const Value& c, std::string* error) {
  const std::string& type = c["type"].asString();
  if (type == "leaf") {
    s.kind = Step::Kind::Leaf;
    s.tag = int(c["tag"].asNumber());
    s.config = c["config"];
    ANode* n = node(s.tag);
    Anim probe;
    if (!n || n->type != Type::Value) {
      if (error) *error = "group: no animated value node " + std::to_string(s.tag);
      return false;
    }
    return parseAnim(probe, s.config, error);
  }
  if (type == "wait") {
    s.kind = Step::Kind::Wait;
    s.wait = std::max(0.0, c["duration"].asNumber());
    return true;
  }
  if (type == "sequence" || type == "parallel") {
    s.kind = type == "sequence" ? Step::Kind::Sequence : Step::Kind::Parallel;
    s.stopTogether = !c["stopTogether"].isBool() || c["stopTogether"].asBool();
    for (const Value& ch : c["children"].asArray()) {
      s.children.emplace_back();
      if (!parseStep(s.children.back(), ch, error)) return false;
    }
    return true;
  }
  if (type == "loop") {
    s.kind = Step::Kind::Loop;
    s.iterations = c["iterations"].isNumber() ? int(c["iterations"].asNumber()) : -1;
    for (const Value& r : c["resets"].asArray())
      if (r.asArray().size() >= 2) s.resets.emplace_back(int(r.asArray()[0].asNumber()), r.asArray()[1].asNumber());
    s.children.emplace_back();
    return parseStep(s.children.back(), c["child"], error);
  }
  if (error) *error = "group: unknown step type '" + type + "'";
  return false;
}

NativeAnimated::Group* NativeAnimated::group(int id) {
  for (auto& g : groups_)
    if (g->id == id && !g->done) return g.get();
  return nullptr;
}

// Starts `s` at nowMs; `done` runs (once) when it ends, possibly right away.
// Mirrors animations.js: a sequence starts its next step from the previous
// one's end, a parallel ends when all children ended (stopping the others
// on an unfinished end when stopTogether), a loop resets and restarts.
void NativeAnimated::runStep(Group& g, const Step& s, double nowMs, Done done) {
  int gid = g.id;
  switch (s.kind) {
    case Step::Kind::Leaf:
    case Step::Kind::Wait: {
      Anim a;
      a.id = nextLeafId_--;
      if (s.kind == Step::Kind::Wait) {
        a.kind = Anim::Kind::Wait;
        a.duration = s.wait;
      } else {
        a.tag = s.tag;
        parseAnim(a, s.config, nullptr);
        ANode* n = node(s.tag);
        if (!n) {
          done(false, nowMs);
          return;
        }
        if (a.iterations == 0) {
          done(true, nowMs);
          return;
        }
        stopAnimationsOf(s.tag, true);
        a.from = n->value;
      }
      beginIteration(a, nowMs);
      int id = a.id;
      leafDone_[id] = std::move(done);
      leafGroup_[id] = gid;
      g.leaves.push_back(id);
      animations_.push_back(std::move(a));
      // Zero-length steps apply at once (as startAnimation's do).
      Anim& back = animations_.back();
      bool instant = (back.kind == Anim::Kind::Frames || back.kind == Anim::Kind::Wait) && back.duration <= 0 && back.delay <= 0;
      if (instant && step(back, nowMs)) {
        animations_.pop_back();
        leafEnded(id, true, nowMs);
      }
      return;
    }
    case Step::Kind::Sequence: {
      if (s.children.empty()) {
        done(true, nowMs);
        return;
      }
      auto next = std::make_shared<std::function<void(size_t, double)>>();
      auto doneP = std::make_shared<Done>(std::move(done));
      *next = [this, gid, &s, next, doneP](size_t i, double t) {
        Group* gp = group(gid);
        if (!gp) return;
        runStep(*gp, s.children[i], t, [this, gid, &s, next, doneP, i](bool finished, double t2) {
          if (!finished || i + 1 >= s.children.size() || !group(gid)) {
            (*doneP)(finished, t2);
            return;
          }
          (*next)(i + 1, t2);
        });
      };
      (*next)(0, nowMs);
      return;
    }
    case Step::Kind::Parallel: {
      if (s.children.empty()) {
        done(true, nowMs);
        return;
      }
      struct State {
        size_t ended = 0;
        bool allFinished = true;
        std::vector<bool> hasEnded;
        Done done;
        std::vector<int> leavesAt;  // g.leaves size when each child started (unused)
      };
      auto st = std::make_shared<State>();
      st->hasEnded.assign(s.children.size(), false);
      st->done = std::move(done);
      size_t count = s.children.size();
      for (size_t i = 0; i < count; i++) {
        Group* gp = group(gid);
        if (!gp) return;
        runStep(*gp, s.children[i], nowMs, [this, gid, &s, st, i, count](bool finished, double t) {
          if (st->hasEnded[i]) return;
          st->hasEnded[i] = true;
          st->ended++;
          if (!finished) {
            st->allFinished = false;
            if (s.stopTogether) {
              // Stop the other children's running leaves (their ends count
              // as unfinished, as the JS parallel's stop()).
              if (Group* gp = group(gid)) {
                std::vector<int> leaves = gp->leaves;
                for (int leaf : leaves) {
                  for (size_t k = 0; k < animations_.size(); k++)
                    if (animations_[k].id == leaf) {
                      animations_.erase(animations_.begin() + long(k));
                      leafEnded(leaf, false, t);
                      break;
                    }
                }
              }
            }
          }
          if (st->ended == count && st->done) {
            Done d = std::move(st->done);
            st->done = nullptr;
            d(st->allFinished, t);
          }
        });
      }
      return;
    }
    case Step::Kind::Loop: {
      if (s.iterations == 0) {
        done(true, nowMs);
        return;
      }
      auto count = std::make_shared<int>(0);
      auto run = std::make_shared<std::function<void(double)>>();
      auto doneP = std::make_shared<Done>(std::move(done));
      *run = [this, gid, &s, count, run, doneP](double t) {
        Group* gp = group(gid);
        if (!gp) return;
        if (s.iterations >= 0 && *count >= s.iterations) {
          (*doneP)(true, t);
          return;
        }
        // A loop whose iterations take no time would spin forever.
        if (gp->lastRound == t) {
          if (++gp->zeroRounds > 1000) {
            (*doneP)(false, t);
            return;
          }
        } else {
          gp->lastRound = t;
          gp->zeroRounds = 0;
        }
        ++*count;
        for (auto& [tag, v] : s.resets) {
          if (ANode* n = node(tag); n && n->type == Type::Value) {
            stopAnimationsOf(tag, true);
            n->value = v;
            markDirty(*n);
            markNotify(*n);
          }
        }
        runStep(*gp, s.children[0], t, [run, doneP](bool finished, double t2) {
          if (!finished) (*doneP)(false, t2);
          else (*run)(t2);
        });
      };
      (*run)(nowMs);
      return;
    }
  }
}

void NativeAnimated::leafEnded(int leafId, bool finished, double nowMs) {
  auto it = leafDone_.find(leafId);
  if (it == leafDone_.end()) return;
  Done done = std::move(it->second);
  leafDone_.erase(it);
  auto gi = leafGroup_.find(leafId);
  if (gi != leafGroup_.end()) {
    if (Group* g = group(gi->second)) g->leaves.erase(std::remove(g->leaves.begin(), g->leaves.end(), leafId), g->leaves.end());
    leafGroup_.erase(gi);
  }
  if (done) done(finished, nowMs);
}

void NativeAnimated::endGroup(int id, bool finished) {
  Group* g = group(id);
  if (!g) return;
  g->done = true;
  // Leaves still running (a stop): drop them without callbacks.
  for (int leaf : g->leaves) {
    leafDone_.erase(leaf);
    leafGroup_.erase(leaf);
    for (size_t k = 0; k < animations_.size(); k++)
      if (animations_[k].id == leaf) {
        animations_.erase(animations_.begin() + long(k));
        break;
      }
  }
  g->leaves.clear();
  ends.push_back({id, finished, 0});
}

// Ended groups are freed here, outside their steps' callbacks (which point
// into the group's steps).
void NativeAnimated::purgeGroups() {
  groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const std::unique_ptr<Group>& p) { return p->done; }),
                groups_.end());
}

void NativeAnimated::beginIteration(Anim& a, double nowMs) {
  a.start = nowMs + a.delay;
  a.lastVelocity = a.v0;
  a.last = a.from;
}

bool NativeAnimated::stopAnimation(int id, double* lastVelocity, double nowMs) {
  if (Group* g = group(id)) {
    // The caller reports the end: drop the group's leaves quietly.
    if (lastVelocity) *lastVelocity = 0;
    size_t before = ends.size();
    endGroup(id, false);
    ends.resize(before);
    purgeGroups();
    return true;
  }
  for (size_t i = 0; i < animations_.size(); i++) {
    if (animations_[i].id == id) {
      if (lastVelocity) *lastVelocity = animations_[i].kind == Anim::Kind::Spring ? animations_[i].lastVelocity : 0;
      if (animations_[i].kind == Anim::Kind::Clock && nowMs >= animations_[i].start) {
        gen_++;
        step(animations_[i], nowMs);
        flush();
      }
      animations_.erase(animations_.begin() + long(i));
      return true;
    }
  }
  return false;
}

void NativeAnimated::stopAnimationsOf(int tag, bool report) {
  std::vector<int> leaves;
  for (size_t i = 0; i < animations_.size();) {
    if (animations_[i].tag == tag && animations_[i].kind != Anim::Kind::Wait && animations_[i].id < 0) {
      // A group's step: the group goes on with an unfinished end (after
      // this walk: that may start other steps).
      leaves.push_back(animations_[i].id);
      animations_.erase(animations_.begin() + long(i));
    } else if (animations_[i].tag == tag && animations_[i].kind != Anim::Kind::Wait) {
      if (report) {
        ANode* n = node(tag);
        ends.push_back({animations_[i].id, false, n ? n->value : 0});
      }
      animations_.erase(animations_.begin() + long(i));
    } else {
      i++;
    }
  }
  for (int leaf : leaves) leafEnded(leaf, false, clockMs_);
}

// Same formulas as @orktes/jsui-react's JS driver (packages/react/src/animated/
// animations.js), evaluated at the same frame times, so both drivers agree.
bool NativeAnimated::step(Anim& a, double t) {
  if (a.kind == Anim::Kind::Wait) return t >= a.start + a.duration;
  ANode* n = node(a.tag);
  if (!n) return true;
  if (t < a.start) return false;
  double value = n->value;
  bool done = false;
  switch (a.kind) {
    case Anim::Kind::Frames: {
      double p = a.duration > 0 ? (t - a.start) / a.duration : 1;
      if (p >= 1) {
        value = a.duration <= 0 ? a.to : a.from + a.frames.back() * (a.to - a.from);
        done = true;
      } else {
        value = a.from + sampleCurve(a.frames, p) * (a.to - a.from);
      }
      break;
    }
    case Anim::Kind::Spring: {
      double elapsed = (t - a.start) / 1000;
      double c = a.c, m = a.m, k = a.k;
      double v0 = -a.v0;
      double zeta = c / (2 * std::sqrt(k * m));
      double omega0 = std::sqrt(k / m);
      double omega1 = omega0 * std::sqrt(1 - zeta * zeta);
      double x0 = a.to - a.from;
      double position, velocity;
      if (zeta < 1) {
        double env = std::exp(-zeta * omega0 * elapsed);
        double s = std::sin(omega1 * elapsed), co = std::cos(omega1 * elapsed);
        position = a.to - env * (((v0 + zeta * omega0 * x0) / omega1) * s + x0 * co);
        velocity = zeta * omega0 * env * ((s * (v0 + zeta * omega0 * x0)) / omega1 + x0 * co) -
                   env * (co * (v0 + zeta * omega0 * x0) - omega1 * x0 * s);
      } else {
        double env = std::exp(-omega0 * elapsed);
        position = a.to - env * (x0 + (v0 + omega0 * x0) * elapsed);
        velocity = env * (v0 * (elapsed * omega0 - 1) + elapsed * x0 * (omega0 * omega0));
      }
      a.lastVelocity = velocity;
      bool overshot = false;
      if (a.overshootClamping && k != 0) overshot = a.from < a.to ? position > a.to : position < a.to;
      bool resting = std::fabs(velocity) <= a.restS && (k == 0 || std::fabs(a.to - position) <= a.restD);
      if (overshot || resting) {
        if (k != 0) position = a.to;
        a.lastVelocity = 0;
        done = true;
      }
      value = position;
      break;
    }
    case Anim::Kind::Wait: break;
    case Anim::Kind::Clock: {
      // Gated by a node: it stops where this frame's time puts it and
      // starts again from there.
      if (a.runningTag) {
        bool run = evalNum(a.runningTag) != 0;
        if (!run && a.held) return false;
        if (!run) {
          a.held = true;
          value = a.from + (t - a.start) / 1000 * a.speed;
          a.from = value;
          a.start = t;
          break;
        }
        if (a.held) {
          a.held = false;
          a.from = n->value;
          a.start = t;
        }
      }
      value = a.from + (t - a.start) / 1000 * a.speed;
      break;
    }
    case Anim::Kind::Decay: {
      double kd = 1 - a.deceleration;
      value = a.from + (a.velocity / kd) * (1 - std::exp(-kd * (t - a.start)));
      if (std::fabs(a.last - value) < 0.1) done = true;
      a.last = value;
      break;
    }
  }
  n->value = value;
  markDirty(*n);
  markNotify(*n);
  if (!done) return false;
  a.iteration++;
  if (a.iterations < 0 || a.iteration < a.iterations) {
    n->value = a.from;  // RN's native loops restart from the start value
    beginIteration(a, t);
    return false;
  }
  return true;
}

void NativeAnimated::tick(double nowMs) {
  Perf::Scope scope(doc_.perf, Phase::Animated);
  if (nowMs != clockMs_) {
    clockMs_ = nowMs;
    for (int tag : clockNodes_)
      if (ANode* n = node(tag); n && !n->children.empty()) markDirty(*n);
  }
  // Interaction weights moved by this frame's input (InputManager::tickStates).
  if (doc_.interactionsMoved) {
    doc_.interactionsMoved = false;
    for (int tag : interactionNodes_)
      if (ANode* n = node(tag); n && !n->children.empty() && interactionWeight(*n) != n->num) markDirty(*n);
  }
  gen_++;  // clocks gated by a node read this frame's output
  std::vector<int> leaves;  // group steps that ended: they go on after this walk
  for (size_t i = 0; i < animations_.size();) {
    Anim& a = animations_[i];
    if (step(a, nowMs)) {
      if (a.id < 0) {
        leaves.push_back(a.id);
      } else {
        ANode* n = node(a.tag);
        ends.push_back({a.id, true, n ? n->value : 0});
      }
      animations_.erase(animations_.begin() + long(i));
    } else {
      i++;
    }
  }
  for (int leaf : leaves) leafEnded(leaf, true, nowMs);
  purgeGroups();
  flush();
  for (int tag : dirtyNotify_) {
    ANode* n = node(tag);
    if (!n) continue;
    n->notifyQueued = false;
    if (listening_.count(tag)) updates.push_back({tag, n->value});
  }
  dirtyNotify_.clear();
}

// --------------------------------------------------------------- format

// Mirrors formatNumber in packages/react/src/animated/nodes.js.
void NativeAnimated::formatNumber(ANode& n, double v) {
  std::string& s = n.str;
  s.assign(n.prefix);
  if (!n.timePattern.empty()) {
    // time: h / m / s fields of whole seconds (formatTime in nodes.js).
    const std::string& p = n.timePattern;
    double total = std::isfinite(v) && v > 0 ? std::floor(v) : 0;
    bool hasH = p.find('h') != std::string::npos, hasM = p.find('m') != std::string::npos;
    double h = std::floor(total / 3600);
    double m = hasH ? std::fmod(std::floor(total / 60), 60) : std::floor(total / 60);
    double sec = hasM ? std::fmod(total, 60) : total;
    for (size_t i = 0; i < p.size();) {
      char c = p[i];
      if (c == 'h' || c == 'm' || c == 's') {
        size_t j = i;
        while (j < p.size() && p[j] == c) j++;
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.0f", c == 'h' ? h : c == 'm' ? m : sec);
        size_t len = std::strlen(buf);
        if (len < j - i) s.append(j - i - len, '0');
        s += buf;
        i = j;
      } else {
        s += c;
        i++;
      }
    }
    s += n.suffix;
    return;
  }
  if (!std::isfinite(v)) {
    s += formatAnimatedNumber(v);
    s += n.suffix;
    return;
  }
  std::string digits;
  if (n.decimals < 0) {
    digits = formatAnimatedNumber(v);
  } else {
    // toFixed: round half away from zero on the decimal value.
    char buf[64];
    double scale = std::pow(10.0, n.decimals);
    double r = std::round(std::fabs(v) * scale) / scale;
    std::snprintf(buf, sizeof buf, "%.*f", n.decimals, r);
    digits = (v < 0 && r != 0 ? "-" : "") + std::string(buf);
  }
  bool neg = !digits.empty() && digits[0] == '-';
  size_t start = neg ? 1 : 0;
  size_t dot = digits.find('.', start);
  size_t intEnd = dot == std::string::npos ? digits.size() : dot;
  std::string intPart = digits.substr(start, intEnd - start);
  if (int(intPart.size()) < n.minDigits) intPart.insert(0, size_t(n.minDigits) - intPart.size(), '0');
  if (!n.grouping.empty() && intPart.find('e') == std::string::npos) {
    std::string g;
    size_t len = intPart.size();
    for (size_t i = 0; i < len; i++) {
      if (i > 0 && (len - i) % 3 == 0) g += n.grouping;
      g += intPart[i];
    }
    intPart = g;
  }
  if (neg) s += '-';
  s += intPart;
  if (dot != std::string::npos) s.append(digits, dot, std::string::npos);
  s += n.suffix;
}

// -------------------------------------------------------------- expressions

namespace {

// Function ids of expression.js FUNCS, with their arity.
constexpr uint8_t kFuncArity[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
                                  1, 1, 1, 2, 2, 2, 2, 3, 3, 2, 3, 1, 2};
constexpr size_t kFuncCount = sizeof kFuncArity;

bool truthy(double v) { return v != 0 && v == v; }

double callFunc(uint8_t f, const double* a) {
  switch (f) {
    case 0: return std::fabs(a[0]);
    case 1: return a[0] > 0 ? 1 : a[0] < 0 ? -1 : a[0];  // keeps -0 / NaN, like Math.sign
    case 2: return std::floor(a[0]);
    case 3: return std::ceil(a[0]);
    case 4: return jsRound(a[0]);
    case 5: return std::trunc(a[0]);
    case 6: return std::sqrt(a[0]);
    case 7: return std::cbrt(a[0]);
    case 8: return std::exp(a[0]);
    case 9: return std::log(a[0]);
    case 10: return std::log2(a[0]);
    case 11: return std::log10(a[0]);
    case 12: return std::sin(a[0]);
    case 13: return std::cos(a[0]);
    case 14: return std::tan(a[0]);
    case 15: return std::asin(a[0]);
    case 16: return std::acos(a[0]);
    case 17: return std::atan(a[0]);
    case 18: return std::atan2(a[0], a[1]);
    case 19: return std::sinh(a[0]);
    case 20: return std::cosh(a[0]);
    case 21: return std::tanh(a[0]);
    case 22: return std::pow(a[0], a[1]);
    case 23: return std::hypot(a[0], a[1]);
    // Math.min / max: NaN wins, -0 < +0.
    case 24: return a[0] != a[0] || a[1] != a[1] ? NAN : a[0] < a[1] || (a[0] == a[1] && std::signbit(a[0])) ? a[0] : a[1];
    case 25: return a[0] != a[0] || a[1] != a[1] ? NAN : a[0] > a[1] || (a[0] == a[1] && !std::signbit(a[0])) ? a[0] : a[1];
    case 26: {  // clamp = min(max(x, lo), hi)
      double m = callFunc(25, a);
      double b[2] = {m, a[2]};
      return callFunc(24, b);
    }
    case 27: return a[0] + (a[1] - a[0]) * a[2];
    case 28: return a[1] < a[0] ? 0 : 1;
    case 29: {
      double t = (a[2] - a[0]) / (a[1] - a[0]);
      double c[3] = {t, 0, 1};
      t = callFunc(26, c);
      return t * t * (3 - 2 * t);
    }
    case 30: return a[0] - std::floor(a[0]);
    case 31: return std::fmod(std::fmod(a[0], a[1]) + a[1], a[1]);
  }
  return NAN;
}

}  // namespace

// The program is checked once here (known ops, operand indices in range,
// a balanced stack ending with one value), so runExpression needs no checks.
bool NativeAnimated::compileExpression(ANode& n, const Value& c, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = std::string("expression: ") + msg;
    return false;
  };
  const Value::Array& code = c["code"].asArray();
  double slots = c["slots"].asNumber();
  if (!(slots >= 0 && slots <= 4096)) return fail("bad slot count");
  n.slots = uint32_t(slots);
  int depth = 0, maxDepth = 0;
  auto operand = [&](size_t& i, double* out) {
    if (++i >= code.size() || !code[i].isNumber()) return false;
    *out = code[i].asNumber();
    return true;
  };
  for (size_t i = 0; i < code.size(); i++) {
    if (!code[i].isNumber()) return fail("non-number op");
    double raw = code[i].asNumber();
    int op = int(raw);
    if (double(op) != raw) return fail("bad op");
    Instr ins{Op(op)};
    int pops = 0, pushes = 1;
    double arg = 0;
    switch (Op(op)) {
      case Op::Const:
        if (!operand(i, &ins.k)) return fail("missing constant");
        pushes = 1;
        break;
      case Op::Input:
      case Op::Load:
      case Op::Store: {
        if (!operand(i, &arg) || arg < 0 || arg != std::floor(arg)) return fail("bad index");
        size_t limit = Op(op) == Op::Input ? n.inputs.size() : n.slots;
        if (arg >= double(limit)) return fail("index out of range");
        ins.index = uint32_t(arg);
        if (Op(op) == Op::Store) pops = 1, pushes = 0;
        break;
      }
      case Op::Neg:
      case Op::Not: pops = 1; break;
      case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
      case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne:
      case Op::And: case Op::Or: pops = 2; break;
      case Op::Select: pops = 3; break;
      case Op::Call:
        if (!operand(i, &arg) || arg < 0 || arg >= double(kFuncCount) || arg != std::floor(arg)) return fail("bad function");
        ins.fn = uint8_t(arg);
        pops = kFuncArity[ins.fn];
        break;
      default: return fail("unknown op");
    }
    if (depth < pops) return fail("stack underflow");
    depth += pushes - pops;
    maxDepth = std::max(maxDepth, depth);
    n.code.push_back(ins);
  }
  if (depth != 1) return fail("the program must leave one value");
  n.stackSize = uint32_t(maxDepth);
  return true;
}

double NativeAnimated::runExpression(ANode& n) {
  // Inputs are evaluated first: evaluating one may run another expression,
  // which reuses the scratch stacks.
  double inputsSmall[8];
  std::vector<double> inputsBig;
  double* in = inputsSmall;
  if (n.inputs.size() > 8) {
    inputsBig.resize(n.inputs.size());
    in = inputsBig.data();
  }
  for (size_t i = 0; i < n.inputs.size(); i++) in[i] = evalNum(n.inputs[i]);
  exprStack_.resize(n.stackSize);
  exprSlots_.resize(n.slots);
  double* st = exprStack_.data();
  double* slots = exprSlots_.data();
  size_t sp = 0;
  for (const Instr& ins : n.code) {
    switch (ins.op) {
      case Op::Const: st[sp++] = ins.k; break;
      case Op::Input: st[sp++] = in[ins.index]; break;
      case Op::Load: st[sp++] = slots[ins.index]; break;
      case Op::Store: slots[ins.index] = st[--sp]; break;
      case Op::Neg: st[sp - 1] = -st[sp - 1]; break;
      case Op::Not: st[sp - 1] = truthy(st[sp - 1]) ? 0 : 1; break;
      case Op::Select: sp -= 2; st[sp - 1] = truthy(st[sp - 1]) ? st[sp] : st[sp + 1]; break;
      case Op::Call: {
        uint8_t k = kFuncArity[ins.fn];
        sp -= k;
        st[sp] = callFunc(ins.fn, st + sp);
        sp++;
        break;
      }
      default: {
        double b = st[--sp], a = st[sp - 1], r = 0;
        switch (ins.op) {
          case Op::Add: r = a + b; break;
          case Op::Sub: r = a - b; break;
          case Op::Mul: r = a * b; break;
          case Op::Div: r = a / b; break;
          case Op::Mod: r = std::fmod(a, b); break;
          case Op::Pow: r = (b != b || ((a == 1 || a == -1) && std::isinf(b))) ? NAN : std::pow(a, b); break;
          case Op::Lt: r = a < b; break;
          case Op::Le: r = a <= b; break;
          case Op::Gt: r = a > b; break;
          case Op::Ge: r = a >= b; break;
          case Op::Eq: r = a == b; break;
          case Op::Ne: r = !(a == b); break;
          case Op::And: r = truthy(a) ? b : a; break;
          case Op::Or: r = truthy(a) ? a : b; break;
          default: break;
        }
        st[sp - 1] = r;
      }
    }
  }
  double v = sp ? st[sp - 1] : 0;
  return v == v ? v : 0;  // NaN -> 0, as in the JS evaluator
}

double NativeAnimated::interactionWeight(const ANode& n) const {
  auto it = doc_.interactionNodes.find(n.interactionKey);
  if (it == doc_.interactionNodes.end()) return 0;
  Node* v = doc_.get(it->second);
  const StateStyles* s = v ? v->stateStyles.get() : nullptr;
  if (!s || s->interaction != n.interactionKey) return 0;
  return n.instant ? (s->target[n.slot] ? 1 : 0) : double(s->weight[n.slot]);
}

bool NativeAnimated::animating() const {
  for (const Anim& a : animations_) {
    if (a.kind != Anim::Kind::Clock) return true;
    if (a.held) continue;
    // A clock animation nothing reads (no dependents, no JS listener) does
    // not need frames: its value is computed from the time whenever a frame
    // comes.
    auto it = nodes_.find(a.tag);
    if (it != nodes_.end() && (!it->second.children.empty() || listening_.count(a.tag))) return true;
  }
  return liveClocks() > 0;
}

size_t NativeAnimated::liveClocks() const {
  size_t live = 0;
  for (int tag : clockNodes_) {
    auto it = nodes_.find(tag);
    if (it != nodes_.end() && !it->second.children.empty()) live++;
  }
  return live;
}

// ------------------------------------------------------------------- events

void NativeAnimated::addEventMapping(uint32_t viewId, const std::string& eventType, std::vector<std::string> path,
                                     int tag) {
  mappings_.push_back({viewId, eventType, std::move(path), tag});
}

void NativeAnimated::removeEventMapping(uint32_t viewId, const std::string& eventType, int tag) {
  mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                 [&](const Mapping& m) { return m.view == viewId && m.type == eventType && m.tag == tag; }),
                  mappings_.end());
}

bool NativeAnimated::hasEventMapping(uint32_t viewId, const char* eventType) const {
  for (const Mapping& m : mappings_)
    if (m.view == viewId && m.type == eventType) return true;
  return false;
}

void NativeAnimated::handleEvent(const Value& ev) {
  if (mappings_.empty()) return;
  const Value& target = ev["target"];
  if (!target.isNumber()) return;
  uint32_t id = uint32_t(target.asNumber());
  const std::string& type = ev["type"].asString();
  bool any = false;
  for (const Mapping& m : mappings_) {
    if (m.view != id || m.type != type) continue;
    const Value* v = &ev;
    for (const std::string& k : m.path) v = &(*v)[k];
    ANode* n = node(m.tag);
    if (!v->isNumber() || !n || n->type != Type::Value) continue;
    n->value = v->asNumber();
    markDirty(*n);
    markNotify(*n);
    any = true;
  }
  if (any) flush();
}

}  // namespace jsui
