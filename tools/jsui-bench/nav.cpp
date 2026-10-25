// Showcase navigation: loads the site bundle, routes between pages, and prints
// per-frame tick+render times (p50 / p95 / p99 / max), where they go by phase
// (jsui_set_profiling), the slowest frames broken down, and the GC
// collections and pauses (jsui_get_perf gc_*).
//
//   jsui-nav <bundle url or path> [pixel_ratio] [--backend cpu|graphite]
//            [--gpu-wait] [--rounds N] [--frames N] [--worst N] [--no-profile]
//
//   cpu       jsui_render_rgba_incremental into a retained buffer
//   graphite  Skia Graphite on Metal (macOS): jsui_render_gpu_target into a
//             texture, JSUI_GPU_INCREMENTAL once it holds a frame, skipping
//             frames jsui_needs_redraw() says are unchanged (like the hosts);
//             --gpu-wait also waits for the GPU each frame (counted as "gpu")
//
// --hover-grid instead hovers the cards of web/test/bench-apps/hover-grid.jsx
// (a 1280x720 viewport; the bundle is that file built), one card every
// --dwell frames (default 18: 300 ms) in the zigzag order of
// web/test/hover-bench.mjs, and prints the CPU per hover against that
// benchmark's Chrome CSS numbers: frames whose jsui_needs_redraw() is clear
// skip the render, as a host's would.
//
// JSUI_NAV_VERBOSE prints the frames GC ran on.
#include <jsui/jsui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __APPLE__
extern "C" {
int bench_mtl_init(void** device, void** queue);
void* bench_mtl_texture(void* device, int w, int h);
void bench_mtl_wait(void* queue);
void bench_mtl_release(void* obj);
}
#endif

static double nowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

namespace {

// One measured frame: wall time, the phases jsui_get_perf saw (ms), counters.
struct Frame {
  double ms = 0, tick = 0, render = 0, gpu = 0;
  double js = 0, commit = 0, animated = 0, style = 0, layout = 0, input = 0, paint = 0, other = 0, submit = 0;
  double gc = 0, decode = 0;
  unsigned long long paragraphs = 0, measures = 0, layouts = 0, bridge = 0, pipelines = 0, decodes = 0, gcs = 0;
  const char* route = "";
  int sinceRoute = 0;
};

struct Phase {
  const char* name;
  double Frame::*field;
};
const Phase kPhases[] = {{"js", &Frame::js},         {"commit", &Frame::commit}, {"animated", &Frame::animated},
                         {"style", &Frame::style},   {"layout", &Frame::layout}, {"input", &Frame::input},
                         {"paint", &Frame::paint},   {"submit", &Frame::submit}, {"other", &Frame::other},
                         {"gpu wait", &Frame::gpu}};

double pct(std::vector<double> v, double f) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(f * v.size()))];
}

}  // namespace

int main(int argc, char** argv) {
  const char* bundle = nullptr;
  float pr = 1.0f;
  std::string backend = "cpu";
  bool gpuWait = false, profile = true, hoverGrid = false;
  int rounds = 3, perRoute = 200, worstN = 12, dwell = 18;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--backend") backend = next();
    else if (a == "--gpu-wait") gpuWait = true;
    else if (a == "--rounds") rounds = std::max(1, atoi(next()));
    else if (a == "--frames") perRoute = std::max(1, atoi(next()));
    else if (a == "--worst") worstN = std::max(0, atoi(next()));
    else if (a == "--no-profile") profile = false;
    else if (a == "--hover-grid") hoverGrid = true;
    else if (a == "--dwell") dwell = std::max(1, atoi(next()));
    else if (!bundle) bundle = argv[i];
    else pr = float(atof(argv[i]));
  }
  if (!bundle) {
    fprintf(stderr, "usage: jsui-nav <bundle> [pixel_ratio] [--backend cpu|graphite] [--gpu-wait] [--rounds N]\n"
                    "                [--frames N] [--worst N] [--no-profile] [--hover-grid [--dwell N]]\n");
    return 2;
  }
  int W = 1280, H = hoverGrid ? 720 : 800;
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
  if (jsui_load_bundle_url(rt, bundle) != 0) {
    fprintf(stderr, "load failed %s\n", jsui_last_error(rt));
    return 1;
  }
  jsui_set_profiling(rt, profile ? 1 : 0);
  int pw = int(W * pr), ph = int(H * pr);
  std::vector<uint32_t> px;
  JsuiGpuContext* gpu = nullptr;
  void *mdev = nullptr, *mq = nullptr, *mtex = nullptr;
  if (backend == "graphite") {
#ifdef __APPLE__
    if (!bench_mtl_init(&mdev, &mq) || !(gpu = jsui_gpu_create_metal(mdev, mq))) {
      fprintf(stderr, "no Metal / Graphite\n");
      return 1;
    }
    mtex = bench_mtl_texture(mdev, pw, ph);
#else
    fprintf(stderr, "--backend graphite needs macOS (Metal)\n");
    return 1;
#endif
  } else if (backend == "cpu") {
    px.resize(size_t(pw) * ph);
  } else {
    fprintf(stderr, "unknown backend %s\n", backend.c_str());
    return 2;
  }
  bool holdsFrame = false;
  double t = 0;
  std::vector<Frame> frames;
  bool verbose = getenv("JSUI_NAV_VERBOSE") != nullptr;
  const char* route = "/";
  int sinceRoute = 0;
  bool painted = false;  // the last frame rendered
  auto frame = [&](bool record) {
    painted = false;
    double t0 = nowMs();
    jsui_tick(rt, t);
    double t1 = nowMs();
    if (gpu) {
#ifdef __APPLE__
      if (!holdsFrame || jsui_needs_redraw(rt)) {
        JsuiGpuTarget target{};
        target.texture = mtex;
        target.width = pw;
        target.height = ph;
        target.flags = holdsFrame ? JSUI_GPU_INCREMENTAL : 0;
        JsuiRect d;
        if (jsui_render_gpu_target(rt, gpu, &target, &d) == 0) holdsFrame = true;
        painted = true;
      }
#endif
    } else if (!holdsFrame || jsui_needs_redraw(rt)) {
      JsuiRect d;
      jsui_render_rgba_incremental(rt, px.data(), pw, ph, size_t(pw) * 4, &d);
      holdsFrame = painted = true;
    }
    double t2 = nowMs();
#ifdef __APPLE__
    if (gpu && gpuWait) bench_mtl_wait(mq);
#endif
    double t3 = nowMs();
    JsuiPerf p;
    jsui_get_perf(rt, &p, 1);
    if (record) {
      Frame f;
      f.ms = t3 - t0;
      f.tick = t1 - t0;
      f.render = t2 - t1;
      f.gpu = t3 - t2;
      f.js = p.js_us / 1000;
      f.commit = p.commit_us / 1000;
      f.animated = p.animated_us / 1000;
      f.style = p.style_us / 1000;
      f.layout = p.layout_us / 1000;
      f.input = p.input_us / 1000;
      f.paint = p.paint_us / 1000;
      f.submit = p.gpu_submit_us / 1000;
      // what the phases leave of the tick + render calls (not the GPU wait)
      f.other = std::max(0.0, f.tick + f.render -
                                  (f.js + f.commit + f.animated + f.style + f.layout + f.input + f.paint + f.submit));
      f.gc = p.gc_us / 1000;
      f.decode = p.image_decode_us / 1000;
      f.paragraphs = p.paragraph_builds;
      f.measures = p.text_measures;
      f.layouts = p.layout_passes;
      f.bridge = p.bridge_calls;
      f.pipelines = p.pipeline_compiles;
      f.decodes = p.image_decodes;
      f.gcs = p.gc_collections;
      f.route = route;
      f.sinceRoute = sinceRoute;
      frames.push_back(f);
      if (verbose && p.gc_collections)
        printf("  frame %zu: tick %.2f render %.2f ms, %llu gc %.2f ms\n", frames.size(), f.tick, f.render,
               p.gc_collections, f.gc);
    }
    sinceRoute++;
    t += 16.667;
  };
  for (int i = 0; i < 240; i++) frame(false);
  if (hoverGrid) {
    // Card centres (40 px padding, 150x170 cards, 18 px gaps, 6 per row),
    // neighbour to neighbour.
    std::vector<std::pair<float, float>> pts;
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 6; c++) {
        int i = r * 6 + (r % 2 ? 5 - c : c);
        pts.push_back({40.f + (i % 6) * 168 + 75, 40.f + (i / 6) * 188 + 85});
      }
    auto hover = [&](size_t k, bool record, std::vector<std::vector<double>>* byFrame, double* busy, int* paints) {
      auto [x, y] = pts[k % pts.size()];
      double t0 = nowMs();
      jsui_send_mouse_move(rt, x, y);
      double input = nowMs() - t0;
      if (busy) *busy += input;
      int n = 0;
      for (int i = 0; i < dwell; i++) {
        frame(record);
        if (!record) continue;
        *busy += frames.back().ms;
        if (painted) {
          if (byFrame && n < int(byFrame->size())) (*byFrame)[n].push_back(frames.back().ms + (n ? 0 : input));
          n++;
        }
      }
      if (paints) *paints += n;
    };
    jsui_send_mouse_move(rt, 8, 8);
    for (size_t k = 0; k < 6; k++) hover(k, false, nullptr, nullptr, nullptr);  // warm masks, glyphs, pipelines
    jsui_send_mouse_move(rt, 8, 8);
    for (int i = 0; i < 30; i++) frame(false);
    int hovers = rounds * int(pts.size()), paints = 0;
    double busy = 0;
    std::vector<std::vector<double>> byFrame(4);
    for (int k = 0; k < hovers; k++) hover(size_t(k), true, &byFrame, &busy, &paints);
    printf("hover-grid %s%s, %dx%d px, %d hovers every %d frames\n", backend.c_str(),
           gpu && gpuWait ? " (+gpu wait)" : "", pw, ph, hovers, dwell);
    printf("CPU per hover: %.2f ms (tick + render of every frame, input included), %.2f painted frames\n",
           busy / hovers, double(paints) / hovers);
    auto mean = [](const std::vector<double>& v) {
      double s = 0;
      for (double x : v) s += x;
      return v.empty() ? 0 : s / v.size();
    };
    for (size_t i = 0; i < byFrame.size(); i++)
      printf("  painted frame %zu: mean %.3f  p95 %.3f ms\n", i + 1, mean(byFrame[i]), pct(byFrame[i], 0.95));
    double idle = 0;
    int idleN = 0;
    for (int i = 0; i < 60; i++) {
      frame(true);
      if (!painted) idle += frames.back().ms, idleN++;
    }
    printf("  idle frame (tick only): %.3f ms\n", idleN ? idle / idleN : 0);
    if (profile) {
      printf("phase means per frame (ms):");
      for (auto& ph : kPhases) {
        double sum = 0;
        for (auto& f : frames) sum += f.*ph.field;
        printf(" %s %.3f", ph.name, sum / frames.size());
      }
      printf("\n");
    }
    jsui_destroy(rt);
    if (gpu) jsui_gpu_destroy(gpu);
#ifdef __APPLE__
    bench_mtl_release(mtex);
    bench_mtl_release(mq);
    bench_mtl_release(mdev);
#endif
    return 0;
  }
  const char* routes[] = {"/screens", "/motion", "/interactions", "/input", "/playground", "/", "/screens", "/motion"};
  for (int round = 0; round < rounds; round++)
    for (const char* r : routes) {
      std::string j = std::string("\"") + r + "\"";
      route = r;
      sinceRoute = 0;
      jsui_post_message(rt, "route", j.c_str());
      for (int i = 0; i < perRoute; i++) frame(true);
    }

  std::vector<double> all;
  for (auto& f : frames) all.push_back(f.ms);
  printf("backend %s%s, %dx%d px\n", backend.c_str(), gpu && gpuWait ? " (+gpu wait)" : "", pw, ph);
  printf("frames %zu  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms\n", all.size(), pct(all, 0.5), pct(all, 0.95),
         pct(all, 0.99), pct(all, 1.0));
  size_t over = 0;
  for (double f : all) over += f > 16.7;
  printf("frames over 16.7 ms: %zu\n", over);
  if (profile) {
    // Where the time goes: total over the run, and over the frames that
    // matter (the slowest 5%).
    double cut = pct(all, 0.95);
    printf("\nphase       total ms   mean   p95    slowest-5%% mean\n");
    for (auto& ph : kPhases) {
      std::vector<double> v;
      double sum = 0, slowSum = 0;
      size_t slowN = 0;
      for (auto& f : frames) {
        v.push_back(f.*ph.field);
        sum += f.*ph.field;
        if (f.ms >= cut) slowSum += f.*ph.field, slowN++;
      }
      printf("%-10s %9.1f %6.3f %6.3f %8.3f\n", ph.name, sum, sum / frames.size(), pct(v, 0.95),
             slowN ? slowSum / slowN : 0);
    }
    unsigned long long para = 0, meas = 0, lay = 0, pipes = 0, decs = 0;
    for (auto& f : frames) para += f.paragraphs, meas += f.measures, lay += f.layouts, pipes += f.pipelines,
                           decs += f.decodes;
    printf("counters: %llu paragraph builds, %llu text measures, %llu layout passes, %llu pipeline compiles, "
           "%llu image decodes\n",
           para, meas, lay, pipes, decs);
    // Per route: the transition (first 30 frames after the route change)
    // and the rest.
    printf("\nroute          transition p95 / max     steady p95 / max\n");
    for (const char* r : routes) {
      bool seen = false;
      for (const char* q : routes) {
        if (q == r) break;
        if (!strcmp(q, r)) seen = true;
      }
      if (seen) continue;
      std::vector<double> tr, st;
      for (auto& f : frames)
        if (!strcmp(f.route, r)) (f.sinceRoute < 30 ? tr : st).push_back(f.ms);
      printf("%-14s %8.2f / %6.2f       %6.2f / %6.2f\n", r, pct(tr, 0.95), pct(tr, 1.0), pct(st, 0.95),
             pct(st, 1.0));
    }
    if (worstN > 0) {
      std::vector<const Frame*> w;
      for (auto& f : frames) w.push_back(&f);
      std::sort(w.begin(), w.end(), [](const Frame* a, const Frame* b) { return a->ms > b->ms; });
      printf("\nslowest frames (ms): route+frame  total = js commit anim style layout input paint submit other "
             "gpu | gc decode | para meas layouts bridge pipes\n");
      for (int i = 0; i < worstN && i < int(w.size()); i++) {
        const Frame& f = *w[i];
        printf("%s+%-3d %6.2f = %5.2f %5.2f %5.2f %5.2f %6.2f %5.2f %6.2f %5.2f %5.2f %5.2f | %5.2f %5.2f | %llu %llu "
               "%llu %llu %llu\n",
               f.route, f.sinceRoute, f.ms, f.js, f.commit, f.animated, f.style, f.layout, f.input, f.paint, f.submit,
               f.other, f.gpu, f.gc, f.decode, f.paragraphs, f.measures, f.layouts, f.bridge, f.pipelines);
      }
    }
  }
  JsuiPerf p;
  jsui_get_perf(rt, &p, 0);
  unsigned long long gcs = 0;
  double gcMs = 0, gcMax = 0;
  for (auto& f : frames) gcs += f.gcs, gcMs += f.gc, gcMax = std::max(gcMax, f.gc);
  printf("\ngc: %llu collections, %.1f ms total, worst frame's gc %.2f ms\n", gcs, gcMs, gcMax);
  jsui_destroy(rt);
  if (gpu) jsui_gpu_destroy(gpu);
#ifdef __APPLE__
  bench_mtl_release(mtex);
  bench_mtl_release(mq);
  bench_mtl_release(mdev);
#endif
}
