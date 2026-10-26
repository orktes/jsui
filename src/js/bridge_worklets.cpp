// __jsui_native.worklets: react-native-worklets' native module for the
// bundle (@orktes/jsui-react src/worklets.js; js/worklets.h). Jobs go to
// the owner thread with the task's batch (Runtime::toHost), so a worklet
// sees the Animated nodes and views the same task created; runSync is a
// read the owner thread answers (Impl::onOwner: at once single-threaded, a
// proxy call on a JS thread). Synchronizables are thread-safe and called
// directly.
#include "js/engine_impl.h"
#include "js/worklets.h"
#include "runtime.h"

namespace jsui {

namespace {

int intArg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  int32_t v = 0;
  if (i < argc) JS_ToInt32(ctx, &v, argv[i]);
  return v;
}
std::string str(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  return i < argc ? toStdString(ctx, argv[i]) : std::string();
}
Worklets& hub(JSContext* ctx) { return *runtimeOf(ctx).worklets; }

// schedule(runtimeId, job): runs in the next worklet phase.
JSValue js_schedule(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* r = &runtimeOf(ctx);
  r->toHost([r, id = intArg(ctx, argc, argv, 0), job = str(ctx, argc, argv, 1)]() mutable {
    r->worklets->post(id, std::move(job));
  });
  return JS_UNDEFINED;
}

// runSync(runtimeId, job) -> '{"ok":..., "v"|"e": wire}', or null when the
// owner thread did not answer in time (JS-thread mode).
JSValue js_runSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  int id = intArg(ctx, argc, argv, 0);
  std::string job = str(ctx, argc, argv, 1), out;
  bool ran = implOf(ctx).onOwner([&] { out = rt.worklets->runSync(id, job); });
  if (!ran) return JS_NULL;
  return JS_NewStringLen(ctx, out.data(), out.size());
}

// createRuntime(name) -> id (the runtime is made on the owner thread, before
// the jobs scheduled after this call).
JSValue js_createRuntime(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* r = &runtimeOf(ctx);
  int id = r->worklets->reserveRuntimeId();
  r->toHost([r, id, name = str(ctx, argc, argv, 0)]() mutable { r->worklets->create(id, std::move(name)); });
  return JS_NewInt32(ctx, id);
}

JSValue js_syncCreate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return JS_NewInt32(ctx, hub(ctx).syncCreate(str(ctx, argc, argv, 0)));
}
JSValue js_syncGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string s = hub(ctx).syncGet(intArg(ctx, argc, argv, 0), argc > 1 && JS_ToBool(ctx, argv[1]));
  return JS_NewStringLen(ctx, s.data(), s.size());
}
JSValue js_syncSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  hub(ctx).syncSet(intArg(ctx, argc, argv, 0), str(ctx, argc, argv, 1), argc > 2 && JS_ToBool(ctx, argv[2]));
  return JS_UNDEFINED;
}
JSValue js_syncLock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return JS_NewBool(ctx, hub(ctx).syncLock(intArg(ctx, argc, argv, 0)));
}
JSValue js_syncUnlock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  hub(ctx).syncUnlock(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}
JSValue js_syncRetain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  hub(ctx).syncRetain(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}
JSValue js_syncRelease(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  hub(ctx).syncRelease(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

}  // namespace

void installWorkletsBridge(JsEngine::Impl& impl, JSValue native) {
  JSContext* ctx = impl.ctx;
  JSValue w = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"schedule", js_schedule, 2},       {"runSync", js_runSync, 2},       {"createRuntime", js_createRuntime, 1},
             {"syncCreate", js_syncCreate, 1},   {"syncGet", js_syncGet, 2},       {"syncSet", js_syncSet, 3},
             {"syncLock", js_syncLock, 1},       {"syncUnlock", js_syncUnlock, 1}, {"syncRetain", js_syncRetain, 1},
             {"syncRelease", js_syncRelease, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, w, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, w, "uiRuntimeId", JS_NewInt32(ctx, Worklets::kUiRuntime));
  JS_SetPropertyStr(ctx, native, "worklets", w);
}

}  // namespace jsui
