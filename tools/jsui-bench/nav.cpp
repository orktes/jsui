// Showcase navigation: loads the site bundle, routes between pages, and prints
// per-frame tick+render times (p50 / p95 / p99 / max) and
// the GC collections and pauses (jsui_get_perf gc_*).
//   jsui-nav <bundle url or path> [pixel_ratio]
#include <jsui/jsui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static double nowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  int W = 1280, H = 800;
  float pr = argc > 2 ? float(atof(argv[2])) : 1.0f;
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.audio_disabled = 1;
  cfg.width = W;
  cfg.height = H;
  cfg.pixel_ratio = pr;
  cfg.js_timeout_ms = -1;
  cfg.log = [](void*, JsuiLogLevel l, const char* m) {
    if (l >= JSUI_LOG_WARN) fprintf(stderr, "[log] %s\n", m);
  };
  JsuiRuntime* rt = jsui_create(&cfg);
  if (jsui_load_bundle_url(rt, argv[1]) != 0) {
    fprintf(stderr, "load failed %s\n", jsui_last_error(rt));
    return 1;
  }
  int pw = int(W * pr), ph = int(H * pr);
  std::vector<uint32_t> px(size_t(pw) * ph);
  double t = 0;
  std::vector<double> frames;
  auto frame = [&](bool record) {
    double t0 = nowMs();
    jsui_tick(rt, t);
    JsuiRect d;
    jsui_render_rgba_incremental(rt, px.data(), pw, ph, size_t(pw) * 4, &d);
    if (record) frames.push_back(nowMs() - t0);
    t += 16.667;
  };
  for (int i = 0; i < 240; i++) frame(false);
  JsuiPerf p;
  jsui_get_perf(rt, &p, 1);
  const char* routes[] = {"/screens", "/motion", "/interactions", "/input", "/playground", "/", "/screens", "/motion"};
  for (int round = 0; round < 3; round++)
    for (const char* r : routes) {
      std::string j = std::string("\"") + r + "\"";
      jsui_post_message(rt, "route", j.c_str());
      for (int i = 0; i < 200; i++) frame(true);
    }
  jsui_get_perf(rt, &p, 1);
  std::sort(frames.begin(), frames.end());
  auto q = [&](double f) { return frames[std::min(frames.size() - 1, size_t(f * frames.size()))]; };
  printf("frames %zu  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms\n", frames.size(), q(0.5), q(0.95), q(0.99),
         frames.back());
  size_t over = 0;
  for (double f : frames) over += f > 16.7;
  printf("frames over 16.7 ms: %zu\n", over);
  printf("gc: %llu collections (%llu full), %.1f ms total, max pause %.2f ms\n", p.gc_collections,
         p.gc_full_collections, p.gc_us / 1000, p.gc_max_us / 1000);
  jsui_destroy(rt);
}
