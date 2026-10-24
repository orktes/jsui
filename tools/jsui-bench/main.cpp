// jsui-bench: runs a bundle headlessly for N frames on a simulated 60 Hz
// clock and reports where each frame's time goes before (and in) painting.
//
//   jsui-bench bundle.js [--frames 600] [--warmup 120] [--width 1280] [--height 720]
//              [--paint incremental|full|none] [--mouse-sweep X,Y0,Y1,FRAMES]
//              [--patch-state KEY,N] [--surfaces N[,changing|same|one|resize]]
//              [--markers N,json|buffer|native] [--wheel X,Y,DY[,FRAMES]]
//              [--react-stats] [--name NAME] [--json out.json] [--dump out.pam]
//   jsui-bench --suite DIR [--json out.json] [--compare baseline.json]
//
// --suite runs the standard scenarios from DIR (built by tools/jsui-bench/run.sh):
//   interactions  examples/showcase/interactions.jsx (constant native animations)
//   interactions-wheel  the same with a wheel turned over its armory list every
//                 frame (12 px, the direction flipping every 60 frames)
//   list          2000 static rows, one counter changing every frame
//   menu          hover springs; the mouse sweeps over the buttons
//   inventory     500 slots, one jsui_patch_state per frame
//   idle          the inventory with no updates (should do no work)
//   surfaces-*    30 root views (jsui_surface_create) of one bundle: static,
//                 every surface's props changing every frame, the same props
//                 pushed every frame, one surface's props changing per frame,
//                 every surface resized every frame
//   markers-*     N enemy markers moving every frame (scenarios/markers.jsx),
//                 fed as JSON shared state, as a frame data buffer rendered by
//                 React, or as a buffer driving Animated.hostBuffer natively;
//                 for N = 30, 100, 1000 (painting off: the data path only)
//
// Phase times are exclusive (core/perf.h), in microseconds per frame:
//   js        QuickJS execution (timers, rAF, React render + reconcile, events)
//   commit    native side of the bridge (setProps / tree edits / value conversion)
//   animated  native-driven Animated evaluation
//   style     style resolution (applyStyle)
//   layout    Yoga + text measurement
//   input     hit testing / event routing
//   paint     jsui_render_rgba(_incremental)
//   host      the host's own share of pushing data (building the JSON text /
//             filling the buffer) up to the jsui_* call; counted in total
// Counters are per frame: bridge calls, style resolves, Yoga nodes laid out
// (not served from Yoga's cache), text measure callbacks, allocations, calls
// into JS (jsCalls) and microtasks run (jsJobs), messages JS posted to the
// host, ... --react-stats adds React commits and the components each commit
// rendered (through React's DevTools hook, so it costs JS time itself).
// --dump writes the last frame (fully repainted, plus each surface's) as a
// binary PAM (RGBA) image, to compare two builds of a bundle pixel by pixel.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include <yoga/event/event.h>

// Under `valgrind --tool=callgrind --instr-atstart=no`, only the measured
// frames are profiled (not bundle evaluation, mount or warmup).
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_START_INSTRUMENTATION
#define CALLGRIND_STOP_INSTRUMENTATION
#endif

#include "core/document.h"
#include "core/perf.h"
#include "js/engine.h"
#include "jsui/jsui.h"
#include "runtime.h"

// ------------------------------------------------------------ allocations
// Counts heap allocations of the whole process. On glibc every malloc is
// counted (QuickJS, Skia, Yoga and operator new all end up there); elsewhere
// only C++ operator new.
static uint64_t gAllocs = 0;
#if defined(__GLIBC__)
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* malloc(size_t n) {
  gAllocs++;
  return __libc_malloc(n);
}
void* calloc(size_t a, size_t b) {
  gAllocs++;
  return __libc_calloc(a, b);
}
void* realloc(void* p, size_t n) {
  gAllocs++;
  return __libc_realloc(p, n);
}
}
#else
void* operator new(size_t n) {
  gAllocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) {
  gAllocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif

namespace {

// ------------------------------------------------------- Yoga pass counts
struct YogaCounts {
  uint64_t layouts = 0, cachedLayouts = 0, measures = 0, cachedMeasures = 0;
};
YogaCounts gYoga;

void subscribeYoga() {
  using facebook::yoga::Event;
  Event::subscribe([](YGNodeConstRef, Event::Type type, Event::Data data) {
    if (type != Event::LayoutPassEnd) return;
    const auto* d = data.get<Event::LayoutPassEnd>().layoutData;
    if (!d) return;
    gYoga.layouts += uint64_t(d->layouts);
    gYoga.cachedLayouts += uint64_t(d->cachedLayouts);
    gYoga.measures += uint64_t(d->measureCallbacks);
    gYoga.cachedMeasures += uint64_t(d->cachedMeasures);
  });
}

bool readFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

enum class PaintMode { Incremental, Full, None };

struct Options {
  std::string name, bundle;
  int frames = 600, warmup = 120;
  int width = 1280, height = 720;
  PaintMode paint = PaintMode::Incremental;
  bool mouseSweep = false;
  float sweepX = 0, sweepY0 = 0, sweepY1 = 0;
  int sweepFrames = 120;
  std::string patchKey;
  int patchSlots = 0;
  // Root views of app "Plate" (120x40) next to the main surface, and what the
  // host does with their props every frame.
  int surfaces = 0;
  enum class SurfaceProps { None, Changing, Same, One, Resize } surfaceProps = SurfaceProps::None;
  // Enemy markers (scenarios/markers.jsx) moving every frame, and how their
  // positions reach the UI.
  int markers = 0;
  enum class MarkerFeed { Json, Buffer, Native } markerFeed = MarkerFeed::Json;
  // A mouse wheel turned at (wheelX, wheelY) every frame, wheelDy px, the
  // direction flipping every wheelFrames frames.
  bool wheel = false;
  float wheelX = 0, wheelY = 0, wheelDy = 12;
  int wheelFrames = 60;
  bool reactStats = false;
  std::string dump;
};

// Columns of the report.
const char* const kPhaseNames[] = {"js", "commit", "animated", "style", "layout", "input", "paint"};
const jsui::Phase kPhases[] = {jsui::Phase::Js,     jsui::Phase::Commit, jsui::Phase::Animated, jsui::Phase::Style,
                               jsui::Phase::Layout, jsui::Phase::Input,  jsui::Phase::Paint};
constexpr int kPhaseCount = 7;

struct CounterDef {
  const char* name;
  uint64_t (*get)(const jsui::PerfCounters&);
};
const CounterDef kCounters[] = {
    {"bridgeCalls", [](const jsui::PerfCounters& c) { return c.bridgeCalls; }},
    {"setProps", [](const jsui::PerfCounters& c) { return c.setProps; }},
    {"setPropsSkipped", [](const jsui::PerfCounters& c) { return c.setPropsSkipped; }},
    {"styleResolves", [](const jsui::PerfCounters& c) { return c.styleResolves; }},
    {"valueConversions", [](const jsui::PerfCounters& c) { return c.valueConversions; }},
    {"layoutPasses", [](const jsui::PerfCounters& c) { return c.layoutPasses; }},
    {"layoutVisits", [](const jsui::PerfCounters& c) { return c.layoutVisits; }},
    {"textMeasures", [](const jsui::PerfCounters& c) { return c.textMeasures; }},
    {"paragraphBuilds", [](const jsui::PerfCounters& c) { return c.paragraphBuilds; }},
    {"animatedWrites", [](const jsui::PerfCounters& c) { return c.animatedWrites; }},
    {"events", [](const jsui::PerfCounters& c) { return c.events; }},
    {"jsCalls", [](const jsui::PerfCounters& c) { return c.jsCalls; }},
    {"jsJobs", [](const jsui::PerfCounters& c) { return c.jsJobs; }},
    {"hitNodes", [](const jsui::PerfCounters& c) { return c.hitNodes; }},
};
constexpr int kCounterCount = int(sizeof(kCounters) / sizeof(kCounters[0]));
// Extra counters measured by the tool itself.
const char* const kExtraNames[] = {"yogaLayouts", "yogaCachedLayouts", "yogaMeasures", "paragraphLayouts", "allocs",
                                   "hostMessages", "reactCommits", "reactRenders"};
constexpr int kExtraCount = 8;

// --react-stats: a minimal React DevTools hook, installed before the bundle
// runs. Counts commits and the class / function components each commit
// rendered: the fibers of the committed tree with PerformedWork, walking only
// into subtrees React cloned (an untouched subtree keeps the old child).
const char kReactStatsHook[] = R"JS(
(function () {
  var s = (globalThis.__jsuiBenchReact = { commits: 0, renders: 0 });
  function walk(f) {
    for (; f; f = f.sibling) {
      var t = f.tag;
      if ((t === 0 || t === 1 || t === 11 || t === 14 || t === 15) && (f.flags & 1)) s.renders++;
      if (!f.alternate || f.child !== f.alternate.child) walk(f.child);
    }
  }
  globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__ = {
    supportsFiber: true,
    renderers: new Map(),
    inject: function () { return 1; },
    onCommitFiberRoot: function (id, root) { s.commits++; walk(root.current.child); },
    onCommitFiberUnmount: function () {},
    onPostCommitFiberRoot: function () {},
  };
})();
)JS";

struct BenchMessages {
  uint64_t count = 0;
  double reactCommits = 0, reactRenders = 0;
};

struct Stat {
  double mean = 0, p50 = 0, p95 = 0;
};

Stat summarize(std::vector<double> v) {
  Stat s;
  if (v.empty()) return s;
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / double(v.size());
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))]; };
  s.p50 = pct(0.5);
  s.p95 = pct(0.95);
  return s;
}

struct Result {
  std::string name;
  int frames = 0;
  int nodes = 0;
  Stat phase[kPhaseCount];
  Stat host;  // the host's own work (see the header)
  Stat total;
  double counters[kCounterCount] = {};
  double extra[kExtraCount] = {};
};

int countNodes(jsui::Node* n) {
  int c = 1;
  for (jsui::Node* k : n->children()) c += countNodes(k);
  return c;
}

bool run(const Options& o, Result* out) {
  std::string src;
  if (!readFile(o.bundle, &src)) {
    std::fprintf(stderr, "jsui-bench: cannot read %s\n", o.bundle.c_str());
    return false;
  }
  if (o.reactStats) src = kReactStatsHook + src;
  BenchMessages messages;
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.audio_disabled = 1;  // headless: no sound device
  cfg.width = o.width;
  cfg.height = o.height;
  cfg.js_timeout_ms = -1;
  cfg.log = [](void*, JsuiLogLevel level, const char* msg) {
    if (level >= JSUI_LOG_WARN) std::fprintf(stderr, "[bench] %s\n", msg);
  };
  cfg.message_user = &messages;
  cfg.on_message = [](void* user, const char* name, const char* json) {
    auto* m = static_cast<BenchMessages*>(user);
    if (std::strcmp(name, "__jsui_bench:react") == 0) {
      std::sscanf(json, "[%lf,%lf]", &m->reactCommits, &m->reactRenders);
      return;
    }
    m->count++;
  };
  JsuiRuntime* h = jsui_create(&cfg);
  jsui::Runtime* rt = jsui::internalRuntime(h);
  jsui::Perf& perf = rt->doc.perf;
  if (o.markers > 0) {
    const char* app = o.markerFeed == Options::MarkerFeed::Json     ? "JsonMarkers"
                      : o.markerFeed == Options::MarkerFeed::Buffer ? "BufferMarkers"
                                                                    : "NativeMarkers";
    jsui_surface_run(h, app, "{}");
  }
  if (jsui_load_bundle_source(h, src.data(), src.size(), o.bundle.c_str()) != 0) {
    std::fprintf(stderr, "jsui-bench: %s failed to load: %s\n", o.bundle.c_str(), jsui_last_error(h));
    jsui_destroy(h);
    return false;
  }
  std::vector<uint32_t> px(size_t(o.width) * size_t(o.height));
  const size_t rowBytes = size_t(o.width) * 4;
  constexpr int kPlateW = 120, kPlateH = 40;
  auto plateProps = [](int i, int frame) {
    char props[96];
    std::snprintf(props, sizeof props, "{\"name\":\"Enemy %d\",\"hp\":%d,\"level\":%d}", i, (i * 13 + frame) % 101,
                  1 + i % 60);
    return std::string(props);
  };
  std::vector<JsuiRuntime*> surfaces;
  std::vector<std::vector<uint32_t>> platePx;
  for (int i = 0; i < o.surfaces; i++) {
    surfaces.push_back(jsui_surface_create(h, "Plate", plateProps(i, 0).c_str(), kPlateW, kPlateH, 1));
    platePx.emplace_back(size_t(kPlateW) * kPlateH);
  }
  double now = 0;
  int frame = 0;
  // Markers: x, y, hp per enemy, circling around the screen.
  std::vector<float> enemyData(size_t(o.markers) * 3);
  std::string enemyJson;
  double hostUs = 0;
  auto pushMarkers = [&]() {
    for (int i = 0; i < o.markers; i++) {
      double a = double(frame) * 0.02 + double(i) * 0.37;
      double r = 80 + double(i % 7) * 40;
      enemyData[size_t(i) * 3] = float(o.width / 2 + std::cos(a) * r * 1.6);
      enemyData[size_t(i) * 3 + 1] = float(o.height / 2 + std::sin(a) * r);
      enemyData[size_t(i) * 3 + 2] = float((i * 7 + frame) % 101);
    }
    // The game has its data in its own structures; a buffer can take it as
    // is (here: already packed), JSON has to be written out.
    auto t0 = std::chrono::steady_clock::now();
    if (o.markerFeed == Options::MarkerFeed::Json) {
      enemyJson.clear();
      enemyJson += '[';
      char item[96];
      for (int i = 0; i < o.markers; i++) {
        std::snprintf(item, sizeof item, "%s[%.2f,%.2f,%g]", i ? "," : "", double(enemyData[size_t(i) * 3]),
                      double(enemyData[size_t(i) * 3 + 1]), double(enemyData[size_t(i) * 3 + 2]));
        enemyJson += item;
      }
      enemyJson += ']';
    }
    hostUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    // The jsui_* call itself (parsing the JSON / copying the buffer) is
    // the runtime's work: counted as commit (native side of the bridge).
    jsui::Perf::Scope scope(perf, jsui::Phase::Commit);
    if (o.markerFeed == Options::MarkerFeed::Json) jsui_set_state(h, "enemies", enemyJson.c_str());
    else jsui_set_buffer(h, "enemies", JSUI_BUFFER_F32, enemyData.data(), enemyData.size());
  };
  if (o.wheel) jsui_send_mouse_move(h, o.wheelX, o.wheelY);
  auto step = [&]() {
    now += 1000.0 / 60.0;
    if (o.markers > 0) pushMarkers();
    if (o.wheel) {
      // Input arrives between frames, before the tick (like a browser's).
      float dy = (frame / o.wheelFrames) % 2 ? -o.wheelDy : o.wheelDy;
      jsui_set_event_time(h, now - 4);
      jsui_send_mouse_wheel(h, o.wheelX, o.wheelY, 0, dy);
    }
    if (o.mouseSweep) {
      // Triangle wave over the button column.
      double t = double(frame % o.sweepFrames) / double(o.sweepFrames);
      double k = t < 0.5 ? t * 2 : 2 - t * 2;
      jsui_send_mouse_move(h, o.sweepX, float(o.sweepY0 + (o.sweepY1 - o.sweepY0) * k));
    }
    if (!o.patchKey.empty() && o.patchSlots > 0) {
      char patch[128];
      std::snprintf(patch, sizeof patch, "{\"slots\":{\"%d\":{\"count\":%d}}}", (frame * 7) % o.patchSlots,
                    1 + frame % 97);
      jsui_patch_state(h, o.patchKey.c_str(), patch);
    }
    if (o.surfaceProps == Options::SurfaceProps::Resize) {
      for (size_t i = 0; i < surfaces.size(); i++)
        jsui_set_viewport(surfaces[i], float(kPlateW - 30 + (frame + int(i)) % 30), kPlateH, 1);
    } else if (o.surfaceProps != Options::SurfaceProps::None) {
      jsui::Perf::Scope host(perf, jsui::Phase::Js);  // the host's call into JS
      for (size_t i = 0; i < surfaces.size(); i++) {
        // One: only surface frame % N gets new props (the others the same).
        bool changes = o.surfaceProps == Options::SurfaceProps::Changing ||
                       (o.surfaceProps == Options::SurfaceProps::One && int(i) == frame % int(surfaces.size()));
        jsui_surface_set_props(surfaces[i], plateProps(int(i), changes ? frame : 0).c_str());
      }
    }
    jsui_tick(h, now);
    if (o.paint != PaintMode::None && jsui_needs_redraw(h)) {
      jsui::Perf::Scope paint(perf, jsui::Phase::Paint);
      if (o.paint == PaintMode::Full) jsui_render_rgba(h, px.data(), o.width, o.height, rowBytes);
      else jsui_render_rgba_incremental(h, px.data(), o.width, o.height, rowBytes, nullptr);
    }
    for (size_t i = 0; o.paint != PaintMode::None && i < surfaces.size(); i++) {
      if (!jsui_needs_redraw(surfaces[i])) continue;
      jsui::Perf::Scope paint(perf, jsui::Phase::Paint);
      if (o.paint == PaintMode::Full) jsui_render_rgba(surfaces[i], platePx[i].data(), kPlateW, kPlateH, kPlateW * 4);
      else jsui_render_rgba_incremental(surfaces[i], platePx[i].data(), kPlateW, kPlateH, kPlateW * 4, nullptr);
    }
    frame++;
  };
  for (int i = 0; i < o.warmup; i++) step();
  if (o.reactStats) rt->js->eval("__jsuiBenchReact.commits = __jsuiBenchReact.renders = 0;", "bench");
  uint64_t messages0 = messages.count;

  std::vector<double> phase[kPhaseCount], host, total;
  for (auto& v : phase) v.reserve(size_t(o.frames));
  perf.profiling = true;
  perf.reset();
  YogaCounts y0 = gYoga;
  uint64_t pl0 = jsui::gParagraphLayouts, a0 = gAllocs;
  double prevUs[size_t(jsui::Phase::Count)] = {};
  CALLGRIND_START_INSTRUMENTATION;
  for (int i = 0; i < o.frames; i++) {
    step();
    double sum = 0;
    for (int p = 0; p < kPhaseCount; p++) {
      size_t idx = size_t(kPhases[p]);
      double d = perf.us[idx] - prevUs[idx];
      phase[p].push_back(d);
      sum += d;
    }
    host.push_back(hostUs);
    total.push_back(sum + hostUs);
    std::copy(std::begin(perf.us), std::end(perf.us), prevUs);
  }
  CALLGRIND_STOP_INSTRUMENTATION;
  perf.profiling = false;
  uint64_t allocs = gAllocs - a0;
  out->name = o.name;
  out->frames = o.frames;
  out->nodes = 0;
  for (const auto& s : rt->surfaces()) out->nodes += countNodes(s->doc.root());
  for (int p = 0; p < kPhaseCount; p++) out->phase[p] = summarize(phase[p]);
  out->host = summarize(host);
  out->total = summarize(total);
  double f = double(o.frames);
  for (int c = 0; c < kCounterCount; c++) out->counters[c] = double(kCounters[c].get(perf.counters)) / f;
  out->extra[0] = double(gYoga.layouts - y0.layouts) / f;
  out->extra[1] = double(gYoga.cachedLayouts - y0.cachedLayouts) / f;
  out->extra[2] = double(gYoga.measures - y0.measures) / f;
  out->extra[3] = double(jsui::gParagraphLayouts - pl0) / f;
  out->extra[4] = double(allocs) / f;
  out->extra[5] = double(messages.count - messages0) / f;
  if (o.reactStats) {
    rt->js->eval("__jsui_native.postMessage('__jsui_bench:react', "
                 "JSON.stringify([__jsuiBenchReact.commits, __jsuiBenchReact.renders]));",
                 "bench");
    out->extra[6] = messages.reactCommits / f;
    out->extra[7] = messages.reactRenders / f;
  }
  if (!o.dump.empty()) {
    // The main surface, then each surface below it, in one image.
    int h2 = o.height + int(surfaces.size()) * kPlateH;
    std::vector<uint32_t> img(size_t(o.width) * size_t(h2), 0);
    jsui_render_rgba(h, img.data(), o.width, o.height, rowBytes);
    for (size_t i = 0; i < surfaces.size(); i++) {
      jsui_render_rgba(surfaces[i], platePx[i].data(), kPlateW, kPlateH, kPlateW * 4);
      for (int y = 0; y < kPlateH; y++)
        std::copy_n(platePx[i].data() + size_t(y) * kPlateW, std::min(kPlateW, o.width),
                    img.data() + (size_t(o.height) + i * kPlateH + y) * o.width);
    }
    std::ofstream f(o.dump, std::ios::binary);
    f << "P7\nWIDTH " << o.width << "\nHEIGHT " << h2 << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    f.write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size() * 4));
  }
  if (const char* err = jsui_last_error(h); err && *err) std::fprintf(stderr, "[bench] %s: last error: %s\n", o.name.c_str(), err);
  jsui_destroy(h);
  return true;
}

void print(const Result& r) {
  std::printf("\n== %s  (%d frames, %d nodes)\n", r.name.c_str(), r.frames, r.nodes);
  std::printf("  %-10s %10s %10s %10s\n", "phase(us)", "mean", "p50", "p95");
  for (int p = 0; p < kPhaseCount; p++)
    std::printf("  %-10s %10.1f %10.1f %10.1f\n", kPhaseNames[p], r.phase[p].mean, r.phase[p].p50, r.phase[p].p95);
  if (r.host.p95 > 0) std::printf("  %-10s %10.1f %10.1f %10.1f\n", "host", r.host.mean, r.host.p50, r.host.p95);
  std::printf("  %-10s %10.1f %10.1f %10.1f\n", "total", r.total.mean, r.total.p50, r.total.p95);
  std::printf("  per frame:");
  int col = 0;
  auto item = [&](const char* n, double v) {
    if (col++ % 4 == 0) std::printf("\n   ");
    std::printf(" %-18s %9.1f", n, v);
  };
  for (int c = 0; c < kCounterCount; c++) item(kCounters[c].name, r.counters[c]);
  for (int c = 0; c < kExtraCount; c++) item(kExtraNames[c], r.extra[c]);
  std::printf("\n");
}

std::string toJson(const std::vector<Result>& results) {
  std::ostringstream s;
  s << "{\n";
  for (size_t i = 0; i < results.size(); i++) {
    const Result& r = results[i];
    s << "  \"" << r.name << "\": {\"frames\": " << r.frames << ", \"nodes\": " << r.nodes;
    auto stat = [&](const char* n, const Stat& st) {
      s << ", \"" << n << "\": {\"mean\": " << st.mean << ", \"p50\": " << st.p50 << ", \"p95\": " << st.p95 << "}";
    };
    for (int p = 0; p < kPhaseCount; p++) stat(kPhaseNames[p], r.phase[p]);
    stat("host", r.host);
    stat("total", r.total);
    s << ", \"counters\": {";
    for (int c = 0; c < kCounterCount; c++) s << (c ? ", " : "") << "\"" << kCounters[c].name << "\": " << r.counters[c];
    for (int c = 0; c < kExtraCount; c++) s << ", \"" << kExtraNames[c] << "\": " << r.extra[c];
    s << "}}" << (i + 1 < results.size() ? "," : "") << "\n";
  }
  s << "}\n";
  return s.str();
}

// Minimal reader for our own JSON output: "scenario" -> "key" -> mean (or
// counter value), enough for --compare.
std::map<std::string, std::map<std::string, double>> readBaseline(const std::string& path) {
  std::map<std::string, std::map<std::string, double>> out;
  std::string text;
  if (!readFile(path, &text)) return out;
  jsui::Value v = jsui::Value::parseJson(text);
  for (auto& [name, r] : v.asObject()) {
    for (int p = 0; p < kPhaseCount; p++) out[name][kPhaseNames[p]] = r[kPhaseNames[p]]["mean"].asNumber();
    if (r["host"].isObject()) out[name]["host"] = r["host"]["mean"].asNumber();
    out[name]["total"] = r["total"]["mean"].asNumber();
    for (auto& [k, c] : r["counters"].asObject()) out[name][k] = c.asNumber();
  }
  return out;
}

void compare(const std::vector<Result>& results, const std::string& path) {
  auto base = readBaseline(path);
  if (base.empty()) {
    std::fprintf(stderr, "jsui-bench: no baseline in %s\n", path.c_str());
    return;
  }
  std::printf("\n== compared with %s (mean per frame: before -> after)\n", path.c_str());
  for (const Result& r : results) {
    auto it = base.find(r.name);
    if (it == base.end()) continue;
    auto& b = it->second;
    std::printf("  %s\n", r.name.c_str());
    auto line = [&](const char* k, double now) {
      if (!b.count(k)) {  // not in the baseline (an older jsui-bench)
        std::printf("    %-18s %10s -> %10.1f\n", k, "-", now);
        return;
      }
      double was = b[k];
      char delta[32] = "";
      if (was > 0) std::snprintf(delta, sizeof delta, "%+.0f%%", (now / was - 1) * 100);
      std::printf("    %-18s %10.1f -> %10.1f  %s\n", k, was, now, delta);
    };
    for (int p = 0; p < kPhaseCount; p++) line(kPhaseNames[p], r.phase[p].mean);
    if (r.host.mean > 0) line("host", r.host.mean);
    line("total", r.total.mean);
    for (int c = 0; c < kCounterCount; c++) line(kCounters[c].name, r.counters[c]);
    for (int c = 0; c < kExtraCount; c++) line(kExtraNames[c], r.extra[c]);
  }
}

std::vector<Options> suite(const std::string& dir, const Options& base) {
  std::vector<Options> list;
  auto add = [&](const char* name) -> Options& {
    Options o = base;
    o.name = name;
    o.bundle = dir + "/" + name + ".js";
    list.push_back(o);
    return list.back();
  };
  add("interactions");
  // The armory FlatList on the right of interactions.jsx (frame-bench.mjs
  // --scenario scroll turns the wheel at the same spot).
  Options& wheel = add("interactions-wheel");
  wheel.bundle = dir + "/interactions.js";
  wheel.wheel = true;
  wheel.wheelX = float(base.width - 200);
  wheel.wheelY = std::round(float(base.height) * 0.55f);
  add("list");
  Options& menu = add("menu");
  menu.mouseSweep = true;
  menu.sweepX = 200;
  menu.sweepY0 = 150;
  menu.sweepY1 = 620;
  menu.sweepFrames = 240;
  Options& inv = add("inventory");
  inv.patchKey = "inventory";
  inv.patchSlots = 500;
  // The inventory with nothing changing: an idle frame should cost ~nothing.
  Options& idle = add("idle");
  idle.bundle = dir + "/inventory.js";
  for (auto [name, props] : {std::pair{"surfaces-static", Options::SurfaceProps::None},
                             std::pair{"surfaces-props", Options::SurfaceProps::Changing},
                             std::pair{"surfaces-same-props", Options::SurfaceProps::Same},
                             std::pair{"surfaces-one-props", Options::SurfaceProps::One},
                             std::pair{"surfaces-resize", Options::SurfaceProps::Resize}}) {
    Options& s = add(name);
    s.bundle = dir + "/surfaces.js";
    s.surfaces = 30;
    s.surfaceProps = props;
  }
  for (int n : {30, 100, 1000}) {
    for (auto [feed, fname] : {std::pair{Options::MarkerFeed::Json, "json"}, std::pair{Options::MarkerFeed::Buffer, "buffer"},
                               std::pair{Options::MarkerFeed::Native, "native"}}) {
      Options& m = add(("markers-" + std::string(fname) + "-" + std::to_string(n)).c_str());
      m.bundle = dir + "/markers.js";
      m.markers = n;
      m.markerFeed = feed;
      m.paint = PaintMode::None;
    }
  }
  return list;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::string suiteDir, jsonPath, comparePath;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--frames") o.frames = std::max(1, std::atoi(next().c_str()));
    else if (a == "--warmup") o.warmup = std::max(0, std::atoi(next().c_str()));
    else if (a == "--width") o.width = std::atoi(next().c_str());
    else if (a == "--height") o.height = std::atoi(next().c_str());
    else if (a == "--name") o.name = next();
    else if (a == "--json") jsonPath = next();
    else if (a == "--compare") comparePath = next();
    else if (a == "--suite") suiteDir = next();
    else if (a == "--paint") {
      std::string m = next();
      o.paint = m == "full" ? PaintMode::Full : m == "none" ? PaintMode::None : PaintMode::Incremental;
    } else if (a == "--mouse-sweep") {
      o.mouseSweep = std::sscanf(next().c_str(), "%f,%f,%f,%d", &o.sweepX, &o.sweepY0, &o.sweepY1, &o.sweepFrames) == 4;
      o.sweepFrames = std::max(2, o.sweepFrames);
    } else if (a == "--patch-state") {
      std::string v = next();
      auto comma = v.find(',');
      o.patchKey = v.substr(0, comma);
      o.patchSlots = comma == std::string::npos ? 500 : std::atoi(v.c_str() + comma + 1);
    } else if (a == "--surfaces") {
      std::string v = next();
      o.surfaces = std::max(0, std::atoi(v.c_str()));
      if (v.find(",changing") != std::string::npos) o.surfaceProps = Options::SurfaceProps::Changing;
      else if (v.find(",same") != std::string::npos) o.surfaceProps = Options::SurfaceProps::Same;
      else if (v.find(",one") != std::string::npos) o.surfaceProps = Options::SurfaceProps::One;
      else if (v.find(",resize") != std::string::npos) o.surfaceProps = Options::SurfaceProps::Resize;
    } else if (a == "--markers") {
      std::string v = next();
      o.markers = std::max(0, std::atoi(v.c_str()));
      if (v.find(",buffer") != std::string::npos) o.markerFeed = Options::MarkerFeed::Buffer;
      else if (v.find(",native") != std::string::npos) o.markerFeed = Options::MarkerFeed::Native;
    } else if (a == "--wheel") {
      int n = std::sscanf(next().c_str(), "%f,%f,%f,%d", &o.wheelX, &o.wheelY, &o.wheelDy, &o.wheelFrames);
      o.wheel = n >= 3;
      o.wheelFrames = std::max(1, o.wheelFrames);
    } else if (a == "--react-stats") {
      o.reactStats = true;
    } else if (a == "--dump") {
      o.dump = next();
    } else if (a == "-h" || a == "--help") {
      std::printf(
          "usage: jsui-bench <bundle.js> [--frames N] [--warmup N] [--width W] [--height H]\n"
          "                  [--paint incremental|full|none] [--mouse-sweep X,Y0,Y1,FRAMES]\n"
          "                  [--patch-state KEY,SLOTS] [--surfaces N[,changing|same|one|resize]]\n"
          "                  [--markers N,json|buffer|native] [--wheel X,Y,DY[,FRAMES]]\n"
          "                  [--react-stats] [--name NAME] [--json out.json] [--dump out.pam]\n"
          "       jsui-bench --suite DIR [--json out.json] [--compare baseline.json]\n");
      return 0;
    } else o.bundle = a;
  }
  subscribeYoga();
  std::vector<Options> runs;
  if (!suiteDir.empty()) runs = suite(suiteDir, o);
  else if (!o.bundle.empty()) {
    if (o.name.empty()) o.name = o.bundle;
    runs.push_back(o);
  } else {
    std::fprintf(stderr, "no bundle given (see --help)\n");
    return 2;
  }
  std::vector<Result> results;
  for (const Options& r : runs) {
    Result res;
    if (!run(r, &res)) return 1;
    print(res);
    results.push_back(res);
  }
  if (!jsonPath.empty()) {
    std::ofstream f(jsonPath);
    f << toJson(results);
  }
  if (!comparePath.empty()) compare(results, comparePath);
  return 0;
}
