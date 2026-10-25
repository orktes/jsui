// The JS thread (js/js_thread.h).
#include "js/js_thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

#include "runtime.h"

#if defined(__EMSCRIPTEN__)
#define JSUI_NO_JS_THREAD 1
#elif defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace jsui {

namespace {

thread_local JsThread* tlCurrent = nullptr;

// Timer passes the thread runs on its own (between frames, at the last
// frame's time) before it waits for the next frame: React's scheduler
// continues a render across a few 5 ms slices per frame; a setTimeout(0)
// loop does not spin until then.
constexpr int kOwnPassesPerFrame = 64;

// The native stack below the JS limit (JS_SetMaxStackSize) for the C++
// frames around the engine.
constexpr size_t kStackHeadroom = 256 << 10;

double wallMs() { return Runtime::wallMs(); }

}  // namespace

// ---------------------------------------------------------- the OS thread

#if defined(JSUI_NO_JS_THREAD)
struct JsThread::Native {
  bool start(JsThread*, size_t) { return false; }
  void join() {}
};
bool JsThread::available() { return false; }
#elif defined(_WIN32)
struct JsThread::Native {
  HANDLE handle = nullptr;
  static unsigned __stdcall entry(void* p) {
    static_cast<JsThread*>(p)->main();
    return 0;
  }
  bool start(JsThread* t, size_t stack) {
    handle = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, unsigned(stack), entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    return handle != nullptr;
  }
  void join() {
    if (!handle) return;
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
    handle = nullptr;
  }
};
bool JsThread::available() { return true; }
#else
struct JsThread::Native {
  pthread_t thread{};
  bool started = false;
  static void* entry(void* p) {
    static_cast<JsThread*>(p)->main();
    return nullptr;
  }
  bool start(JsThread* t, size_t stack) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    started = pthread_create(&thread, &attr, entry, t) == 0;
    pthread_attr_destroy(&attr);
    return started;
  }
  void join() {
    if (started) pthread_join(thread, nullptr);
    started = false;
  }
};
bool JsThread::available() { return true; }
#endif

// ------------------------------------------------------------- lifetime

JsThread::JsThread(Runtime& rt) : rt_(rt), native_(std::make_unique<Native>()) {
  int kb = rt.config.js_stack_size_kb > 0 ? rt.config.js_stack_size_kb : 1024;
  if (!native_->start(this, (size_t(kb) << 10) + kStackHeadroom)) {
    // No thread (cannot happen where available()): the engine is never
    // made and every message is dropped.
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = exited_ = idle_ = true;
    return;
  }
  // The engine is created on the thread (its prelude runs there); nothing
  // of the runtime moves meanwhile.
  std::unique_lock<std::mutex> lock(mutex_);
  idleCv_.wait(lock, [this] { return ready_; });
}

JsThread::~JsThread() { stop(); }

JsThread* JsThread::current() { return tlCurrent; }

void JsThread::main() {
  tlCurrent = this;
  engine_ = JsEngine::create(rt_, Kind::QuickJs);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
  }
  idleCv_.notify_all();
  afterWork();  // what the prelude recorded
  loop();
  // What the context still has pending, for Runtime::endCalls.
  std::vector<uint64_t> calls = engine_->pendingCalls();
  engine_.reset();
  flushHost();  // host callbacks of the teardown (logs)
  {
    std::lock_guard<std::mutex> lock(callsMutex_);
    endedCalls_ = std::move(calls);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exited_ = idle_ = true;
  }
  idleCv_.notify_all();
  tlCurrent = nullptr;
}

void JsThread::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!native_) return;
    stop_.store(true);  // the watchdog interrupts running JS (interruptHandler)
  }
  cv_.notify_all();
  // A JS thread waiting in a proxied read gets it served until it is out.
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (exited_) break;
    idleCv_.wait_for(lock, std::chrono::milliseconds(1));
    lock.unlock();
    rt_.hostQueue.drain();
  }
  native_->join();
  std::lock_guard<std::mutex> lock(mutex_);
  native_.reset();
  queue_.clear();
}

// ------------------------------------------------------------ owner side

void JsThread::post(Inbound::Message&& m) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exited_ || stop_.load()) return;
    queue_.push_back(std::move(m));
    idle_ = false;
  }
  cv_.notify_one();
}

bool JsThread::takeBatch(JsBatch* out) {
  std::lock_guard<std::mutex> lock(batchMutex_);
  if (batches_.empty()) return false;
  *out = std::move(batches_.front());
  batches_.pop_front();
  published_.store(batches_.size(), std::memory_order_release);
  return true;
}

void JsThread::recycle(JsBatch&& b) {
  b.commands.clear();
  std::lock_guard<std::mutex> lock(batchMutex_);
  if (pool_.size() < 4) pool_.push_back(std::move(b.commands));
}

bool JsThread::idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return idle_ && queue_.empty();
}

bool JsThread::waitIdle(double timeoutMs, const std::function<void()>& serve) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::microseconds(int64_t(std::max(0.0, timeoutMs) * 1000));
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (idle_ && queue_.empty()) return true;
      auto now = std::chrono::steady_clock::now();
      if (timeoutMs >= 0 && now >= deadline) return false;
      // Short waits: proxied reads (HostQueue::call) do not notify here.
      auto until = now + std::chrono::microseconds(500);
      if (timeoutMs >= 0) until = std::min(until, deadline);
      idleCv_.wait_until(lock, until);
      if (idle_ && queue_.empty()) return true;
    }
    if (serve) serve();
  }
}

double JsThread::busyUs(bool reset) {
  return double(reset ? busyUs_.exchange(0) : busyUs_.load());
}

bool JsThread::takeCall(uint64_t id) {
  std::lock_guard<std::mutex> lock(callsMutex_);
  return calls_.erase(id) > 0;
}

bool JsThread::hasCall(uint64_t id) const {
  std::lock_guard<std::mutex> lock(callsMutex_);
  return calls_.count(id) > 0;
}

std::vector<uint64_t> JsThread::pendingCalls() const {
  std::lock_guard<std::mutex> lock(callsMutex_);
  return endedCalls_;
}

bool JsThread::hasPendingWork(double nowMs) const {
  if (hasBatches() || !idle()) return true;
  return nextDue_.load(std::memory_order_relaxed) <= nowMs;
}

void JsThread::mergePerf(PerfCounters& into) {
  std::lock_guard<std::mutex> lock(statsMutex_);
  into += counters_;
  counters_ = PerfCounters();
}

JsEngine::GcStats JsThread::gcStats(bool reset) {
  std::lock_guard<std::mutex> lock(statsMutex_);
  GcStats s = gc_;
  if (reset) gc_ = GcStats();
  return s;
}

// --------------------------------------------------------------- JS side

bool JsThread::publish(CommandBuffer& commands) {
  if (commands.empty() && replies_.empty() && host_.empty()) return false;
  JsBatch b;
  {
    std::lock_guard<std::mutex> lock(batchMutex_);
    if (!pool_.empty()) {
      b.commands = std::move(pool_.back());
      pool_.pop_back();
    }
  }
  b.commands.swapRecorded(commands);
  b.replies.swap(replies_);
  b.host.swap(host_);
  {
    std::lock_guard<std::mutex> lock(batchMutex_);
    batches_.push_back(std::move(b));
    published_.store(batches_.size(), std::memory_order_release);
  }
  // A waiting sync applies as soon as JS is idle; a sleeping host may want
  // to tick now.
  if (rt_.config.on_js_ready) rt_.config.on_js_ready(rt_.config.js_ready_user);
  return true;
}

void JsThread::flushHost() {
  if (replies_.empty() && host_.empty()) return;
  JsBatch b;
  b.counted = false;
  b.replies.swap(replies_);
  b.host.swap(host_);
  std::lock_guard<std::mutex> lock(batchMutex_);
  batches_.push_back(std::move(b));
  published_.store(batches_.size(), std::memory_order_release);
}

void JsThread::callStarted(uint64_t id) {
  std::lock_guard<std::mutex> lock(callsMutex_);
  calls_.insert(id);
}

void JsThread::callEnded(uint64_t id) {
  std::lock_guard<std::mutex> lock(callsMutex_);
  calls_.erase(id);
}

bool JsThread::shouldYield() const {
  if (syncing_.load(std::memory_order_relaxed)) return false;
  double slice = rt_.jsSliceMs;
  return slice > 0 && wallMs() - taskStart_ >= slice;
}

bool JsThread::ownWorkDue() const {
  if (lockstep() || !engine_) return false;
  double now = rt_.publishedNow.load(std::memory_order_acquire);
  if (engine_->nextTimer() > now) return false;
  return now != ownFrame_ || ownPasses_ < kOwnPassesPerFrame;
}

void JsThread::loop() {
  std::deque<Inbound::Message> work;
  for (;;) {
    bool own = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        if (stop_.load()) return;
        if (!queue_.empty()) break;
        if ((own = ownWorkDue())) break;
        idle_ = true;
        idleCv_.notify_all();
        if (engine_->debuggerAttached()) {
          // DevTools requests that arrived while JS waited (a timed wait:
          // the debugger's own threads do not wake this one).
          lock.unlock();
          engine_->pollDebugger();
          lock.lock();
          if (queue_.empty() && !stop_.load()) cv_.wait_for(lock, std::chrono::milliseconds(50));
        } else {
          cv_.wait(lock);
        }
      }
      idle_ = false;
      work.swap(queue_);
    }
    double t0 = wallMs();
    busy_.store(true, std::memory_order_release);
    if (!work.empty()) deliver(work);
    else if (own) runOwnWork();
    busy_.store(false, std::memory_order_release);
    busyUs_.fetch_add(uint64_t((wallMs() - t0) * 1000));
    afterWork();
    // Garbage is collected while nothing waits: never in the middle of a
    // task, and the owner thread never pauses for it.
    bool quiet;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quiet = queue_.empty();
    }
    if (quiet && !ownWorkDue() && engine_->collectIdle(true, true)) afterWork();
  }
}

void JsThread::deliver(std::deque<Inbound::Message>& work) {
  using Kind = Inbound::Message::Kind;
  for (size_t i = 0; i < work.size() && !stop_.load(); i++) {
    Inbound::Message& m = work[i];
    if (m.kind == Kind::Frame) {
      // A JS thread that fell behind runs the latest frame only (timers and
      // rAF at its time).
      bool later = false;
      for (size_t j = i + 1; j < work.size() && !later; j++) later = work[j].kind == Kind::Frame;
      if (later) continue;
      now_ = m.t;
      ownFrame_ = -1;
    } else {
      now_ = m.now;
    }
    taskStart_ = wallMs();
    // One batch per message: its reply goes out with its commands.
    engine_->beginBatch();
    rt_.inbound.deliver(m, *engine_);
    engine_->endBatch();
  }
  work.clear();
}

void JsThread::runOwnWork() {
  double now = rt_.publishedNow.load(std::memory_order_acquire);
  if (now != ownFrame_) {
    ownFrame_ = now;
    ownPasses_ = 0;
  }
  ownPasses_++;
  now_ = std::max(now_, now);
  taskStart_ = wallMs();
  engine_->beginBatch();
  engine_->runTimers(now_);
  engine_->endBatch();
}

void JsThread::afterWork() {
  if (!engine_) return;
  // Host callbacks made outside a task (none expected) still go out.
  flushHost();
  nextDue_.store(engine_->wantsFrame() ? -std::numeric_limits<double>::infinity() : engine_->nextTimer(),
                 std::memory_order_relaxed);
  memory_.store(engine_->memoryUsage(), std::memory_order_relaxed);
  GcStats g = engine_->gcStats(true);
  std::lock_guard<std::mutex> lock(statsMutex_);
  engine_->mergePerf(counters_);
  gc_.collections += g.collections;
  gc_.full += g.full;
  gc_.us += g.us;
  gc_.maxUs = std::max(gc_.maxUs, g.maxUs);
}

// ---- engine calls: on the JS thread (Inbound::deliver, posted tasks)

bool JsThread::eval(const std::string& source, const std::string& name) { return engine_->eval(source, name); }
void JsThread::runTimers(double nowMs) { engine_->runTimers(nowMs); }
void JsThread::runAnimationFrame(double nowMs) { engine_->runAnimationFrame(nowMs); }
void JsThread::runJobs() { engine_->runJobs(); }
int JsThread::dispatchEvent(const Value& event) { return engine_->dispatchEvent(event); }
void JsThread::hostMessage(const std::string& name, const std::string& json) { engine_->hostMessage(name, json); }
void JsThread::layoutEvent(uint32_t id, float x, float y, float w, float h) { engine_->layoutEvent(id, x, y, w, h); }
bool JsThread::callGlobal(const char* name, const Value* args, int argc) {
  return engine_->callGlobal(name, args, argc);
}
bool JsThread::settleCall(uint64_t id, bool ok, const std::string& payload) {
  return engine_->settleCall(id, ok, payload);
}
bool JsThread::dropCall(uint64_t id) { return engine_->dropCall(id); }
void JsThread::measured(const std::vector<MeasureResult>& results) {
  if (engine_) engine_->measured(results);
}
void JsThread::beginBatch() { engine_->beginBatch(); }
void JsThread::endBatch() { engine_->endBatch(); }
void JsThread::collectNow() {
  if (engine_) engine_->collectNow();
}

}  // namespace jsui
