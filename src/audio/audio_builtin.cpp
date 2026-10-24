// The built-in audio backend: plays the JsuiAudioCommands itself when the
// embedder installs no audio handler (JsuiConfig.on_audio), with miniaudio
// (cmake/Deps.cmake, JSUI_AUDIO). Built without JSUI_AUDIO there is no
// built-in backend (sounds need a handler).
//
// - The engine and its output device open on the first PLAY or PRELOAD, so
//   hosts that never play a sound never open a device. If the device cannot
//   be opened, that is logged once and every voice / preload fails.
// - Sound bytes come through the runtime's loader (files, http, data: URIs
//   and custom JsuiLoadFn loaders alike) and are decoded on a worker thread:
//   short sounds fully (f32 at the engine's rate, shared by their voices),
//   long ones (> 1 MB encoded or > 20 s) streamed, each voice decoding the
//   encoded bytes in memory as it plays. A PLAY before the bytes are there
//   waits for them; a load or decode error fails its voices.
// - Each bus is an ma_sound_group: BUS ramps its fader, mute sets its volume.
//   A voice's volume (PLAY fade in, SET, STOP fade out) is its fader; SET
//   ramps pitch and pan per update().
// - End-of-sound callbacks run on the audio thread: they are queued and
//   reported (and the voice freed) from update() on the runtime thread.
//
// JSUI_AUDIO_BACKEND=null plays to miniaudio's null device (no hardware:
// CI); any other miniaudio backend name (alsa, pulseaudio, wasapi, ...)
// picks that backend.
#include "audio/audio.h"

#if JSUI_AUDIO

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "runtime.h"

namespace jsui {
namespace {

// Larger (encoded) or longer sounds are streamed instead of decoded whole.
constexpr size_t kStreamBytes = 1 << 20;
constexpr double kStreamSeconds = 20;

// A loaded sound, shared by the voices that play it (they keep it alive
// after an UNLOAD).
struct SoundData {
  bool stream = false;
  std::vector<uint8_t> encoded;  // streamed
  std::vector<float> pcm;        // decoded: interleaved f32 at the engine's rate
  uint32_t channels = 0;
  uint64_t frames = 0;
  double duration = -1;  // < 0: unknown
};

ma_decoder_config decoderConfig(uint32_t sampleRate) { return ma_decoder_config_init(ma_format_f32, 0, sampleRate); }

// On the worker thread: the bytes -> a playable sound, or an error.
std::shared_ptr<SoundData> decode(std::vector<uint8_t> bytes, uint32_t sampleRate, std::string* error) {
  ma_decoder_config cfg = decoderConfig(sampleRate);
  ma_decoder dec;
  if (bytes.empty() || ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) {
    *error = "unsupported or corrupt audio data";
    return nullptr;
  }
  auto out = std::make_shared<SoundData>();
  out->channels = dec.outputChannels;
  ma_uint64 length = 0;
  if (ma_decoder_get_length_in_pcm_frames(&dec, &length) != MA_SUCCESS) length = 0;
  if (length > 0) out->duration = double(length) / sampleRate;
  out->stream = bytes.size() > kStreamBytes || out->duration > kStreamSeconds;
  if (out->stream) {
    out->frames = length;
    ma_decoder_uninit(&dec);
    out->encoded = std::move(bytes);
    return out;
  }
  const ma_uint64 chunk = 4096;
  if (length > 0) out->pcm.reserve(size_t(length) * out->channels);
  for (;;) {
    size_t at = out->pcm.size();
    out->pcm.resize(at + size_t(chunk) * out->channels);
    ma_uint64 got = 0;
    ma_result r = ma_decoder_read_pcm_frames(&dec, out->pcm.data() + at, chunk, &got);
    out->pcm.resize(at + size_t(got) * out->channels);
    if (r != MA_SUCCESS || got == 0) break;
  }
  ma_decoder_uninit(&dec);
  out->frames = out->pcm.size() / std::max<uint32_t>(out->channels, 1);
  if (out->frames == 0) {
    *error = "no audio frames";
    return nullptr;
  }
  out->duration = double(out->frames) / sampleRate;
  out->pcm.shrink_to_fit();
  return out;
}

// A linear ramp of pitch or pan, in engine milliseconds.
struct Ramp {
  bool active = false;
  float from = 0, to = 0;
  double start = 0, ms = 0;
  float at(double now) const {
    double t = ms > 0 ? std::clamp((now - start) / ms, 0.0, 1.0) : 1.0;
    return float(from + (to - from) * t);
  }
};

class BuiltinAudio;

struct Voice {
  enum State { Waiting, Playing, Paused, Stopping };
  BuiltinAudio* owner = nullptr;
  uint64_t id = 0;
  std::string url, bus;
  State state = Waiting;
  bool pauseOnStart = false;  // PAUSE while waiting for the sound
  float volume = 1, pitch = 1, pan = 0, fadeMs = 0;
  bool loop = false;
  double offset = 0;
  std::shared_ptr<SoundData> data;
  ma_audio_buffer_ref buffer;
  ma_decoder decoder;
  ma_sound sound;
  bool bufferInit = false, decoderInit = false, soundInit = false;
  Ramp pitchRamp, panRamp;
  double stopDeadline = 0;
};

struct Bus {
  ma_sound_group group;
  bool init = false;
  float volume = 1;
  bool muted = false;
};

// A sound by URL: loading (voices / preloads wait) or loaded.
struct Sound {
  uint64_t token = 0;   // this load (results of an older one are ignored)
  uint64_t loadId = 0;  // the loader's request, while it runs
  std::shared_ptr<SoundData> data;
  int preloads = 0;  // PRELOADs to answer
  std::vector<uint64_t> waiting;
  bool unloaded = false;  // UNLOAD while loading: forget it once loaded
};

class BuiltinAudio final : public AudioBackend {
 public:
  explicit BuiltinAudio(Runtime& rt) : rt_(rt) {}
  ~BuiltinAudio() override;

  void command(const JsuiAudioCommand& cmd) override;
  void update() override;
  bool busy() const override;

  // Audio thread.
  void ended(uint64_t voice) {
    std::lock_guard<std::mutex> lock(endedMutex_);
    ended_.push_back(voice);
  }

 private:
  bool ensureEngine();
  double nowMs() { return double(ma_engine_get_time_in_milliseconds(&engine_)); }
  Bus& bus(const std::string& name);
  void initBus(Bus& b);
  Sound& request(const std::string& url);
  void loaded(const std::string& url, uint64_t token, Resource res);
  void resolve(const std::string& url, uint64_t token, std::shared_ptr<SoundData> data, const std::string& error);
  void play(const JsuiAudioCommand& c);
  void start(Voice& v);
  void stop(Voice& v, float fadeMs);
  void set(Voice& v, float volume, float pitch, float pan, float fadeMs);
  void finish(uint64_t voice, int status);
  void release(Voice& v);

  // Decoding worker.
  struct Job {
    std::string url;
    uint64_t token;
    std::vector<uint8_t> bytes;
  };
  struct Done {
    std::string url;
    uint64_t token;
    std::shared_ptr<SoundData> data;
    std::string error;
  };
  void submit(Job job);
  void work();

  Runtime& rt_;
  bool engineTried_ = false, engineOk_ = false;
  ma_context context_;
  ma_engine engine_;
  uint32_t sampleRate_ = 48000;
  std::unordered_map<std::string, std::unique_ptr<Bus>> buses_;
  std::unordered_map<std::string, Sound> sounds_;
  std::unordered_map<uint64_t, std::unique_ptr<Voice>> voices_;
  uint64_t nextToken_ = 1;

  mutable std::mutex endedMutex_;
  std::vector<uint64_t> ended_;

  std::thread worker_;
  mutable std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::deque<Job> jobs_;
  std::vector<Done> done_;
  int decoding_ = 0;  // jobs submitted and not yet taken back (jobMutex_)
  bool quit_ = false;
};

BuiltinAudio::~BuiltinAudio() {
  // No reports from here: the runtime is tearing the Audio down.
  for (auto& [url, s] : sounds_)
    if (s.loadId) rt_.loader.cancel(s.loadId);
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      quit_ = true;
    }
    jobCv_.notify_all();
    worker_.join();
  }
  for (auto& [id, v] : voices_) release(*v);
  voices_.clear();
  for (auto& [name, b] : buses_)
    if (b->init) ma_sound_group_uninit(&b->group);
  buses_.clear();
  if (engineOk_) {
    ma_engine_uninit(&engine_);
    ma_context_uninit(&context_);
  }
}

bool BuiltinAudio::ensureEngine() {
  if (engineTried_) return engineOk_;
  engineTried_ = true;
  // The platform's backends, without miniaudio's null fallback (a machine
  // without a device fails instead of "playing" silently), unless asked for.
  ma_backend backends[MA_BACKEND_COUNT];
  size_t count = 0;
  const char* env = std::getenv("JSUI_AUDIO_BACKEND");
  if (env && *env) {
    // "null", "alsa", "pulseaudio", "coreaudio", "wasapi"...: miniaudio's
    // backend names, any case, without spaces.
    auto key = [](const char* name) {
      std::string k;
      for (const char* p = name; *p; p++)
        if (std::isalnum((unsigned char)*p)) k += char(std::tolower((unsigned char)*p));
      return k;
    };
    for (int b = 0; b < MA_BACKEND_COUNT && !count; b++)
      if (key(ma_get_backend_name(ma_backend(b))) == key(env)) backends[count++] = ma_backend(b);
    if (!count) {
      rt_.log(JSUI_LOG_WARN, std::string("audio: unknown JSUI_AUDIO_BACKEND '") + env + "'; sounds are silent");
      return false;
    }
  } else {
    ma_backend all[MA_BACKEND_COUNT];
    size_t n = 0;
    ma_get_enabled_backends(all, MA_BACKEND_COUNT, &n);
    for (size_t i = 0; i < n; i++)
      if (all[i] != ma_backend_null && all[i] != ma_backend_custom) backends[count++] = all[i];
  }
  ma_context_config cc = ma_context_config_init();
  if (ma_context_init(backends, ma_uint32(count), &cc, &context_) != MA_SUCCESS) {
    rt_.log(JSUI_LOG_WARN, "audio: no audio backend available; sounds are silent");
    return false;
  }
  ma_engine_config ec = ma_engine_config_init();
  ec.pContext = &context_;
  if (ma_engine_init(&ec, &engine_) != MA_SUCCESS) {
    ma_context_uninit(&context_);
    rt_.log(JSUI_LOG_WARN, "audio: cannot open the audio output device; sounds are silent");
    return false;
  }
  engineOk_ = true;
  sampleRate_ = ma_engine_get_sample_rate(&engine_);
  rt_.log(JSUI_LOG_DEBUG, std::string("audio: ") + ma_get_backend_name(context_.backend) + " output, " +
                              std::to_string(sampleRate_) + " Hz");
  for (auto& [name, b] : buses_) initBus(*b);
  return true;
}

Bus& BuiltinAudio::bus(const std::string& name) {
  auto& b = buses_[name];
  if (!b) {
    b = std::make_unique<Bus>();
    if (engineOk_) initBus(*b);
  }
  return *b;
}

void BuiltinAudio::initBus(Bus& b) {
  if (b.init || ma_sound_group_init(&engine_, 0, nullptr, &b.group) != MA_SUCCESS) return;
  b.init = true;
  ma_sound_group_set_fade_in_milliseconds(&b.group, b.volume, b.volume, 0);
  ma_sound_group_set_volume(&b.group, b.muted ? 0.f : 1.f);
}

void BuiltinAudio::command(const JsuiAudioCommand& c) {
  switch (c.op) {
    case JSUI_AUDIO_PRELOAD: {
      std::string url = c.sound ? c.sound : "";
      if (!ensureEngine()) {
        rt_.audio->soundLoaded(url, false, -1);
        return;
      }
      Sound& s = request(url);
      if (s.data) rt_.audio->soundLoaded(url, true, s.data->duration);
      else s.preloads++;
      return;
    }
    case JSUI_AUDIO_UNLOAD: {
      auto it = sounds_.find(c.sound ? c.sound : "");
      if (it == sounds_.end()) return;
      if (it->second.data) sounds_.erase(it);  // playing voices keep their data
      else it->second.unloaded = true;
      return;
    }
    case JSUI_AUDIO_PLAY:
      play(c);
      return;
    case JSUI_AUDIO_STOP:
    case JSUI_AUDIO_PAUSE:
    case JSUI_AUDIO_RESUME:
    case JSUI_AUDIO_SET: {
      auto it = voices_.find(c.voice);
      if (it == voices_.end()) return;
      Voice& v = *it->second;
      if (c.op == JSUI_AUDIO_STOP) {
        stop(v, c.fade_ms);
      } else if (c.op == JSUI_AUDIO_SET) {
        set(v, c.volume, c.pitch, c.pan, c.fade_ms);
      } else if (v.state == Voice::Waiting) {
        v.pauseOnStart = c.op == JSUI_AUDIO_PAUSE;
      } else if (c.op == JSUI_AUDIO_PAUSE && v.state == Voice::Playing) {
        ma_sound_stop(&v.sound);
        v.state = Voice::Paused;
      } else if (c.op == JSUI_AUDIO_RESUME && v.state == Voice::Paused) {
        ma_sound_start(&v.sound);
        v.state = Voice::Playing;
      }
      return;
    }
    case JSUI_AUDIO_BUS: {
      Bus& b = bus(c.bus ? c.bus : "");
      b.volume = std::max(c.volume, 0.f);
      b.muted = c.muted != 0;
      if (b.init) {
        ma_sound_group_set_fade_in_milliseconds(&b.group, -1, b.volume, ma_uint64(std::max(c.fade_ms, 0.f)));
        ma_sound_group_set_volume(&b.group, b.muted ? 0.f : 1.f);
      }
      return;
    }
    case JSUI_AUDIO_STOP_ALL: {
      std::vector<uint64_t> ids;
      for (auto& [id, v] : voices_)
        if (!c.bus || v->bus == c.bus) ids.push_back(id);
      for (uint64_t id : ids) {
        auto it = voices_.find(id);
        if (it != voices_.end()) stop(*it->second, c.fade_ms);
      }
      return;
    }
  }
}

Sound& BuiltinAudio::request(const std::string& url) {
  auto [it, fresh] = sounds_.try_emplace(url);
  Sound& s = it->second;
  s.unloaded = false;
  if (fresh) {
    uint64_t token = s.token = nextToken_++;
    s.loadId = rt_.loader.load(url, [this, url, token](Resource res) { loaded(url, token, std::move(res)); });
  }
  return s;
}

void BuiltinAudio::loaded(const std::string& url, uint64_t token, Resource res) {
  auto it = sounds_.find(url);
  if (it == sounds_.end() || it->second.token != token) return;
  it->second.loadId = 0;
  if (!res.ok()) {
    std::string why = res.error.empty() ? "status " + std::to_string(res.status) : res.error;
    resolve(url, token, nullptr, "cannot load (" + why + ")");
    return;
  }
  submit(Job{url, token, std::move(res.data)});
}

void BuiltinAudio::resolve(const std::string& url, uint64_t token, std::shared_ptr<SoundData> data,
                           const std::string& error) {
  auto it = sounds_.find(url);
  if (it == sounds_.end() || it->second.token != token) return;
  int preloads = it->second.preloads;
  std::vector<uint64_t> waiting = std::move(it->second.waiting);
  // A failed sound is forgotten (a later request tries again), so is an
  // unloaded one once its waiting voices have started.
  if (data && !it->second.unloaded) {
    it->second.data = data;
    it->second.preloads = 0;
  } else {
    sounds_.erase(it);
  }
  if (!data) rt_.log(JSUI_LOG_WARN, "audio: " + url + ": " + error);
  for (int i = 0; i < preloads; i++) rt_.audio->soundLoaded(url, data != nullptr, data ? data->duration : -1);
  for (uint64_t id : waiting) {
    auto v = voices_.find(id);
    if (v == voices_.end() || v->second->state != Voice::Waiting) continue;
    if (!data) {
      finish(id, JSUI_AUDIO_FAILED);
      continue;
    }
    v->second->data = data;
    start(*v->second);
  }
}

void BuiltinAudio::play(const JsuiAudioCommand& c) {
  if (!ensureEngine()) {
    rt_.audio->voiceEnded(c.voice, JSUI_AUDIO_FAILED);
    return;
  }
  auto owned = std::make_unique<Voice>();
  Voice& v = *owned;
  v.owner = this;
  v.id = c.voice;
  v.url = c.sound ? c.sound : "";
  v.bus = c.bus ? c.bus : "";
  v.volume = std::max(c.volume, 0.f);
  v.pitch = c.pitch;
  v.pan = c.pan;
  v.fadeMs = c.fade_ms;
  v.loop = c.loop != 0;
  v.offset = c.offset_s;
  voices_[v.id] = std::move(owned);
  Sound& s = request(v.url);
  if (s.data) {
    v.data = s.data;
    start(v);
  } else {
    s.waiting.push_back(v.id);
  }
}

void BuiltinAudio::start(Voice& v) {
  const SoundData& d = *v.data;
  ma_data_source* source = nullptr;
  if (d.stream) {
    ma_decoder_config cfg = decoderConfig(sampleRate_);
    if (ma_decoder_init_memory(d.encoded.data(), d.encoded.size(), &cfg, &v.decoder) == MA_SUCCESS) {
      v.decoderInit = true;
      source = &v.decoder;
    }
  } else if (ma_audio_buffer_ref_init(ma_format_f32, d.channels, d.pcm.data(), d.frames, &v.buffer) == MA_SUCCESS) {
    v.bufferInit = true;
    v.buffer.sampleRate = sampleRate_;
    source = &v.buffer;
  }
  Bus& b = bus(v.bus);
  if (!source || !b.init ||
      ma_sound_init_from_data_source(&engine_, source, MA_SOUND_FLAG_NO_SPATIALIZATION, &b.group, &v.sound) !=
          MA_SUCCESS) {
    finish(v.id, JSUI_AUDIO_FAILED);
    return;
  }
  v.soundInit = true;
  ma_sound_set_end_callback(
      &v.sound, [](void* user, ma_sound*) { static_cast<Voice*>(user)->owner->ended(static_cast<Voice*>(user)->id); },
      &v);
  ma_sound_set_pitch(&v.sound, std::max(v.pitch, 0.01f));
  ma_sound_set_pan(&v.sound, std::clamp(v.pan, -1.f, 1.f));
  ma_sound_set_looping(&v.sound, v.loop);
  if (v.offset > 0) ma_sound_seek_to_pcm_frame(&v.sound, ma_uint64(v.offset * sampleRate_));
  bool fade = v.fadeMs > 0;
  ma_sound_set_fade_in_milliseconds(&v.sound, fade ? 0.f : v.volume, v.volume, fade ? ma_uint64(v.fadeMs) : 0);
  if (v.pauseOnStart) {
    v.state = Voice::Paused;
    return;
  }
  v.state = Voice::Playing;
  if (ma_sound_start(&v.sound) != MA_SUCCESS) finish(v.id, JSUI_AUDIO_FAILED);
}

void BuiltinAudio::stop(Voice& v, float fadeMs) {
  if (v.state == Voice::Stopping) return;
  if (v.state != Voice::Playing || fadeMs <= 0) {
    finish(v.id, JSUI_AUDIO_STOPPED);
    return;
  }
  // Fades out on the audio thread, then stops; update() reports it.
  ma_sound_stop_with_fade_in_milliseconds(&v.sound, ma_uint64(fadeMs));
  v.state = Voice::Stopping;
  v.pitchRamp.active = v.panRamp.active = false;
  v.stopDeadline = nowMs() + fadeMs + 250;
}

void BuiltinAudio::set(Voice& v, float volume, float pitch, float pan, float fadeMs) {
  v.volume = std::max(volume, 0.f);
  v.pitch = pitch;
  v.pan = pan;
  if (!v.soundInit || v.state == Voice::Stopping) return;  // waiting: applied on start
  float ms = std::max(fadeMs, 0.f);
  ma_sound_set_fade_in_milliseconds(&v.sound, -1, v.volume, ma_uint64(ms));
  float p = std::max(pitch, 0.01f), q = std::clamp(pan, -1.f, 1.f);
  if (ms <= 0) {
    v.pitchRamp.active = v.panRamp.active = false;
    ma_sound_set_pitch(&v.sound, p);
    ma_sound_set_pan(&v.sound, q);
    return;
  }
  double now = nowMs();
  v.pitchRamp = Ramp{true, ma_sound_get_pitch(&v.sound), p, now, ms};
  v.panRamp = Ramp{true, ma_sound_get_pan(&v.sound), q, now, ms};
}

void BuiltinAudio::release(Voice& v) {
  if (v.soundInit) ma_sound_uninit(&v.sound);
  if (v.decoderInit) ma_decoder_uninit(&v.decoder);
  if (v.bufferInit) ma_audio_buffer_ref_uninit(&v.buffer);
  v.soundInit = v.decoderInit = v.bufferInit = false;
}

void BuiltinAudio::finish(uint64_t voice, int status) {
  auto it = voices_.find(voice);
  if (it == voices_.end()) return;
  std::unique_ptr<Voice> v = std::move(it->second);
  voices_.erase(it);
  release(*v);
  rt_.audio->voiceEnded(voice, status);
}

void BuiltinAudio::update() {
  std::vector<Done> done;
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    done.swap(done_);
    decoding_ -= int(done.size());
  }
  for (auto& d : done) resolve(d.url, d.token, std::move(d.data), d.error);

  std::vector<uint64_t> ended;
  {
    std::lock_guard<std::mutex> lock(endedMutex_);
    ended.swap(ended_);
  }
  for (uint64_t id : ended) {
    auto it = voices_.find(id);
    if (it == voices_.end()) continue;
    finish(id, it->second->state == Voice::Stopping ? JSUI_AUDIO_STOPPED : JSUI_AUDIO_FINISHED);
  }

  if (!engineOk_ || voices_.empty()) return;
  double now = nowMs();
  std::vector<uint64_t> stopped;
  for (auto& [id, v] : voices_) {
    if (v->state == Voice::Stopping) {
      if (!ma_sound_is_playing(&v->sound) || now >= v->stopDeadline) stopped.push_back(id);
      continue;
    }
    if (v->pitchRamp.active) {
      ma_sound_set_pitch(&v->sound, v->pitchRamp.at(now));
      v->pitchRamp.active = now < v->pitchRamp.start + v->pitchRamp.ms;
    }
    if (v->panRamp.active) {
      ma_sound_set_pan(&v->sound, v->panRamp.at(now));
      v->panRamp.active = now < v->panRamp.start + v->panRamp.ms;
    }
  }
  for (uint64_t id : stopped) finish(id, JSUI_AUDIO_STOPPED);
}

bool BuiltinAudio::busy() const {
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (decoding_ > 0) return true;
  }
  {
    std::lock_guard<std::mutex> lock(endedMutex_);
    if (!ended_.empty()) return true;
  }
  for (auto& [id, v] : voices_)
    if (v->state == Voice::Stopping || v->pitchRamp.active || v->panRamp.active) return true;
  return false;
}

void BuiltinAudio::submit(Job job) {
  std::lock_guard<std::mutex> lock(jobMutex_);
  if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
  jobs_.push_back(std::move(job));
  decoding_++;
  jobCv_.notify_one();
}

void BuiltinAudio::work() {
  std::unique_lock<std::mutex> lock(jobMutex_);
  for (;;) {
    jobCv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
    if (quit_) return;
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    uint32_t rate = sampleRate_;
    lock.unlock();
    Done d{std::move(job.url), job.token, nullptr, std::string()};
    d.data = decode(std::move(job.bytes), rate, &d.error);
    lock.lock();
    done_.push_back(std::move(d));
  }
}

}  // namespace

std::unique_ptr<AudioBackend> createBuiltinAudio(Runtime& rt) { return std::make_unique<BuiltinAudio>(rt); }

}  // namespace jsui

#else  // !JSUI_AUDIO

namespace jsui {

std::unique_ptr<AudioBackend> createBuiltinAudio(Runtime&) { return nullptr; }

}  // namespace jsui

#endif
