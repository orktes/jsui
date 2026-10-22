// __jsui_native.animated: the native Animated driver's bridge (React
// Native's NativeAnimatedModule surface), used by @jsui/react when an
// animation runs with useNativeDriver: true. See animated/native_animated.h.
#include "animated/native_animated.h"
#include "js/engine_impl.h"
#include "runtime.h"

namespace jsui {

namespace {

NativeAnimated& na(JSContext* ctx) { return *runtimeOf(ctx).animated; }

int intArg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  int32_t v = 0;
  if (i < argc) JS_ToInt32(ctx, &v, argv[i]);
  return v;
}

double numArg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  double v = 0;
  if (i < argc) JS_ToFloat64(ctx, &v, argv[i]);
  return v == v ? v : 0;
}

JSValue js_createNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string error;
  if (!na(ctx).createNode(intArg(ctx, argc, argv, 0), argc > 1 ? toValue(ctx, argv[1]) : Value(), &error))
    return JS_ThrowTypeError(ctx, "Animated (native): %s", error.c_str());
  return JS_UNDEFINED;
}

JSValue js_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).connectNodes(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_disconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).disconnectNodes(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_connectToView(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).connectToView(intArg(ctx, argc, argv, 0), uint32_t(intArg(ctx, argc, argv, 1)));
  na(ctx).flush();
  return JS_UNDEFINED;
}

JSValue js_disconnectFromView(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).disconnectFromView(intArg(ctx, argc, argv, 0), uint32_t(intArg(ctx, argc, argv, 1)));
  return JS_UNDEFINED;
}

JSValue js_dropNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).dropNode(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_setValue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).setValue(intArg(ctx, argc, argv, 0), numArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_setOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).setOffset(intArg(ctx, argc, argv, 0), numArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_flattenOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).flattenOffset(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_extractOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).extractOffset(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_getValue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return JS_NewFloat64(ctx, na(ctx).getValue(intArg(ctx, argc, argv, 0)));
}

// startAnimation(id, valueTag, config): starts on the runtime clock now.
JSValue js_startAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string error;
  Runtime& rt = runtimeOf(ctx);
  if (!rt.animated->startAnimation(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1),
                                   argc > 2 ? toValue(ctx, argv[2]) : Value(), rt.now, &error))
    return JS_ThrowTypeError(ctx, "Animated (native): %s", error.c_str());
  return JS_UNDEFINED;
}

// stopAnimation(id) -> {velocity} if it was running, else null. The caller
// reads the value with getValue and reports the end itself.
JSValue js_stopAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  double velocity = 0;
  if (!na(ctx).stopAnimation(intArg(ctx, argc, argv, 0), &velocity)) return JS_NULL;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "velocity", JS_NewFloat64(ctx, velocity));
  return o;
}

// addEventMapping(viewId, eventType, path[], valueTag)
JSValue js_addEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 4) return JS_UNDEFINED;
  std::vector<std::string> path;
  Value keys = toValue(ctx, argv[2]);
  for (const Value& k : keys.asArray()) path.push_back(k.asString());
  na(ctx).addEventMapping(uint32_t(intArg(ctx, argc, argv, 0)), toStdString(ctx, argv[1]), std::move(path),
                          intArg(ctx, argc, argv, 3));
  return JS_UNDEFINED;
}

JSValue js_removeEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) return JS_UNDEFINED;
  na(ctx).removeEventMapping(uint32_t(intArg(ctx, argc, argv, 0)), toStdString(ctx, argv[1]),
                             intArg(ctx, argc, argv, 2));
  return JS_UNDEFINED;
}

JSValue js_startListening(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).startListening(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_stopListening(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  na(ctx).stopListening(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

}  // namespace

void installAnimatedBridge(JsEngine::Impl& impl, JSValue native) {
  JSContext* ctx = impl.ctx;
  JSValue a = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"createNode", js_createNode, 2},           {"connect", js_connect, 2},
             {"disconnect", js_disconnect, 2},           {"connectToView", js_connectToView, 2},
             {"disconnectFromView", js_disconnectFromView, 2}, {"dropNode", js_dropNode, 1},
             {"setValue", js_setValue, 2},               {"setOffset", js_setOffset, 2},
             {"flattenOffset", js_flattenOffset, 1},     {"extractOffset", js_extractOffset, 1},
             {"getValue", js_getValue, 1},               {"startAnimation", js_startAnimation, 3},
             {"stopAnimation", js_stopAnimation, 1},     {"addEventMapping", js_addEventMapping, 4},
             {"removeEventMapping", js_removeEventMapping, 3}, {"startListening", js_startListening, 1},
             {"stopListening", js_stopListening, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, a, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, native, "animated", a);
}

}  // namespace jsui
