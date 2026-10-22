// QuickJS host: context lifetime, native->JS entry bookkeeping (watchdog,
// microtask drain, unhandled rejections), timers and requestAnimationFrame.
// The bridge (__jsui_native) lives in bridge.cpp, the helpers behind the web
// globals in host_api.cpp and their JS half in prelude.cpp.
#include "js/engine.h"

#include <algorithm>
#include <cmath>

#include "core/document.h"
#include "js/engine_impl.h"
#include "runtime.h"

namespace jsui {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------- helpers

Runtime& runtimeOf(JSContext* ctx) { return static_cast<JsEngine::Impl*>(JS_GetContextOpaque(ctx))->rt; }
JsEngine::Impl& implOf(JSContext* ctx) { return *static_cast<JsEngine::Impl*>(JS_GetContextOpaque(ctx)); }

std::string toStdString(JSContext* ctx, JSValueConst v) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) {
    // ToString itself threw (e.g. a Symbol or a throwing toString): swallow.
    JS_FreeValue(ctx, JS_GetException(ctx));
    return {};
  }
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

std::string formatError(JSContext* ctx, JSValueConst exc) {
  std::string msg;
  if (JS_IsError(exc)) {
    msg = toStdString(ctx, exc);  // "TypeError: message"
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
      std::string st = toStdString(ctx, stack);
      while (!st.empty() && (st.back() == '\n' || st.back() == ' ')) st.pop_back();
      if (!st.empty()) msg += "\n" + st;
    }
    JS_FreeValue(ctx, stack);
    if (JS_IsException(stack)) JS_FreeValue(ctx, JS_GetException(ctx));
  } else if (JS_IsObject(exc) && !JS_IsFunction(ctx, exc)) {
    JSValue j = JS_JSONStringify(ctx, exc, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(j)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      msg = toStdString(ctx, exc);
    } else {
      msg = toStdString(ctx, j);
    }
    JS_FreeValue(ctx, j);
  } else {
    msg = toStdString(ctx, exc);
  }
  return msg;
}

Value toValue(JSContext* ctx, JSValueConst v, int depth) {
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
    if (JS_GetLength(ctx, v, &len)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return Value();
    }
    arr.reserve(size_t(std::min<int64_t>(len, 1 << 20)));
    for (int64_t i = 0; i < len && i < (1 << 20); i++) {
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
        if (JS_IsException(e)) {
          JS_FreeValue(ctx, JS_GetException(ctx));  // throwing getter: skip the key
        } else if (!JS_IsUndefined(e) && !JS_IsFunction(ctx, e)) {
          if (const std::string* k = implOf(ctx).atomName(props[i].atom))
            obj.insert_or_assign(*k, toValue(ctx, e, depth + 1));
        }
        JS_FreeValue(ctx, e);
      }
      JS_FreePropertyEnum(ctx, props, n);
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
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

bool bytesOf(JSContext* ctx, JSValueConst v, std::vector<uint8_t>* out) {
  out->clear();
  if (JS_IsUndefined(v) || JS_IsNull(v)) return true;
  if (JS_IsString(v)) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return false;
    out->assign(s, s + len);
    JS_FreeCString(ctx, s);
    return true;
  }
  size_t size = 0;
  if (JS_IsArrayBuffer(v)) {
    uint8_t* p = JS_GetArrayBuffer(ctx, &size, v);
    if (!p && size) return false;
    if (p) out->assign(p, p + size);
    return true;
  }
  size_t offset = 0, bytes = 0, per = 0;
  JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &offset, &bytes, &per);
  if (JS_IsException(buf)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    // DataView: {buffer, byteOffset, byteLength}
    JSValue b = JS_GetPropertyStr(ctx, v, "buffer");
    bool ok = false;
    if (JS_IsArrayBuffer(b)) {
      uint8_t* p = JS_GetArrayBuffer(ctx, &size, b);
      double off = 0, len = 0;
      JSValue o = JS_GetPropertyStr(ctx, v, "byteOffset"), l = JS_GetPropertyStr(ctx, v, "byteLength");
      JS_ToFloat64(ctx, &off, o);
      JS_ToFloat64(ctx, &len, l);
      JS_FreeValue(ctx, o);
      JS_FreeValue(ctx, l);
      if (p && off >= 0 && len >= 0 && off + len <= double(size)) {
        out->assign(p + size_t(off), p + size_t(off + len));
        ok = true;
      }
    }
    JS_FreeValue(ctx, b);
    return ok;
  }
  uint8_t* p = JS_GetArrayBuffer(ctx, &size, buf);
  JS_FreeValue(ctx, buf);
  if (!p || offset + bytes > size) return bytes == 0;
  out->assign(p + offset, p + offset + bytes);
  return true;
}

const std::string* JsEngine::Impl::atomName(JSAtom atom) {
  auto it = atomNames.find(atom);
  if (it != atomNames.end()) return &it->second;
  size_t len = 0;
  const char* k = JS_AtomToCStringLen(ctx, &len, atom);
  if (!k) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return nullptr;
  }
  if (atomNames.size() >= 4096) {  // pathological key churn: start over
    for (auto& [a, name] : atomNames) JS_FreeAtom(ctx, a);
    atomNames.clear();
  }
  auto& slot = atomNames[JS_DupAtom(ctx, atom)];
  slot.assign(k, len);
  JS_FreeCString(ctx, k);
  return &slot;
}

// ------------------------------------------------- entry bookkeeping

void JsEngine::Impl::enter() {
  if (depth++ == 0) {
    JS_UpdateStackTop(jrt);  // the embedder may call us from different stack depths
    deadline = Clock::now() + std::chrono::microseconds(int64_t(std::max(0.0, timeoutMs) * 1000));
    interrupted = false;
  }
}

void JsEngine::Impl::leave() {
  if (--depth == 0 && !draining) drain();
}

JSValue JsEngine::Impl::call(JSValueConst fn, JSValueConst thisVal, int argc, JSValueConst* argv) {
  enter();
  JSValue r = JS_Call(ctx, fn, thisVal, argc, argv);
  if (JS_IsException(r)) {
    reportException();
    r = JS_EXCEPTION;
  }
  leave();
  return r;
}

void JsEngine::Impl::runJobsRaw() {
  JSContext* pctx = nullptr;
  for (int guard = 0; guard < 10000000; guard++) {
    int r = JS_ExecutePendingJob(jrt, &pctx);
    if (r == 0) break;
    if (r < 0) reportException();
  }
}

void JsEngine::Impl::drain() {
  draining = true;
  depth++;
  // Microtasks get a fresh watchdog budget of their own.
  deadline = Clock::now() + std::chrono::microseconds(int64_t(std::max(0.0, timeoutMs) * 1000));
  interrupted = false;
  for (int round = 0; round < 64; round++) {
    runJobsRaw();
    if (!rejections.empty()) {
      auto list = std::move(rejections);
      rejections.clear();
      for (auto& r : list) {
        reportValue(r.reason, "Unhandled promise rejection: ");
        JS_FreeValue(ctx, r.promise);
        JS_FreeValue(ctx, r.reason);
      }
    }
    if (layoutQueue.empty()) break;
    layoutScratch.swap(layoutQueue);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetProperty(ctx, global, atomOnLayout);
    for (auto& e : layoutScratch) {
      if (!JS_IsFunction(ctx, fn)) break;
      JSValue args[2] = {JS_NewUint32(ctx, e.id), JS_NewObject(ctx)};
      JS_SetPropertyStr(ctx, args[1], "x", JS_NewFloat64(ctx, e.x));
      JS_SetPropertyStr(ctx, args[1], "y", JS_NewFloat64(ctx, e.y));
      JS_SetPropertyStr(ctx, args[1], "width", JS_NewFloat64(ctx, e.w));
      JS_SetPropertyStr(ctx, args[1], "height", JS_NewFloat64(ctx, e.h));
      JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
      if (JS_IsException(r)) reportException();
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, args[1]);
    }
    layoutScratch.clear();
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
  }
  depth--;
  draining = false;
}

void JsEngine::Impl::reportValue(JSValueConst exc, const char* prefix) {
  std::string msg = prefix + formatError(ctx, exc);
  if (interrupted) {
    msg = "Script timed out: a single JS call ran longer than " + std::to_string(int(timeoutMs)) +
          " ms and was interrupted (js_timeout_ms).\n" + msg;
    interrupted = false;
  }
  rt.reportError(msg);
}

void JsEngine::Impl::reportException() {
  JSValue exc = JS_GetException(ctx);
  reportValue(exc, "");
  JS_FreeValue(ctx, exc);
}

void JsEngine::Impl::addTimer(int id, double due, double interval, JSValue fn, std::vector<JSValue> args) {
  timers[id] = {due, interval, timerSeq++, fn, std::move(args)};
  earliestDue = std::min(earliestDue, due);
}

uint64_t JsEngine::Impl::startLoad(LoadRequest req, std::function<void(uint64_t, Resource&)> onDone) {
  std::weak_ptr<Impl*> weak = alive;
  auto idBox = std::make_shared<uint64_t>(0);
  uint64_t id = rt.loader.load(std::move(req), [weak, idBox, onDone = std::move(onDone)](Resource res) {
    auto strong = weak.lock();
    if (!strong) return;
    onDone(*idBox, res);
  });
  *idBox = id;
  return id;
}

void JsEngine::Impl::dropPending(uint64_t id) {
  auto it = pending.find(id);
  if (it == pending.end()) return;
  JS_FreeValue(ctx, it->second.resolve);
  JS_FreeValue(ctx, it->second.reject);
  pending.erase(it);
}

namespace {

// ------------------------------------------------------------- timers

JSValue js_setTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int repeat) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);  // string code is not supported
  double ms = 0;
  if (argc > 1 && JS_ToFloat64(ctx, &ms, argv[1])) return JS_EXCEPTION;
  if (!(ms >= 0) || !std::isfinite(ms)) ms = 0;
  ms = std::min(ms, 2147483647.0);
  auto& impl = implOf(ctx);
  int id = impl.nextTimer++;
  if (impl.nextTimer <= 0) impl.nextTimer = 1;
  std::vector<JSValue> args;
  for (int i = 2; i < argc; i++) args.push_back(JS_DupValue(ctx, argv[i]));
  impl.addTimer(id, impl.rt.now + ms, repeat ? ms : -1.0, JS_DupValue(ctx, argv[0]), std::move(args));
  return JS_NewInt32(ctx, id);
}

void freeTimer(JSContext* ctx, JsEngine::Impl::Timer& t) {
  JS_FreeValue(ctx, t.fn);
  for (auto& a : t.args) JS_FreeValue(ctx, a);
  t.args.clear();
}

JSValue js_clearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int id = 0;
  if (argc < 1 || !JS_IsNumber(argv[0]) || JS_ToInt32(ctx, &id, argv[0])) return JS_UNDEFINED;
  auto& impl = implOf(ctx);
  auto it = impl.timers.find(id);
  if (it != impl.timers.end()) {
    freeTimer(ctx, it->second);
    impl.timers.erase(it);
  }
  return JS_UNDEFINED;
}

JSValue js_requestAnimationFrame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "requestAnimationFrame: callback is not a function");
  auto& impl = implOf(ctx);
  int id = impl.nextRaf++;
  if (impl.nextRaf <= 0) impl.nextRaf = 1;
  impl.rafs.push_back({id, JS_DupValue(ctx, argv[0])});
  return JS_NewInt32(ctx, id);
}

JSValue js_cancelAnimationFrame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int id = 0;
  if (argc < 1 || JS_ToInt32(ctx, &id, argv[0]) || id <= 0) return JS_UNDEFINED;
  auto& impl = implOf(ctx);
  for (auto* list : {&impl.rafs, &impl.rafsRunning}) {
    for (auto& r : *list) {
      if (r.id == id) {
        JS_FreeValue(ctx, r.fn);
        r.fn = JS_UNDEFINED;
        r.id = 0;
      }
    }
  }
  return JS_UNDEFINED;
}

JSValue js_now(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewFloat64(ctx, runtimeOf(ctx).now); }

// ------------------------------------------------------- QuickJS hooks

int interruptHandler(JSRuntime*, void* opaque) {
  auto* impl = static_cast<JsEngine::Impl*>(opaque);
  if (impl->depth == 0 || impl->timeoutMs <= 0) return 0;
  if (Clock::now() < impl->deadline) return 0;
  impl->interrupted = true;
  return 1;
}

void rejectionTracker(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
  auto* impl = static_cast<JsEngine::Impl*>(opaque);
  if (!handled) {
    impl->rejections.push_back({JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)});
    return;
  }
  auto& list = impl->rejections;
  for (size_t i = 0; i < list.size(); i++) {
    if (JS_VALUE_GET_PTR(list[i].promise) == JS_VALUE_GET_PTR(promise)) {
      JS_FreeValue(ctx, list[i].promise);
      JS_FreeValue(ctx, list[i].reason);
      list.erase(list.begin() + long(i));
      break;
    }
  }
}

void setFn(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn, int len) {
  JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, len));
}

}  // namespace

// ------------------------------------------------------------- JsEngine

JsEngine::JsEngine(Runtime& rt) : impl_(std::make_unique<Impl>(rt)) {
  Impl& impl = *impl_;
  impl.alive = std::make_shared<Impl*>(&impl);
  const JsuiConfig& cfg = rt.config;
  impl.timeoutMs = cfg.js_timeout_ms == 0 ? 5000.0 : double(cfg.js_timeout_ms);

  impl.jrt = JS_NewRuntime();
  JS_SetRuntimeOpaque(impl.jrt, &impl);
  JS_SetMemoryLimit(impl.jrt, size_t(cfg.js_memory_limit_mb > 0 ? cfg.js_memory_limit_mb : 256) << 20);
  JS_SetMaxStackSize(impl.jrt, size_t(cfg.js_stack_size_kb > 0 ? cfg.js_stack_size_kb : 1024) << 10);
  JS_SetInterruptHandler(impl.jrt, interruptHandler, &impl);
  JS_SetHostPromiseRejectionTracker(impl.jrt, rejectionTracker, &impl);
  impl.ctx = JS_NewContext(impl.jrt);
  JSContext* ctx = impl.ctx;
  JS_SetContextOpaque(ctx, &impl);
  JS_AddIntrinsicDOMException(ctx);
  impl.atomDispatch = JS_NewAtom(ctx, "__jsui_dispatchEvent");
  impl.atomHostMessage = JS_NewAtom(ctx, "__jsui_onHostMessage");
  impl.atomOnLayout = JS_NewAtom(ctx, "__jsui_onLayout");

  JSValue global = JS_GetGlobalObject(ctx);
  setFn(ctx, global, "requestAnimationFrame", js_requestAnimationFrame, 1);
  setFn(ctx, global, "cancelAnimationFrame", js_cancelAnimationFrame, 1);
  JS_SetPropertyStr(ctx, global, "setTimeout",
                    JS_NewCFunctionMagic(ctx, js_setTimer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, global, "setInterval",
                    JS_NewCFunctionMagic(ctx, js_setTimer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
  setFn(ctx, global, "clearTimeout", js_clearTimer, 1);
  setFn(ctx, global, "clearInterval", js_clearTimer, 1);

  // performance.now() is the runtime's clock (the embedder's tick time), the
  // same timeline as rAF timestamps and timers, so headless renders are
  // deterministic.
  JSValue perf = JS_GetPropertyStr(ctx, global, "performance");
  if (!JS_IsObject(perf)) {
    JS_FreeValue(ctx, perf);
    perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "performance", JS_DupValue(ctx, perf));
  }
  JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_now, "now", 0));
  JS_SetPropertyStr(ctx, perf, "timeOrigin", JS_NewFloat64(ctx, 0));
  JS_FreeValue(ctx, perf);

  installBridge(impl, global);
  JSValue host = JS_NewObject(ctx);
  installHostApi(impl, global, host);
  impl.enter();
  runPrelude(impl, host);
  impl.leave();
  JS_FreeValue(ctx, host);
  JS_FreeValue(ctx, global);
}

JsEngine::~JsEngine() {
  Impl& impl = *impl_;
  JSContext* ctx = impl.ctx;
  impl.alive.reset();  // loader callbacks still queued become no-ops
  for (auto& [id, p] : impl.pending) {
    impl.rt.loader.cancel(id);
    JS_FreeValue(ctx, p.resolve);
    JS_FreeValue(ctx, p.reject);
  }
  impl.pending.clear();
  for (auto& [id, t] : impl.timers) freeTimer(ctx, t);
  impl.timers.clear();
  for (auto* list : {&impl.rafs, &impl.rafsRunning})
    for (auto& r : *list) JS_FreeValue(ctx, r.fn);
  impl.rafs.clear();
  impl.rafsRunning.clear();
  for (auto& r : impl.rejections) {
    JS_FreeValue(ctx, r.promise);
    JS_FreeValue(ctx, r.reason);
  }
  impl.rejections.clear();
  for (auto& [a, name] : impl.atomNames) JS_FreeAtom(ctx, a);
  impl.atomNames.clear();
  JS_FreeAtom(ctx, impl.atomDispatch);
  JS_FreeAtom(ctx, impl.atomHostMessage);
  JS_FreeAtom(ctx, impl.atomOnLayout);
  JS_FreeContext(ctx);
  JS_FreeRuntime(impl.jrt);
}

bool JsEngine::eval(const std::string& source, const std::string& name) {
  Impl& impl = *impl_;
  impl.enter();
  // JS_Eval needs a NUL-terminated buffer; std::string guarantees one.
  JSValue r = JS_Eval(impl.ctx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
  bool ok = !JS_IsException(r);
  if (!ok) impl.reportException();
  JS_FreeValue(impl.ctx, r);
  impl.leave();
  return ok;
}

void JsEngine::runJobs() {
  if (impl_->depth == 0) {
    impl_->enter();
    impl_->leave();  // drains
  }
}

void JsEngine::runTimers(double nowMs) {
  Impl& impl = *impl_;
  JSContext* ctx = impl.ctx;
  // Zero-delay timers queued by callbacks (MessageChannel, setImmediate,
  // promise-scheduled work) run in later passes of the same tick, bounded so
  // a self-rescheduling timer cannot starve the frame.
  constexpr int kMaxPasses = 8;
  for (int pass = 0; pass < kMaxPasses && impl.earliestDue <= nowMs; pass++) {
    impl.dueScratch.clear();
    double later = 1e300;
    for (auto& [id, t] : impl.timers) {
      if (t.due <= nowMs) impl.dueScratch.push_back({t.due, t.seq, id});
      else later = std::min(later, t.due);
    }
    impl.earliestDue = later;
    std::sort(impl.dueScratch.begin(), impl.dueScratch.end(), [](const Impl::DueRef& a, const Impl::DueRef& b) {
      return a.due != b.due ? a.due < b.due : a.seq < b.seq;
    });
    for (auto& d : impl.dueScratch) {
      auto it = impl.timers.find(d.id);
      if (it == impl.timers.end() || it->second.seq != d.seq) continue;  // cleared meanwhile
      Impl::Timer& t = it->second;
      JSValue fn = JS_DupValue(ctx, t.fn);
      std::vector<JSValue> args;
      if (t.interval >= 0) {
        for (auto& a : t.args) args.push_back(JS_DupValue(ctx, a));
        t.due = nowMs + std::max(1.0, t.interval);
        t.seq = impl.timerSeq++;
        impl.earliestDue = std::min(impl.earliestDue, t.due);
      } else {
        args = std::move(t.args);
        JS_FreeValue(ctx, t.fn);
        impl.timers.erase(it);
      }
      JSValue r = impl.call(fn, JS_UNDEFINED, int(args.size()), args.data());
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, fn);
      for (auto& a : args) JS_FreeValue(ctx, a);
    }
  }
}

void JsEngine::runAnimationFrame(double nowMs) {
  Impl& impl = *impl_;
  if (impl.rafs.empty()) return;
  JSContext* ctx = impl.ctx;
  // Callbacks registered while running go to the next frame.
  impl.rafsRunning.swap(impl.rafs);
  JSValue ts = JS_NewFloat64(ctx, nowMs);
  for (size_t i = 0; i < impl.rafsRunning.size(); i++) {
    if (impl.rafsRunning[i].id == 0) continue;
    JSValue fn = impl.rafsRunning[i].fn;
    impl.rafsRunning[i].fn = JS_UNDEFINED;
    impl.rafsRunning[i].id = 0;
    JSValue r = impl.call(fn, JS_UNDEFINED, 1, &ts);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
  }
  impl.rafsRunning.clear();
}

bool JsEngine::hasPendingWork(double nowMs) const {
  return impl_->earliestDue <= nowMs || !impl_->rafs.empty() || JS_IsJobPending(impl_->jrt);
}

int JsEngine::dispatchEvent(const Value& event) {
  Impl& impl = *impl_;
  JSContext* ctx = impl.ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetProperty(ctx, global, impl.atomDispatch);
  int flags = 0;
  if (JS_IsFunction(ctx, fn)) {
    JSValue arg = fromValue(ctx, event);
    JSValue r = impl.call(fn, JS_UNDEFINED, 1, &arg);
    if (JS_IsBool(r)) flags = JS_ToBool(ctx, r) ? (kHandled | kDefaultPrevented) : 0;
    else if (JS_IsNumber(r)) JS_ToInt32(ctx, &flags, r);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
  }
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
  return flags;
}

void JsEngine::hostMessage(const std::string& name, const std::string& json) {
  Impl& impl = *impl_;
  JSContext* ctx = impl.ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetProperty(ctx, global, impl.atomHostMessage);
  if (JS_IsFunction(ctx, fn)) {
    JSValue args[2] = {JS_NewStringLen(ctx, name.data(), name.size()), JS_NewStringLen(ctx, json.data(), json.size())};
    JSValue r = impl.call(fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
  }
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
}

void JsEngine::layoutEvent(uint32_t id, float x, float y, float w, float h) {
  Impl& impl = *impl_;
  impl.layoutQueue.push_back({id, x, y, w, h});
  // Outside JS, deliver right away; otherwise drain() delivers once JS unwinds.
  if (impl.depth == 0) {
    impl.enter();
    impl.leave();
  }
}

bool JsEngine::inCall() const { return impl_->depth > 0; }

size_t JsEngine::memoryUsage() const {
  JSMemoryUsage u;
  JS_ComputeMemoryUsage(impl_->jrt, &u);
  return size_t(u.malloc_size);
}

}  // namespace jsui
