// LottieView (render/lottie.h): Skottie compositions from inline data and
// data: URIs, intrinsic sizing, the progress style, native playback on the
// runtime clock (autoPlay, loop, speed, segments, pause / resume / reset),
// finish events, colorFilters, repaints limited to the view, failures, and
// the JS bridge (__jsui_native.lottieData / lottieCommand). Skipped when
// the build has no Skottie (JSUI_LOTTIE off), except the failure path.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/lottie.h"
#include "render/painter.h"
#include "js/engine.h"
#include "render_util.h"
#include "test.h"

using rtest::Scene;

namespace {

// 100 x 50, 30 fps, frames 0..60 (2 s): a 20 x 20 blue square (layer
// "box", fill "Fill 1") whose center moves linearly from x 10 to 90 at y 25.
const char kComp[] = R"J({"v":"5.7.4","fr":30,"ip":0,"op":60,"w":100,"h":50,"nm":"test","ddd":0,"assets":[],
"layers":[{"ddd":0,"ind":1,"ty":4,"nm":"box","sr":1,"ip":0,"op":60,"st":0,"bm":0,"ao":0,
 "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},
  "p":{"a":1,"k":[{"t":0,"s":[10,25,0],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":60,"s":[90,25,0]}]}},
 "shapes":[{"ty":"gr","nm":"group","it":[
  {"ty":"rc","nm":"rect","d":1,"p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[20,20]},"r":{"a":0,"k":0}},
  {"ty":"fl","nm":"Fill 1","c":{"a":0,"k":[0,0,1,1]},"o":{"a":0,"k":100},"r":1},
  {"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}]}]}]})J";

constexpr uint32_t kBlue = 0xffff0000, kRed = 0xff0000ff;

struct LottieScene : Scene {
  LottieScene(int w, int h) : Scene(w, h) {}
  // tick(now) + a full CPU render (Scene::render always ticks at 0).
  void frame(double now) {
    px.assign(size_t(dw()) * dh(), 0);
    rt->tick(now);
    rt->render(px.data(), dw(), dh(), size_t(dw()) * 4);
  }
  // A LottieView of kComp with `props` / `style` (JSON object bodies).
  jsui::Node* view(const std::string& props, const std::string& style) {
    std::string key = rt->lotties->addData(kComp);
    return add(R"({"type":"LottieView","props":{"source":")" + key + "\"" + (props.empty() ? "" : "," + props) +
               R"(},"style":{)" + style + "}}");
  }
  // x of the square's center on row `y` (-1: not drawn).
  double squareX(int y = 25) const {
    int first = -1, last = -1;
    for (int x = 0; x < dw(); x++)
      if (a(x, y) > 128 && b(x, y) > 128) {
        if (first < 0) first = x;
        last = x;
      }
    return first < 0 ? -1 : (first + last + 1) / 2.0;
  }
};

const double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

TEST(lottie_progress_and_intrinsic_size) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(200, 100);
  // No size: the composition's 100 x 50.
  jsui::Node* v = s.view("", R"("alignSelf":"flex-start","lottieProgress":0.5)");
  s.frame(0);
  s.save("lottie_progress");
  CHECK_NEAR(v->layoutRect().width, 100, 0.01);
  CHECK_NEAR(v->layoutRect().height, 50, 0.01);
  CHECK_NEAR(s.squareX(), 50, 1);
  CHECK_EQ(s.at(50, 25), kBlue);
  CHECK_EQ(s.a(10, 25), 0);
  // progress 0 / 1: the first and the last frame.
  v->patchStyle({{"lottieProgress", jsui::Value(0.0)}});
  s.frame(16);
  CHECK_NEAR(s.squareX(), 10, 1);
  v->patchStyle({{"lottieProgress", jsui::Value(1.0)}});
  s.frame(32);
  CHECK_NEAR(s.squareX(), 90, 1);
  // Not playing on its own: no frames wanted.
  CHECK(!s.rt->wantsFrame());
}

TEST(lottie_resize_modes) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(200, 100);
  // 200 x 100 box, contain (the default): scaled 2x, square center 100.
  s.view("", R"("width":200,"height":100,"lottieProgress":0.5)");
  s.frame(0);
  CHECK_NEAR(s.squareX(50), 100, 1.5);
  CHECK_EQ(s.at(100, 50), kBlue);
  CHECK_EQ(s.a(70, 50), 0);  // the square is 40 wide now: 80..120
  CHECK_EQ(s.at(85, 50), kBlue);
  LottieScene c(200, 100);
  // center: never scales up.
  c.view(R"("resizeMode":"center")", R"("width":200,"height":100,"lottieProgress":0.5)");
  c.frame(0);
  CHECK_EQ(c.at(100, 50), kBlue);
  CHECK_EQ(c.a(85, 50), 0);
}

TEST(lottie_autoplay_plays_on_the_runtime_clock) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(100, 50);
  jsui::Node* v = s.view(R"("autoPlay":true,"loop":false)", R"("width":100,"height":50)");
  s.frame(1000);  // loads and starts here
  CHECK_NEAR(s.squareX(), 10, 1);
  CHECK(s.rt->wantsFrame());
  s.frame(2000);  // 1 s of 2 s
  CHECK_NEAR(s.squareX(), 50, 1.5);
  CHECK(s.rt->needsRedraw() == false);
  s.frame(2500);
  CHECK_NEAR(s.squareX(), 70, 1.5);
  s.frame(4000);  // past the end: the last frame, stopped
  CHECK_NEAR(s.squareX(), 90, 1);
  auto* st = jsui::lottie::ensure(v);
  CHECK(!st->playing);
  CHECK_NEAR(st->pos, 1, 1e-9);
  CHECK(!s.rt->wantsFrame());
}

TEST(lottie_loop_speed_and_reverse) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(100, 50);
  jsui::Node* v = s.view(R"("autoPlay":true,"speed":2)", R"("width":100,"height":50)");
  s.frame(0);
  s.frame(500);  // 2x: half way
  CHECK_NEAR(s.squareX(), 50, 1.5);
  s.frame(1250);  // looped: 2.5 s of playback = 0.25
  CHECK_NEAR(s.squareX(), 30, 1.5);
  CHECK(s.rt->wantsFrame());
  CHECK(jsui::lottie::ensure(v)->playing);
  // Backwards: from the end.
  LottieScene r(100, 50);
  r.view(R"("autoPlay":true,"loop":false,"speed":-1)", R"("width":100,"height":50)");
  r.frame(0);
  CHECK_NEAR(r.squareX(), 90, 1);
  r.frame(1000);
  CHECK_NEAR(r.squareX(), 50, 1.5);
  r.frame(3000);
  CHECK_NEAR(r.squareX(), 10, 1);
}

TEST(lottie_commands_segments_pause_resume_reset) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(100, 50);
  jsui::Node* v = s.view(R"("loop":false)", R"("width":100,"height":50)");
  // Before the load: remembered and applied when it loads.
  CHECK(jsui::lottie::command(v, "play", 30, 60));
  s.frame(0);
  CHECK_NEAR(s.squareX(), 50, 1);  // frame 30
  s.frame(500);                    // 15 frames later
  CHECK_NEAR(s.squareX(), 70, 1.5);
  CHECK(jsui::lottie::command(v, "pause", kNaN, kNaN));
  s.frame(900);
  CHECK_NEAR(s.squareX(), 70, 1.5);
  CHECK(!s.rt->wantsFrame());
  CHECK(jsui::lottie::command(v, "resume", kNaN, kNaN));
  s.frame(1000);  // re-anchors here
  s.frame(1250);
  CHECK_NEAR(s.squareX(), 80, 1.5);
  // A reset while playing a segment cancels it: back to frame 0, stopped.
  CHECK(jsui::lottie::command(v, "reset", kNaN, kNaN));
  bool cancelled = false;
  for (auto& e : s.rt->lotties->events)
    if (e.kind == jsui::LottieCache::Event::Kind::Finished && e.cancelled) cancelled = true;
  CHECK(cancelled);
  s.frame(1300);
  CHECK_NEAR(s.squareX(), 10, 1);
  // play() with no frames: the whole composition from the start; backwards
  // segments (end < start) play down.
  CHECK(jsui::lottie::command(v, "play", 60, 0));
  s.frame(1400);
  CHECK_NEAR(s.squareX(), 90, 1);
  s.frame(2400);
  CHECK_NEAR(s.squareX(), 50, 1.5);
  CHECK(!jsui::lottie::command(v, "explode", kNaN, kNaN));
}

TEST(lottie_color_filters) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(100, 100);
  // The same source three times: plain, recolored by layer name, and by a
  // keypath that matches nothing.
  s.view("", R"("position":"absolute","top":0,"width":100,"height":50,"lottieProgress":0.5)");
  s.view(R"("colorFilters":[{"keypath":"box","color":"#ff0000"}])",
         R"("position":"absolute","top":50,"width":100,"height":50,"lottieProgress":0.5)");
  s.frame(0);
  CHECK_EQ(s.at(50, 25), kBlue);
  CHECK_EQ(s.at(50, 75), kRed);
  LottieScene k(100, 50);
  k.view(R"("colorFilters":[{"keypath":"**.Fill 1","color":"#ff0000"},{"keypath":"nope","color":"#00ff00"}])",
         R"("width":100,"height":50,"lottieProgress":0.5)");
  k.frame(0);
  CHECK_EQ(k.at(50, 25), kRed);
}

// A playing view repaints only itself.
TEST(lottie_playback_repaints_only_the_view) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(300, 200);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":300,"height":200,"backgroundColor":"#202020"}})J");
  s.view(R"("autoPlay":true)", R"("position":"absolute","left":150,"top":100,"width":100,"height":50)");
  int dw = s.dw(), dh = s.dh();
  std::vector<uint32_t> buf(size_t(dw) * dh, 0);
  s.rt->tick(0);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  s.rt->tick(16);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  s.rt->tick(500);
  CHECK(s.rt->needsRedraw());
  SkIRect dirty = s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(SkIRect::MakeXYWH(149, 99, 102, 52).contains(dirty));
  if (!SkIRect::MakeXYWH(149, 99, 102, 52).contains(dirty))
    std::fprintf(stderr, "  dirty %d %d %d %d\n", dirty.left(), dirty.top(), dirty.width(), dirty.height());
  CHECK(jsui::lastPaintStats(s.doc()).nodesPainted <= 3);
}

TEST(lottie_data_uri_and_failures) {
  LottieScene s(100, 50);
  // A data: URI source (percent-encoded JSON).
  std::string enc;
  for (unsigned char c : std::string(kComp)) {
    char buf[4];
    if (std::isalnum(c)) enc += char(c);
    else {
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      enc += buf;
    }
  }
  jsui::Node* ok = s.add(R"({"type":"LottieView","props":{"source":{"uri":"data:application/json,)" + enc +
                         R"("}},"style":{"position":"absolute","width":100,"height":50,"lottieProgress":0.5}})");
  jsui::Node* bad = s.add(R"({"type":"LottieView","props":{"source":")" + s.rt->lotties->addData("{\"nope\":1}") +
                          R"("},"style":{"position":"absolute","width":100,"height":50}})");
  s.frame(0);
  if (jsui::LottieCache::available()) {
    CHECK_NEAR(s.squareX(), 50, 1);
    // JSON, but not a composition: failed once parsed.
    CHECK(s.rt->lotties->request(jsui::lottie::ensure(bad)->url) == jsui::LottieCache::State::Failed);
    CHECK(!s.rt->lotties->get(jsui::lottie::ensure(bad)->url, jsui::Value()));
    CHECK(!s.rt->lotties->error(jsui::lottie::ensure(bad)->url).empty());
  } else {
    CHECK(s.rt->lotties->request(jsui::lottie::ensure(ok)->url) == jsui::LottieCache::State::Failed);
  }
  CHECK(!s.rt->wantsFrame());
}

// The bridge: lottieData / lottieCommand and the load / finish events, as
// @orktes/jsui-react's LottieView uses them.
TEST(lottie_js_bridge_events) {
  if (!jsui::LottieCache::available()) return;
  LottieScene s(100, 50);
  std::string js = std::string("var comp = ") + kComp + R"J(;
var n = __jsui_native;
globalThis.log = [];
globalThis.__jsui_dispatchEvent = function (ev) { log.push(ev.type + (ev.isCancelled !== undefined ? ':' + ev.isCancelled : '')); return 1; };
var key = n.lottieData(JSON.stringify(comp));
if (key !== n.lottieData(JSON.stringify(comp))) throw new Error('keys differ');
if (n.lottie !== true) throw new Error('no lottie');
var id = n.createNode('LottieView');
n.setProps(id, { source: key, loop: false, style: { width: 100, height: 50 }, __events: ['load', 'animationFinish'] });
n.appendChild(n.rootId, id);
globalThis.view = id;
)J";
  CHECK(s.rt->loadSource(js, "lottie.js"));
  s.frame(0);
  s.frame(16);
  auto expect = [&](const std::string& cond) {
    return s.rt->js->eval("if (!(" + cond + ")) throw new Error('log: ' + log.join(','));", "check.js");
  };
  CHECK(expect("log.join(',') === 'load'"));
  CHECK(expect("__jsui_native.lottieCommand(view, 'play', 45) === true"));
  s.frame(100);
  s.frame(700);  // 15 frames: done
  s.frame(716);
  CHECK(expect("log.join(',') === 'load,animationfinish:false'"));
  CHECK_NEAR(s.squareX(), 90, 1);
}
