// JS on its own thread (docs/ARCHITECTURE.md "JS thread"): the engine
// Runtime uses in JS-thread mode (Runtime::setJsThreadMode). It owns a
// thread that creates, runs and frees the QuickJS engine; the owner thread
// (the one calling jsui_tick) never runs JS and never waits for it, except
// inside a sync the host asks for (jsui_sync, JSUI_TICK_SYNC_JS, lockstep).
//
// Owner -> JS: Inbound messages are queued here (post) and delivered on the
// JS thread in post order, each stamped with the runtime clock it was
// posted at (what performance.now() and new timers see while it runs).
// Frames coalesce: a JS thread that is behind runs one Frame (the latest)
// for several queued ones.
//
// JS -> owner: each JS task (one message with its microtasks, or a run of
// due timers) publishes one JsBatch: the commands it recorded
// (js/command_buffer.h), the replies to the messages it answered and the
// host callbacks it made (Runtime::toHost), applied by the owner thread in
// that order (Runtime::applyJs), batch by batch. A React commit is one task,
// so the owner never sees half of it.
//
// Between frames the thread runs timers due at the last frame time
// (publishedNow: React's scheduler continues there) and collects garbage
// while it has nothing else to do. Lockstep mode (tests and tools): no
// timers of its own; every message is waited for (Inbound::post), which
// gives the frames single-threaded mode gives.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/perf.h"
#include "js/command_buffer.h"
#include "js/engine.h"
#include "js/inbound.h"

namespace jsui {

class Runtime;

// What one JS task hands the owner thread.
struct JsBatch {
  CommandBuffer commands;
  struct Reply {
    uint32_t seq;
    int flags;
    bool event;
  };
  std::vector<Reply> replies;
  std::vector<std::function<void()>> host;
  // A task's batch (FrameSnapshot::appliedBatches counts these; not the
  // host callbacks a proxied read flushes ahead of its task).
  bool counted = true;
};

class JsThread final : public JsEngine {
 public:
  // Whether this build can run JS on a thread (not in WebAssembly).
  static bool available();
  // Starts the thread and creates the engine on it; returns once it exists.
  explicit JsThread(Runtime& rt);
  ~JsThread() override;
  // The JsThread whose thread calls this (nullptr on any other thread).
  static JsThread* current();

  // ---- owner thread
  void post(Inbound::Message&& m);
  // The published batches, oldest first (nullptr when there are none).
  bool takeBatch(JsBatch* out);
  // A batch the owner applied: its arrays are reused.
  void recycle(JsBatch&& b);
  bool hasBatches() const { return published_.load(std::memory_order_acquire) > 0; }
  // Waits until the JS thread is idle (nothing queued, no task running, no
  // timer of its own due), running `serve` (proxied reads) meanwhile.
  // timeoutMs < 0: no limit. False on timeout.
  bool waitIdle(double timeoutMs, const std::function<void()>& serve);
  bool idle() const;
  // Lockstep: every post is waited for; no timers of the thread's own.
  void setLockstep(bool on) { lockstep_.store(on); }
  bool lockstep() const { return lockstep_.load(std::memory_order_relaxed); }
  // A sync is waiting: React's scheduler does not yield (shouldYield).
  void setSyncing(bool on) { syncing_.store(on); }
  // JS is running a task (perf / tests: "jsBusy").
  bool busy() const { return busy_.load(std::memory_order_acquire); }
  // Wall time JS spent in tasks, in microseconds, since the last call with
  // reset.
  double busyUs(bool reset);
  // The owner is applying a batch (host callbacks run): reloads and surface
  // destruction wait (JsEngine::inCall).
  int applying = 0;
  // Ends the thread: interrupts running JS, frees the engine on its thread
  // and joins. The pending Host.calls stay readable (pendingCalls).
  void stop() override;
  // Owner side of Host.call bookkeeping (jsui_resolve_call): a call is
  // answerable once until JS settles or cancels it.
  bool takeCall(uint64_t id) override;

  // ---- JS thread
  // The end of a JS task: publishes what it recorded (and the replies and
  // host callbacks since the last batch). False when there was nothing.
  bool publish(CommandBuffer& commands);
  // Publishes the replies and host callbacks made so far, without the
  // task's commands (a proxied read runs after them: Runtime::proxy).
  void flushHost();
  void reply(uint32_t seq, int flags, bool event) { replies_.push_back({seq, flags, event}); }
  void hostItem(std::function<void()> fn) { host_.push_back(std::move(fn)); }
  void callStarted(uint64_t id);
  void callEnded(uint64_t id);
  // The clock of the message being delivered (performance.now, timers).
  double now() const { return now_; }
  // React's scheduler slice: measured from the start of the task.
  bool shouldYield() const;
  bool stopping() const { return stop_.load(std::memory_order_relaxed); }

  // ---- JsEngine: owner-thread queries answer from what the thread
  // published; the engine calls (eval, dispatchEvent, ...) run on the JS
  // thread only (Inbound::deliver on it, tasks posted to it).
  bool eval(const std::string& source, const std::string& name) override;
  void runTimers(double nowMs) override;
  void runAnimationFrame(double nowMs) override;
  void runJobs() override;
  bool hasPendingWork(double nowMs) const override;
  int dispatchEvent(const Value& event) override;
  void hostMessage(const std::string& name, const std::string& json) override;
  void layoutEvent(uint32_t id, float x, float y, float w, float h) override;
  bool callGlobal(const char* name, const Value* args, int argc) override;
  void releaseBuffer(const std::string& key) override {}
  bool hasCall(uint64_t id) const override;
  bool settleCall(uint64_t id, bool ok, const std::string& payload) override;
  bool dropCall(uint64_t id) override;
  std::vector<uint64_t> pendingCalls() const override;
  void measured(const std::vector<MeasureResult>& results) override;
  void mergePerf(PerfCounters& into) override;
  bool inCall() const override { return applying > 0; }
  void beginBatch() override;
  void endBatch() override;
  size_t memoryUsage() const override { return memory_.load(std::memory_order_relaxed); }
  bool collectIdle(bool, bool) override { return false; }  // the thread collects itself
  GcStats gcStats(bool reset) override;
  void collectNow() override;

 private:
  void main();
  void loop();
  // Delivers the messages taken from the queue, in order.
  void deliver(std::deque<Inbound::Message>& work);
  // Timers due at publishedNow (not in lockstep), bounded per frame.
  bool ownWorkDue() const;
  void runOwnWork();
  void afterWork();

  Runtime& rt_;
  std::unique_ptr<JsEngine> engine_;  // created, used and freed on the thread
  struct Native;                       // the OS thread
  std::unique_ptr<Native> native_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;      // the JS thread waits for work here
  std::condition_variable idleCv_;  // the owner waits for idle / exit here
  std::deque<Inbound::Message> queue_;
  bool ready_ = false, idle_ = false, exited_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<bool> lockstep_{false}, syncing_{false}, busy_{false};

  // Published batches and the arrays they give back.
  mutable std::mutex batchMutex_;
  std::deque<JsBatch> batches_;
  std::vector<CommandBuffer> pool_;
  std::atomic<size_t> published_{0};

  // What the thread tells the owner (hasPendingWork, memoryUsage, ...).
  std::atomic<double> nextDue_{1e300};  // -inf: rAF callbacks wait for a frame
  std::atomic<size_t> memory_{0};
  std::atomic<uint64_t> busyUs_{0};
  mutable std::mutex statsMutex_;
  PerfCounters counters_;  // what JS counted, waiting for mergePerf
  GcStats gc_;

  // Host.calls JS made and has not settled (jsui_resolve_call).
  mutable std::mutex callsMutex_;
  std::unordered_set<uint64_t> calls_;
  std::vector<uint64_t> endedCalls_;  // pendingCalls after stop()

  // ---- JS thread only
  std::vector<JsBatch::Reply> replies_;
  std::vector<std::function<void()>> host_;
  double now_ = 0;
  double taskStart_ = 0;
  double ownFrame_ = -1;  // the frame time own timer passes were counted for
  int ownPasses_ = 0;
};

}  // namespace jsui
