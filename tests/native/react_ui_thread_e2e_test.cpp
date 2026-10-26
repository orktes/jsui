// A UI thread (jsui_record_gpu_frame, the Godot integration's ui_thread):
// the host's main thread sends input, host buffers, state, messages and hit
// tests, a second thread ticks the runtime and records each frame (layout,
// damage, paint), and the main thread replays the recording into its
// retained frame. The runtime is handed from one thread to the other, never
// used by both at once; JS runs on its own thread. With the real
// @orktes/jsui-react bundle of react_js_thread_e2e (stall.jsx: markers
// following a host buffer through native Animated, a fling, host state, a
// commit of two views), every frame must equal a single-threaded runtime
// fed the same, rendered whole (the two run one after the other). Run under
// ThreadSanitizer in CI.
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "js/js_thread.h"
#include "jsui/jsui.h"
#include "render/painter.h"
#include "runtime.h"
#include "surface.h"
#include "test.h"

namespace {

constexpr int kW = 400, kH = 300, kMarkers = 32;

struct Host {
  std::vector<std::string> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  Host() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = JSUI_JS_THREAD_ON;
    cfg.width = kW;
    cfg.height = kH;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<Host*>(u)->messages.push_back(std::string(name) + "=" + (json ? json : ""));
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
  }
  ~Host() { jsui_destroy(rt); }
  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "stall.js") == 0; }
};

// Runs jobs on a thread of its own, one at a time; wait() returns once the
// last one has run (the hand-off: what the job did happens before).
class Worker {
 public:
  Worker() : thread_([this] { loop(); }) {}
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(m_);
      quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  void run(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(m_);
      job_ = std::move(job);
    }
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [&] { return !job_; });
  }

 private:
  void loop() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
      cv_.wait(lock, [&] { return quit_ || job_; });
      if (quit_) return;
      std::function<void()> job = job_;
      lock.unlock();
      job();
      lock.lock();
      job_ = nullptr;
      cv_.notify_all();
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::function<void()> job_;
  bool quit_ = false;
  std::thread thread_;
};

sk_sp<SkSurface> wrap(std::vector<uint32_t>& px) {
  SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  SkSurfaceProps props = jsui::surfaceProps();
  return SkSurfaces::WrapPixels(info, px.data(), kW * 4, &props);
}

// One run of the scenario: with `uiThread`, ticked and recorded on a thread
// of its own and replayed into a retained frame on this one; otherwise
// ticked and rendered whole here. The two runs go one after the other
// (one runtime at a time).
struct Run {
  std::vector<std::vector<uint32_t>> frames;
  std::vector<int> hits;
  std::vector<std::string> messages;
  int painted = 0;
  bool dropOk = true;
};

Run runScenario(const std::string& src, bool uiThread) {
  Run out;
  Host h;
  std::vector<float> pos(kMarkers, 0.f);
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos.data(), pos.size());
  CHECK(h.load(src));
  if (uiThread) {
    // Graphite contexts only: the host renders as usual otherwise.
    JsuiGpuTarget t{};
    t.texture = &t;
    t.width = kW;
    t.height = kH;
    JsuiGpuFrame* f = reinterpret_cast<JsuiGpuFrame*>(&t);
    CHECK_EQ(jsui_record_gpu_frame(h.rt, nullptr, &t, &f), -1);
    CHECK(f == nullptr);
  }
  std::unique_ptr<Worker> ui = uiThread ? std::make_unique<Worker>() : nullptr;
  std::vector<uint32_t> px(kW * kH, 0);
  sk_sp<SkSurface> retained = wrap(px);
  jsui::Surface& surface = jsui::internalRuntime(h.rt)->main();
  const void* key = px.data();
  auto record = [&](bool tick) {
    std::unique_ptr<jsui::Surface::RecordedFrame> rec;
    ui->run([&] {
      if (tick) jsui_tick_ex(h.rt, h.now += 16, JSUI_TICK_SYNC_JS);
      jsui::Surface::CanvasChoice choice{nullptr, key, true};
      choice.splitDamage = true;
      rec = surface.recordFrame(key, kW, kH, choice, /*growCurves=*/true, /*gpu=*/false);
    });
    ui->wait();
    return rec;
  };
  for (int f = 0; f < 160; f++) {
    // Main thread: this frame's input and host data.
    for (int i = 0; i < kMarkers; i++) pos[i] = float((f * 3 + i * 7) % 380);
    jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos.data(), pos.size());
    if (f % 15 == 5) {
      std::string label = "\"frame " + std::to_string(f) + "\"";
      jsui_set_state(h.rt, "label", label.c_str());
    }
    if (f % 40 == 20) jsui_post_message(h.rt, "pair", "null");
    // A fling on the list, and the mouse moving over the markers.
    if (f == 30) jsui_send_touch(h.rt, 0, 0, 300, 260);
    if (f > 30 && f <= 33) jsui_send_touch(h.rt, 0, 1, 300, 260 - 30.f * float(f - 30));
    if (f == 34) jsui_send_touch(h.rt, 0, 2, 300, 170);
    jsui_send_mouse_move(h.rt, float((f * 5) % kW), float((f * 2) % kH));
    out.hits.push_back(jsui_hit_test(h.rt, 250.f, float(165 + (f % 100))));
    if (uiThread) {
      // The UI thread ticks and records; this thread replays.
      std::unique_ptr<jsui::Surface::RecordedFrame> rec = record(true);
      CHECK(rec != nullptr);
      if (!rec) break;
      if (!surface.replayFrame(*rec, retained->getCanvas()).isEmpty()) out.painted++;
    } else {
      jsui_tick_ex(h.rt, h.now += 16, JSUI_TICK_SYNC_JS);
      jsui_render_rgba(h.rt, px.data(), kW, kH, kW * 4);
    }
    out.frames.push_back(px);
  }
  if (uiThread) {
    // A recording the host drops: the next one repaints fully.
    std::unique_ptr<jsui::Surface::RecordedFrame> rec = record(true);
    surface.discardFrame(*rec);
    out.dropOk = jsui_needs_redraw(h.rt) == 1;
    rec = record(false);
    out.dropOk = out.dropOk && rec && rec->painted == SkIRect::MakeWH(kW, kH);
  }
  out.messages = h.messages;
  return out;
}

}  // namespace

TEST(react_ui_thread_e2e) {
  const char* path = std::getenv("JSUI_REACT_STALL_BUNDLE");
  if (!path) {
    std::printf("  (skipped: JSUI_REACT_STALL_BUNDLE not set)\n");
    return;
  }
  if (!jsui::JsThread::available()) return;
  std::ifstream file(path, std::ios::binary);
  std::string src((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  CHECK(!src.empty());
  Run a = runScenario(src, true), b = runScenario(src, false);
  int mismatched = 0, hitMismatches = 0, maxDiff = 0;
  CHECK_EQ(a.frames.size(), b.frames.size());
  for (size_t f = 0; f < std::min(a.frames.size(), b.frames.size()); f++) {
    int bad = 0;
    for (size_t i = 0; i < a.frames[f].size(); i++) {
      uint32_t x = a.frames[f][i], y = b.frames[f][i];
      if (x == y) continue;
      int d = 0;
      for (int c = 0; c < 32; c += 8) d = std::max(d, std::abs(int((x >> c) & 0xff) - int((y >> c) & 0xff)));
      maxDiff = std::max(maxDiff, d);
      if (d > 1) bad++;
    }
    if (bad) mismatched++;
    if (a.hits[f] != b.hits[f]) hitMismatches++;
  }
  std::printf("  %zu frames, %d painted, %d differ from the reference (max channel diff %d), %d hit tests differ\n",
              a.frames.size(), a.painted, mismatched, maxDiff, hitMismatches);
  CHECK_EQ(a.frames.size(), size_t(160));
  CHECK(a.painted > 100);  // the markers move every frame
  CHECK_EQ(mismatched, 0);
  CHECK_EQ(hitMismatches, 0);
  CHECK(a.messages == b.messages);
  CHECK(a.dropOk);
}
