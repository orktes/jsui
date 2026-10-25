// __jsui_native: the node-tree bridge used by @orktes/jsui-react's host config,
// plus imperative methods (measure, focus, scrollTo), the animation fast
// path (patchStyle), host messages, fonts and reload.
//
// Mutations are not applied here: each call converts its arguments and
// records a command (js/command_buffer.h), which the engine applies when JS
// returns to native. Calls that read native state (measure, getProps) or
// reach the embedder (postMessage, fonts) apply what is recorded first. In
// the runtime's JS-thread mode (Runtime::jsThreadMode) reads come from the
// frame snapshot or a proxy call instead, and embedder callbacks are queued
// for the tick (Runtime::toHost).
#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/document.h"
#include "input/input.h"
#include "dev_session.h"
#include "js/engine_impl.h"
#include "js/inspector.h"
#include "render/lottie.h"
#include "render/shader_view.h"
#include "include/effects/SkRuntimeEffect.h"
#include "runtime.h"

namespace jsui {

namespace {

// Recording side of a tree / prop mutation from the reconciler (the
// conversion; applying it is timed in the same phase).
#define JSUI_COMMIT_SCOPE                                    \
  implOf(ctx).counters->bridgeCalls++;                       \
  Perf::Scope commitScope(implOf(ctx).perf(), Phase::Commit)

using Op = CommandBuffer::Op;

CommandBuffer& commands(JSContext* ctx) { return implOf(ctx).commands; }

// An op of CommandBuffer::appliesAtOnce: applied now single-threaded (its
// events reach JS inside the call); on a JS thread with the batch (they
// arrive as messages later).
void applyAtOnce(JSContext* ctx) {
  JsEngine::Impl& impl = implOf(ctx);
  if (!impl.threaded) impl.applyNow();
}

// A node id argument (0, which no node has, when it is not a number).
// Whether the node exists is checked when the command is applied.
uint32_t idArg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  uint32_t id = 0;
  if (i >= argc || !JS_IsNumber(argv[i]) || JS_ToUint32(ctx, &id, argv[i])) return 0;
  return id;
}

// For reads: the node as JS sees it, after what it recorded is applied.
Node* nodeArg(JSContext* ctx, JSValueConst v) {
  if (!JS_IsNumber(v)) return nullptr;
  uint32_t id = 0;
  if (JS_ToUint32(ctx, &id, v)) return nullptr;
  implOf(ctx).syncPoint();
  return runtimeOf(ctx).findNode(id);
}

// createNode(type, rootTag) -> id: the node belongs to that surface's
// document. The id is taken now; the node exists natively once the Create
// command is applied.
JSValue js_createNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  NodeType t;
  std::string name = argc > 0 ? toStdString(ctx, argv[0]) : "";
  if (!nodeTypeFromName(name, &t)) return JS_ThrowTypeError(ctx, "unknown element type '%s'", name.c_str());
  uint32_t tag = idArg(ctx, argc, argv, 1);
  uint32_t id = runtimeOf(ctx).allocNodeId();
  commands(ctx).push(Op::Create, id, uint32_t(t), tag);
  return JS_NewUint32(ctx, id);
}

JSValue js_setProps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeValue(ctx, argv[1], w);
  cmds.end(Op::SetProps, idArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// ---- updateProps(id, reactProps, hidden, keepStyle): the host config's
// prop path done natively. Builds what native sees from the React props
// (functions become props.__events tokens, symbols / children / ref / key
// are dropped, the style is flattened, pointerEvents moves into the style,
// hidden adds display: none) and hands it to Node::setProps, which diffs it
// against the current props. Same result as @orktes/jsui-react's
// toNativeProps + nativePropsChanged, without interpreting either in JS.
// keepStyle (setNativeProps of non-style props): reactProps.style is
// ignored and the node keeps the style it has when the command is applied
// (patchStyle's and native Animated's writes included).

// React prop -> props.__events tokens (packages/react/src/instances.js
// EVENT_TOKENS; "...Capture" variants map like the bubbling prop, except for
// the responder props).
const std::vector<std::string>* eventTokens(const std::string& key) {
  static const std::unordered_map<std::string, std::vector<std::string>> table = [] {
    const std::vector<std::string> responder = {"responder", "pointerMove"};
    std::unordered_map<std::string, std::vector<std::string>> t = {
        {"onClick", {"click"}},
        {"onPress", {"press"}},
        {"onPointerDown", {"pointerDown"}},
        {"onPointerUp", {"pointerUp"}},
        {"onPointerMove", {"pointerMove"}},
        {"onPointerCancel", {"pointerCancel"}},
        {"onPointerEnter", {"hoverIn", "pointerEnter"}},
        {"onPointerLeave", {"hoverOut", "pointerLeave"}},
        {"onMouseEnter", {"hoverIn", "pointerEnter"}},
        {"onMouseLeave", {"hoverOut", "pointerLeave"}},
        {"onWheel", {"wheel"}},
        {"onKeyDown", {"keyDown"}},
        {"onKeyUp", {"keyUp"}},
        {"onFocus", {"focus"}},
        {"onBlur", {"blur"}},
        {"onScroll", {"scroll"}},
        {"onTextInput", {"textInput"}},
        {"onChange", {"change"}},
        {"onSelectionChange", {"selectionChange"}},
        {"onGamepadButton", {"gamepadButton"}},
        {"onGamepadAxis", {"gamepadAxis"}},
        {"onLayout", {"layout"}},
        {"onLoad", {"load"}},
        {"onError", {"error"}},
        {"onProgress", {"progress"}},
        {"onAnimationFinish", {"animationFinish"}},
    };
    std::vector<std::pair<std::string, std::vector<std::string>>> capture;
    for (auto& [k, v] : t) capture.push_back({k + "Capture", v});
    for (auto& c : capture) t.insert(std::move(c));
    for (const char* k : {"onStartShouldSetResponder", "onMoveShouldSetResponder", "onStartShouldSetResponderCapture",
                          "onMoveShouldSetResponderCapture", "onResponderGrant", "onResponderMove",
                          "onResponderRelease"})
      t[k] = responder;
    return t;
  }();
  auto it = table.find(key);
  return it == table.end() ? nullptr : &it->second;
}

// StyleSheet.flatten into the style object being written: nested arrays,
// falsy entries skipped, later objects win (the reader keeps a key's last
// value), undefined values ignored. Returns how many objects it merged;
// sets *pointerEvents when one of them had a pointerEvents value.
int flattenStyle(JSContext* ctx, JSValueConst s, CommandBuffer::Writer& w, int depth, bool* pointerEvents) {
  if (depth > 32 || !JS_IsObject(s) || JS_IsFunction(ctx, s)) return 0;
  if (JS_IsArray(s)) {
    int64_t len = 0;
    if (JS_GetLength(ctx, s, &len)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return 0;
    }
    int n = 0;
    for (int64_t i = 0; i < len && i < (1 << 20); i++) {
      JSValue e = JS_GetPropertyInt64(ctx, s, i);
      n += flattenStyle(ctx, e, w, depth + 1, pointerEvents);
      JS_FreeValue(ctx, e);
    }
    return n;
  }
  implOf(ctx).counters->valueConversions++;
  JSPropertyEnum* props = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &props, &count, s, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 0;
  }
  JsEngine::Impl& impl = implOf(ctx);
  for (uint32_t i = 0; i < count; i++) {
    JSValue e = JS_GetProperty(ctx, s, props[i].atom);
    if (JS_IsException(e)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else if (!JS_IsUndefined(e) && !JS_IsFunction(ctx, e)) {
      if (const std::string* k = impl.atomName(props[i].atom)) {
        if (*k == "pointerEvents") *pointerEvents = true;
        w.key(*k);
        writeValue(ctx, e, w, depth + 1);
      }
    }
    JS_FreeValue(ctx, e);
  }
  JS_FreePropertyEnum(ctx, props, count);
  return 1;
}

// A responder handler's static hint for input routing (a PanResponder marks
// its handlers): 1 when `fn[key] === true`, 0 when false, -1 when unset.
int handlerHint(JSContext* ctx, JSValueConst fn, const char* key) {
  JSValue v = JS_GetPropertyStr(ctx, fn, key);
  int r = JS_IsBool(v) ? (JS_ToBool(ctx, v) ? 1 : 0) : -1;
  JS_FreeValue(ctx, v);
  return r;
}

JSValue js_updateProps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2 || !JS_IsObject(argv[1])) return JS_UNDEFINED;
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id) return JS_UNDEFINED;
  bool hidden = argc > 2 && JS_ToBool(ctx, argv[2]);
  bool keepStyle = argc > 3 && JS_ToBool(ctx, argv[3]);
  JsEngine::Impl& impl = implOf(ctx);
  // Event tokens, deduplicated (static strings; a node has a handful).
  constexpr int kMaxEvents = 64;
  const std::string* events[kMaxEvents];
  int eventCount = 0;
  JSValue style = JS_UNDEFINED, pointerEvents = JS_UNDEFINED;
  // Static responder hints (docs/ARCHITECTURE.md "Input routing details"):
  // native decides with them when JS's answer to a touch is late.
  bool blocksNative = false, keepsGesture = false;
  JSPropertyEnum* keys = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &keys, &count, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
    return JS_EXCEPTION;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  w.beginObject();
  for (uint32_t i = 0; i < count; i++) {
    const std::string* k = impl.atomName(keys[i].atom);
    if (!k || *k == "children" || *k == "ref" || *k == "key") continue;
    JSValue v = JS_GetProperty(ctx, argv[1], keys[i].atom);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      continue;
    }
    if (*k == "style") {
      if (keepStyle) JS_FreeValue(ctx, v);
      else style = v;
      continue;
    }
    if (JS_IsFunction(ctx, v)) {
      if (const auto* toks = eventTokens(*k))
        for (const std::string& t : *toks)
          if (eventCount < kMaxEvents &&
              std::none_of(events, events + eventCount, [&](const std::string* e) { return *e == t; }))
            events[eventCount++] = &t;
      // A PanResponder blocks native gestures unless it asks
      // onShouldBlockNativeResponder; a termination request may refuse
      // unless the PanResponder has no onPanResponderTerminationRequest.
      if (*k == "onResponderGrant") blocksNative = handlerHint(ctx, v, "__blocksNative") == 1;
      else if (*k == "onResponderTerminationRequest") keepsGesture = handlerHint(ctx, v, "__keepsGesture") != 0;
    } else if (!JS_IsUndefined(v) && !JS_IsSymbol(v)) {
      if (*k == "pointerEvents") pointerEvents = JS_DupValue(ctx, v);
      w.key(*k);
      writeValue(ctx, v, w, 1);
    }
    JS_FreeValue(ctx, v);
  }
  JS_FreePropertyEnum(ctx, keys, count);
  uint8_t flags = 0;
  if (keepStyle) {
    // The style is merged when the command is applied (the current one).
    flags = (hidden ? CommandBuffer::kHidden : 0) | (JS_ToBool(ctx, pointerEvents) ? CommandBuffer::kPointerEvents : 0);
  } else {
    size_t noStyle = w.mark();
    w.key("style", 5);
    w.beginObject();
    bool flatPointerEvents = false;
    bool hasStyle = flattenStyle(ctx, style, w, 1, &flatPointerEvents) > 0;
    // RN accepts pointerEvents as a prop; native reads it from the style.
    if (JS_ToBool(ctx, pointerEvents) && !flatPointerEvents) {
      w.key("pointerEvents", 13);
      writeValue(ctx, pointerEvents, w, 1);
      hasStyle = true;
    }
    if (hidden) {
      w.key("display", 7);
      w.string("none", 4);
      hasStyle = true;
    }
    if (hasStyle) w.endObject();
    else w.rollback(noStyle);
  }
  JS_FreeValue(ctx, style);
  JS_FreeValue(ctx, pointerEvents);
  if (blocksNative) {
    w.key("__blocksNative", 14);
    w.boolean(true);
  }
  if (keepsGesture) {
    w.key("__keepsGesture", 14);
    w.boolean(true);
  }
  if (eventCount) {
    w.key("__events", 8);
    w.beginArray(uint32_t(eventCount));
    for (int i = 0; i < eventCount; i++) w.string(events[i]->data(), events[i]->size());
  }
  w.endObject();
  cmds.end(keepStyle ? Op::SetPropsKeepStyle : Op::SetProps, id, 0, flags);
  return JS_UNDEFINED;
}

// shouldYield(): React's scheduler asks this between units of concurrent
// work (the CLI bundles scheduler with that check,
// packages/cli/src/specialize.js):
// true once this tick's JS budget is spent, so a transition yields to the
// next frame. performance.now() is the frame clock and never advances
// within a tick, so the scheduler's own check would never yield.
JSValue js_shouldYield(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_NewBool(ctx, implOf(ctx).shouldYield());
}

// getProps(id): the props native holds for a node (what updateProps built
// from the React props, with patchStyle's writes), or undefined. A read,
// for the inspector (setNativeProps uses updateProps' keepStyle instead).
JSValue js_getProps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  JsEngine::Impl& impl = implOf(ctx);
  if (impl.threaded) {
    // The owner thread's tree (as of the last batch applied): a proxy read.
    uint32_t id = idArg(ctx, argc, argv, 0);
    Runtime& rt = impl.rt;
    Value props;
    if (!id || !rt.proxy([&] {
          if (Node* n = rt.findNode(id)) props = n->props();
        }))
      return JS_UNDEFINED;
    return props.isObject() ? fromValue(ctx, props) : JS_UNDEFINED;
  }
  Node* n = nodeArg(ctx, argv[0]);
  return n && n->props().isObject() ? fromValue(ctx, n->props()) : JS_UNDEFINED;
}

// patchStyle(id, partial): merges keys into props.style; null/undefined
// removes a key. Meant for per-frame animation of paint keys (transform,
// opacity, colors): Node::setProps can skip relayout when only those change.
JSValue js_patchStyle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2 || !JS_IsObject(argv[1])) return JS_UNDEFINED;
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id) return JS_UNDEFINED;
  JSPropertyEnum* keys = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &keys, &count, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) return JS_EXCEPTION;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  w.beginObject();
  for (uint32_t i = 0; i < count; i++) {
    const std::string* k = implOf(ctx).atomName(keys[i].atom);
    JSValue v = JS_GetProperty(ctx, argv[1], keys[i].atom);
    if (k && !JS_IsException(v)) {
      // undefined / null remove the key (a null value in the partial).
      w.key(*k);
      if (JS_IsUndefined(v) || JS_IsNull(v)) w.null();
      else writeValue(ctx, v, w);
    }
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
  }
  JS_FreePropertyEnum(ctx, keys, count);
  w.endObject();
  cmds.end(Op::PatchStyle, id);
  return JS_UNDEFINED;
}

JSValue js_setText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeString(ctx, argv[1], w);
  cmds.end(Op::SetText, idArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// listenGlobal(type, on): the bundle has (on) or no longer has global
// listeners for events of `type` (addGlobalListener). Input asks
// Runtime::listensGlobally whether a late keydown may still be vetoed.
JSValue js_listenGlobal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeString(ctx, argv[0], w);
  cmds.end(Op::ListenGlobal, JS_ToBool(ctx, argv[1]) ? 1 : 0);
  return JS_UNDEFINED;
}

// Tree edits are checked when applied (unknown nodes, cycles, nodes of
// another surface).
JSValue js_appendChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  commands(ctx).push(Op::Append, idArg(ctx, argc, argv, 0), idArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_insertBefore(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  commands(ctx).push(Op::Insert, idArg(ctx, argc, argv, 0), idArg(ctx, argc, argv, 1), idArg(ctx, argc, argv, 2));
  return JS_UNDEFINED;
}

JSValue js_removeChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  commands(ctx).push(Op::Remove, idArg(ctx, argc, argv, 0), idArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

// configureLayoutAnimation({duration, create, update, delete}): arms a
// LayoutAnimation for the commit in progress (@orktes/jsui-react calls it from
// prepareForCommit after LayoutAnimation.configureNext, so it is recorded
// before the commit's tree edits). Each of create / update / delete: {type,
// property, duration, delay, springDamping}. No object: disarms.
JSValue js_configureLayoutAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    commands(ctx).push(Op::ArmLayoutAnim, 0, 0, 0, CommandBuffer::kDisarm);
    return JS_UNDEFINED;
  }
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeValue(ctx, argv[0], w);
  cmds.end(Op::ArmLayoutAnim, 0);
  return JS_UNDEFINED;
}

JSValue js_destroyNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 1) return JS_UNDEFINED;
  commands(ctx).push(Op::Destroy, idArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// {x, y, width, height, pageX, pageY}
JSValue rectObject(JSContext* ctx, const Rect& r, const Rect& a) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, r.x));
  JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, r.y));
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, r.width));
  JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, r.height));
  JS_SetPropertyStr(ctx, o, "pageX", JS_NewFloat64(ctx, a.x));
  JS_SetPropertyStr(ctx, o, "pageY", JS_NewFloat64(ctx, a.y));
  return o;
}

// measure(id) -> rect | null. JS-thread mode: the layout of the last frame
// (FrameSnapshot::layouts; null for a node not attached then).
JSValue js_measure(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JsEngine::Impl& impl = implOf(ctx);
  if (impl.threaded) {
    uint32_t id = idArg(ctx, argc, argv, 0);
    std::shared_ptr<const FrameSnapshot> snap = impl.rt.snapshots.current();
    if (!id || !snap) return JS_NULL;
    auto it = snap->layouts.find(id);
    return it == snap->layouts.end() ? JS_NULL : rectObject(ctx, it->second.local, it->second.page);
  }
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (!n) return JS_NULL;
  // Layout may be stale mid-commit; onLayout events it raises are queued
  // until JS unwinds (see JsEngine::layoutEvent).
  n->document()->layout();
  return rectObject(ctx, n->layoutRect(), n->absoluteRect());
}

// measureAsync(id, callback): React Native's measure. callback(rect | null)
// runs after the layout that follows the commit in progress (the request is
// a command; Runtime::answerMeasures), so it sees the committed tree.
JSValue js_measureAsync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "measureAsync(id, callback)");
  JsEngine::Impl& impl = implOf(ctx);
  uint32_t req = impl.nextMeasure++;
  impl.measureCallbacks[req] = JS_DupValue(ctx, argv[1]);
  commands(ctx).push(Op::Measure, idArg(ctx, argc, argv, 0), req);
  return JS_UNDEFINED;
}

// viewport(rootTag?) -> {width, height, scale}: logical viewport size of a
// surface (the main one by default; Dimensions). An auto-sized axis reports
// its laid-out size.
JSValue js_viewport(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  if (implOf(ctx).threaded) {  // the surface table of the snapshot
    std::shared_ptr<const FrameSnapshot> snap = rt.snapshots.current();
    if (!snap || snap->surfaces.empty()) return JS_UNDEFINED;
    const FrameSnapshot::SurfaceInfo* s = &snap->surfaces[0];
    uint32_t tag = 0;
    if (argc > 0 && JS_IsNumber(argv[0]) && !JS_ToUint32(ctx, &tag, argv[0]))
      if (const FrameSnapshot::SurfaceInfo* t = snap->surface(tag)) s = t;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, s->width));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, s->height));
    JS_SetPropertyStr(ctx, o, "scale", JS_NewFloat64(ctx, s->scale));
    return o;
  }
  const Document* d = &rt.doc;
  uint32_t tag = 0;
  if (argc > 0 && JS_IsNumber(argv[0]) && !JS_ToUint32(ctx, &tag, argv[0]))
    if (Surface* s = rt.surfaceByTag(tag)) d = &s->doc;
  const Document& doc = *d;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, doc.width()));
  JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, doc.height()));
  JS_SetPropertyStr(ctx, o, "scale", JS_NewFloat64(ctx, doc.pixelRatio()));
  return o;
}

// Imperative view methods are commands too. Single-threaded they apply at
// once (CommandBuffer::appliesAtOnce): focus and scroll events reach JS
// inside the call, as before.
JSValue js_focus(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (uint32_t id = idArg(ctx, argc, argv, 0)) {
    commands(ctx).push(Op::Focus, id);
    applyAtOnce(ctx);
  }
  return JS_UNDEFINED;
}

JSValue js_blur(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (uint32_t id = idArg(ctx, argc, argv, 0)) {
    commands(ctx).push(Op::Blur, id);
    applyAtOnce(ctx);
  }
  return JS_UNDEFINED;
}

// scrollTo(id, x, y, animated): InputManager clamps to the content extent and
// dispatches scroll events. animated: eases there on the runtime clock;
// otherwise the jump is immediate (and stops a running fling).
JSValue js_scrollTo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id) return JS_UNDEFINED;
  double x = 0, y = 0;
  if (argc > 1) JS_ToFloat64(ctx, &x, argv[1]);
  if (argc > 2) JS_ToFloat64(ctx, &y, argv[2]);
  bool animated = argc > 3 && JS_ToBool(ctx, argv[3]) > 0;
  commands(ctx).pushXY(Op::ScrollTo, id, x, y, animated ? CommandBuffer::kAnimated : 0);
  applyAtOnce(ctx);
  return JS_UNDEFINED;
}

// scrollToEnd(id, contentId, horizontal, animated): scrolls to the end of
// the content container (`contentId`, the ScrollView's first child in
// React; 0: the ScrollView itself) on the scroll axis, measured when the
// command is applied.
JSValue js_scrollToEnd(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id) return JS_UNDEFINED;
  uint8_t flags = (argc > 2 && JS_ToBool(ctx, argv[2]) > 0 ? CommandBuffer::kHorizontal : 0) |
                  (argc > 3 && JS_ToBool(ctx, argv[3]) > 0 ? CommandBuffer::kAnimated : 0);
  commands(ctx).push(Op::ScrollToEnd, id, idArg(ctx, argc, argv, 1), 0, flags);
  applyAtOnce(ctx);
  return JS_UNDEFINED;
}

// lottieData(json) -> key: hands an inline composition (LottieView
// source={require('./anim.json')}) to native once; views use the key as
// their source. The same JSON gives the same key.
JSValue js_lottieData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "lottieData: expected a JSON string");
  return JS_NewString(ctx, runtimeOf(ctx).lotties->addData(toStdString(ctx, argv[0])).c_str());
}

// lottieCommand(id, cmd, a, b): LottieView play(startFrame, endFrame) /
// pause / resume / reset. Frames are NaN (undefined) for the whole
// composition. Returns false for an unknown view or command.
JSValue js_lottieCommand(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id || argc < 2) return JS_FALSE;
  double a = std::numeric_limits<double>::quiet_NaN(), b = a;
  if (argc > 2 && JS_IsNumber(argv[2])) JS_ToFloat64(ctx, &a, argv[2]);
  if (argc > 3 && JS_IsNumber(argv[3])) JS_ToFloat64(ctx, &b, argv[3]);
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeString(ctx, argv[1], w);
  cmds.end(Op::LottieCommand, id, 0, 0, a, b);
  JsEngine::Impl& impl = implOf(ctx);
  if (impl.threaded) {
    // Applied with the batch: whether the view exists is not known yet;
    // the command name is (lottie::command).
    std::string cmd = toStdString(ctx, argv[1]);
    return JS_NewBool(ctx, cmd == "play" || cmd == "pause" || cmd == "resume" || cmd == "reset");
  }
  return JS_NewBool(ctx, impl.applyNow().ok);
}

// {error} | {uniforms: [{name, type, count, color}], children: [name]}.
JSValue shaderInfoOf(JSContext* ctx, const ShaderEffects::Program& prog) {
  JSValue out = JS_NewObject(ctx);
  if (!prog.effect) {
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, prog.error.c_str()));
    return out;
  }
  static const char* const kTypes[] = {"float",    "float2",   "float3", "float4", "float2x2", "float3x3",
                                       "float4x4", "int",      "int2",   "int3",   "int4"};
  JSValue uniforms = JS_NewArray(ctx);
  uint32_t i = 0;
  for (const SkRuntimeEffect::Uniform& u : prog.effect->uniforms()) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, u.name.data(), u.name.size()));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, kTypes[int(u.type)]));
    JS_SetPropertyStr(ctx, o, "count", JS_NewInt32(ctx, u.count));
    JS_SetPropertyStr(ctx, o, "color", JS_NewBool(ctx, u.isColor()));
    JS_SetPropertyUint32(ctx, uniforms, i++, o);
  }
  JSValue children = JS_NewArray(ctx);
  i = 0;
  for (const SkRuntimeEffect::Child& c : prog.effect->children())
    JS_SetPropertyUint32(ctx, children, i++, JS_NewStringLen(ctx, c.name.data(), c.name.size()));
  JS_SetPropertyStr(ctx, out, "uniforms", uniforms);
  JS_SetPropertyStr(ctx, out, "children", children);
  return out;
}

// shaderInfo(sksl) -> shaderInfoOf: compiles an SkSL shader (ShaderView
// source) like the view does, so JS can report errors or list what the
// shader takes.
JSValue js_shaderInfo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "shaderInfo: expected an SkSL string");
  return shaderInfoOf(ctx, *runtimeOf(ctx).shaders->get(toStdString(ctx, argv[0])));
}

// shaderPreload(sksl, images?) -> shaderInfoOf: compiles it and warms its
// GPU pipelines in the next frame (ShaderEffects::preload; `images` as the
// view's prop will be).
JSValue js_shaderPreload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "shaderPreload: expected an SkSL string");
  Value images = argc > 1 && JS_IsObject(argv[1]) ? toValue(ctx, argv[1]) : Value();
  return shaderInfoOf(ctx, *runtimeOf(ctx).shaders->preload(toStdString(ctx, argv[0]), images));
}

// shadersWarming() -> the number of preloaded shaders still waiting for
// their warm-up frame.
JSValue js_shadersWarming(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_NewInt32(ctx, int(runtimeOf(ctx).shaders->warmPending()));
}

// flashScrollIndicators(id): shows the overlay scroll indicators briefly.
JSValue js_flashScrollIndicators(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (uint32_t id = idArg(ctx, argc, argv, 0)) {
    commands(ctx).push(Op::FlashScrollIndicators, id);
    applyAtOnce(ctx);
  }
  return JS_UNDEFINED;
}

// setTextInputSelection(id, start, end): TextInput selection from JS
// (ref.setSelection, the controlled `selection` prop). UTF-16 indices.
JSValue js_setTextInputSelection(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint32_t id = idArg(ctx, argc, argv, 0);
  if (!id) return JS_UNDEFINED;
  double start = 0, end = 0;
  if (argc > 1) JS_ToFloat64(ctx, &start, argv[1]);
  end = start;
  if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) JS_ToFloat64(ctx, &end, argv[2]);
  commands(ctx).pushXY(Op::SetSelection, id, start, end);
  applyAtOnce(ctx);
  return JS_UNDEFINED;
}

// getClipboard() -> string, setClipboard(text): the embedder's clipboard
// (jsui_set_clipboard_callbacks) or the runtime's in-process one.
// JS-thread mode: the embedder's getter runs on the owner thread (a proxy
// read, after the clipboard writes queued before it); the setter is queued.
JSValue js_getClipboard(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Runtime& rt = runtimeOf(ctx);
  std::string t;
  rt.proxy([&] { t = rt.clipboardText(); });
  return JS_NewStringLen(ctx, t.data(), t.size());
}

JSValue js_setClipboard(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* rt = &runtimeOf(ctx);
  rt->toHost([rt, text = argc > 0 ? toStdString(ctx, argv[0]) : std::string()] { rt->setClipboardText(text); });
  return JS_UNDEFINED;
}

JSValue js_postMessage(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  // The embedder's on_message may look at the tree. (JS-thread mode: it runs
  // in the tick, after this batch is applied.)
  if (!implOf(ctx).threaded) implOf(ctx).syncPoint();
  std::string name = argc > 0 ? toStdString(ctx, argv[0]) : "";
  std::string json = "null";
  if (argc > 1) {
    if (JS_IsString(argv[1])) {
      json = toStdString(ctx, argv[1]);
    } else if (!JS_IsUndefined(argv[1])) {
      JSValue j = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
      if (JS_IsException(j)) return j;
      if (JS_IsString(j)) json = toStdString(ctx, j);
      JS_FreeValue(ctx, j);
    }
  }
  Runtime* r = &rt;
  rt.toHost([r, name = std::move(name), json = std::move(json)] {
    if (r->session && r->session->joined())
      r->session->uiEvent("message", {{"name", Value(name)}, {"data", json.empty() ? Value() : Value::parseJson(json)}});
    if (r->config.on_message) r->config.on_message(r->config.message_user, name.c_str(), json.c_str());
  });
  return JS_UNDEFINED;
}

void fontLoaded(JsEngine::Impl& impl, uint64_t id, Resource& res, const std::string& family, int weight,
                int italic) {
  auto it = impl.pending.find(id);
  if (it == impl.pending.end()) return;
  JSContext* ctx = impl.ctx;
  Runtime& rt = impl.rt;
  JSValue resolve = it->second.resolve, reject = it->second.reject;
  impl.pending.erase(it);
  std::string error;
  if (!res.ok()) {
    error = "loadFont: failed to load " + res.url + " (status " + std::to_string(res.status) +
            (res.error.empty() ? "" : ", " + res.error) + ")";
  } else {
    // The FontManager is the owner thread's (layout measures with it).
    std::string why = "the runtime did not answer in time";
    bool registered = false;
    impl.onOwner([&] {
      registered = rt.fonts.registerFont(family, res.data.data(), res.data.size(), weight, italic, &why);
      if (registered) rt.fontsChanged();
    });
    if (!registered) error = "loadFont: " + res.url + ": " + why;
  }
  if (error.empty()) {
    JSValue ok = JS_TRUE;
    JS_FreeValue(ctx, impl.call(resolve, JS_UNDEFINED, 1, &ok));
  } else {
    rt.jsLog(JSUI_LOG_WARN, error);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewStringLen(ctx, error.data(), error.size()));
    JS_FreeValue(ctx, impl.call(reject, JS_UNDEFINED, 1, &err));
    JS_FreeValue(ctx, err);
  }
  JS_FreeValue(ctx, resolve);
  JS_FreeValue(ctx, reject);
}

// Font face hints: weight <= 0 and italic < 0 take the file's own.
void fontHints(JSContext* ctx, int argc, JSValueConst* argv, int at, int* weight, int* italic) {
  *weight = 0;
  *italic = -1;
  if (argc > at && JS_IsNumber(argv[at])) JS_ToInt32(ctx, weight, argv[at]);
  if (argc > at + 1) {
    if (JS_IsBool(argv[at + 1])) *italic = JS_ToBool(ctx, argv[at + 1]) ? 1 : 0;
    else if (JS_IsNumber(argv[at + 1])) JS_ToInt32(ctx, italic, argv[at + 1]);
  }
}

// loadFont(family, url, weight, italic) -> Promise<true>
JSValue js_loadFont(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto& impl = implOf(ctx);
  Runtime& rt = impl.rt;
  std::string family = argc > 0 && JS_IsString(argv[0]) ? toStdString(ctx, argv[0]) : "";
  std::string url = argc > 1 ? toStdString(ctx, argv[1]) : "";
  int weight, italic;
  fontHints(ctx, argc, argv, 2, &weight, &italic);
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise)) return promise;
  if (url.empty()) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "loadFont(family, url): url is required"));
    JS_FreeValue(ctx, JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &err));
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
  }
  LoadRequest req;
  req.url = ResourceLoader::resolve(rt.baseUrl, url);
  JsEngine::Impl* ip = &impl;
  uint64_t id = impl.startLoad(std::move(req), [ip, family, weight, italic](uint64_t id, Resource& res) {
    fontLoaded(*ip, id, res, family, weight, italic);
  });
  impl.pending[id] = {funcs[0], funcs[1]};
  return promise;
}

// loadFontData(family, bytes, weight, italic) -> true; throws on bad data.
// `bytes`: ArrayBuffer / typed array / DataView. Empty family = the file's.
// Font registration runs where the FontManager lives (Impl::onOwner: after
// what JS recorded single-threaded, so fontsChanged() re-measures the text
// JS set before; a proxied call on a JS thread).
JSValue js_loadFontData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  std::string family = argc > 0 && JS_IsString(argv[0]) ? toStdString(ctx, argv[0]) : "";
  std::vector<uint8_t> data;
  if (argc < 2 || JS_IsString(argv[1]) || !bytesOf(ctx, argv[1], &data) || data.empty())
    return JS_ThrowTypeError(ctx, "loadFontData(family, bytes): bytes must be an ArrayBuffer or typed array");
  int weight, italic;
  fontHints(ctx, argc, argv, 2, &weight, &italic);
  std::string why = "the runtime did not answer in time";
  bool ok = false;
  implOf(ctx).onOwner([&] {
    ok = rt.fonts.registerFont(family, data.data(), data.size(), weight, italic, &why);
    if (ok) rt.fontsChanged();
  });
  if (!ok) return JS_ThrowTypeError(ctx, "loadFontData: %s", why.c_str());
  return JS_TRUE;
}

// unloadFont(family) -> boolean
JSValue js_unloadFont(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  if (argc < 1) return JS_FALSE;
  std::string family = toStdString(ctx, argv[0]);
  bool ok = false;
  implOf(ctx).onOwner([&] {
    ok = rt.fonts.unregisterFamily(family);
    if (ok) rt.fontsChanged();
  });
  return JS_NewBool(ctx, ok);
}

// getFontFamilies() -> string[] (registration order, built-in Inter first)
JSValue js_getFontFamilies(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Runtime& rt = runtimeOf(ctx);
  std::vector<std::string> names;
  implOf(ctx).onOwner([&] { names = rt.fonts.familyNames(); });
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < names.size(); i++)
    JS_SetPropertyUint32(ctx, arr, uint32_t(i), JS_NewStringLen(ctx, names[i].data(), names[i].size()));
  return arr;
}

// setFontFallbacks(families: string[] | "A, B")
JSValue js_setFontFallbacks(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  std::vector<std::string> list;
  auto addList = [&](const std::string& csv) {
    size_t i = 0;
    while (i <= csv.size()) {
      size_t c = csv.find(',', i);
      if (c == std::string::npos) c = csv.size();
      std::string f = csv.substr(i, c - i);
      i = c + 1;
      size_t a = f.find_first_not_of(" \t\"'"), b = f.find_last_not_of(" \t\"'");
      if (a != std::string::npos) list.push_back(f.substr(a, b - a + 1));
    }
  };
  if (argc > 0 && JS_IsArray(argv[0])) {
    int64_t n = 0;
    JS_GetLength(ctx, argv[0], &n);
    for (int64_t i = 0; i < n; i++) {
      JSValue e = JS_GetPropertyInt64(ctx, argv[0], i);
      if (JS_IsString(e)) addList(toStdString(ctx, e));
      JS_FreeValue(ctx, e);
    }
  } else if (argc > 0 && JS_IsString(argv[0])) {
    addList(toStdString(ctx, argv[0]));
  }
  implOf(ctx).onOwner([&] {
    uint32_t gen = rt.fonts.generation;
    rt.fonts.setFallbackFamilies(std::move(list));
    if (rt.fonts.generation != gen) rt.fontsChanged();
  });
  return JS_UNDEFINED;
}

// setDefaultFontFamily(family): the family of text whose style names none.
JSValue js_setDefaultFontFamily(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  if (argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
  std::string family = toStdString(ctx, argv[0]);
  implOf(ctx).onOwner([&] {
    uint32_t gen = rt.fonts.generation;
    rt.fonts.setDefaultFamily(family);
    if (rt.fonts.generation != gen) rt.fontsChanged();
  });
  return JS_UNDEFINED;
}

// surfaceApp(rootTag) -> {appKey, initialProps} | null: the app the host
// asked the surface to run (AppRegistry skips its auto-start on the main
// surface when the host picked one).
JSValue js_surfaceApp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint32_t tag = 0;
  if (argc < 1 || !JS_IsNumber(argv[0]) || JS_ToUint32(ctx, &tag, argv[0])) return JS_NULL;
  const std::string* appKey = nullptr;
  const std::string* props = nullptr;
  std::shared_ptr<const FrameSnapshot> snap;
  if (implOf(ctx).threaded) {  // the surface table of the snapshot
    snap = runtimeOf(ctx).snapshots.current();
    if (const FrameSnapshot::SurfaceInfo* s = snap ? snap->surface(tag) : nullptr) {
      appKey = &s->appKey;
      props = &s->propsJson;
    }
  } else if (Surface* s = runtimeOf(ctx).surfaceByTag(tag)) {
    appKey = &s->appKey;
    props = &s->propsJson;
  }
  if (!appKey || appKey->empty()) return JS_NULL;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "appKey", JS_NewStringLen(ctx, appKey->data(), appKey->size()));
  JS_SetPropertyStr(ctx, o, "initialProps", JS_ParseJSON(ctx, props->data(), props->size(), "<props>"));
  return o;
}

JSValue js_reload(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Runtime* rt = &runtimeOf(ctx);
  rt->toHost([rt] { rt->requestReload(); });
  return JS_UNDEFINED;
}

// devError(text): shows `text` as the error (dev overlay, jsui_last_error);
// devError(null) clears it. Hot updates (jsui dev) report build and update
// errors through this.
JSValue js_devError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* rt = &runtimeOf(ctx);
  if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
    rt->toHost([rt] {
      if (!rt->lastError.empty()) {
        rt->lastError.clear();
        rt->redrawAll();
      }
    });
    return JS_UNDEFINED;
  }
  rt->jsReportError(toStdString(ctx, argv[0]));
  return JS_UNDEFINED;
}

// evalScript(source, name): evaluates a script in the global scope (hot
// updates); `name` is the file name stack traces show. Returns its value.
JSValue js_evalScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  size_t len = 0;
  const char* src = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!src) return JS_EXCEPTION;
  std::string name = argc > 1 ? toStdString(ctx, argv[1]) : std::string("<eval>");
#if JSUI_DEBUGGER
  if (JsEngine::Impl& impl = implOf(ctx); impl.inspector) impl.inspector->scriptLoaded(name, std::string(src, len));
#endif
  JSValue r = JS_Eval(ctx, src, len, name.c_str(), JS_EVAL_TYPE_GLOBAL);
  JS_FreeCString(ctx, src);
  return r;
}

}  // namespace

void installAnimatedBridge(JsEngine::Impl& impl, JSValue native);  // bridge_animated.cpp
void installAudioBridge(JsEngine::Impl& impl, JSValue native);     // bridge_audio.cpp

void installBridge(JsEngine::Impl& impl, JSValue global) {
  JSContext* ctx = impl.ctx;
  JSValue native = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"createNode", js_createNode, 1},     {"setProps", js_setProps, 2},
             {"updateProps", js_updateProps, 4},
             {"getProps", js_getProps, 1},
             {"shouldYield", js_shouldYield, 0},
             {"patchStyle", js_patchStyle, 2},     {"setText", js_setText, 2},
             {"appendChild", js_appendChild, 2},   {"insertBefore", js_insertBefore, 3},
             {"removeChild", js_removeChild, 2},   {"destroyNode", js_destroyNode, 1},
             {"measure", js_measure, 1},           {"focus", js_focus, 1},
             {"measureAsync", js_measureAsync, 2},
             {"blur", js_blur, 1},                 {"scrollTo", js_scrollTo, 4},
             {"scrollToEnd", js_scrollToEnd, 4},
             {"postMessage", js_postMessage, 2},   {"loadFont", js_loadFont, 4},
             {"reload", js_reload, 0},             {"viewport", js_viewport, 1},
             {"surfaceApp", js_surfaceApp, 1},     {"devError", js_devError, 1},
             {"evalScript", js_evalScript, 2},
             {"setTextInputSelection", js_setTextInputSelection, 3},
             {"getClipboard", js_getClipboard, 0}, {"setClipboard", js_setClipboard, 1},
             {"flashScrollIndicators", js_flashScrollIndicators, 1},
             {"configureLayoutAnimation", js_configureLayoutAnimation, 1},
             {"lottieData", js_lottieData, 1},     {"lottieCommand", js_lottieCommand, 4},
             {"shaderInfo", js_shaderInfo, 1},
             {"shaderPreload", js_shaderPreload, 2},
             {"shadersWarming", js_shadersWarming, 0},
             {"loadFontData", js_loadFontData, 4}, {"unloadFont", js_unloadFont, 1},
             {"getFontFamilies", js_getFontFamilies, 0},
             {"setFontFallbacks", js_setFontFallbacks, 1},
             {"setDefaultFontFamily", js_setDefaultFontFamily, 1},
             {"listenGlobal", js_listenGlobal, 2}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, native, "rootId", JS_NewUint32(ctx, impl.rt.doc.root()->id()));
  // updateProps(id, props, hidden, true) keeps the node's current style
  // (setNativeProps).
  JS_SetPropertyStr(ctx, native, "updatePropsKeepsStyle", JS_TRUE);
  // TextInput text is edited natively (change / selectionchange events).
  JS_SetPropertyStr(ctx, native, "textEditing", JS_TRUE);
  // LottieView plays (the build has Skottie).
  JS_SetPropertyStr(ctx, native, "lottie", JS_NewBool(ctx, LottieCache::available()));
  JS_SetPropertyStr(ctx, native, "devMode", JS_NewBool(ctx, impl.rt.config.dev_mode != 0));
  // JS-thread mode: measure reads the last frame; the RN-style callback
  // forms (HostInstance.measure*, UIManager.measure*) use measureAsync.
  JS_SetPropertyStr(ctx, native, "asyncMeasure", JS_NewBool(ctx, impl.threaded));
  installAnimatedBridge(impl, native);
  installHostDataBridge(impl, native);
  installAudioBridge(impl, native);
  JS_SetPropertyStr(ctx, global, "__jsui_native", native);
}

}  // namespace jsui
