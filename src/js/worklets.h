// Worklets: react-native-worklets' runtimes (docs/ARCHITECTURE.md
// "Worklets").
//
// A worklet is a function the build marked with 'worklet' (the CLI's
// worklets transform, packages/cli/src/worklets.js, a copy of Software
// Mansion's Babel plugin): an object carrying its source text, a hash and
// the values it captured. @orktes/jsui-react's `react-native-worklets`
// (packages/react/src/worklets.js) serializes it with its arguments into a
// JSON "wire" text and hands it to a worklet runtime here, which evaluates
// the code once per hash and calls it with the captured values bound.
//
// Each worklet runtime is a QuickJS runtime + context of its own (no heap
// shared with the bundle's; values cross as copies), created on first use,
// so a bundle that never schedules a worklet pays nothing. They all run on
// the owner thread (the one calling jsui_tick: JSUI's UI thread), in the
// tick's worklet phase (Runtime::tick, after the frame's JS and before
// native Animated) and in jsui_tick_native: queued jobs, then due timers,
// then requestAnimationFrame callbacks with the frame time. The UI runtime
// (id 2, react-native-worklets' UIRuntimeId) exists once per Runtime;
// createWorkletRuntime makes more (ids from 3), ticked the same way.
//
// What a worklet sends back to the bundle (runOnJS / scheduleOnRN calls,
// runOnUIAsync results, released remote functions) is queued and delivered
// after the worklet phase as one __jsui_onWorklets(json) call (an Inbound
// call: on a JS thread it is posted there). From a worklet, an Animated
// value that was captured or passed in reads and writes the native Animated
// node directly (NativeAnimated::setValue), so the change shows on this
// frame with no JS involved.
//
// Synchronizables (createSynchronizable) are values kept here, behind a
// lock, readable and writable from the bundle (any thread) and from every
// worklet runtime.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jsui {

class Runtime;
class WorkletRuntime;

class Worklets {
 public:
  // react-native-worklets' UIRuntimeId (RuntimeKind.UI).
  static constexpr int kUiRuntime = 2;

  explicit Worklets(Runtime& rt);
  ~Worklets();

  // ---- from the bundle (the bridge). Ids are taken on any thread; the
  // rest runs on the owner thread (the bridge goes through Runtime::toHost
  // / Runtime::proxy).
  // An id for a runtime createWorkletRuntime makes (created by create()).
  int reserveRuntimeId() { return nextRuntimeId_.fetch_add(1); }
  void create(int id, std::string name);
  // Queues a job (`[worklet, args, promiseId]` as wire JSON, or several:
  // `[[...], ...]` with `batch`) for runtime `id`'s next worklet phase. The
  // UI runtime is created by its first job; an unknown worker id is
  // reported.
  void post(int id, std::string json);
  // Runs a job now and returns `{"ok":true,"v":wire}` or
  // `{"ok":false,"e":wire}` (runOnUISync / runOnRuntimeSync).
  std::string runSync(int id, const std::string& json);

  // ---- the tick's worklet phase (owner thread)
  // `toJs`: deliver what the worklets sent to the bundle now (jsui_tick);
  // false keeps it for the next tick (jsui_tick_native runs no JS).
  void tick(double nowMs, bool toJs);
  // A worklet runtime exists (the tick has a worklet phase).
  bool active() const { return !runtimes_.empty() || !jobs_.empty(); }
  // requestAnimationFrame callbacks, queued jobs or messages for the bundle
  // wait for a frame.
  bool wantsFrame() const;
  // The earliest timer of any worklet runtime (1e300: none).
  double nextTimer() const;
  // Bytes the worklet runtimes' heaps hold (0 while none exists).
  size_t memoryUsage() const;
  size_t runtimeCount() const { return runtimes_.size(); }
  // The bundle is reloaded: every worklet runtime and synchronizable goes
  // (what they hold refers to the old context).
  void reset();

  // ---- from a worklet runtime (owner thread)
  // An op for the bundle (`["c", fn, args]`, `["r", id, n]`, `["p", ...]`).
  void toJs(std::string op) { toJs_.push_back(std::move(op)); }
  Runtime& runtime() { return rt_; }

  // ---- synchronizables (any thread)
  // `json`: the value as wire JSON.
  int syncCreate(std::string json);
  // The value (nullptr-equivalent "null" text for an unknown id).
  // `blocking`: waits for a lock another thread holds (getBlocking).
  std::string syncGet(int id, bool blocking);
  void syncSet(int id, std::string json, bool blocking);
  // lock() / unlock() (setBlocking with an updater). False when the lock
  // was not taken in time (kLockTimeoutMs: a lock held across a frame by
  // mistake must not hang the UI thread).
  bool syncLock(int id);
  void syncUnlock(int id);
  void syncRetain(int id);
  void syncRelease(int id);
  static constexpr int kLockTimeoutMs = 1000;

 private:
  struct Sync;
  std::shared_ptr<Sync> sync(int id);
  WorkletRuntime* get(int id, bool create);

  Runtime& rt_;
  std::map<int, std::unique_ptr<WorkletRuntime>> runtimes_;
  // Jobs for runtimes not created yet (a worker's create() comes first, but
  // its initializer job can be queued in the same batch).
  std::vector<std::pair<int, std::string>> jobs_;
  std::vector<std::string> toJs_;
  std::atomic<int> nextRuntimeId_{3};
  std::mutex syncMutex_;
  std::unordered_map<int, std::shared_ptr<Sync>> syncs_;
  std::atomic<int> nextSync_{1};
};

}  // namespace jsui
