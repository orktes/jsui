// The UI thread of a JsuiView's runtime (see jsui_view.h "UI thread"): runs
// one job at a time; the main thread hands a frame over with post() and
// takes it back with wait(), which orders everything the job did before
// what the main thread does next.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "jsui_view.h"

namespace jsui_godot {

class JsuiView::UiWorker {
 public:
  UiWorker() : thread_([this] { loop(); }) {}
  ~UiWorker() {
    {
      std::lock_guard<std::mutex> lock(m_);
      quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  UiWorker(const UiWorker&) = delete;
  UiWorker& operator=(const UiWorker&) = delete;

  // Main thread, while idle.
  void post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(m_);
      job_ = std::move(job);
      busy_.store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
  }
  // A job is in flight (a hint: wait() is what synchronizes).
  bool busy() const { return busy_.load(std::memory_order_acquire); }
  void wait() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [&] { return !job_; });
  }
  bool current() const { return std::this_thread::get_id() == thread_.get_id(); }

 private:
  void loop() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
      cv_.wait(lock, [&] { return quit_ || job_; });
      if (quit_) return;
      std::function<void()> job = job_;
      lock.unlock();
      job();
      lock.lock();
      job_ = nullptr;
      busy_.store(false, std::memory_order_release);
      cv_.notify_all();
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::function<void()> job_;
  std::atomic<bool> busy_{false};
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace jsui_godot
