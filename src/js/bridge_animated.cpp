// __jsui_native.animated: the native Animated driver's bridge (React
// Native's NativeAnimatedModule surface), used by @orktes/jsui-react when an
// animation runs with useNativeDriver: true. See animated/native_animated.h.
// Graph changes are recorded as commands (js/command_buffer.h). In the
// runtime's JS-thread mode getValue and stopAnimation's velocity come from
// the frame snapshot (frame_snapshot.h), or from what JS itself wrote since.
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
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  if (argc > 1) writeValue(ctx, argv[1], w);
  else w.null();
  cmds.end(Op::AnimCreate, tagArg(ctx, argc, argv, 0));
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
  uint32_t tag = tagArg(ctx, argc, argv, 0);
  double v = numArg(ctx, argc, argv, 1);
  commands(ctx).pushXY(Op::AnimSetValue, tag, v, 0);
  JsEngine::Impl& impl = implOf(ctx);
  if (impl.threaded) impl.animWrites[int(int32_t(tag))] = {v, impl.publishedBatches + 1};
  return JS_UNDEFINED;
}

JSValue js_setOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).pushXY(Op::AnimSetOffset, tagArg(ctx, argc, argv, 0), numArg(ctx, argc, argv, 1), 0);
  return JS_UNDEFINED;
}

// Flattening / extracting moves the raw value by the offset, which JS-thread
// mode does not follow: getValue answers from the snapshot again (one frame
// behind until the command is applied).
JSValue js_flattenOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimFlattenOffset, tagArg(ctx, argc, argv, 0));
  implOf(ctx).animWrites.erase(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

JSValue js_extractOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimExtractOffset, tagArg(ctx, argc, argv, 0));
  implOf(ctx).animWrites.erase(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// JS-thread mode: drops the bookkeeping of writes and starts the snapshot
// `snap` includes (only now and then: it is only a bound on memory).
void sweepWrites(JsEngine::Impl& impl, const FrameSnapshot& snap) {
  if (impl.animWrites.size() + impl.animStarts.size() < 256) return;
  for (auto it = impl.animWrites.begin(); it != impl.animWrites.end();)
    it = it->second.batch <= snap.appliedBatches ? impl.animWrites.erase(it) : std::next(it);
  for (auto it = impl.animStarts.begin(); it != impl.animStarts.end();)
    it = it->second <= snap.appliedBatches ? impl.animStarts.erase(it) : std::next(it);
}

// A read: what JS recorded is applied first. JS-thread mode: the value JS
// set last, unless the snapshot is newer than that write.
JSValue js_getValue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  JsEngine::Impl& impl = implOf(ctx);
  int tag = intArg(ctx, argc, argv, 0);
  if (impl.threaded) {
    std::shared_ptr<const FrameSnapshot> snap = impl.rt.snapshots.current();
    auto w = impl.animWrites.find(tag);
    if (w != impl.animWrites.end()) {
      if (!snap || snap->appliedBatches < w->second.batch) return JS_NewFloat64(ctx, w->second.value);
      impl.animWrites.erase(w);
    }
    if (!snap) return JS_NewFloat64(ctx, 0);
    sweepWrites(impl, *snap);
    auto v = snap->values.find(tag);
    return JS_NewFloat64(ctx, v == snap->values.end() ? 0 : v->second);
  }
  impl.syncPoint();
  return JS_NewFloat64(ctx, na(ctx).getValue(tag));
}

// startAnimation(id, valueTag, config): starts on the runtime clock now.
JSValue js_startAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  if (argc > 2) writeValue(ctx, argv[2], w);
  else w.null();
  cmds.end(Op::AnimStart, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 1));
  JsEngine::Impl& impl = implOf(ctx);
  // JS-thread mode: running from this batch on (stopAnimation below).
  if (impl.threaded) impl.animStarts[intArg(ctx, argc, argv, 0)] = impl.publishedBatches + 1;
  const CommandBuffer::Result& r = impl.applyNow();
  if (!r.ok) return JS_ThrowTypeError(ctx, "Animated (native): %s", r.error.c_str());
  return JS_UNDEFINED;
}

// stopAnimation(id) -> {velocity} if it was running, else null. The caller
// reads the value with getValue and reports the end itself.
//
// JS-thread mode: the stop is applied with the batch; the answer comes from
// the snapshot (whether it ran on the last frame, and its velocity then), or
// {velocity: 0} for an animation started after it. One frame's
// approximation: the final value still arrives through __jsui_onAnimatedEnd.
JSValue js_stopAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  commands(ctx).push(Op::AnimStop, tagArg(ctx, argc, argv, 0));
  JsEngine::Impl& impl = implOf(ctx);
  double velocity = 0;
  if (impl.threaded) {
    int id = intArg(ctx, argc, argv, 0);
    std::shared_ptr<const FrameSnapshot> snap = impl.rt.snapshots.current();
    auto st = impl.animStarts.find(id);
    bool unseen = st != impl.animStarts.end() && (!snap || snap->appliedBatches < st->second);
    if (st != impl.animStarts.end()) impl.animStarts.erase(st);
    if (!unseen) {
      if (!snap) return JS_NULL;
      auto a = snap->animations.find(id);
      if (a == snap->animations.end()) return JS_NULL;
      velocity = a->second;
    }
  } else {
    const CommandBuffer::Result& r = impl.applyNow();
    if (!r.ok) return JS_NULL;
    velocity = r.number;
  }
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "velocity", JS_NewFloat64(ctx, velocity));
  return o;
}

// addEventMapping(viewId, eventType, path[], valueTag)
JSValue js_addEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 4) return JS_UNDEFINED;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeValue(ctx, argv[2], w);   // the path (its keys)
  writeString(ctx, argv[1], w);  // the event type
  cmds.end(Op::AnimAddEventMapping, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 3));
  return JS_UNDEFINED;
}

JSValue js_removeEventMapping(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) return JS_UNDEFINED;
  CommandBuffer& cmds = commands(ctx);
  CommandBuffer::Writer w = cmds.begin();
  writeString(ctx, argv[1], w);
  cmds.end(Op::AnimRemoveEventMapping, tagArg(ctx, argc, argv, 0), tagArg(ctx, argc, argv, 2));
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
