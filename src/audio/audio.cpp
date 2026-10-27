#include "audio/audio.h"

#include "runtime.h"

namespace jsui {

Audio::Audio(Runtime& rt) : rt_(rt) {
  fn_ = rt.config.on_audio;
  user_ = rt.config.audio_user;
  disabled_ = rt.config.audio_disabled != 0;
}

Audio::~Audio() = default;

bool Audio::available() {
  if (disabled_) return false;
  if (fn_) return true;
  if (!builtinTried_) {
    builtinTried_ = true;
    builtin_ = createBuiltinAudio(rt_);
    if (!builtin_) rt_.log(JSUI_LOG_INFO, "audio: no audio handler and no built-in backend; sounds are silent");
  }
  return builtin_ != nullptr;
}

void Audio::send(const JsuiAudioCommand& cmd) {
  if (fn_) fn_(user_, &cmd);
  else if (builtin_) builtin_->command(cmd);
}

uint64_t Audio::play(const std::string& url, const std::string& bus, float volume, float pitch, float pan, bool loop,
                     float fadeMs, double offsetS, uint64_t id) {
  if (!id) id = reserveVoice();
  live_.insert(id);
  if (!available()) {
    voiceEnded(id, JSUI_AUDIO_FAILED);
    return id;
  }
  JsuiAudioCommand c{};
  c.op = JSUI_AUDIO_PLAY;
  c.voice = id;
  c.sound = url.c_str();
  c.bus = bus.c_str();
  c.volume = volume;
  c.pitch = pitch;
  c.pan = pan;
  c.loop = loop ? 1 : 0;
  c.fade_ms = fadeMs;
  c.offset_s = offsetS;
  send(c);
  return id;
}

uint64_t Audio::playFor(std::function<void(int)> ended, const std::string& url, const std::string& bus,
                       float volume, float pitch, float pan, bool loop, float fadeMs, double offsetS) {
  uint64_t id = reserveVoice();
  foreignVoices_[id] = std::move(ended);
  return play(url, bus, volume, pitch, pan, loop, fadeMs, offsetS, id);
}

void Audio::preloadFor(const std::string& url, std::function<void(bool, double)> loaded) {
  foreignLoads_[url].push_back(std::move(loaded));
  preload(url);
}

void Audio::voice(JsuiAudioOp op, uint64_t voice, float volume, float pitch, float pan, float fadeMs) {
  if (!live_.count(voice) || !available()) return;
  JsuiAudioCommand c{};
  c.op = op;
  c.voice = voice;
  c.volume = volume;
  c.pitch = pitch;
  c.pan = pan;
  c.fade_ms = fadeMs;
  send(c);
}

void Audio::bus(const std::string& bus, float volume, bool muted, float fadeMs) {
  if (!available()) return;
  JsuiAudioCommand c{};
  c.op = JSUI_AUDIO_BUS;
  c.bus = bus.c_str();
  c.volume = volume;
  c.muted = muted ? 1 : 0;
  c.fade_ms = fadeMs;
  send(c);
}

void Audio::stopAll(const std::string& bus, float fadeMs) {
  if (live_.empty() || !available()) return;
  JsuiAudioCommand c{};
  c.op = JSUI_AUDIO_STOP_ALL;
  c.bus = bus.empty() ? nullptr : bus.c_str();
  c.fade_ms = fadeMs;
  send(c);
}

void Audio::preload(const std::string& url) {
  if (!available()) {
    soundLoaded(url, false, -1);
    return;
  }
  JsuiAudioCommand c{};
  c.op = JSUI_AUDIO_PRELOAD;
  c.sound = url.c_str();
  send(c);
}

void Audio::unload(const std::string& url) {
  if (!available()) return;
  JsuiAudioCommand c{};
  c.op = JSUI_AUDIO_UNLOAD;
  c.sound = url.c_str();
  send(c);
}

void Audio::contextEnded() {
  if (!live_.empty() && available()) {
    JsuiAudioCommand c{};
    c.op = JSUI_AUDIO_STOP_ALL;
    send(c);
  }
  live_.clear();
  events_.clear();
  foreignVoices_.clear();
  foreignLoads_.clear();
}

void Audio::voiceEnded(uint64_t voice, int status) {
  if (!live_.erase(voice)) return;
  auto foreign = foreignVoices_.find(voice);
  if (foreign != foreignVoices_.end()) {
    std::function<void(int)> fn = std::move(foreign->second);
    foreignVoices_.erase(foreign);
    if (fn) fn(status);
    return;
  }
  events_.push_back(Event{false, voice, status, std::string(), 0});
}

void Audio::soundLoaded(const std::string& url, bool ok, double durationS) {
  auto foreign = foreignLoads_.find(url);
  if (foreign != foreignLoads_.end()) {
    std::vector<std::function<void(bool, double)>> fns = std::move(foreign->second);
    foreignLoads_.erase(foreign);
    for (auto& fn : fns) fn(ok, durationS);
  }
  events_.push_back(Event{true, 0, ok ? 1 : 0, url, durationS});
}

void Audio::update() {
  if (builtin_) builtin_->update();
}

std::vector<Audio::Event> Audio::takeEvents() {
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

}  // namespace jsui
