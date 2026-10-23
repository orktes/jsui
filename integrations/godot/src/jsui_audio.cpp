#include "jsui_audio.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "jsui_view.h"

using namespace godot;

namespace jsui_godot {

namespace {

constexpr size_t kIdlePlayers = 8;  // pooled AudioStreamPlayers kept per view
constexpr float kSilentDb = -80.f;

double nowMs() { return double(Time::get_singleton()->get_ticks_usec()) / 1000.0; }

float toDb(float gain) { return gain <= 1e-4f ? kSilentDb : std::max(kSilentDb, 20.f * std::log10(gain)); }

String toGodot(const std::string& s) { return String::utf8(s.c_str(), int64_t(s.size())); }

uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

// A RIFF WAVE file as an AudioStreamWAV: PCM 8 / 16-bit as they are, PCM 24 /
// 32-bit and float 32 / 64-bit converted to 16-bit. Mono or stereo.
// (AudioStreamWAV.load_from_buffer exists from Godot 4.4; this library
// builds against the 4.3 API.)
Ref<AudioStream> parseWav(const uint8_t* p, size_t n, std::string& why) {
  uint32_t format = 0, channels = 0, rate = 0, bits = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
  for (size_t at = 12; at + 8 <= n;) {
    uint32_t len = le32(p + at + 4);
    const uint8_t* body = p + at + 8;
    size_t avail = std::min<size_t>(len, n - at - 8);
    if (!std::memcmp(p + at, "fmt ", 4) && avail >= 16) {
      format = le16(body);
      channels = le16(body + 2);
      rate = le32(body + 4);
      bits = le16(body + 14);
      if (format == 0xFFFE && avail >= 26) format = le16(body + 24);  // WAVE_FORMAT_EXTENSIBLE: the subformat
    } else if (!std::memcmp(p + at, "data", 4)) {
      data = body;
      size = avail;
      break;
    }
    at += 8 + size_t(len) + (len & 1);
  }
  if (!data || !rate) {
    why = "not a valid WAV file";
    return {};
  }
  if (channels < 1 || channels > 2) {
    why = "WAV with " + std::to_string(channels) + " channels (mono or stereo only)";
    return {};
  }
  const bool pcm = format == 1, flt = format == 3;
  if (!(pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(flt && (bits == 32 || bits == 64))) {
    why = "WAV format " + std::to_string(format) + " with " + std::to_string(bits) + "-bit samples is not supported";
    return {};
  }
  const size_t frame = size_t(bits / 8) * channels;
  const size_t samples = (size / frame) * channels;
  PackedByteArray out;
  AudioStreamWAV::Format fmt = AudioStreamWAV::FORMAT_16_BITS;
  if (pcm && bits == 8) {
    fmt = AudioStreamWAV::FORMAT_8_BITS;
    out.resize(int64_t(samples));
    uint8_t* o = out.ptrw();
    for (size_t i = 0; i < samples; i++) o[i] = uint8_t(data[i] ^ 0x80);  // unsigned -> signed
  } else if (pcm && bits == 16) {
    out.resize(int64_t(samples * 2));
    std::memcpy(out.ptrw(), data, samples * 2);
  } else {
    out.resize(int64_t(samples * 2));
    uint8_t* o = out.ptrw();
    for (size_t i = 0; i < samples; i++) {
      const uint8_t* s = data + i * (bits / 8);
      double v;
      if (pcm && bits == 24) {
        v = double(int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24)) / 2147483648.0;
      } else if (pcm) {
        v = double(int32_t(le32(s))) / 2147483648.0;
      } else if (bits == 32) {
        uint32_t u = le32(s);
        float f;
        std::memcpy(&f, &u, 4);
        v = f;
      } else {
        uint64_t u = uint64_t(le32(s)) | uint64_t(le32(s + 4)) << 32;
        std::memcpy(&v, &u, 8);
      }
      int16_t q = int16_t(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
      o[i * 2] = uint8_t(q & 0xff);
      o[i * 2 + 1] = uint8_t((uint16_t(q) >> 8) & 0xff);
    }
  }
  Ref<AudioStreamWAV> wav;
  wav.instantiate();
  wav->set_format(fmt);
  wav->set_mix_rate(int32_t(rate));
  wav->set_stereo(channels == 2);
  wav->set_data(out);
  return wav;
}

// Bytes of a sound file -> a stream, by the file's header.
Ref<AudioStream> decode(const uint8_t* p, size_t n, std::string& why) {
  if (n >= 12 && !std::memcmp(p, "RIFF", 4) && !std::memcmp(p + 8, "WAVE", 4)) return parseWav(p, n, why);
  PackedByteArray bytes;
  bytes.resize(int64_t(n));
  if (n) std::memcpy(bytes.ptrw(), p, n);
  if (n >= 4 && !std::memcmp(p, "OggS", 4)) {
    Ref<AudioStreamOggVorbis> ogg = AudioStreamOggVorbis::load_from_buffer(bytes);
    if (ogg.is_null()) why = "not an Ogg Vorbis stream (Opus is not supported)";
    return ogg;
  }
  if ((n >= 3 && !std::memcmp(p, "ID3", 3)) || (n >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0)) {
    Ref<AudioStreamMP3> mp3;
    mp3.instantiate();
    mp3->set_data(bytes);
    if (mp3->get_data().is_empty()) {
      why = "not a valid MP3 file";
      return {};
    }
    return mp3;
  }
  if (n >= 4 && !std::memcmp(p, "fLaC", 4)) {
    why = "FLAC is not supported (use WAV, Ogg Vorbis or MP3)";
    return {};
  }
  why = n ? "unknown audio format (WAV, Ogg Vorbis or MP3)" : "empty file";
  return {};
}

// `s` with its loop flag set to `loop`: `s` itself when it already is, else
// a copy (so other voices of the sound keep theirs). Null for stream types
// without a loop flag.
Ref<AudioStream> withLoop(const Ref<AudioStream>& s, bool loop) {
  if (auto* wav = Object::cast_to<AudioStreamWAV>(s.ptr())) {
    if (!loop && wav->get_loop_mode() == AudioStreamWAV::LOOP_DISABLED) return s;
    Ref<AudioStreamWAV> d = wav->duplicate();
    d->set_loop_mode(loop ? AudioStreamWAV::LOOP_FORWARD : AudioStreamWAV::LOOP_DISABLED);
    if (loop) {
      // The whole sound (an imported WAV may carry loop points of its own).
      d->set_loop_begin(0);
      d->set_loop_end(int32_t(std::llround(wav->get_length() * wav->get_mix_rate())));
    }
    return d;
  }
  if (auto* ogg = Object::cast_to<AudioStreamOggVorbis>(s.ptr())) {
    if (ogg->has_loop() == loop) return s;
    Ref<AudioStreamOggVorbis> d = ogg->duplicate();
    d->set_loop(loop);
    return d;
  }
  if (auto* mp3 = Object::cast_to<AudioStreamMP3>(s.ptr())) {
    if (mp3->has_loop() == loop) return s;
    Ref<AudioStreamMP3> d = mp3->duplicate();
    d->set_loop(loop);
    return d;
  }
  return {};
}

}  // namespace

// ------------------------------------------------------------------- ramps

float ViewAudio::Ramp::at(double now) const {
  if (done(now)) return to;
  float x = float(std::max(0.0, now - t0) / dur);
  return from + (to - from) * x;
}

void ViewAudio::Ramp::set(float target, double now, float ms) {
  from = at(now);
  to = target;
  t0 = now;
  dur = ms > 0 ? ms : 0;
}

// --------------------------------------------------------------- lifecycle

ViewAudio::~ViewAudio() {
  // The players are the view's children and go with it.
  for (Fetch* f : fetches_) delete f;
}

void ViewAudio::attach(JsuiRuntime* rt) {
  // The old runtime's fetch callbacks never run now.
  for (Fetch* f : fetches_) delete f;
  fetches_.clear();
  for (auto& [id, v] : voices_)
    if (v.player) releasePlayer(v.player);
  voices_.clear();
  buses_.clear();
  ended_.clear();
  loaded_.clear();
  played_.clear();
  for (auto it = sounds_.begin(); it != sounds_.end();) {
    if (it->second.state == Sound::Ready) {
      it->second.report = false;
      ++it;
    } else {
      it = sounds_.erase(it);
    }
  }
  rt_ = rt;
}

void ViewAudio::dropPlayers() {
  for (auto& [id, v] : voices_) {
    if (!v.player) continue;
    v.player->set_stream_paused(false);  // first: a playback stopped while paused stayed in the AudioServer (leaked at exit)
    v.player->stop();
    v.player->set_stream(Ref<AudioStream>());
    v.player = nullptr;
    v.stopping = true;  // not loading: whatever comes next ends it
  }
  for (AudioStreamPlayer* p : idle_) p->set_stream(Ref<AudioStream>());
  idle_.clear();
  dying_ = true;
}

void ViewAudio::onAudio(void* user, const JsuiAudioCommand* cmd) {
  if (cmd) static_cast<ViewAudio*>(user)->command(*cmd);
}

void ViewAudio::setEnabled(bool on) {
  if (on == enabled_) return;
  enabled_ = on;
  if (!on) stopAll(std::string(), 0);
}

// ---------------------------------------------------------------- commands

void ViewAudio::command(const JsuiAudioCommand& c) {
  const double now = nowMs();
  const std::string sound = c.sound ? c.sound : "";
  switch (c.op) {
    case JSUI_AUDIO_PRELOAD: {
      if (!enabled_) {
        loaded_.push_back({sound, false, -1});
        break;
      }
      Sound& s = load(sound);
      s.report = true;
      if (s.state != Sound::Loading) loaded(sound, s);
      break;
    }
    case JSUI_AUDIO_UNLOAD: {
      // Playing voices keep their stream; a load in flight still has to
      // answer its PLAYs and PRELOAD.
      auto it = sounds_.find(sound);
      if (it != sounds_.end() && it->second.state != Sound::Loading) sounds_.erase(it);
      break;
    }
    case JSUI_AUDIO_PLAY: play(c); break;
    case JSUI_AUDIO_STOP: {
      auto it = voices_.find(c.voice);
      if (it != voices_.end()) stop(it->second, c.fade_ms, now);
      break;
    }
    case JSUI_AUDIO_PAUSE:
    case JSUI_AUDIO_RESUME: {
      auto it = voices_.find(c.voice);
      if (it == voices_.end()) break;
      Voice& v = it->second;
      v.paused = c.op == JSUI_AUDIO_PAUSE;
      if (v.player) v.player->set_stream_paused(v.paused);
      break;
    }
    case JSUI_AUDIO_SET: {
      auto it = voices_.find(c.voice);
      if (it == voices_.end()) break;
      it->second.volume.set(std::max(0.f, c.volume), now, c.fade_ms);
      it->second.pitch.set(c.pitch, now, c.fade_ms);
      if (it->second.player) apply(it->second, now);
      break;
    }
    case JSUI_AUDIO_BUS: {
      if (!c.bus) break;
      buses_[c.bus].set(c.muted ? 0.f : std::max(0.f, c.volume), now, c.fade_ms);
      for (auto& [id, v] : voices_)
        if (v.player && v.bus == c.bus) apply(v, now);
      break;
    }
    case JSUI_AUDIO_STOP_ALL: stopAll(c.bus ? c.bus : "", c.fade_ms); break;
  }
}

void ViewAudio::play(const JsuiAudioCommand& c) {
  if (!enabled_ || !c.sound) {
    ended_.push_back({c.voice, JSUI_AUDIO_FAILED});
    return;
  }
  Voice v;
  v.id = c.voice;
  v.url = c.sound;
  v.bus = c.bus ? c.bus : "";
  v.volume.to = v.volume.from = std::max(0.f, c.volume);
  v.pitch.to = v.pitch.from = c.pitch;
  v.fade_in = c.fade_ms;
  v.offset = std::max(0.0, c.offset_s);
  v.loop = c.loop != 0;
  Sound& s = load(v.url);
  if (s.state == Sound::Failed) {
    ended_.push_back({c.voice, JSUI_AUDIO_FAILED});
    loaded(v.url, s);  // warns, forgets it
    return;
  }
  Voice& added = voices_[v.id] = std::move(v);
  if (s.state == Sound::Ready) start(added, s);  // else when it has loaded
}

void ViewAudio::stopAll(const std::string& bus, float fadeMs) {
  const double now = nowMs();
  std::vector<uint64_t> ids;
  for (auto& [id, v] : voices_)
    if (bus.empty() || v.bus == bus) ids.push_back(id);
  for (uint64_t id : ids) {
    auto it = voices_.find(id);
    if (it != voices_.end()) stop(it->second, fadeMs, now);
  }
}

void ViewAudio::stop(Voice& v, float fadeMs, double now) {
  if (!v.player || v.paused || fadeMs <= 0) {
    end(v.id, JSUI_AUDIO_STOPPED);
    return;
  }
  // Fades out from where it is (also mid fade-in), then ends (update).
  v.stopping = true;
  v.fade.set(0.f, now, fadeMs);
}

void ViewAudio::end(uint64_t id, int status) {
  auto it = voices_.find(id);
  if (it == voices_.end()) return;
  if (it->second.player) releasePlayer(it->second.player);
  voices_.erase(it);
  ended_.push_back({id, status});
}

// ------------------------------------------------------------------ loading

ViewAudio::Sound& ViewAudio::load(const std::string& url) {
  auto found = sounds_.find(url);
  if (found != sounds_.end()) return found->second;
  Sound& s = sounds_[url];
  String path = toGodot(url);
  if (path.begins_with("res://") || path.begins_with("user://")) {
    int cut = path.find("?");
    if (cut < 0) cut = path.find("#");
    if (cut >= 0) path = path.substr(0, cut);
    // Imported audio (the editor imports the bundle's sounds; an exported
    // game has only the imported ones), else the file's bytes.
    ResourceLoader* loader = ResourceLoader::get_singleton();
    if (loader->exists(path, "AudioStream")) {
      s.stream = loader->load(path, "AudioStream");
      if (s.stream.is_null()) s.why = "ResourceLoader could not load it";
    } else if (FileAccess::file_exists(path)) {
      PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
      s.stream = decode(bytes.ptr(), size_t(bytes.size()), s.why);
    } else {
      s.why = "file not found";
    }
    s.state = s.stream.is_valid() ? Sound::Ready : Sound::Failed;
    return s;  // the caller calls loaded() (it may report it, or wait for it)
  }
  if (!rt_) {
    s.state = Sound::Failed;
    s.why = "no runtime";
    return s;
  }
  // Everything else through the runtime's loader (a dev server, a file
  // path): answered in a later tick (onFetched -> loaded()).
  auto* f = new Fetch{this, url};
  fetches_.insert(f);
  jsui_fetch(rt_, url.c_str(), &ViewAudio::onFetched, f);
  return s;
}

void ViewAudio::onFetched(void* user, int status, const char*, const void* data, size_t size) {
  auto* f = static_cast<Fetch*>(user);
  ViewAudio* self = f->audio;
  std::string url = std::move(f->url);
  self->fetches_.erase(f);
  delete f;
  auto it = self->sounds_.find(url);
  if (it == self->sounds_.end() || it->second.state != Sound::Loading) return;
  Sound& s = it->second;
  if (status == 200) s.stream = decode(static_cast<const uint8_t*>(data), size, s.why);
  else s.why = status ? "HTTP status " + std::to_string(status) : "network error";
  s.state = s.stream.is_valid() ? Sound::Ready : Sound::Failed;
  self->loaded(url, s);
}

void ViewAudio::loaded(const std::string& url, Sound& s) {
  if (s.state == Sound::Ready && s.duration < 0) {
    double len = s.stream->get_length();
    s.duration = len > 0 ? len : -1;
  } else if (s.state == Sound::Failed) {
    fail(url, s.why);
  }
  if (s.report) loaded_.push_back({url, s.state == Sound::Ready, s.duration});
  s.report = false;
  // The voices that waited for it (in the order they were asked for).
  std::vector<uint64_t> waiting;
  for (auto& [id, v] : voices_)
    if (!v.player && v.url == url) waiting.push_back(id);
  for (uint64_t id : waiting) {
    auto it = voices_.find(id);
    if (it == voices_.end()) continue;
    if (s.state == Sound::Ready) start(it->second, s);
    else end(id, JSUI_AUDIO_FAILED);
  }
  if (s.state == Sound::Failed) sounds_.erase(url);  // a later request tries again (s is gone)
}

void ViewAudio::fail(const std::string& url, const std::string& why) {
  if (!warned_.insert(url).second) return;
  UtilityFunctions::push_warning("JsuiView: cannot play " + toGodot(url) + ": " + toGodot(why));
}

// ------------------------------------------------------------------ voices

void ViewAudio::start(Voice& v, Sound& s) {
  Ref<AudioStream> stream;
  if (s.loopable) {
    Ref<AudioStream>& variant = v.loop ? s.looped : s.plain;
    if (variant.is_null()) variant = withLoop(s.stream, v.loop);
    if (variant.is_null()) s.loopable = false;
    stream = variant;
  }
  if (stream.is_null()) {
    // A stream type without a loop flag: played as it is, restarted when it
    // ends if the voice loops.
    stream = s.stream;
    v.restart = v.loop;
  }
  AudioStreamPlayer* p = takePlayer();
  if (!p) {
    end(v.id, JSUI_AUDIO_FAILED);
    return;
  }
  v.player = p;
  v.godot_bus = view_.resolve_audio_bus(toGodot(v.bus));
  const double now = nowMs();
  v.fade = Ramp{};
  if (v.fade_in > 0) v.fade = Ramp{0.f, 1.f, now, double(v.fade_in)};  // from silence, from the start
  p->set_stream(stream);
  p->set_bus(v.godot_bus);
  v.last_db = 1e9f;
  v.last_pitch = -1.f;
  apply(v, now);
  p->play(float(v.offset));
  if (v.paused) p->set_stream_paused(true);
  played_.push_back({v.id, v.url, v.bus});
}

void ViewAudio::apply(Voice& v, double now) {
  float db = toDb(v.volume.at(now) * v.fade.at(now) * busGain(v.bus, now));
  if (std::abs(db - v.last_db) > 0.01f) {
    v.player->set_volume_db(db);
    v.last_db = db;
  }
  float pitch = std::clamp(v.pitch.at(now), 0.01f, 16.f);
  if (pitch != v.last_pitch) {
    v.player->set_pitch_scale(pitch);
    v.last_pitch = pitch;
  }
}

float ViewAudio::busGain(const std::string& bus, double now) const {
  auto it = buses_.find(bus);
  return it == buses_.end() ? 1.f : it->second.at(now);
}

AudioStreamPlayer* ViewAudio::takePlayer() {
  if (!idle_.empty()) {
    AudioStreamPlayer* p = idle_.back();
    idle_.pop_back();
    return p;
  }
  if (dying_ || !view_.is_inside_tree()) return nullptr;
  auto* p = memnew(AudioStreamPlayer);
  p->set_name("JsuiVoice");
  // Internal: not in get_children(), so the game's code never sees them.
  view_.add_child(p, true, Node::INTERNAL_MODE_BACK);
  return p;
}

void ViewAudio::releasePlayer(AudioStreamPlayer* p) {
  p->set_stream_paused(false);  // first: a playback stopped while paused stayed in the AudioServer (leaked at exit)
  p->stop();
  p->set_stream(Ref<AudioStream>());
  if (idle_.size() < kIdlePlayers) {
    idle_.push_back(p);
  } else {
    p->queue_free();
  }
}

// ------------------------------------------------------------------- frame

void ViewAudio::update(JsuiRuntime* rt) {
  const double now = nowMs();
  std::vector<std::pair<uint64_t, int>> done;
  for (auto& [id, v] : voices_) {
    if (!v.player) continue;
    if (v.stopping && v.fade.done(now)) {
      done.push_back({id, JSUI_AUDIO_STOPPED});
      continue;
    }
    apply(v, now);
    // Played to the end: its player stopped on its own (a paused one is
    // not playing either).
    if (!v.paused && !v.player->is_playing()) {
      if (v.restart) v.player->play(0);
      else done.push_back({id, JSUI_AUDIO_FINISHED});
    }
  }
  for (auto& [id, status] : done) end(id, status);
  // Reports, then the game's signals (they may stop the view).
  std::vector<std::pair<uint64_t, int>> ended;
  ended.swap(ended_);
  std::vector<Loaded> loads;
  loads.swap(loaded_);
  std::vector<Played> played;
  played.swap(played_);
  if (rt) {
    for (auto& [id, status] : ended) jsui_audio_voice_ended(rt, id, status);
    for (Loaded& l : loads) jsui_audio_sound_loaded(rt, l.url.c_str(), l.ok ? 1 : 0, l.duration);
  }
  for (Played& p : played) view_.emit_signal("sound_played", int64_t(p.id), toGodot(p.url), toGodot(p.bus));
  for (auto& [id, status] : ended) view_.emit_signal("sound_ended", int64_t(id), status);
}

Array ViewAudio::voices() const {
  const double now = nowMs();
  Array out;
  for (auto& [id, v] : voices_) {
    Dictionary d;
    d["voice"] = int64_t(id);
    d["url"] = toGodot(v.url);
    d["bus"] = toGodot(v.bus);
    d["godot_bus"] = v.godot_bus;
    d["player"] = v.player;
    d["volume"] = v.volume.at(now) * v.fade.at(now) * busGain(v.bus, now);
    d["pitch"] = v.pitch.at(now);
    d["loop"] = v.loop;
    d["paused"] = v.paused;
    d["loading"] = v.player == nullptr;
    out.push_back(d);
  }
  return out;
}

}  // namespace jsui_godot
