// UI audio through the C API: the JS bridge's requests reaching the audio
// handler (JsuiConfig.on_audio) as JsuiAudioCommands with resolved URLs,
// voice-ended / sound-loaded reports going back to JS (__jsui_onAudio) on
// the next tick, STOP_ALL when the JS context goes away, audio_disabled, and
// jsui_fetch. The @orktes/jsui-react layer (Sound, useSound, useMusic,
// UISounds) is covered by packages/react/test/sound.test.js.
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct Cmd {
  int op;
  uint64_t voice;
  std::string sound, bus;
  float volume, pitch, pan, fade;
  double offset;
  int loop, muted;
  bool busNull;
};

struct AudioHost {
  JsuiRuntime* rt = nullptr;
  std::vector<Cmd> cmds;
  std::vector<std::string> messages;
  double t = 0;
  bool endAtOnce = false;  // report every PLAY as failed from inside the handler

  explicit AudioHost(bool handler = true, bool disabled = false) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 32;
    cfg.height = 32;
    cfg.base_url = "http://game.test/ui/bundle.js";
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<AudioHost*>(u)->messages.push_back(std::string(name) + "=" + (json ? json : ""));
    };
    cfg.message_user = this;
    if (handler) {
      cfg.on_audio = [](void* u, const JsuiAudioCommand* c) {
        auto* h = static_cast<AudioHost*>(u);
        h->cmds.push_back({int(c->op), c->voice, c->sound ? c->sound : "", c->bus ? c->bus : "", c->volume, c->pitch,
                           c->pan, c->fade_ms, c->offset_s, c->loop, c->muted, c->bus == nullptr});
        if (h->endAtOnce && c->op == JSUI_AUDIO_PLAY) jsui_audio_voice_ended(h->rt, c->voice, JSUI_AUDIO_FAILED);
      };
      cfg.audio_user = this;
    }
    cfg.audio_disabled = disabled ? 1 : 0;
    rt = jsui_create(&cfg);
  }
  ~AudioHost() {
    if (rt) jsui_destroy(rt);
  }
  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "bundle.js") == 0; }
  void tick() { jsui_tick(rt, t += 16); }
  std::string take() {
    std::string s;
    for (auto& m : messages) s += (s.empty() ? "" : " ") + m;
    messages.clear();
    return s;
  }
};

// Posts every report __jsui_onAudio receives; play(...) posts the voice id.
const char* kAudioBundle = R"(
  const N = __jsui_native;
  globalThis.__jsui_onAudio = (events) => N.postMessage('audio', JSON.stringify(events));
  globalThis.play = (...a) => { const id = N.audioPlay(...a); N.postMessage('voice', String(id)); return id; };
)";

void eval(AudioHost& h, const std::string& js) {
  std::string src = std::string(kAudioBundle) + js;
  CHECK(h.load(src));
}

}  // namespace

TEST(audio_play_reaches_the_handler_with_a_resolved_url) {
  AudioHost h;
  eval(h, R"(
    play('sfx/click.wav', 'ui', 0.5, 1.25, -0.5, false, 40, 0.1);
    play('/music/menu.ogg', '', 1, 1, 0, true, 0, 0);
    play('https://cdn.test/a.mp3');
  )");
  CHECK_EQ(h.take(), std::string("voice=1 voice=2 voice=3"));
  CHECK_EQ(h.cmds.size(), size_t(3));
  if (h.cmds.size() != 3) return;
  const Cmd& a = h.cmds[0];
  CHECK_EQ(a.op, int(JSUI_AUDIO_PLAY));
  CHECK_EQ(a.voice, uint64_t(1));
  CHECK_EQ(a.sound, std::string("http://game.test/ui/sfx/click.wav"));
  CHECK_EQ(a.bus, std::string("ui"));
  CHECK_NEAR(a.volume, 0.5, 1e-6);
  CHECK_NEAR(a.pitch, 1.25, 1e-6);
  CHECK_NEAR(a.pan, -0.5, 1e-6);
  CHECK_NEAR(a.fade, 40, 1e-6);
  CHECK_NEAR(a.offset, 0.1, 1e-9);
  CHECK_EQ(a.loop, 0);
  CHECK_EQ(h.cmds[1].sound, std::string("http://game.test/music/menu.ogg"));
  CHECK_EQ(h.cmds[1].bus, std::string("sfx"));  // default bus
  CHECK_EQ(h.cmds[1].loop, 1);
  CHECK_EQ(h.cmds[2].sound, std::string("https://cdn.test/a.mp3"));
  CHECK_NEAR(h.cmds[2].volume, 1, 1e-6);  // defaults
  CHECK_NEAR(h.cmds[2].pitch, 1, 1e-6);
}

TEST(audio_voice_ended_reaches_js_on_the_next_tick_once) {
  AudioHost h;
  eval(h, "play('a.wav'); play('b.wav');");
  h.take();
  jsui_audio_voice_ended(h.rt, 2, JSUI_AUDIO_STOPPED);
  jsui_audio_voice_ended(h.rt, 2, JSUI_AUDIO_FINISHED);  // ignored: already ended
  jsui_audio_voice_ended(h.rt, 99, JSUI_AUDIO_FINISHED);  // unknown
  CHECK(jsui_wants_frame(h.rt));
  CHECK_EQ(h.take(), std::string());
  h.tick();
  CHECK_EQ(h.take(), std::string("audio=[[0,2,1]]"));
  // Voice commands for ended voices are not sent.
  h.cmds.clear();
  CHECK(h.load(std::string(kAudioBundle) + "N.audioVoice(3, 2, 1, 1, 0, 0); N.audioVoice(6, 1, 0.25, 1, 0, 100);"));
  CHECK_EQ(h.cmds.size(), size_t(0));  // a new bundle: a new context, every voice was stopped
}

TEST(audio_voice_commands_bus_preload_and_stop_all) {
  AudioHost h;
  eval(h, R"(
    const v = play('loop.ogg', 'music', 1, 1, 0, true, 0, 0);
    N.audioVoice(6, v, 0.3, 0.9, 0.2, 250);
    N.audioVoice(4, v, 0.3, 0.9, 0.2, 0);
    N.audioVoice(5, v, 0.3, 0.9, 0.2, 0);
    N.audioVoice(2, v, 1, 1, 0, 0);  // not a voice op: ignored
    N.audioBus('music', 0.5, true, 300);
    N.postMessage('pre', N.audioPreload('sfx/boom.wav'));
    N.audioUnload('sfx/boom.wav');
    N.audioStopAll('music', 800);
    N.audioStopAll(null, 0);
  )");
  CHECK_EQ(h.take(), std::string("voice=1 pre=http://game.test/ui/sfx/boom.wav"));
  std::vector<int> ops;
  for (auto& c : h.cmds) ops.push_back(c.op);
  std::vector<int> want = {JSUI_AUDIO_PLAY, JSUI_AUDIO_SET,     JSUI_AUDIO_PAUSE,    JSUI_AUDIO_RESUME,
                           JSUI_AUDIO_BUS,  JSUI_AUDIO_PRELOAD, JSUI_AUDIO_UNLOAD,   JSUI_AUDIO_STOP_ALL,
                           JSUI_AUDIO_STOP_ALL};
  CHECK(ops == want);
  if (ops != want) return;
  CHECK_NEAR(h.cmds[1].volume, 0.3, 1e-6);
  CHECK_NEAR(h.cmds[1].pitch, 0.9, 1e-6);
  CHECK_NEAR(h.cmds[1].fade, 250, 1e-6);
  CHECK_EQ(h.cmds[4].bus, std::string("music"));
  CHECK_EQ(h.cmds[4].muted, 1);
  CHECK_NEAR(h.cmds[4].fade, 300, 1e-6);
  CHECK_EQ(h.cmds[5].sound, std::string("http://game.test/ui/sfx/boom.wav"));
  CHECK_EQ(h.cmds[7].bus, std::string("music"));
  CHECK(h.cmds[8].busNull);
  // Preload answer.
  jsui_audio_sound_loaded(h.rt, "http://game.test/ui/sfx/boom.wav", 1, 1.5);
  h.tick();
  CHECK_EQ(h.take(), std::string("audio=[[1,\"http://game.test/ui/sfx/boom.wav\",1,1.5]]"));
}

TEST(audio_reload_stops_every_voice_and_drops_their_reports) {
  AudioHost h;
  eval(h, "play('a.wav'); play('b.wav');");
  h.take();
  h.cmds.clear();
  jsui_audio_voice_ended(h.rt, 1, JSUI_AUDIO_FINISHED);  // queued for the old context
  jsui_reload(h.rt);
  CHECK(!h.cmds.empty());
  if (!h.cmds.empty()) {
    CHECK_EQ(h.cmds[0].op, int(JSUI_AUDIO_STOP_ALL));
    CHECK(h.cmds[0].busNull);
    CHECK_NEAR(h.cmds[0].fade, 0, 1e-6);
  }
  jsui_audio_voice_ended(h.rt, 2, JSUI_AUDIO_STOPPED);  // the handler's answer: ignored
  h.tick();
  CHECK_EQ(h.take(), std::string());
  // Voice ids keep counting (never reused).
  CHECK(h.load(std::string(kAudioBundle) + "play('c.wav');"));
  CHECK_EQ(h.take(), std::string("voice=3"));
}

TEST(audio_ended_inside_the_handler) {
  AudioHost h;
  h.endAtOnce = true;
  eval(h, "play('missing.wav');");
  h.take();
  h.tick();
  CHECK_EQ(h.take(), std::string("audio=[[0,1,-1]]"));
}

TEST(audio_disabled_fails_voices_and_preloads) {
  AudioHost h(true, true);
  eval(h, "play('a.wav'); N.audioPreload('b.wav'); N.audioBus('ui', 0, false, 0);");
  CHECK_EQ(h.cmds.size(), size_t(0));
  h.take();
  h.tick();
  CHECK_EQ(h.take(), std::string("audio=[[0,1,-1],[1,\"http://game.test/ui/b.wav\",0,-1]]"));
}

namespace {
struct FetchResult {
  int calls = 0, status = -1;
  std::string type, body;
};
}  // namespace

TEST(audio_jsui_fetch_reads_through_the_loader) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.base_url = "http://game.test/ui/bundle.js";
  cfg.audio_disabled = 1;
  struct Loader {
    std::vector<std::string> urls;
  } loader;
  cfg.load = [](void* u, JsuiRuntime* rt, uint64_t id, const char* url) {
    static_cast<Loader*>(u)->urls.push_back(url);
    std::string body = "RIFFdata";
    if (std::string(url).find("missing") != std::string::npos) jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
    else jsui_resource_complete(rt, id, 200, "audio/wav", body.data(), body.size());
  };
  cfg.load_user = &loader;
  JsuiRuntime* rt = jsui_create(&cfg);
  FetchResult ok, missing;
  auto done = [](void* u, int status, const char* type, const void* data, size_t size) {
    auto* r = static_cast<FetchResult*>(u);
    r->calls++;
    r->status = status;
    r->type = type ? type : "";
    r->body.assign(static_cast<const char*>(data), size);
  };
  jsui_fetch(rt, "sfx/click.wav", done, &ok);
  jsui_fetch(rt, "missing.wav", done, &missing);
  CHECK_EQ(ok.calls, 0);  // delivered inside a tick
  jsui_tick(rt, 16);
  CHECK_EQ(ok.calls, 1);
  CHECK_EQ(ok.status, 200);
  CHECK_EQ(ok.type, std::string("audio/wav"));
  CHECK_EQ(ok.body, std::string("RIFFdata"));
  CHECK_EQ(missing.calls, 1);
  CHECK_EQ(missing.status, 404);
  CHECK(!loader.urls.empty() && loader.urls[0] == "http://game.test/ui/sfx/click.wav");
  jsui_destroy(rt);
}
