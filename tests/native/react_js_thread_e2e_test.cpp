// JS on its own thread with a real @orktes/jsui-react bundle
// (packages/cli/test/e2e/stall.jsx), through the C API:
// - while JS is stalled (80 ms busy handlers, garbage collection), every
//   frame's markers follow this frame's host buffer (native Animated:
//   interpolation and Animated.derive too), a fling keeps going and the
//   tick + render stays short;
// - jsui_set_state + JSUI_TICK_SYNC_JS shows the new state on that frame;
// - jsui_sync gives up on busy JS and never applies half a commit.
// Built and run by the @orktes/jsui-cli test suite with
// JSUI_REACT_STALL_BUNDLE=<path>; skipped without it.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/value.h"
#include "js/js_thread.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define JSUI_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define JSUI_TEST_SANITIZED 1
#endif
#endif

namespace {

constexpr int kMarkers = 32;

double wallMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct StallE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::vector<uint32_t> pixels = std::vector<uint32_t>(400 * 300);
  int stallsOk = 0, stallsFailed = 0;

  StallE2E() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = JSUI_JS_THREAD_ON;
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<StallE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
  }
  ~StallE2E() { jsui_destroy(rt); }
  jsui::Runtime& runtime() { return *jsui::internalRuntime(rt); }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) == 0;
  }
  bool has(const std::string& name) const {
    for (auto& m : messages)
      if (m.first == name) return true;
    return false;
  }
  jsui::Node* find(const std::string& nativeID) {
    std::function<jsui::Node*(jsui::Node*)> walk = [&](jsui::Node* n) -> jsui::Node* {
      if (n->props()["nativeID"].asString() == nativeID) return n;
      for (jsui::Node* k : n->children())
        if (jsui::Node* f = walk(k)) return f;
      return nullptr;
    };
    return walk(runtime().doc.root());
  }
  // The nativeIDs starting with `prefix`, in tree order.
  void collect(jsui::Node* n, const std::string& prefix, std::vector<std::string>& out) {
    const std::string& id = n->props()["nativeID"].asString();
    if (id.rfind(prefix, 0) == 0) out.push_back(id);
    for (jsui::Node* k : n->children()) collect(k, prefix, out);
  }
  static double transform(jsui::Node* n, size_t i, const char* key) {
    if (!n) return -1e9;
    const auto& a = n->props()["style"]["transform"].asArray();
    return i < a.size() ? a[i][key].asNumber(-1e9) : -1e9;
  }
  void stall(int ms) {
    std::string args = "{\"ms\":" + std::to_string(ms) + "}";
    jsui_call_ui(
        rt, "stall", args.c_str(),
        [](void* u, uint64_t, int ok, const char*) {
          auto* self = static_cast<StallE2E*>(u);
          (ok ? self->stallsOk : self->stallsFailed)++;
        },
        this);
  }
  // Ticks (synced) until the bundle rendered.
  bool start(const char* path, std::vector<float>& pos) {
    jsui_set_buffer(rt, "pos", JSUI_BUFFER_F32, pos.data(), pos.size());
    if (!load(path)) return false;
    for (int i = 0; i < 100 && !(has("ready") && find("m" + std::to_string(kMarkers - 1))); i++)
      jsui_tick_ex(rt, now += 16, JSUI_TICK_SYNC_JS);
    return has("ready");
  }
};

// The pairs the bundle shows: whole ones only (a commit is applied whole).
bool pairsWhole(StallE2E& e, size_t* count) {
  std::vector<std::string> ids;
  e.collect(e.runtime().doc.root(), "p", ids);
  std::vector<std::string> a, b;
  for (auto& id : ids) (id[1] == 'a' ? a : b).push_back(id.substr(2));
  if (count) *count = a.size();
  return a == b;
}

}  // namespace

// Acceptance A: UI that follows host buffers through native Animated stays
// right every frame while JS is stalled (two 80 ms handlers, a garbage
// collection), a fling keeps going and the owner's frames stay short.
TEST(react_js_thread_e2e_stall) {
  const char* path = std::getenv("JSUI_REACT_STALL_BUNDLE");
  if (!path) {
    std::printf("  (skipped: JSUI_REACT_STALL_BUNDLE not set)\n");
    return;
  }
  if (!jsui::JsThread::available()) return;
  StallE2E e;
  CHECK_EQ(jsui_js_thread(e.rt), int(JSUI_JS_THREAD_ON));
  std::vector<float> pos(kMarkers, 0.f);
  CHECK(e.start(path, pos));
  std::vector<jsui::Node*> markers;
  for (int i = 0; i < kMarkers; i++) markers.push_back(e.find("m" + std::to_string(i)));
  for (jsui::Node* m : markers) CHECK(m != nullptr);
  jsui::Node* list = e.find("list");
  CHECK(list != nullptr);
  if (!list || std::find(markers.begin(), markers.end(), nullptr) != markers.end()) return;
  jsui::Runtime& r = e.runtime();

  const int frames = 240;
  const int stallMs = 80;
  std::vector<double> frameMs;
  int wrong = 0, busyFrames = 0, flingStalls = 0, flingStuck = 0;
  float lastScroll = 0;
  double t0 = wallMs();
  for (int f = 1; f <= frames; f++) {
    // Frames paced as a host would (16 ms).
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point() +
                                  std::chrono::microseconds(int64_t((t0 + f * 16.0) * 1000)));
    for (int i = 0; i < kMarkers; i++) pos[i] = float(std::fmod(f * 3.0 + i * 7.0, 300.0));
    jsui_set_buffer(e.rt, "pos", JSUI_BUFFER_F32, pos.data(), pos.size());
    // A fling before each stall: dragged 30 px a frame, then released.
    for (int start : {20, 110}) {
      if (f == start) jsui_send_touch(e.rt, 0, 0, 300, 260);
      if (f > start && f <= start + 3) jsui_send_touch(e.rt, 0, 1, 300, 260 - 30.f * float(f - start));
      if (f == start + 4) jsui_send_touch(e.rt, 0, 2, 300, 170);
    }
    if (f == 30 || f == 120) e.stall(stallMs);
    if (f == 170) jsui_post_message(e.rt, "garbage", "null");
    if (f == 180) r.inbound.task([&r] { r.js->collectNow(); });
    bool busy = r.jsBusy();
    double a = wallMs();
    jsui_tick(e.rt, e.now += 16);
    jsui_render_rgba(e.rt, e.pixels.data(), 400, 300, 400 * 4);
    frameMs.push_back(wallMs() - a);
    busy = busy || r.jsBusy();
    if (busy) busyFrames++;

    // This frame's buffer, on this frame.
    for (int i = 0; i < kMarkers; i++) {
      double x = StallE2E::transform(markers[i], 0, "translateX");
      double y = StallE2E::transform(markers[i], 1, "translateY");
      double wantX = i == 1 ? pos[i] * 2.0 + 1 : pos[i];
      double wantY = i == 0 ? pos[i] / 2.0 : 0;
      if (std::abs(x - wantX) > 1e-3 || std::abs(y - wantY) > 1e-3) {
        if (wrong++ < 5) std::fprintf(stderr, "  frame %d marker %d: %g, %g (want %g, %g)\n", f, i, x, y, wantX, wantY);
      }
    }
    // The fling goes on while JS is busy.
    if (busy && ((f > 24 && f <= 36) || (f > 114 && f <= 126))) {
      flingStalls++;
      if (!(list->scrollY > lastScroll)) flingStuck++;
    }
    lastScroll = list->scrollY;
  }
  CHECK_EQ(wrong, 0);
  CHECK(busyFrames >= 4);
  CHECK(flingStalls >= 4);
  CHECK_EQ(flingStuck, 0);
  CHECK(lastScroll > 500);

  std::vector<double> sorted = frameMs;
  std::sort(sorted.begin(), sorted.end());
  double p50 = sorted[sorted.size() / 2], p95 = sorted[sorted.size() * 95 / 100], p100 = sorted.back();
  std::printf("  owner frame (tick + render) with JS stalled: p50 %.3f ms, p95 %.3f ms, max %.3f ms; "
              "JS busy on %d of %d frames\n",
              p50, p95, p100, busyFrames, frames);
#if !defined(JSUI_TEST_SANITIZED) && defined(NDEBUG)
  CHECK(p95 < 4.0);
  CHECK(p100 < stallMs / 10.0);
#endif

  // The stalls answered, the garbage is gone.
  CHECK_EQ(jsui_sync(e.rt, 5000), 1);
  jsui_tick(e.rt, e.now += 16);
  CHECK_EQ(e.stallsOk, 2);
  CHECK_EQ(e.stallsFailed, 0);
  CHECK(r.js->gcStats(false).collections > 0);
}

// Acceptance B: a sync tick shows state set before it on that frame;
// jsui_sync gives up on busy JS, and nothing applies half a commit.
TEST(react_js_thread_e2e_sync) {
  const char* path = std::getenv("JSUI_REACT_STALL_BUNDLE");
  if (!path) {
    std::printf("  (skipped: JSUI_REACT_STALL_BUNDLE not set)\n");
    return;
  }
  if (!jsui::JsThread::available()) return;
  StallE2E e;
  std::vector<float> pos(kMarkers, 0.f);
  CHECK(e.start(path, pos));
  jsui::Node* label = e.find("label");
  CHECK(label != nullptr);
  if (!label) return;

  // Host state on the frame it was set for.
  int late = 0;
  for (int i = 0; i < 20; i++) {
    std::string v = "v" + std::to_string(i);
    jsui_set_state(e.rt, "label", ("\"" + v + "\"").c_str());
    jsui_tick_ex(e.rt, e.now += 16, JSUI_TICK_SYNC_JS);
    std::string shown = label->children().empty() ? "" : label->children()[0]->text();
    if (shown != v && late++ < 3) std::fprintf(stderr, "  frame %d shows '%s'\n", i, shown.c_str());
  }
  CHECK_EQ(late, 0);

  // 500 ms of busy JS: a sync with a 30 ms limit gives up.
  jsui_post_message(e.rt, "pair", "null");
  e.stall(500);
  double a = wallMs();
  CHECK_EQ(jsui_sync(e.rt, 30), 0);
  double waited = wallMs() - a;
  CHECK(waited >= 25 && waited < 250);
  size_t pairs = 0;
  CHECK(pairsWhole(e, &pairs));

  // Random ticks, syncs, stalls and commits: only whole pairs, ever.
  std::mt19937 rng(1234);
  int posted = 1, split = 0;
  for (int i = 0; i < 300; i++) {
    switch (rng() % 6) {
      case 0:
      case 1:
        jsui_post_message(e.rt, "pair", "null");
        posted++;
        break;
      case 2:
        e.stall(int(rng() % 8));
        break;
      case 3:
        jsui_tick(e.rt, e.now += 16);
        break;
      case 4:
        jsui_tick_ex(e.rt, e.now += 16, JSUI_TICK_SYNC_JS);
        break;
      default:
        jsui_sync(e.rt, double(rng() % 4));
        break;
    }
    if (!pairsWhole(e, nullptr)) split++;
  }
  CHECK_EQ(split, 0);
  CHECK_EQ(jsui_sync(e.rt, -1), 1);
  CHECK(pairsWhole(e, &pairs));
  CHECK_EQ(pairs, size_t(posted));
  jsui_tick(e.rt, e.now += 16);
  CHECK_EQ(e.stallsFailed, 0);
}
