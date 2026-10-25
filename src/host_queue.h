// Host callbacks and proxied reads for JS on its own thread (see
// docs/ARCHITECTURE.md "Shared state").
//
// The embedder's callbacks (on_message, on_state, on_call, on_call_cancel,
// on_audio, log, the resource loader, the clipboard setter) run on the
// owner thread, the one calling jsui_tick. In the runtime's JS-thread mode
// (Runtime::jsThreadMode) the bridge does not call them while JS runs: it
// posts them here, and the owner thread runs them in order inside the next
// tick (drain()). A read only the owner thread can answer (the embedder's
// clipboard, the props a node holds) is a proxy call(): the caller waits
// until the owner thread ran it, for a bounded time.
//
// Today JS still runs on the owner thread, where call() runs the read at
// once, after what was posted before it (the order stays FIFO).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace jsui {

class HostQueue {
 public:
  // The thread constructing it is the owner thread.
  HostQueue() : owner_(std::this_thread::get_id()) {}
  HostQueue(const HostQueue&) = delete;
  HostQueue& operator=(const HostQueue&) = delete;
  ~HostQueue() { clear(); }

  // Any thread: `fn` runs on the owner thread in the next drain().
  void post(std::function<void()> fn);
  // Any thread: runs `fn` on the owner thread and returns once it ran (true),
  // or false when it did not run within `timeoutMs` (it never runs then) or
  // the queue was cleared. On the owner thread: drains, then runs `fn`.
  bool call(const std::function<void()>& fn, double timeoutMs);
  // Owner thread: runs what was posted, in order (callbacks may post more;
  // those run too, up to a bound per drain). Re-entrant: a callback that
  // drains continues with the next item. Returns how many ran.
  int drain();
  bool empty() const { return size_.load(std::memory_order_acquire) == 0; }
  // Drops what was posted; waiting call()s return false.
  void clear();
  bool onOwnerThread() const { return std::this_thread::get_id() == owner_; }

 private:
  struct Proxy {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, abandoned = false;
  };
  struct Item {
    std::function<void()> fn;
    std::shared_ptr<Proxy> proxy;  // set for call()
  };
  bool pop(Item* out);
  void run(Item& item);

  std::thread::id owner_;
  std::mutex mutex_;
  std::deque<Item> items_;
  std::atomic<size_t> size_{0};
};

}  // namespace jsui
