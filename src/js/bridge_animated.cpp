// __jsui_native.animated: the native Animated driver's bridge (React
// Native's NativeAnimatedModule surface), used by @orktes/jsui-react when an
// animation runs with useNativeDriver: true. See animated/native_animated.h.
// Graph changes are recorded as commands (js/command_buffer.h).
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

using Op = CommandBuffer::Op;

CommandBuffer& commands(JSContext* ctx) { return implOf(ctx).commands; }

// Tags travel as the bits of an int32.
uint32_t tagArg(JSContext* ctx, int argc, JSValueConst* argv, int i) { return uint32_t(intArg(ctx, argc, argv, i)); }

// Graph changes are commands (js/command_buffer.h), applied in call order
// with the node tree's. createNode and startAnimation apply at once
// single-threaded: they throw on a bad config.
JSValue js_createNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).pushValue(Op::AnimCreate, tagArg(ctx, argc, argv, 0), argc > 1 ? toValue(ctx, argv[1]) : Value());
  const CommandBuffer::Result& r = implOf(ctx).applyNow();
  if (!r.ok) return JS_ThrowTypeError(ctx, "Animated (native): %s", r.error.c_str());
  return JS_UNDEFINED;
}

JSValue js_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimConnect, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_disconnect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimDisconnect, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_connectToView(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimConnectView, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_disconnectFromView(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimDisconnectView, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue js_dropNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimDrop, tagArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_setValue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).pushXY(Op::AnimSetValue, tagArg(ctx, argc, argv, 0), numArg(ctx, argc, argv, 1), 0);
  return JS_UNDEFINED;
}

JSValue js_setOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).pushXY(Op::AnimSetOffset, tagArg(ctx, argc, argv, 0), numArg(ctx, argc, argv, 1), 0);
  return JS_UNDEFINED;
}

JSValue js_flattenOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimFlattenOffset, tagArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_extractOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimExtractOffset, tagArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// A read: what JS recorded is applied first.
JSValue js_getValue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  implOf(ctx).syncPoint();
  return JS_NewFloat64(ctx, na(ctx).getValue(intArg(ctx, argc, argv, 0)));
}

// startAnimation(id, valueTag, config): starts on the runtime clock now.
JSValue js_startAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).pushValue(Op::AnimStart, tagArg(ctx, argc, argv, 0), argc > 2 ? toValue(ctx, argv[2]) : Value(),
                          tagArg(ctx, argc, argv, 1));
  const CommandBuffer::Result& r = implOf(ctx).applyNow();
  if (!r.ok) return JS_ThrowTypeError(ctx, "Animated (native): %s", r.error.c_str());
  return JS_UNDEFINED;
}

// stopAnimation(id) -> {velocity} if it was running, else null. The caller
// reads the value with getValue and reports the end itself.
JSValue js_stopAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimStop, tagArg(ctx, argc, argv, 0));
  const CommandBuffer::Result& r = implOf(ctx).applyNow();
  if (!r.ok) return JS_NULL;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "velocity", JS_NewFloat64(ctx, r.number));
  return o;
}

// addEventMapping(viewId, eventType, path[], valueTag)
JSValue js_addEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 4) return JS_UNDEFINED;
  Value keys = toValue(ctx, argv[2]);
  CommandBuffer& cmds = commands(ctx);
  cmds.pushString(Op::AnimAddEventMapping, tagArg(ctx, argc, argv, 0), toStdString(ctx, argv[1]),
                  tagArg(ctx, argc, argv, 3), uint32_t(keys.asArray().size()));
  for (const Value& k : keys.asArray()) cmds.pushString(k.asString());
  return JS_UNDEFINED;
}

JSValue js_removeEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) return JS_UNDEFINED;
  commands(ctx).pushString(Op::AnimRemoveEventMapping, tagArg(ctx, argc, argv, 0), toStdString(ctx, argv[1]),
                           tagArg(ctx, argc, argv, 2));
  return JS_UNDEFINED;
}

JSValue js_startListening(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimStartListening, tagArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_stopListening(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimStopListening, tagArg(ctx, argc, argv, 0));
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
