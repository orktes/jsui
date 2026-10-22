// QuickJS host. SKELETON: node bridge, console and timers. fetch, rAF,
// fonts, host messages and events are layered on top.
#include "js/engine.h"

#include <quickjs.h>

#include <map>
#include <vector>

#include "core/document.h"
#include "runtime.h"

namespace jsui {

struct JsEngine::Impl {
  Runtime& rt;
  JSRuntime* jrt = nullptr;
  JSContext* ctx = nullptr;

  struct Timer {
    double due;
    double interval;  // < 0: one-shot
    JSValue fn;
  };
  std::map<int, Timer> timers;
  int nextTimer = 1;

  explicit Impl(Runtime& r) : rt(r) {}
};

namespace {

Runtime& runtimeOf(JSContext* ctx) { return static_cast<JsEngine::Impl*>(JS_GetContextOpaque(ctx))->rt; }
JsEngine::Impl& implOf(JSContext* ctx) { return *static_cast<JsEngine::Impl*>(JS_GetContextOpaque(ctx)); }

std::string toStdString(JSContext* ctx, JSValueConst v) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) return {};
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

// JSValue -> Value. Functions and undefined become null; depth-limited.
Value toValue(JSContext* ctx, JSValueConst v, int depth = 0) {
  if (depth > 32) return Value();
  if (JS_IsBool(v)) return Value(bool(JS_ToBool(ctx, v)));
  if (JS_IsNumber(v)) {
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return Value(d);
  }
  if (JS_IsString(v)) return Value(toStdString(ctx, v));
  if (JS_IsArray(v)) {
    Value::Array arr;
    int64_t len = 0;
    JS_GetLength(ctx, v, &len);
    arr.reserve(size_t(len));
    for (int64_t i = 0; i < len; i++) {
      JSValue e = JS_GetPropertyInt64(ctx, v, i);
      arr.push_back(toValue(ctx, e, depth + 1));
      JS_FreeValue(ctx, e);
    }
    return Value(std::move(arr));
  }
  if (JS_IsObject(v) && !JS_IsFunction(ctx, v)) {
    Value::Object obj;
    JSPropertyEnum* props = nullptr;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
      for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetProperty(ctx, v, props[i].atom);
        if (!JS_IsUndefined(e) && !JS_IsFunction(ctx, e)) {
          const char* k = JS_AtomToCString(ctx, props[i].atom);
          obj[k] = toValue(ctx, e, depth + 1);
          JS_FreeCString(ctx, k);
        }
        JS_FreeValue(ctx, e);
      }
      JS_FreePropertyEnum(ctx, props, n);
    }
    return Value(std::move(obj));
  }
  return Value();
}

JSValue fromValue(JSContext* ctx, const Value& v) {
  if (v.isBool()) return JS_NewBool(ctx, v.asBool());
  if (v.isNumber()) return JS_NewFloat64(ctx, v.asNumber());
  if (v.isString()) return JS_NewStringLen(ctx, v.asString().data(), v.asString().size());
  if (v.isArray()) {
    JSValue a = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& e : v.asArray()) JS_SetPropertyUint32(ctx, a, i++, fromValue(ctx, e));
    return a;
  }
  if (v.isObject()) {
    JSValue o = JS_NewObject(ctx);
    for (auto& [k, e] : v.asObject()) JS_SetPropertyStr(ctx, o, k.c_str(), fromValue(ctx, e));
    return o;
  }
  return JS_NULL;
}

Node* nodeArg(JSContext* ctx, JSValueConst v) {
  uint32_t id = 0;
  if (JS_ToUint32(ctx, &id, v)) return nullptr;
  return runtimeOf(ctx).doc.get(id);
}

JSValue js_createNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  NodeType t;
  std::string name = argc > 0 ? toStdString(ctx, argv[0]) : "";
  if (!nodeTypeFromName(name, &t)) return JS_ThrowTypeError(ctx, "unknown element type '%s'", name.c_str());
  return JS_NewUint32(ctx, runtimeOf(ctx).doc.createNode(t)->id());
}

JSValue js_setProps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) n->setProps(toValue(ctx, argv[1]));
  return JS_UNDEFINED;
}

JSValue js_setText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) n->setText(toStdString(ctx, argv[1]));
  return JS_UNDEFINED;
}

JSValue js_appendChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  if (p && c) p->appendChild(c);
  return JS_UNDEFINED;
}

JSValue js_insertBefore(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  Node* b = nodeArg(ctx, argv[2]);
  if (p && c) p->insertBefore(c, b);
  return JS_UNDEFINED;
}

JSValue js_removeChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  Node* p = nodeArg(ctx, argv[0]);
  Node* c = nodeArg(ctx, argv[1]);
  if (p && c) p->removeChild(c);
  return JS_UNDEFINED;
}

JSValue js_destroyNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  if (Node* n = nodeArg(ctx, argv[0])) runtimeOf(ctx).doc.destroyNode(n);
  return JS_UNDEFINED;
}

JSValue js_console(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
  std::string msg;
  for (int i = 0; i < argc; i++) {
    if (i) msg += ' ';
    if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]) && !JS_IsError(argv[i])) {
      JSValue j = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
      msg += toStdString(ctx, j);
      JS_FreeValue(ctx, j);
    } else {
      msg += toStdString(ctx, argv[i]);
    }
  }
  runtimeOf(ctx).log(JsuiLogLevel(magic), msg);
  return JS_UNDEFINED;
}

JSValue js_setTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int repeat) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  double ms = 0;
  if (argc > 1) JS_ToFloat64(ctx, &ms, argv[1]);
  if (!(ms >= 0)) ms = 0;
  auto& impl = implOf(ctx);
  int id = impl.nextTimer++;
  impl.timers[id] = {impl.rt.now + ms, repeat ? ms : -1.0, JS_DupValue(ctx, argv[0])};
  return JS_NewInt32(ctx, id);
}

JSValue js_clearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int id = 0;
  if (argc < 1 || JS_ToInt32(ctx, &id, argv[0])) return JS_UNDEFINED;
  auto& impl = implOf(ctx);
  auto it = impl.timers.find(id);
  if (it != impl.timers.end()) {
    JS_FreeValue(ctx, it->second.fn);
    impl.timers.erase(it);
  }
  return JS_UNDEFINED;
}

JSValue js_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_NewFloat64(ctx, runtimeOf(ctx).now);
}

void reportException(JSContext* ctx) {
  JSValue exc = JS_GetException(ctx);
  std::string msg = toStdString(ctx, exc);
  JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
  if (JS_IsString(stack)) msg += "\n" + toStdString(ctx, stack);
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, exc);
  runtimeOf(ctx).reportError(msg);
}

}  // namespace

JsEngine::JsEngine(Runtime& rt) : impl_(std::make_unique<Impl>(rt)) {
  impl_->jrt = JS_NewRuntime();
  impl_->ctx = JS_NewContext(impl_->jrt);
  JSContext* ctx = impl_->ctx;
  JS_SetContextOpaque(ctx, impl_.get());

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue native = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"createNode", js_createNode, 1},     {"setProps", js_setProps, 2},
             {"setText", js_setText, 2},           {"appendChild", js_appendChild, 2},
             {"insertBefore", js_insertBefore, 3}, {"removeChild", js_removeChild, 2},
             {"destroyNode", js_destroyNode, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, native, "rootId", JS_NewUint32(ctx, rt.doc.root()->id()));
  JS_SetPropertyStr(ctx, global, "__jsui_native", native);

  JSValue console = JS_NewObject(ctx);
  struct Level {
    const char* name;
    int level;
  } levels[] = {{"log", JSUI_LOG_INFO},  {"info", JSUI_LOG_INFO},   {"debug", JSUI_LOG_DEBUG},
                {"warn", JSUI_LOG_WARN}, {"error", JSUI_LOG_ERROR}};
  for (auto& l : levels) {
    JS_SetPropertyStr(ctx, console, l.name, JS_NewCFunctionMagic(ctx, js_console, l.name, 1, JS_CFUNC_generic_magic, l.level));
  }
  JS_SetPropertyStr(ctx, global, "console", console);

  JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunctionMagic(ctx, js_setTimer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunctionMagic(ctx, js_setTimer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_clearTimer, "clearTimeout", 1));
  JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_clearTimer, "clearInterval", 1));
  JSValue perf = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_now, "now", 0));
  JS_SetPropertyStr(ctx, global, "performance", perf);
  JS_SetPropertyStr(ctx, global, "globalThis", JS_DupValue(ctx, global));
  JS_FreeValue(ctx, global);
}

JsEngine::~JsEngine() {
  for (auto& [id, t] : impl_->timers) JS_FreeValue(impl_->ctx, t.fn);
  impl_->timers.clear();
  JS_FreeContext(impl_->ctx);
  JS_FreeRuntime(impl_->jrt);
}

bool JsEngine::eval(const std::string& source, const std::string& name) {
  JSValue r = JS_Eval(impl_->ctx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
  bool ok = !JS_IsException(r);
  if (!ok) reportException(impl_->ctx);
  JS_FreeValue(impl_->ctx, r);
  runJobs();
  return ok;
}

void JsEngine::runJobs() {
  JSContext* pctx = nullptr;
  for (;;) {
    int r = JS_ExecutePendingJob(impl_->jrt, &pctx);
    if (r == 0) break;
    if (r < 0) reportException(pctx);
  }
}

void JsEngine::runTimers(double nowMs) {
  JSContext* ctx = impl_->ctx;
  // Collect due timers first: callbacks may add or clear timers.
  std::vector<int> due;
  for (auto& [id, t] : impl_->timers)
    if (t.due <= nowMs) due.push_back(id);
  for (int id : due) {
    auto it = impl_->timers.find(id);
    if (it == impl_->timers.end()) continue;
    JSValue fn = JS_DupValue(ctx, it->second.fn);
    if (it->second.interval >= 0) {
      it->second.due = nowMs + std::max(1.0, it->second.interval);
    } else {
      JS_FreeValue(ctx, it->second.fn);
      impl_->timers.erase(it);
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(r)) reportException(ctx);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
    runJobs();
  }
}

void JsEngine::runAnimationFrame(double) {}

bool JsEngine::hasPendingWork(double nowMs) const {
  for (auto& [id, t] : impl_->timers)
    if (t.due <= nowMs) return true;
  return JS_IsJobPending(impl_->jrt);
}

bool JsEngine::dispatchEvent(const Value& event) {
  JSContext* ctx = impl_->ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, global, "__jsui_dispatchEvent");
  bool handled = false;
  if (JS_IsFunction(ctx, fn)) {
    JSValue arg = fromValue(ctx, event);
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r)) reportException(ctx);
    else handled = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
  }
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
  runJobs();
  return handled;
}

void JsEngine::hostMessage(const std::string&, const std::string&) {}
void JsEngine::layoutEvent(uint32_t, float, float, float, float) {}

}  // namespace jsui
