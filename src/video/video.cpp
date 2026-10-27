#include "video/video.h"

#include "runtime.h"

namespace jsui {

Video::Video(Runtime& rt) : rt_(rt) {
  fn_ = rt.config.on_video;
  user_ = rt.config.video_user;
}

void Video::send(const JsuiVideoCommand& c) {
  if (fn_) fn_(user_, &c);
}

void Video::open(uint64_t id, const std::vector<std::string>& sources, const std::vector<std::string>& types,
                 const std::string& texture, bool paused, bool muted, float volume, bool loop, float rate,
                 double progressMs) {
  start(id, sources, types, texture, paused, muted, volume, loop, rate, progressMs, nullptr);
}

void Video::start(uint64_t id, const std::vector<std::string>& sources, const std::vector<std::string>& types,
                  const std::string& texture, bool paused, bool muted, float volume, bool loop, float rate,
                  double progressMs, std::function<void(const JsuiVideoStatus&)> report) {
  if (!id || open_.count(id) || sources.empty()) return;
  Open& v = open_[id];
  // Registered before the handler hears of it: it may answer from inside.
  v.report = std::move(report);
  v.texture = texture;
  v.progressMs = progressMs > 0 ? progressMs : 0;
  v.last.id = id;
  v.last.state = JSUI_VIDEO_LOADING;
  if (!fn_) {
    JsuiVideoStatus s{};
    s.state = JSUI_VIDEO_FAILED;
    s.error = "no video handler (JsuiConfig.on_video)";
    status(id, s);
    return;
  }
  std::vector<const char*> urls, mimes;
  for (size_t i = 0; i < sources.size(); i++) {
    urls.push_back(sources[i].c_str());
    mimes.push_back(i < types.size() && !types[i].empty() ? types[i].c_str() : nullptr);
  }
  JsuiVideoCommand c{};
  c.op = JSUI_VIDEO_OPEN;
  c.video = id;
  c.url = urls[0];
  c.sources = urls.data();
  c.source_types = mimes.data();
  c.source_count = int(urls.size());
  c.texture = texture.c_str();
  c.paused = paused;
  c.muted = muted;
  c.volume = volume;
  c.loop = loop;
  c.rate = rate;
  send(c);
}

void Video::openFor(std::function<void(const JsuiVideoStatus&)> report, uint64_t id,
                    const std::vector<std::string>& sources, const std::vector<std::string>& types,
                    const std::string& texture, bool paused, bool muted, float volume, bool loop, float rate) {
  start(id, sources, types, texture, paused, muted, volume, loop, rate, 0, std::move(report));
}

void Video::set(uint64_t id, bool paused, bool muted, float volume, bool loop, float rate) {
  if (!open_.count(id)) return;
  JsuiVideoCommand c{};
  c.op = JSUI_VIDEO_SET;
  c.video = id;
  c.paused = paused;
  c.muted = muted;
  c.volume = volume;
  c.loop = loop;
  c.rate = rate;
  send(c);
}

void Video::seek(uint64_t id, double timeS) {
  if (!open_.count(id)) return;
  JsuiVideoCommand c{};
  c.op = JSUI_VIDEO_SEEK;
  c.video = id;
  c.time_s = timeS;
  send(c);
}

void Video::close(uint64_t id) {
  auto it = open_.find(id);
  if (it == open_.end()) return;
  std::string texture = it->second.texture;
  open_.erase(it);
  JsuiVideoCommand c{};
  c.op = JSUI_VIDEO_CLOSE;
  c.video = id;
  c.texture = texture.c_str();
  send(c);
  rt_.textures.remove(texture);
  rt_.buffers.remove(texture + ".time");
}

void Video::contextEnded() {
  std::vector<uint64_t> ids;
  for (auto& [id, v] : open_) ids.push_back(id);
  for (uint64_t id : ids) close(id);
  events_.clear();
}

void Video::queue(Open& v, const Event& e) {
  v.last = e;
  for (auto& q : events_)
    if (q.id == e.id) {
      q = e;
      return;
    }
  events_.push_back(e);
}

void Video::status(uint64_t id, const JsuiVideoStatus& s) {
  auto it = open_.find(id);
  if (it == open_.end()) return;
  Open& v = it->second;
  if (v.report) {
    // A child runtime's video: the child keeps its own progress buffers.
    v.report(s);
    return;
  }
  Event e{id, s.state, s.duration_s, s.position_s, s.buffered_s, s.width, s.height, s.error ? s.error : ""};
  // The position for Animated.hostBuffer progress bars, every report.
  double d = e.duration > 0 ? e.duration : 0;
  float t[3] = {float(d ? e.position / d : 0), float(d ? e.buffered / d : 0), float(e.position)};
  rt_.buffers.set(v.texture + ".time", JSUI_BUFFER_F32, t, 3);
  // JS: changes at once, the position at its progress rate.
  const Event& l = v.last;
  bool changed = e.state != l.state || e.duration != l.duration || e.width != l.width || e.height != l.height ||
                 e.error != l.error;
  bool moved = e.position != l.position || e.buffered != l.buffered;
  if (!changed && !(moved && rt_.now - v.lastProgressAt >= v.progressMs)) return;
  v.lastProgressAt = rt_.now;  // JS heard the position now
  queue(v, e);
}

std::vector<Video::Event> Video::takeEvents() {
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

}  // namespace jsui
