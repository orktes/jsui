// Video (jsui.h "Video"): the JS <Video> view's requests, routed to the
// embedder's video handler (JsuiConfig.on_video), and the status reports
// that go back to JS (and to the "<texture>.time" host buffer).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"

namespace jsui {

class Runtime;

class Video {
 public:
  explicit Video(Runtime& rt);

  // ---- requests (from JS)
  // A video id for a later open() (any thread: JS on its own thread names
  // the video before the request reaches the owner thread).
  uint64_t reserve() { return next_.fetch_add(1); }
  // Plays the first of `sources` (absolute URLs; `types`: MIME type or "")
  // the host can into host texture `texture`. Status reports reach JS at
  // once on state changes, position-only ones at most every `progressMs`
  // (0: every report).
  void open(uint64_t id, const std::vector<std::string>& sources, const std::vector<std::string>& types,
            const std::string& texture, bool paused, bool muted, float volume, bool loop, float rate,
            double progressMs);
  void set(uint64_t id, bool paused, bool muted, float volume, bool loop, float rate);
  void seek(uint64_t id, double timeS);
  void close(uint64_t id);
  // The JS context is going away: closes every video.
  void contextEnded();

  // ---- reports (from the handler)
  void status(uint64_t id, const JsuiVideoStatus& s);

  struct Event {
    uint64_t id;
    int state;
    double duration, position, buffered;
    int width, height;
    std::string error;
  };
  // Per tick: the reports for JS (the latest per video).
  std::vector<Event> takeEvents();
  bool hasEvents() const { return !events_.empty(); }

 private:
  struct Open {
    std::string texture;
    double progressMs = 250;
    double lastProgressAt = -1e18;  // runtime clock, ms
    Event last{};                   // what JS last heard
  };
  void send(const JsuiVideoCommand& c);
  void queue(Open& v, const Event& e);

  Runtime& rt_;
  JsuiVideoFn fn_ = nullptr;
  void* user_ = nullptr;
  std::atomic<uint64_t> next_{1};
  std::unordered_map<uint64_t, Open> open_;
  std::vector<Event> events_;
};

}  // namespace jsui
