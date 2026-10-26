// <Video> through the C API: the JS bridge's requests reaching the video
// handler (JsuiConfig.on_video) as JsuiVideoCommands with resolved source
// alternatives, jsui_video_status reports going back to JS (__jsui_onVideo)
// on the next tick (changes at once, the position at the progress rate) and
// to the "<texture>.time" buffer, CLOSE when the JS context goes away (and
// the texture removed), and every video failing without a handler. The
// @orktes/jsui-react <Video> is covered by packages/react/test/video.test.js.
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct Cmd {
  int op;
  uint64_t video;
  std::vector<std::string> sources, types;
  std::string url, texture;
  double time;
  float volume, rate;
  int paused, muted, loop;
};

struct VideoHost {
  JsuiRuntime* rt = nullptr;
  std::vector<Cmd> cmds;
  std::vector<std::string> messages;
  double t = 0;

  explicit VideoHost(bool handler = true) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 32;
    cfg.height = 32;
    cfg.base_url = "http://game.test/ui/bundle.js";
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<VideoHost*>(u)->messages.push_back(std::string(name) + "=" + (json ? json : ""));
    };
    cfg.message_user = this;
    if (handler) {
      cfg.on_video = [](void* u, const JsuiVideoCommand* c) {
        Cmd m{int(c->op), c->video, {}, {}, c->url ? c->url : "", c->texture ? c->texture : "", c->time_s,
              c->volume, c->rate, c->paused, c->muted, c->loop};
        for (int i = 0; i < c->source_count; i++) {
          m.sources.push_back(c->sources[i]);
          m.types.push_back(c->source_types && c->source_types[i] ? c->source_types[i] : "-");
        }
        static_cast<VideoHost*>(u)->cmds.push_back(m);
      };
      cfg.video_user = this;
    }
    rt = jsui_create(&cfg);
  }
  ~VideoHost() {
    if (rt) jsui_destroy(rt);
  }
  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "bundle.js") == 0; }
  void tick(double dt = 16) { jsui_tick(rt, t += dt); }
  std::string take() {
    std::string s;
    for (auto& m : messages) s += (s.empty() ? "" : " ") + m;
    messages.clear();
    return s;
  }
  void status(uint64_t video, int state, double duration, double position, int w = 0, int h = 0) {
    JsuiVideoStatus s{};
    s.state = state;
    s.duration_s = duration;
    s.position_s = position;
    s.buffered_s = position;
    s.width = w;
    s.height = h;
    jsui_video_status(rt, video, &s);
  }
};

// Posts every report __jsui_onVideo receives; open(...) posts the video id.
const char* kVideoBundle = R"(
  var N = __jsui_native;
  globalThis.__jsui_onVideo = (events) => N.postMessage('video', JSON.stringify(events));
  globalThis.open = (...a) => { const id = N.videoOpen(...a); N.postMessage('id', String(id)); return id; };
)";

void eval(VideoHost& h, const std::string& js) {
  CHECK(h.load(std::string(kVideoBundle) + js));
}

}  // namespace

TEST(video_open_reaches_the_handler_with_resolved_sources) {
  VideoHost h;
  eval(h, R"(
    open(['clips/intro.webm', 'https://cdn.test/intro.m3u8'], ['video/webm', 'application/vnd.apple.mpegurl'],
         'intro', false, true, 0.5, true, 1.5, 100);
    open('/movies/a.mp4', null, 'a');
  )");
  h.tick();
  CHECK_EQ(h.take(), std::string("id=1 id=2"));
  CHECK_EQ(h.cmds.size(), size_t(2));
  if (h.cmds.size() != 2) return;
  const Cmd& a = h.cmds[0];
  CHECK_EQ(a.op, int(JSUI_VIDEO_OPEN));
  CHECK_EQ(a.video, uint64_t(1));
  CHECK_EQ(a.sources.size(), size_t(2));
  CHECK_EQ(a.url, std::string("http://game.test/ui/clips/intro.webm"));
  CHECK_EQ(a.sources[1], std::string("https://cdn.test/intro.m3u8"));
  CHECK_EQ(a.types[0], std::string("video/webm"));
  CHECK_EQ(a.types[1], std::string("application/vnd.apple.mpegurl"));
  CHECK_EQ(a.texture, std::string("intro"));
  CHECK_EQ(a.paused, 0);
  CHECK_EQ(a.muted, 1);
  CHECK_EQ(a.loop, 1);
  CHECK(a.volume == 0.5f && a.rate == 1.5f);
  const Cmd& b = h.cmds[1];
  CHECK_EQ(b.url, std::string("http://game.test/movies/a.mp4"));
  CHECK_EQ(b.types[0], std::string("-"));  // no type
}

TEST(video_set_seek_close_reach_the_handler_and_close_removes_the_texture) {
  VideoHost h;
  eval(h, "open('a.mp4', null, 'clip');");
  h.tick();
  unsigned char px[4] = {1, 2, 3, 255};
  CHECK_EQ(jsui_set_texture_pixels(h.rt, "clip", px, 1, 1, 0, JSUI_TEXTURE_OPAQUE), 1);
  CHECK(h.load("__jsui_native.videoSet(1, true, false, 0.25, false, 2); __jsui_native.videoSeek(1, 42.5);"
               "__jsui_native.videoSeek(7, 1);"));  // unknown video: ignored
  h.tick();
  CHECK_EQ(h.cmds.size(), size_t(3));
  if (h.cmds.size() != 3) return;
  CHECK_EQ(h.cmds[1].op, int(JSUI_VIDEO_SET));
  CHECK_EQ(h.cmds[1].paused, 1);
  CHECK(h.cmds[1].volume == 0.25f && h.cmds[1].rate == 2.f);
  CHECK_EQ(h.cmds[2].op, int(JSUI_VIDEO_SEEK));
  CHECK(h.cmds[2].time == 42.5);
  CHECK(h.load("__jsui_native.videoClose(1); __jsui_native.videoSet(1, false, false, 1, false, 1);"));
  h.tick();
  CHECK_EQ(h.cmds.size(), size_t(4));  // the SET after CLOSE is ignored
  if (h.cmds.size() != 4) return;
  CHECK_EQ(h.cmds[3].op, int(JSUI_VIDEO_CLOSE));
  CHECK_EQ(h.cmds[3].texture, std::string("clip"));
  CHECK_EQ(jsui_remove_texture(h.rt, "clip"), 0);  // already removed with the video
}

TEST(video_status_reaches_js_changes_at_once_the_position_at_the_progress_rate) {
  VideoHost h;
  eval(h, "open('a.mp4', null, 'clip', false, false, 1, false, 1, 100);");
  h.tick();
  h.take();
  h.status(1, JSUI_VIDEO_LOADING, 0, 0);  // same as the open state: nothing new
  h.tick();
  CHECK_EQ(h.take(), std::string(""));
  h.status(1, JSUI_VIDEO_PLAYING, 10, 0, 640, 360);
  h.tick();
  CHECK_EQ(h.take(), std::string("video=[[1,2,10,0,0,640,360,\"\"]]"));
  // Position only: once per 100 ms; the latest one per tick.
  h.status(1, JSUI_VIDEO_PLAYING, 10, 0.05, 640, 360);
  h.status(1, JSUI_VIDEO_PLAYING, 10, 0.06, 640, 360);
  h.tick();
  CHECK_EQ(h.take(), std::string(""));  // within 100 ms of the last report JS heard
  h.tick(120);
  h.status(1, JSUI_VIDEO_PLAYING, 10, 0.2, 640, 360);
  h.tick();
  CHECK_EQ(h.take(), std::string("video=[[1,2,10,0.2,0.2,640,360,\"\"]]"));
  // ... but every report goes to the buffer.
  h.status(1, JSUI_VIDEO_PLAYING, 10, 5, 640, 360);
  JsuiBufferType type;
  size_t n = 0;
  const float* t = static_cast<const float*>(jsui_get_buffer(h.rt, "clip.time", &type, &n));
  CHECK(t && n == 3 && type == JSUI_BUFFER_F32);
  if (t && n == 3) CHECK(t[0] == 0.5f && t[1] == 0.5f && t[2] == 5.f);
  // A state change goes through at once, with the latest position.
  h.status(1, JSUI_VIDEO_ENDED, 10, 10, 640, 360);
  h.tick();
  CHECK_EQ(h.take(), std::string("video=[[1,4,10,10,10,640,360,\"\"]]"));
}

TEST(video_closes_when_the_js_context_goes_away) {
  VideoHost h;
  eval(h, "open('a.mp4', null, 'a'); open('b.mp4', null, 'b');");
  h.tick();
  jsui_reload(h.rt);
  h.tick();
  int closes = 0;
  for (auto& c : h.cmds) closes += c.op == JSUI_VIDEO_CLOSE;
  CHECK_EQ(closes, 2);
  h.status(1, JSUI_VIDEO_PLAYING, 1, 0);  // closed: ignored
  h.tick();
  CHECK_EQ(h.take().find("video="), std::string::npos);
}

TEST(video_fails_without_a_handler) {
  VideoHost h(false);
  eval(h, "open('a.mp4', null, 'clip');");
  h.tick();
  h.tick();
  std::string got = h.take();
  CHECK(got.find("video=[[1,-1,") != std::string::npos);
  CHECK(got.find("no video handler") != std::string::npos);
}
