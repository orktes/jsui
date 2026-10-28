// Worklet runtimes (js/worklets.h): one QuickJS runtime + context each,
// ticked on the owner thread. The JS half (wire format, timers,
// requestAnimationFrame, the react-native-worklets functions a worklet
// calls) is worklets_prelude.cpp.
#include "js/worklets.h"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "animated/native_animated.h"
#include "js/engine_impl.h"
#include "js/inspector.h"
#include "js/js_thread.h"
#include "runtime.h"

namespace jsui {

const char* workletsPrelude();  // worklets_prelude.cpp

using Clock = std::chrono::steady_clock;

class WorkletRuntime {
 public:
  WorkletRuntime(Worklets& hub, int id, std::string name);
  ~WorkletRuntime();
  bool ok() const { return JS_IsObject(api_); }
  // `batch`: wire JSON of `[[worklet, args, promiseId], ...]`.
  void runJobs(const std::string& batch);
  std::string runSync(const std::string& job);
  void tick(double nowMs);
  bool wantsFrame() const { return wantFrame_; }
  double nextTimer() const { return nextDue_; }
  size_t memory() const { return JS_GetMallocSize(jrt_); }
  // Once it goes idle after running something: a cycle collection, so
  // closures that captured remote functions (a requestAnimationFrame loop
  // refers to itself) are freed and the bundle gets them back.
  void collectIfIdle(double nowMs);

  Worklets& hub;
  const int id;
  const std::string name;
  bool wantFrame_ = false;
  double nextDue_ = 1e300;

 private:
  // Calls the prelude's `api[fn](arg...)` as one entry: watchdog armed,
  // exceptions reported, microtasks drained after.
  JSValue enter(const char* fn, int argc, JSValueConst* argv);
  void drain();
  void report(JSValueConst exc, const char* prefix);
  static int interrupt(JSRuntime*, void* opaque);
  static void rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void* opaque);

  JSRuntime* jrt_ = nullptr;
  JSContext* ctx_ = nullptr;
  JSValue api_ = JS_UNDEFINED;  // {jobs, runSync, tick} from the prelude
  int depth_ = 0;
  bool ran_ = false;  // entered since the last collection
  double timeoutMs_ = 5000;
  Clock::time_point deadline_;
  struct Rejection {
    JSValue promise, reason;
  };
  std::vector<Rejection> rejections_;

  friend struct UiHost;
};

namespace {

WorkletRuntime& self(JSContext* ctx) { return *static_cast<WorkletRuntime*>(JS_GetContextOpaque(ctx)); }
Runtime& rtOf(JSContext* ctx) { return self(ctx).hub.runtime(); }

double num(JSContext* ctx, int argc, JSValueConst* argv, int i, double def = 0) {
  double v = def;
  if (i < argc) JS_ToFloat64(ctx, &v, argv[i]);
  return v;
}
int intArg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  int32_t v = 0;
  if (i < argc) JS_ToInt32(ctx, &v, argv[i]);
  return v;
}
std::string str(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  return i < argc ? toStdString(ctx, argv[i]) : std::string();
}

// ---- the host object the prelude gets (`host`) --------------------------

JSValue h_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int level = std::clamp(intArg(ctx, argc, argv, 0), 0, 3);
  rtOf(ctx).log(JsuiLogLevel(level), str(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

// reportError(error | text, prefix)
JSValue h_reportError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string prefix = str(ctx, argc, argv, 1);
  std::string text = argc > 0 ? (JS_IsString(argv[0]) ? toStdString(ctx, argv[0]) : formatError(ctx, argv[0])) : "";
  rtOf(ctx).reportError(prefix + text);
  return JS_UNDEFINED;
}

JSValue h_now(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewFloat64(ctx, rtOf(ctx).now); }

// evalCode(source, name) -> the value of the expression (a worklet's code).
JSValue h_evalCode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string src = str(ctx, argc, argv, 0), name = str(ctx, argc, argv, 1);
  if (name.empty()) name = "worklet";
  return JS_Eval(ctx, src.c_str(), src.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
}

JSValue h_toJS(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.toJs(str(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

// post(runtimeId, job): a job for another worklet runtime (or this one),
// run in the next worklet phase.
JSValue h_post(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.post(intArg(ctx, argc, argv, 0), str(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

JSValue h_wantFrame(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  self(ctx).wantFrame_ = true;
  return JS_UNDEFINED;
}

// timer(due): a timer was set (the tick enters JS again by then).
JSValue h_timer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  WorkletRuntime& r = self(ctx);
  r.nextDue_ = std::min(r.nextDue_, num(ctx, argc, argv, 0, 1e300));
  return JS_UNDEFINED;
}

// Native Animated values (the owner thread's graph; values JS made native).
JSValue h_animSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  NativeAnimated& a = *rtOf(ctx).animated;
  int tag = intArg(ctx, argc, argv, 0);
  double v = num(ctx, argc, argv, 1);
  if (v == v && a.hasNode(tag)) a.setValue(tag, v);
  return JS_UNDEFINED;
}
JSValue h_animOffset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  NativeAnimated& a = *rtOf(ctx).animated;
  int tag = intArg(ctx, argc, argv, 0);
  double v = num(ctx, argc, argv, 1);
  if (v == v && a.hasNode(tag)) a.setOffset(tag, v);
  return JS_UNDEFINED;
}
// The value with its offset (Animated.Value#__getValue).
JSValue h_animGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  NativeAnimated& a = *rtOf(ctx).animated;
  int tag = intArg(ctx, argc, argv, 0);
  return JS_NewFloat64(ctx, a.hasNode(tag) ? a.nodeOutput(tag) : 0);
}

JSValue h_syncCreate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return JS_NewInt32(ctx, self(ctx).hub.syncCreate(str(ctx, argc, argv, 0)));
}
JSValue h_syncGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string s = self(ctx).hub.syncGet(intArg(ctx, argc, argv, 0), argc > 1 && JS_ToBool(ctx, argv[1]));
  return JS_NewStringLen(ctx, s.data(), s.size());
}
JSValue h_syncSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.syncSet(intArg(ctx, argc, argv, 0), str(ctx, argc, argv, 1), argc > 2 && JS_ToBool(ctx, argv[2]));
  return JS_UNDEFINED;
}
JSValue h_syncLock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return JS_NewBool(ctx, self(ctx).hub.syncLock(intArg(ctx, argc, argv, 0)));
}
JSValue h_syncUnlock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.syncUnlock(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}
JSValue h_syncRetain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.syncRetain(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}
JSValue h_syncRelease(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  self(ctx).hub.syncRelease(intArg(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

}  // namespace

// ------------------------------------------------------------ WorkletRuntime

int WorkletRuntime::interrupt(JSRuntime*, void* opaque) {
  auto* r = static_cast<WorkletRuntime*>(opaque);
  if (r->depth_ == 0 || r->timeoutMs_ <= 0) return 0;
  return Clock::now() >= r->deadline_ ? 1 : 0;
}

void WorkletRuntime::rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
  auto* r = static_cast<WorkletRuntime*>(opaque);
  if (!handled) {
    r->rejections_.push_back({JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)});
    return;
  }
  for (size_t i = 0; i < r->rejections_.size(); i++) {
    if (JS_VALUE_GET_PTR(r->rejections_[i].promise) == JS_VALUE_GET_PTR(promise)) {
      JS_FreeValue(ctx, r->rejections_[i].promise);
      JS_FreeValue(ctx, r->rejections_[i].reason);
      r->rejections_.erase(r->rejections_.begin() + long(i));
      return;
    }
  }
}

WorkletRuntime::WorkletRuntime(Worklets& h, int rid, std::string nm) : hub(h), id(rid), name(std::move(nm)) {
  const JsuiConfig& cfg = hub.runtime().config;
  timeoutMs_ = cfg.js_timeout_ms == 0 ? 5000.0 : double(cfg.js_timeout_ms);
  jrt_ = JS_NewRuntime();
  JS_SetMemoryLimit(jrt_, size_t(cfg.js_memory_limit_mb > 0 ? cfg.js_memory_limit_mb : 256) << 20);
  JS_SetMaxStackSize(jrt_, size_t(cfg.js_stack_size_kb > 0 ? cfg.js_stack_size_kb : 512) << 10);
  JS_SetInterruptHandler(jrt_, interrupt, this);
  JS_SetHostPromiseRejectionTracker(jrt_, rejection, this);
  ctx_ = JS_NewContext(jrt_);
  JS_SetContextOpaque(ctx_, this);
#if JSUI_DEBUGGER
  // A debugger's CPU profile samples worklets too: the statement traps go
  // into code parsed from here on (armed only when a sample is due).
  if (Inspector* ins = hub.runtime().inspector.get()) ins->addWorklet(ctx_, name);
#endif

  JSValue global = JS_GetGlobalObject(ctx_);
  // performance.now() is the runtime clock (the tick's time), as in the
  // bundle; QuickJS-ng's own `now` is read-only, so the object is replaced.
  JSValue perf = JS_NewObject(ctx_);
  JS_SetPropertyStr(ctx_, perf, "now", JS_NewCFunction(ctx_, h_now, "now", 0));
  JS_SetPropertyStr(ctx_, perf, "timeOrigin", JS_NewFloat64(ctx_, 0));
  JS_DefinePropertyValueStr(ctx_, global, "performance", perf,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);

  JSValue host = JS_NewObject(ctx_);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"log", h_log, 2},           {"reportError", h_reportError, 2}, {"now", h_now, 0},
             {"evalCode", h_evalCode, 2}, {"toJS", h_toJS, 1},               {"post", h_post, 2},
             {"wantFrame", h_wantFrame, 0}, {"timer", h_timer, 1}, {"animSet", h_animSet, 2},     {"animOffset", h_animOffset, 2},
             {"animGet", h_animGet, 1},   {"syncCreate", h_syncCreate, 1},   {"syncGet", h_syncGet, 2},
             {"syncSet", h_syncSet, 3},   {"syncLock", h_syncLock, 1},       {"syncUnlock", h_syncUnlock, 1},
             {"syncRetain", h_syncRetain, 1}, {"syncRelease", h_syncRelease, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx_, host, f.name, JS_NewCFunction(ctx_, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx_, host, "devMode", JS_NewBool(ctx_, cfg.dev_mode != 0));
  JS_SetPropertyStr(ctx_, host, "kind", JS_NewInt32(ctx_, id == Worklets::kUiRuntime ? 2 : 3));
  JS_SetPropertyStr(ctx_, host, "runtimeId", JS_NewInt32(ctx_, id));
  JS_SetPropertyStr(ctx_, host, "name", JS_NewStringLen(ctx_, name.data(), name.size()));

  // console (the bundle's prelude piece: the same formatting), then the
  // worklet runtime's own prelude, which returns its entry points.
  size_t n = 0;
  const PreludePiece* pieces = preludePieces(&n);
  depth_++;
  deadline_ = Clock::now() + std::chrono::milliseconds(int64_t(timeoutMs_ > 0 ? timeoutMs_ : 0));
  for (const char* src : {pieces[0].source, workletsPrelude()}) {
    const char* file = src == pieces[0].source ? pieces[0].name : "jsui:worklets/prelude.js";
    JSValue fn = JS_Eval(ctx_, src, std::strlen(src), file, JS_EVAL_TYPE_GLOBAL);
    JSValue r = JS_IsException(fn) ? JS_EXCEPTION : JS_Call(ctx_, fn, JS_UNDEFINED, 1, &host);
    if (JS_IsException(r)) {
      JSValue exc = JS_GetException(ctx_);
      report(exc, "[Worklets] runtime setup failed: ");
      JS_FreeValue(ctx_, exc);
    } else if (src != pieces[0].source) {
      api_ = r;
      r = JS_UNDEFINED;
    }
    JS_FreeValue(ctx_, r);
    JS_FreeValue(ctx_, fn);
  }
  depth_--;
  drain();
  JS_FreeValue(ctx_, host);
  JS_FreeValue(ctx_, global);
}

WorkletRuntime::~WorkletRuntime() {
  for (auto& r : rejections_) {
    JS_FreeValue(ctx_, r.promise);
    JS_FreeValue(ctx_, r.reason);
  }
  rejections_.clear();
  JS_FreeValue(ctx_, api_);
#if JSUI_DEBUGGER
  if (Inspector* ins = hub.runtime().inspector.get()) ins->removeWorklet(ctx_);
#endif
  JS_FreeContext(ctx_);
  JS_FreeRuntime(jrt_);
}

void WorkletRuntime::report(JSValueConst exc, const char* prefix) {
  hub.runtime().reportError(std::string(prefix) + formatError(ctx_, exc));
}

void WorkletRuntime::drain() {
  for (int round = 0; round < 64; round++) {
    JSContext* c = nullptr;
    int r;
    bool any = false;
    while ((r = JS_ExecutePendingJob(jrt_, &c)) != 0) {
      any = true;
      if (r < 0) {
        JSValue exc = JS_GetException(c ? c : ctx_);
        report(exc, "[Worklets] Uncaught (in a microtask) ");
        JS_FreeValue(ctx_, exc);
      }
    }
    if (!any) break;
  }
  auto list = std::move(rejections_);
  rejections_.clear();
  for (auto& r : list) {
    report(r.reason, "[Worklets] Unhandled promise rejection: ");
    JS_FreeValue(ctx_, r.promise);
    JS_FreeValue(ctx_, r.reason);
  }
}

JSValue WorkletRuntime::enter(const char* fn, int argc, JSValueConst* argv) {
  if (!ok()) return JS_UNDEFINED;
  ran_ = true;
  JSValue f = JS_GetPropertyStr(ctx_, api_, fn);
  if (depth_++ == 0) deadline_ = Clock::now() + std::chrono::milliseconds(int64_t(timeoutMs_ > 0 ? timeoutMs_ : 0));
  JSValue r = JS_Call(ctx_, f, JS_UNDEFINED, argc, argv);
  depth_--;
  JS_FreeValue(ctx_, f);
  if (JS_IsException(r)) {
    JSValue exc = JS_GetException(ctx_);
    bool timedOut = JS_IsUncatchableError(exc);
    report(exc, timedOut ? "[Worklets] Script timed out: " : "[Worklets] ");
    JS_FreeValue(ctx_, exc);
    r = JS_UNDEFINED;
  }
  if (depth_ == 0) drain();
  return r;
}

void WorkletRuntime::runJobs(const std::string& batch) {
  JSValue arg = JS_NewStringLen(ctx_, batch.data(), batch.size());
  JS_FreeValue(ctx_, enter("jobs", 1, &arg));
  JS_FreeValue(ctx_, arg);
}

std::string WorkletRuntime::runSync(const std::string& job) {
  JSValue arg = JS_NewStringLen(ctx_, job.data(), job.size());
  JSValue r = enter("runSync", 1, &arg);
  JS_FreeValue(ctx_, arg);
  std::string out = JS_IsString(r) ? toStdString(ctx_, r) : std::string(R"({"ok":false,"e":"worklet runtime failed"})");
  JS_FreeValue(ctx_, r);
  return out;
}

void WorkletRuntime::tick(double nowMs) {
  // Nothing due and no frame callback: no JS entry at all.
  if (!wantFrame_ && nextDue_ > nowMs) return;
  bool frame = wantFrame_;
  wantFrame_ = false;  // requestAnimationFrame from the callbacks sets it again
  JSValue args[2] = {JS_NewFloat64(ctx_, nowMs), JS_NewBool(ctx_, frame)};
  JSValue r = enter("tick", 2, args);
  double due = 1e300;
  if (JS_IsNumber(r)) JS_ToFloat64(ctx_, &due, r);
  nextDue_ = due == due ? due : 1e300;
  JS_FreeValue(ctx_, r);
}

void WorkletRuntime::collectIfIdle(double nowMs) {
  if (!ran_ || wantFrame_ || nextDue_ <= nowMs + 100 || !ok()) return;
  ran_ = false;
  JS_RunGC(jrt_);
  drain();  // FinalizationRegistry callbacks (remote function releases)
  ran_ = false;
}

// ------------------------------------------------------------ Worklets

struct Worklets::Sync {
  std::recursive_timed_mutex user;  // lock() / unlock(), getBlocking / setBlocking
  std::mutex m;                     // json, owner, depth
  std::string json;
  std::thread::id owner;
  int depth = 0;
  std::atomic<int> refs{1};
};

Worklets::Worklets(Runtime& rt) : rt_(rt) {}
Worklets::~Worklets() = default;

WorkletRuntime* Worklets::get(int id, bool create) {
  auto it = runtimes_.find(id);
  if (it != runtimes_.end()) return it->second.get();
  if (!create || id != kUiRuntime) return nullptr;
  auto r = std::make_unique<WorkletRuntime>(*this, id, "UI");
  WorkletRuntime* p = r.get();
  runtimes_.emplace(id, std::move(r));
  return p;
}

void Worklets::create(int id, std::string name) {
  if (runtimes_.count(id)) return;
  runtimes_.emplace(id, std::make_unique<WorkletRuntime>(*this, id, std::move(name)));
}

void Worklets::post(int id, std::string json) { jobs_.emplace_back(id, std::move(json)); }

std::string Worklets::runSync(int id, const std::string& json) {
  WorkletRuntime* r = get(id, true);
  if (!r) return R"({"ok":false,"e":"[Worklets] unknown worklet runtime"})";
  // Jobs scheduled before it run first (program order).
  std::vector<std::pair<int, std::string>> keep;
  std::string batch;
  for (auto& [jid, job] : jobs_) {
    if (jid != id) {
      keep.emplace_back(jid, std::move(job));
      continue;
    }
    batch += batch.empty() ? "[" : ",";
    batch += job;
  }
  jobs_ = std::move(keep);
  if (!batch.empty()) r->runJobs(batch + "]");
  return r->runSync(json);
}

void Worklets::tick(double nowMs, bool js) {
  Perf::Scope scope(rt_.doc.perf, Phase::Worklet);
#if JSUI_DEBUGGER
  struct Marks {  // a CPU profile's worklet track: running from here, idle after
    Inspector* ins;
    explicit Marks(Inspector* i) : ins(i) {
      if (ins) ins->workletsRunning(true);
    }
    ~Marks() {
      if (ins) ins->workletsRunning(false);
    }
  } marks(rt_.inspector.get());
#endif
  // Queued jobs, consecutive ones for the same runtime as one batch (one
  // entry, one microtask drain). Jobs they post wait for the next tick.
  if (!jobs_.empty()) {
    auto jobs = std::move(jobs_);
    jobs_.clear();
    for (size_t i = 0; i < jobs.size();) {
      int id = jobs[i].first;
      std::string batch = "[";
      size_t j = i;
      for (; j < jobs.size() && jobs[j].first == id; j++) {
        if (j > i) batch += ",";
        batch += jobs[j].second;
      }
      batch += "]";
      if (WorkletRuntime* r = get(id, true)) r->runJobs(batch);
      else rt_.reportError("[Worklets] no worklet runtime with id " + std::to_string(id));
      i = j;
    }
  }
  // Timers, then requestAnimationFrame callbacks (with the frame time).
  // (A runtime created by a callback below waits for the next tick.)
  std::vector<WorkletRuntime*> list;
  for (auto& [id, r] : runtimes_) list.push_back(r.get());
  for (WorkletRuntime* r : list) r->tick(nowMs);
  for (WorkletRuntime* r : list) r->collectIfIdle(nowMs);
  // What they sent the bundle, as one call (in order).
  if (js && !toJs_.empty() && rt_.js) {
    std::string json = "[";
    for (size_t i = 0; i < toJs_.size(); i++) {
      if (i) json += ",";
      json += toJs_[i];
    }
    json += "]";
    toJs_.clear();
    rt_.inbound.call("__jsui_onWorklets", {Value(std::move(json))});
  }
}

bool Worklets::wantsFrame() const {
  if (!jobs_.empty() || !toJs_.empty()) return true;
  for (auto& [id, r] : runtimes_)
    if (r->wantsFrame()) return true;
  return false;
}

double Worklets::nextTimer() const {
  double due = 1e300;
  for (auto& [id, r] : runtimes_) due = std::min(due, r->nextTimer());
  return due;
}

size_t Worklets::memoryUsage() const {
  size_t n = 0;
  for (auto& [id, r] : runtimes_) n += r->memory();
  return n;
}

void Worklets::reset() {
  runtimes_.clear();
  jobs_.clear();
  toJs_.clear();
  std::lock_guard<std::mutex> g(syncMutex_);
  syncs_.clear();
}

// ---- synchronizables

std::shared_ptr<Worklets::Sync> Worklets::sync(int id) {
  std::lock_guard<std::mutex> g(syncMutex_);
  auto it = syncs_.find(id);
  return it == syncs_.end() ? nullptr : it->second;
}

int Worklets::syncCreate(std::string json) {
  auto s = std::make_shared<Sync>();
  s->json = std::move(json);
  int id = nextSync_.fetch_add(1);
  std::lock_guard<std::mutex> g(syncMutex_);
  syncs_.emplace(id, std::move(s));
  return id;
}

std::string Worklets::syncGet(int id, bool blocking) {
  auto s = sync(id);
  if (!s) return R"({"$":"u"})";
  bool locked = blocking && syncLock(id);
  std::string out;
  {
    std::lock_guard<std::mutex> g(s->m);
    out = s->json;
  }
  if (locked) syncUnlock(id);
  return out;
}

void Worklets::syncSet(int id, std::string json, bool blocking) {
  auto s = sync(id);
  if (!s) return;
  bool locked = blocking && syncLock(id);
  {
    std::lock_guard<std::mutex> g(s->m);
    s->json = std::move(json);
  }
  if (locked) syncUnlock(id);
}

bool Worklets::syncLock(int id) {
  auto s = sync(id);
  if (!s) return false;
  if (!s->user.try_lock_for(std::chrono::milliseconds(kLockTimeoutMs))) {
    rt_.jsLog(JSUI_LOG_WARN, "[Worklets] a Synchronizable stayed locked for " + std::to_string(kLockTimeoutMs) +
                                 " ms (lock() without unlock()?)");
    return false;
  }
  std::lock_guard<std::mutex> g(s->m);
  s->owner = std::this_thread::get_id();
  s->depth++;
  return true;
}

void Worklets::syncUnlock(int id) {
  auto s = sync(id);
  if (!s) return;
  {
    std::lock_guard<std::mutex> g(s->m);
    // Only the thread holding it unlocks (an unlock() without lock() is a
    // no-op, not undefined behaviour).
    if (s->depth == 0 || s->owner != std::this_thread::get_id()) return;
    if (--s->depth == 0) s->owner = std::thread::id();
  }
  s->user.unlock();
}

void Worklets::syncRetain(int id) {
  if (auto s = sync(id)) s->refs++;
}

void Worklets::syncRelease(int id) {
  std::lock_guard<std::mutex> g(syncMutex_);
  auto it = syncs_.find(id);
  if (it != syncs_.end() && --it->second->refs <= 0) syncs_.erase(it);
}

}  // namespace jsui
