// ViewAudio: the audio handler (JsuiConfig.on_audio) of a JsuiView's runtime.
// The UI's sounds play through Godot's own audio, so they go through the
// game's buses, volume settings and effects (include/jsui/jsui.h "Audio").
//
//   * Sounds: res:// and user:// URLs load with ResourceLoader (imported
//     audio, also in exported games) or else as bytes through FileAccess;
//     other URLs (a dev server's http://, file paths) as bytes through
//     jsui_fetch (the runtime's loader). Bytes become an AudioStreamWAV
//     (parsed here: PCM 8 / 16 / 24 / 32-bit, float), AudioStreamOggVorbis or
//     AudioStreamMP3 by their header. Loaded streams are cached per URL;
//     PLAYs that arrive while their sound loads start when it is there.
//   * Voices: an AudioStreamPlayer each, internal children of the view,
//     reused (a small pool). A looping voice plays a copy of the stream with
//     its loop flag on (other voices of the sound keep theirs off). Pitch is
//     pitch_scale, the start offset play(from_position). Pan is not applied:
//     AudioStreamPlayer has no pan, and a bus per voice would cost more than
//     UI sounds are worth.
//   * Volume: voice volume x fade x the JSUI bus's volume (the UI's own
//     volume / mute per bus, BUS commands) -> volume_db. Fades and ramps
//     (PLAY fade in, STOP fade out, SET, BUS) advance every frame in
//     update(). Godot's bus volumes are never touched.
//   * Buses: a JSUI bus plays on the Godot bus `audio_bus_map` names for it,
//     else the Godot bus of the same name (case-insensitive), else the
//     view's `audio_bus` (default "Master").
//   * Reports: each voice ends exactly once (jsui_audio_voice_ended:
//     finished, stopped, failed), each PRELOAD is answered
//     (jsui_audio_sound_loaded). Both are queued and handed to the runtime in
//     update(), on the main thread before the tick, as are the view's
//     sound_played / sound_ended signals (the handler runs while JS does).
//     A voice has finished when its player stops playing on its own.
#pragma once

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jsui/jsui.h"

namespace jsui_godot {

class JsuiView;

class ViewAudio {
 public:
  explicit ViewAudio(JsuiView& view) : view_(view) {}
  ~ViewAudio();

  // The runtime the handler serves (after jsui_create), or nullptr once it is
  // gone: its voices, reports and loads in flight are dropped (loaded sounds
  // stay cached).
  void attach(JsuiRuntime* rt);
  static void onAudio(void* user, const JsuiAudioCommand* cmd);
  // Every frame on the main thread before the runtime's tick: fades, voices
  // that ended, the queued reports (to rt) and signals.
  void update(JsuiRuntime* rt);

  // GDScript (JsuiView): stop_all_sounds, audio_enabled, get_audio_voices.
  void stopAll(const std::string& bus, float fadeMs);  // bus "": every voice
  void setEnabled(bool on);
  godot::Array voices() const;
  // The view is being deleted (NOTIFICATION_PREDELETE): Godot deletes its
  // children, the players, before its destructor stops the runtime.
  void dropPlayers();

 private:
  // A value ramped linearly over time (ms on the wall clock).
  struct Ramp {
    float from = 1.f, to = 1.f;
    double t0 = 0, dur = 0;
    float at(double now) const;
    void set(float target, double now, float ms);
    bool done(double now) const { return dur <= 0 || now >= t0 + dur; }
  };
  struct Sound {
    enum State { Loading, Ready, Failed } state = Loading;
    godot::Ref<godot::AudioStream> stream;  // as loaded
    godot::Ref<godot::AudioStream> plain, looped;  // with the loop flag off / on (made on first use)
    bool loopable = true;  // the stream type has a loop flag (else voices restart when they end)
    bool report = false;   // a PRELOAD waits for the result
    double duration = -1;
    std::string why;       // Failed: what went wrong
  };
  struct Voice {
    uint64_t id = 0;
    std::string url, bus;
    godot::AudioStreamPlayer* player = nullptr;  // nullptr while the sound loads
    godot::String godot_bus;
    Ramp volume, pitch, fade;
    float fade_in = 0;
    double offset = 0;
    bool loop = false, restart = false, paused = false, stopping = false;
    float last_db = 1e9f, last_pitch = -1.f;
  };
  // A jsui_fetch in flight (the callback's user pointer, freed by the callback
  // or, since it then never runs, when the runtime goes).
  struct Fetch {
    ViewAudio* audio;
    std::string url;
  };

  void command(const JsuiAudioCommand& c);
  void play(const JsuiAudioCommand& c);
  // Starts loading `url` unless it is cached or loading; returns its entry.
  Sound& load(const std::string& url);
  void loaded(const std::string& url, Sound& s);  // Loading -> Ready / Failed: report, start waiting voices
  static void onFetched(void* user, int status, const char* type, const void* data, size_t size);
  void start(Voice& v, Sound& s);
  void stop(Voice& v, float fadeMs, double now);
  // Ends the voice: frees its player and queues the report. Erases it.
  void end(uint64_t id, int status);
  void apply(Voice& v, double now);
  godot::AudioStreamPlayer* takePlayer();
  void releasePlayer(godot::AudioStreamPlayer* p);
  float busGain(const std::string& bus, double now) const;
  void fail(const std::string& url, const std::string& why);

  JsuiView& view_;
  JsuiRuntime* rt_ = nullptr;
  bool enabled_ = true;
  bool dying_ = false;  // dropPlayers(): no new players
  std::map<std::string, Sound> sounds_;
  std::map<uint64_t, Voice> voices_;
  std::map<std::string, Ramp> buses_;
  std::set<Fetch*> fetches_;
  std::vector<godot::AudioStreamPlayer*> idle_;
  // Queued for update().
  std::vector<std::pair<uint64_t, int>> ended_;
  struct Loaded {
    std::string url;
    bool ok;
    double duration;
  };
  std::vector<Loaded> loaded_;
  struct Played {
    uint64_t id;
    std::string url, bus;
  };
  std::vector<Played> played_;
  std::set<std::string> warned_;
};

}  // namespace jsui_godot
