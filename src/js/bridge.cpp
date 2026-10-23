// __jsui_native: the node-tree bridge used by @orktes/jsui-react's host config,
// plus imperative methods (measure, focus, scrollTo), the animation fast
// path (patchStyle), host messages, fonts and reload.
#include <algorithm>

#include "core/document.h"
#include "input/input.h"
#include "js/engine_impl.h"
#include "runtime.h"

namespace jsui {

namespace {

// Native side of a tree / prop mutation from the reconciler.
#define JSUI_COMMIT_SCOPE                                    \
  Document& commitDoc = runtimeOf(ctx).doc;                  \
  commitDoc.perf.counters.bridgeCalls++;                     \
  Perf::Scope commitScope(commitDoc.perf, Phase::Commit)

Node* nodeArg(JSContext* ctx, JSValueConst v) {
  if (!JS_IsNumber(v)) return nullptr;
  uint32_t id = 0;
  if (JS_ToUint32(ctx, &id, v)) return nullptr;
  return runtimeOf(ctx).doc.get(id);
}

JSValue js_createNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  NodeType t;
  std::string name = argc > 0 ? toStdString(ctx, argv[0]) : "";
  if (!nodeTypeFromName(name, &t)) return JS_ThrowTypeError(ctx, "unknown element type '%s'", name.c_str());
  return JS_NewUint32(ctx, runtimeOf(ctx).doc.createNode(t)->id());
}

JSValue js_setProps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) n->setProps(toValue(ctx, argv[1]));
  return JS_UNDEFINED;
}

// patchStyle(id, partial): merges keys into props.style; null/undefined
// removes a key. Meant for per-frame animation of paint keys (transform,
// opacity, colors): Node::setProps can skip relayout when only those change.
JSValue js_patchStyle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2 || !JS_IsObject(argv[1])) return JS_UNDEFINED;
  Node* n = nodeArg(ctx, argv[0]);
  if (!n) return JS_UNDEFINED;
  Value::Object partial;
  JSPropertyEnum* keys = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &keys, &count, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) return JS_EXCEPTION;
  for (uint32_t i = 0; i < count; i++) {
    const std::string* k = implOf(ctx).atomName(keys[i].atom);
    JSValue v = JS_GetProperty(ctx, argv[1], keys[i].atom);
    if (k && !JS_IsException(v)) {
      // undefined / null remove the key (a null Value in the partial).
      partial.insert_or_assign(*k, JS_IsUndefined(v) || JS_IsNull(v) ? Value() : toValue(ctx, v));
    }
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
  }
  JS_FreePropertyEnum(ctx, keys, count);
  n->patchStyle(partial);
  return JS_UNDEFINED;
}

JSValue js_setText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) n->setText(toStdString(ctx, argv[1]));
  return JS_UNDEFINED;
}

bool isAncestorOrSelf(Node* a, Node* n) {
  for (Node* p = n; p; p = p->parent())
    if (p == a) return true;
  return false;
}

JSValue js_appendChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  if (p && c && !isAncestorOrSelf(c, p)) p->appendChild(c);
  return JS_UNDEFINED;
}

JSValue js_insertBefore(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  Node* b = argc > 2 ? nodeArg(ctx, argv[2]) : nullptr;
  if (b && b->parent() != p) b = nullptr;
  if (p && c && c != b && !isAncestorOrSelf(c, p)) p->insertBefore(c, b);
  return JS_UNDEFINED;
}

JSValue js_removeChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 2) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  if (p && c) runtimeOf(ctx).doc.removeChild(p, c);  // may keep it as a LayoutAnimation ghost
  return JS_UNDEFINED;
}

// configureLayoutAnimation({duration, create, update, delete}): arms a
// LayoutAnimation for the commit in progress (@orktes/jsui-react calls it from
// prepareForCommit after LayoutAnimation.configureNext). Each of create /
// update / delete: {type, property, duration, delay, springDamping}.
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

JSValue js_configureLayoutAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Document& doc = runtimeOf(ctx).doc;
  if (argc < 1 || !JS_IsObject(argv[0])) {
    doc.disarmLayoutAnimation();
    return JS_UNDEFINED;
  }
  Value cfg = toValue(ctx, argv[0]);
  double duration = cfg["duration"].isNumber() ? cfg["duration"].asNumber() : 300;
  LayoutAnimConfig c;
  c.create = layoutAnimSpec(cfg["create"], duration);
  c.update = layoutAnimSpec(cfg["update"], duration);
  c.remove = layoutAnimSpec(cfg["delete"], duration);
  doc.armLayoutAnimation(c);
  return JS_UNDEFINED;
}

JSValue js_destroyNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JSUI_COMMIT_SCOPE;
  if (argc < 1) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) runtimeOf(ctx).doc.destroyNode(n);
  return JS_UNDEFINED;
}

JSValue js_measure(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (!n) return JS_NULL;
  // Layout may be stale mid-commit; onLayout events it raises are queued
  // until JS unwinds (see JsEngine::layoutEvent).
  runtimeOf(ctx).doc.layout();
  Rect r = n->layoutRect(), a = n->absoluteRect();
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, r.x));
  JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, r.y));
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, r.width));
  JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, r.height));
  JS_SetPropertyStr(ctx, o, "pageX", JS_NewFloat64(ctx, a.x));
  JS_SetPropertyStr(ctx, o, "pageY", JS_NewFloat64(ctx, a.y));
  return o;
}

// viewport() -> {width, height, scale}: logical viewport size (Dimensions).
JSValue js_viewport(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  const Document& doc = runtimeOf(ctx).doc;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, doc.width()));
  JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, doc.height()));
  JS_SetPropertyStr(ctx, o, "scale", JS_NewFloat64(ctx, doc.pixelRatio()));
  return o;
}

JSValue js_focus(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (n) runtimeOf(ctx).input->focus(n);
  return JS_UNDEFINED;
}

JSValue js_blur(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (n) runtimeOf(ctx).input->blur(n);
  return JS_UNDEFINED;
}

// scrollTo(id, x, y, animated): InputManager clamps to the content extent and
// dispatches scroll events. animated: eases there on the runtime clock;
// otherwise the jump is immediate (and stops a running fling).
JSValue js_scrollTo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (!n) return JS_UNDEFINED;
  double x = 0, y = 0;
  if (argc > 1) JS_ToFloat64(ctx, &x, argv[1]);
  if (argc > 2) JS_ToFloat64(ctx, &y, argv[2]);
  bool animated = argc > 3 && JS_ToBool(ctx, argv[3]) > 0;
  Runtime& rt = runtimeOf(ctx);
  rt.doc.layout();
  rt.input->scrollTo(n, float(x == x ? x : 0), float(y == y ? y : 0), animated);
  return JS_UNDEFINED;
}

// flashScrollIndicators(id): shows the overlay scroll indicators briefly.
JSValue js_flashScrollIndicators(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (!n) return JS_UNDEFINED;
  Runtime& rt = runtimeOf(ctx);
  rt.doc.layout();
  rt.input->flashScrollIndicators(n);
  return JS_UNDEFINED;
}

// setTextInputSelection(id, start, end): TextInput selection from JS
// (ref.setSelection, the controlled `selection` prop). UTF-16 indices.
JSValue js_setTextInputSelection(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? nodeArg(ctx, argv[0]) : nullptr;
  if (!n) return JS_UNDEFINED;
  double start = 0, end = 0;
  if (argc > 1) JS_ToFloat64(ctx, &start, argv[1]);
  end = start;
  if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) JS_ToFloat64(ctx, &end, argv[2]);
  auto toIndex = [](double v) { return v == v ? int(std::clamp(v, 0.0, 1e9)) : 0; };
  runtimeOf(ctx).input->setInputSelection(n, toIndex(start), toIndex(end));
  return JS_UNDEFINED;
}

// getClipboard() -> string, setClipboard(text): the embedder's clipboard
// (jsui_set_clipboard_callbacks) or the runtime's in-process one.
JSValue js_getClipboard(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  std::string t = runtimeOf(ctx).clipboardText();
  return JS_NewStringLen(ctx, t.data(), t.size());
}

JSValue js_setClipboard(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  runtimeOf(ctx).setClipboardText(argc > 0 ? toStdString(ctx, argv[0]) : std::string());
  return JS_UNDEFINED;
}

JSValue js_postMessage(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
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
  if (rt.config.on_message) rt.config.on_message(rt.config.message_user, name.c_str(), json.c_str());
  return JS_UNDEFINED;
}

// Text measurement caches depend on the font set: dirty every Text node.
void invalidateText(Node* n) {
  if (n->type() == NodeType::Text) n->markDirty();
  for (Node* c : n->children()) invalidateText(c);
}

void fontLoaded(JsEngine::Impl& impl, uint64_t id, Resource& res, const std::string& family, int weight,
                bool italic) {
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
  } else if (!rt.fonts.registerFont(family, res.data.data(), res.data.size(), weight, italic)) {
    error = "loadFont: " + res.url + " is not a supported font file";
  }
  if (error.empty()) {
    invalidateText(rt.doc.root());
    rt.doc.markLayoutDirty();
    JSValue ok = JS_TRUE;
    JS_FreeValue(ctx, impl.call(resolve, JS_UNDEFINED, 1, &ok));
  } else {
    rt.log(JSUI_LOG_WARN, error);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewStringLen(ctx, error.data(), error.size()));
    JS_FreeValue(ctx, impl.call(reject, JS_UNDEFINED, 1, &err));
    JS_FreeValue(ctx, err);
  }
  JS_FreeValue(ctx, resolve);
  JS_FreeValue(ctx, reject);
}

// loadFont(family, url, weight, italic) -> Promise<true>
JSValue js_loadFont(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto& impl = implOf(ctx);
  Runtime& rt = impl.rt;
  std::string family = argc > 0 ? toStdString(ctx, argv[0]) : "";
  std::string url = argc > 1 ? toStdString(ctx, argv[1]) : "";
  int weight = 400;
  if (argc > 2 && JS_IsNumber(argv[2])) JS_ToInt32(ctx, &weight, argv[2]);
  bool italic = argc > 3 && JS_ToBool(ctx, argv[3]);
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise)) return promise;
  if (family.empty() || url.empty()) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "loadFont(family, url): family and url are required"));
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

JSValue js_reload(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  runtimeOf(ctx).requestReload();
  return JS_UNDEFINED;
}

}  // namespace

void installAnimatedBridge(JsEngine::Impl& impl, JSValue native);  // bridge_animated.cpp

void installBridge(JsEngine::Impl& impl, JSValue global) {
  JSContext* ctx = impl.ctx;
  JSValue native = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"createNode", js_createNode, 1},     {"setProps", js_setProps, 2},
             {"patchStyle", js_patchStyle, 2},     {"setText", js_setText, 2},
             {"appendChild", js_appendChild, 2},   {"insertBefore", js_insertBefore, 3},
             {"removeChild", js_removeChild, 2},   {"destroyNode", js_destroyNode, 1},
             {"measure", js_measure, 1},           {"focus", js_focus, 1},
             {"blur", js_blur, 1},                 {"scrollTo", js_scrollTo, 4},
             {"postMessage", js_postMessage, 2},   {"loadFont", js_loadFont, 4},
             {"reload", js_reload, 0},             {"viewport", js_viewport, 0},
             {"setTextInputSelection", js_setTextInputSelection, 3},
             {"getClipboard", js_getClipboard, 0}, {"setClipboard", js_setClipboard, 1},
             {"flashScrollIndicators", js_flashScrollIndicators, 1},
             {"configureLayoutAnimation", js_configureLayoutAnimation, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, native, "rootId", JS_NewUint32(ctx, impl.rt.doc.root()->id()));
  // TextInput text is edited natively (change / selectionchange events).
  JS_SetPropertyStr(ctx, native, "textEditing", JS_TRUE);
  JS_SetPropertyStr(ctx, native, "devMode", JS_NewBool(ctx, impl.rt.config.dev_mode != 0));
  installAnimatedBridge(impl, native);
  installHostDataBridge(impl, native);
  JS_SetPropertyStr(ctx, global, "__jsui_native", native);
}

}  // namespace jsui
