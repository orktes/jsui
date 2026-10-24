// Applying the bridge's commands (js/command_buffer.h) to the runtime: the
// native side of what __jsui_native's mutation calls used to do directly.
#include "js/command_buffer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "animated/native_animated.h"
#include "core/document.h"
#include "input/input.h"
#include "render/lottie.h"
#include "runtime.h"

namespace jsui {

namespace {

// The input of the surface showing `n`.
InputManager& inputOf(Runtime& rt, Node* n) { return *rt.surfaceOf(n->document()).input; }

// Nodes of different surfaces cannot be joined (a portal into another
// surface's tree, for example).
bool sameDocument(Runtime& rt, Node* a, Node* b) {
  if (a->document() == b->document()) return true;
  rt.log(JSUI_LOG_WARN, "a node cannot move to another surface (createPortal into another root view?)");
  return false;
}

bool isAncestorOrSelf(Node* a, Node* n) {
  for (Node* p = n; p; p = p->parent())
    if (p == a) return true;
  return false;
}

int tagOf(uint32_t bits) { return int(int32_t(bits)); }

// configureLayoutAnimation's {duration, create, update, delete}: each of
// create / update / delete is {type, property, duration, delay,
// springDamping}.
LayoutAnimSpec layoutAnimSpec(const Value& v, double duration) {
  LayoutAnimSpec s;
  if (!v.isObject()) return s;
  s.enabled = true;
  s.duration = v["duration"].isNumber() ? v["duration"].asNumber() : duration;
  s.delay = v["delay"].isNumber() ? v["delay"].asNumber() : 0;
  if (v["springDamping"].isNumber()) s.springDamping = float(v["springDamping"].asNumber());
  const std::string type = v["type"].isString() ? v["type"].asString() : "easeInEaseOut";
  s.curve = type == "linear"    ? LayoutAnimCurve::Linear
            : type == "easeIn"  ? LayoutAnimCurve::EaseIn
            : type == "easeOut" ? LayoutAnimCurve::EaseOut
            : type == "spring"  ? LayoutAnimCurve::Spring
            : type == "keyboard" ? LayoutAnimCurve::Keyboard
                                 : LayoutAnimCurve::EaseInEaseOut;
  const std::string prop = v["property"].isString() ? v["property"].asString() : "opacity";
  s.property = prop == "scaleX"    ? LayoutAnimProperty::ScaleX
               : prop == "scaleY"  ? LayoutAnimProperty::ScaleY
               : prop == "scaleXY" ? LayoutAnimProperty::ScaleXY
                                   : LayoutAnimProperty::Opacity;
  return s;
}

}  // namespace

void CommandBuffer::apply(Runtime& rt) {
  depth_++;
  while (next_ < cmds_.size()) {
    // A copy: applying may record more commands (and grow cmds_).
    Command cmd = cmds_[next_++];
    applyOne(rt, cmd);
    applied_++;
  }
  if (--depth_ == 0) clear();
}

void CommandBuffer::clear() {
  if (depth_ > 0) return;
  cmds_.clear();
  values_.clear();
  objects_.clear();
  strings_.clear();
  next_ = 0;
}

void CommandBuffer::applyOne(Runtime& rt, const Command& cmd) {
  // Payloads are moved out of the arrays before anything runs: a command
  // that calls into JS can grow them.
  switch (cmd.op) {
    case Op::Create: {
      Document* doc = &rt.doc;
      if (cmd.c)
        if (Surface* s = rt.surfaceByTag(cmd.c)) doc = &s->doc;
      doc->createNode(NodeType(cmd.b), cmd.a);
      return;
    }
    case Op::SetProps: {
      Value props = std::move(values_[cmd.arg]);
      if (Node* n = rt.findNode(cmd.a)) n->setProps(props);
      return;
    }
    case Op::SetPropsKeepStyle: {
      // What updateProps(id, {...props, style: getProps(id).style}, hidden)
      // built: the node's style as it is now (with patchStyle's and native
      // Animated's writes), plus pointerEvents and hidden.
      Value props = std::move(values_[cmd.arg]);
      Node* n = rt.findNode(cmd.a);
      if (!n) return;
      Value::Object flat;
      const Value& cur = n->props()["style"];
      bool hasStyle = cur.isObject();
      if (hasStyle) flat = cur.asObject();
      if ((cmd.flags & kPointerEvents) && !flat.count("pointerEvents")) {
        flat.insert_or_assign("pointerEvents", props["pointerEvents"]);
        hasStyle = true;
      }
      if (cmd.flags & kHidden) {
        flat.insert_or_assign("display", Value("none"));
        hasStyle = true;
      }
      if (hasStyle) props.mutableObject().insert_or_assign("style", Value(std::move(flat)));
      n->setProps(props);
      return;
    }
    case Op::PatchStyle: {
      Value::Object partial = std::move(objects_[cmd.arg]);
      if (Node* n = rt.findNode(cmd.a)) n->patchStyle(partial);
      return;
    }
    case Op::SetText: {
      std::string text = std::move(strings_[cmd.arg]);
      if (Node* n = rt.findNode(cmd.a)) n->setText(std::move(text));
      return;
    }
    case Op::Append: {
      Node* p = rt.findNode(cmd.a);
      Node* c = rt.findNode(cmd.b);
      if (p && c && !isAncestorOrSelf(c, p) && sameDocument(rt, p, c)) p->appendChild(c);
      return;
    }
    case Op::Insert: {
      Node* p = rt.findNode(cmd.a);
      Node* c = rt.findNode(cmd.b);
      Node* b = cmd.c ? rt.findNode(cmd.c) : nullptr;
      if (b && b->parent() != p) b = nullptr;
      if (p && c && c != b && !isAncestorOrSelf(c, p) && sameDocument(rt, p, c)) p->insertBefore(c, b);
      return;
    }
    case Op::Remove: {
      Node* p = rt.findNode(cmd.a);
      Node* c = rt.findNode(cmd.b);
      if (p && c) p->document()->removeChild(p, c);  // may keep it as a LayoutAnimation ghost
      return;
    }
    case Op::Destroy:
      if (Node* n = rt.findNode(cmd.a)) n->document()->destroyNode(n);
      return;
    case Op::ArmLayoutAnim: {
      // The commit may touch any surface: all of them are armed.
      if (cmd.flags & kDisarm) {
        for (auto& s : rt.surfaces()) s->doc.disarmLayoutAnimation();
        return;
      }
      Value cfg = std::move(values_[cmd.arg]);
      double duration = cfg["duration"].isNumber() ? cfg["duration"].asNumber() : 300;
      LayoutAnimConfig c;
      c.create = layoutAnimSpec(cfg["create"], duration);
      c.update = layoutAnimSpec(cfg["update"], duration);
      c.remove = layoutAnimSpec(cfg["delete"], duration);
      for (auto& s : rt.surfaces()) s->doc.armLayoutAnimation(c);
      return;
    }
    case Op::Focus:
      if (Node* n = rt.findNode(cmd.a)) inputOf(rt, n).focus(n);
      return;
    case Op::Blur:
      if (Node* n = rt.findNode(cmd.a)) inputOf(rt, n).blur(n);
      return;
    case Op::ScrollTo: {
      Node* n = rt.findNode(cmd.a);
      if (!n) return;
      n->document()->layout();
      inputOf(rt, n).scrollTo(n, float(cmd.x == cmd.x ? cmd.x : 0), float(cmd.y == cmd.y ? cmd.y : 0),
                              (cmd.flags & kAnimated) != 0);
      return;
    }
    case Op::ScrollToEnd: {
      // The content container's laid-out size minus the ScrollView's, on the
      // scroll axis (what HostInstance.scrollToEnd measured in JS).
      Node* n = rt.findNode(cmd.a);
      if (!n) return;
      // An unknown content node measures as zero, like measure() in JS.
      n->document()->layout();
      Node* content = cmd.b ? rt.findNode(cmd.b) : n;
      if (content) content->document()->layout();
      Rect self = n->layoutRect();
      Rect inner = content ? content->layoutRect() : Rect{};
      bool horizontal = cmd.flags & kHorizontal;
      double x = horizontal ? std::max(0.0, double(inner.width) - double(self.width)) : 0;
      double y = horizontal ? 0 : std::max(0.0, double(inner.height) - double(self.height));
      inputOf(rt, n).scrollTo(n, float(x), float(y), (cmd.flags & kAnimated) != 0);
      return;
    }
    case Op::FlashScrollIndicators: {
      Node* n = rt.findNode(cmd.a);
      if (!n) return;
      n->document()->layout();
      inputOf(rt, n).flashScrollIndicators(n);
      return;
    }
    case Op::SetSelection: {
      Node* n = rt.findNode(cmd.a);
      if (!n) return;
      auto toIndex = [](double v) { return v == v ? int(std::clamp(v, 0.0, 1e9)) : 0; };
      inputOf(rt, n).setInputSelection(n, toIndex(cmd.x), toIndex(cmd.y));
      return;
    }
    case Op::LottieCommand: {
      std::string name = std::move(strings_[cmd.arg]);
      Node* n = rt.findNode(cmd.a);
      result_.ok = n && lottie::command(n, name, cmd.x, cmd.y);
      return;
    }
    case Op::AnimCreate: {
      Value config = std::move(values_[cmd.arg]);
      result_.error.clear();
      result_.ok = rt.animated->createNode(tagOf(cmd.a), config, &result_.error);
      return;
    }
    case Op::AnimConnect:
      rt.animated->connectNodes(tagOf(cmd.a), tagOf(cmd.b));
      return;
    case Op::AnimDisconnect:
      rt.animated->disconnectNodes(tagOf(cmd.a), tagOf(cmd.b));
      return;
    case Op::AnimConnectView:
      rt.animated->connectToView(tagOf(cmd.a), cmd.b);
      rt.animated->flush();
      return;
    case Op::AnimDisconnectView:
      rt.animated->disconnectFromView(tagOf(cmd.a), cmd.b);
      return;
    case Op::AnimDrop:
      rt.animated->dropNode(tagOf(cmd.a));
      return;
    case Op::AnimSetValue:
      rt.animated->setValue(tagOf(cmd.a), cmd.x);
      return;
    case Op::AnimSetOffset:
      rt.animated->setOffset(tagOf(cmd.a), cmd.x);
      return;
    case Op::AnimFlattenOffset:
      rt.animated->flattenOffset(tagOf(cmd.a));
      return;
    case Op::AnimExtractOffset:
      rt.animated->extractOffset(tagOf(cmd.a));
      return;
    case Op::AnimStart: {
      // Starts on the runtime clock now (single-threaded: the time JS called).
      Value config = std::move(values_[cmd.arg]);
      result_.error.clear();
      result_.ok = rt.animated->startAnimation(tagOf(cmd.a), tagOf(cmd.b), config, rt.now, &result_.error);
      return;
    }
    case Op::AnimStop:
      result_.number = 0;
      result_.ok = rt.animated->stopAnimation(tagOf(cmd.a), &result_.number);
      return;
    case Op::AnimAddEventMapping: {
      std::string type = std::move(strings_[cmd.arg]);
      std::vector<std::string> path;
      path.reserve(cmd.count);
      for (uint32_t i = 0; i < cmd.count; i++) path.push_back(std::move(strings_[cmd.arg + 1 + i]));
      rt.animated->addEventMapping(cmd.a, type, std::move(path), tagOf(cmd.b));
      return;
    }
    case Op::AnimRemoveEventMapping: {
      std::string type = std::move(strings_[cmd.arg]);
      rt.animated->removeEventMapping(cmd.a, type, tagOf(cmd.b));
      return;
    }
    case Op::AnimStartListening:
      rt.animated->startListening(tagOf(cmd.a));
      return;
    case Op::AnimStopListening:
      rt.animated->stopListening(tagOf(cmd.a));
      return;
  }
}

}  // namespace jsui
