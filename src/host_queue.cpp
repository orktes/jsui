#include "host_queue.h"

#include <chrono>

namespace jsui {

namespace {
// A callback that posts again on every run (an on_message handler answering
// with jsui_post_message, whose JS answers with postMessage) leaves the rest
// for the next tick instead of spinning in this one.
constexpr int kMaxPerDrain = 4096;
}  // namespace

void HostQueue::post(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.push_back(Item{std::move(fn), nullptr});
  size_.store(items_.size(), std::memory_order_release);
}

bool HostQueue::call(const std::function<void()>& fn, double timeoutMs) {
  if (onOwnerThread()) {
    drain();
    fn();
    return true;
  }
  auto proxy = std::make_shared<Proxy>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // `fn` refers to the caller's stack: the owner runs it only while the
    // caller still waits (Proxy::abandoned).
    items_.push_back(Item{[&fn] { fn(); }, proxy});
    size_.store(items_.size(), std::memory_order_release);
  }
  std::unique_lock<std::mutex> lock(proxy->m);
  proxy->cv.wait_for(lock, std::chrono::duration<double, std::milli>(timeoutMs),
                     [&] { return proxy->done || proxy->abandoned; });
  if (!proxy->done) proxy->abandoned = true;
  return proxy->done;
}

bool HostQueue::pop(Item* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (items_.empty()) return false;
  *out = std::move(items_.front());
  items_.pop_front();
  size_.store(items_.size(), std::memory_order_release);
  return true;
}

void HostQueue::run(Item& item) {
  if (!item.proxy) {
    item.fn();
    return;
  }
  // Held while `fn` runs: the waiting caller cannot time out and return
  // (freeing what `fn` refers to) in the middle of it.
  std::lock_guard<std::mutex> lock(item.proxy->m);
  if (item.proxy->abandoned) return;
  item.fn();
  item.proxy->done = true;
  item.proxy->cv.notify_all();
}

int HostQueue::drain() {
  int n = 0;
  Item item;
  while (n < kMaxPerDrain && pop(&item)) {
    run(item);
    item = Item{};
    n++;
  }
  return n;
}

void HostQueue::clear() {
  std::deque<Item> dropped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped.swap(items_);
    size_.store(0, std::memory_order_release);
  }
  for (Item& item : dropped) {
    if (!item.proxy) continue;
    std::lock_guard<std::mutex> lock(item.proxy->m);
    item.proxy->abandoned = true;
    item.proxy->cv.notify_all();
  }
}

}  // namespace jsui
