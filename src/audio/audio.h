// UI audio (jsui.h "Audio"): the JS Sound API's requests, routed to the
// embedder's audio handler (JsuiConfig.on_audio) or the built-in backend,
// and the voice-ended / sound-loaded reports that go back to JS.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "jsui/jsui.h"

namespace jsui {

class Runtime;

// A player for JsuiAudioCommands (the built-in backend implements it).
// Reports go through Runtime::audio (voiceEnded / soundLoaded) on the
// runtime's thread.
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  virtual void command(const JsuiAudioCommand& cmd) = 0;
  // Every jsui_tick(), on the runtime's thread (deliver reports, free
  // finished voices).
  virtual void update() {}
};

// The library's built-in backend (audio_builtin.cpp: miniaudio when built
// with JSUI_AUDIO), or nullptr when the build has none.
std::unique_ptr<AudioBackend> createBuiltinAudio(Runtime& rt);

class Audio {
 public:
  explicit Audio(Runtime& rt);
  ~Audio();

  // ---- requests (from JS)
  // Starts a voice of `url` (absolute). Returns its id (never 0).
  uint64_t play(const std::string& url, const std::string& bus, float volume, float pitch, float pan, bool loop,
                float fadeMs, double offsetS);
  // STOP / PAUSE / RESUME / SET of a live voice (ignored for ended ones).
  void voice(JsuiAudioOp op, uint64_t voice, float volume, float pitch, float pan, float fadeMs);
  void bus(const std::string& bus, float volume, bool muted, float fadeMs);
  // `bus` empty: every voice.
  void stopAll(const std::string& bus, float fadeMs);
  void preload(const std::string& url);
  void unload(const std::string& url);
  // The JS context is going away: stops every voice and forgets them (their
  // reports are no longer delivered).
  void contextEnded();

  // ---- reports (from the handler / backend)
  void voiceEnded(uint64_t voice, int status);
  void soundLoaded(const std::string& url, bool ok, double durationS);

  // Per tick: lets the backend run, then returns the reports for JS.
  struct Event {
    bool loaded;     // false: voice ended
    uint64_t voice;  // voice ended
    int status;      // JSUI_AUDIO_* / loaded: 1 ok, 0 failed
    std::string url;
    double duration;
  };
  void update();
  std::vector<Event> takeEvents();
  bool hasEvents() const { return !events_.empty(); }
  bool live(uint64_t voice) const { return live_.count(voice) > 0; }

 private:
  // The handler: on_audio, else the built-in backend (made on first use),
  // else nothing (every voice fails).
  void send(const JsuiAudioCommand& cmd);
  bool available();

  Runtime& rt_;
  JsuiAudioFn fn_ = nullptr;
  void* user_ = nullptr;
  bool disabled_ = false;
  bool builtinTried_ = false;
  std::unique_ptr<AudioBackend> builtin_;
  uint64_t nextVoice_ = 1;
  std::unordered_set<uint64_t> live_;
  std::vector<Event> events_;
};

}  // namespace jsui
