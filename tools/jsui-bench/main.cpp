// jsui-bench: runs a bundle headlessly for N frames on a simulated 60 Hz
// clock and reports where each frame's time goes before (and in) painting.
//
//   jsui-bench bundle.js [--frames 600] [--warmup 120] [--width 1280] [--height 720]
//              [--paint incremental|full|none] [--mouse-sweep X,Y0,Y1,FRAMES]
//              [--patch-state KEY,N] [--name NAME] [--json out.json]
//   jsui-bench --suite DIR [--json out.json] [--compare baseline.json]
//
// --suite runs the standard scenarios from DIR (built by tools/jsui-bench/run.sh):
//   interactions  examples/showcase/interactions.jsx (constant native animations)
//   list          2000 static rows, one counter changing every frame
//   menu          hover springs; the mouse sweeps over the buttons
//   inventory     500 slots, one jsui_patch_state per frame
//   idle          the inventory with no updates (should do no work)
//
// Phase times are exclusive (core/perf.h), in microseconds per frame:
//   js        QuickJS execution (timers, rAF, React render + reconcile, events)
//   commit    native side of the bridge (setProps / tree edits / value conversion)
//   animated  native-driven Animated evaluation
//   style     style resolution (applyStyle)
//   layout    Yoga + text measurement
//   input     hit testing / event routing
//   paint     jsui_render_rgba(_incremental)
// Counters are per frame: bridge calls, style resolves, Yoga nodes laid out
// (not served from Yoga's cache), text measure callbacks, allocations, ...
#include <algorithm>
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
    {"textMeasures", [](const jsui::PerfCounters& c) { return c.textMeasures; }},
    {"paragraphBuilds", [](const jsui::PerfCounters& c) { return c.paragraphBuilds; }},
    {"animatedWrites", [](const jsui::PerfCounters& c) { return c.animatedWrites; }},
    {"events", [](const jsui::PerfCounters& c) { return c.events; }},
};
constexpr int kCounterCount = int(sizeof(kCounters) / sizeof(kCounters[0]));
// Extra counters measured by the tool itself.
const char* const kExtraNames[] = {"yogaLayouts", "yogaCachedLayouts", "yogaMeasures", "paragraphLayouts", "allocs"};
constexpr int kExtraCount = 5;

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
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = o.width;
  cfg.height = o.height;
  cfg.js_timeout_ms = -1;
  cfg.log = [](void*, JsuiLogLevel level, const char* msg) {
    if (level >= JSUI_LOG_WARN) std::fprintf(stderr, "[bench] %s\n", msg);
  };
  JsuiRuntime* h = jsui_create(&cfg);
  jsui::Runtime* rt = jsui::internalRuntime(h);
  jsui::Perf& perf = rt->doc.perf;
  if (jsui_load_bundle_source(h, src.data(), src.size(), o.bundle.c_str()) != 0) {
    std::fprintf(stderr, "jsui-bench: %s failed to load: %s\n", o.bundle.c_str(), jsui_last_error(h));
    jsui_destroy(h);
    return false;
  }
  std::vector<uint32_t> px(size_t(o.width) * size_t(o.height));
  const size_t rowBytes = size_t(o.width) * 4;
  double now = 0;
  int frame = 0;
  auto step = [&]() {
    now += 1000.0 / 60.0;
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
    jsui_tick(h, now);
    if (o.paint != PaintMode::None && jsui_needs_redraw(h)) {
      jsui::Perf::Scope paint(perf, jsui::Phase::Paint);
      if (o.paint == PaintMode::Full) jsui_render_rgba(h, px.data(), o.width, o.height, rowBytes);
      else jsui_render_rgba_incremental(h, px.data(), o.width, o.height, rowBytes, nullptr);
    }
    frame++;
  };
  for (int i = 0; i < o.warmup; i++) step();

  std::vector<double> phase[kPhaseCount], total;
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
    total.push_back(sum);
    std::copy(std::begin(perf.us), std::end(perf.us), prevUs);
  }
  CALLGRIND_STOP_INSTRUMENTATION;
  perf.profiling = false;
  uint64_t allocs = gAllocs - a0;
  out->name = o.name;
  out->frames = o.frames;
  out->nodes = countNodes(rt->doc.root());
  for (int p = 0; p < kPhaseCount; p++) out->phase[p] = summarize(phase[p]);
  out->total = summarize(total);
  double f = double(o.frames);
  for (int c = 0; c < kCounterCount; c++) out->counters[c] = double(kCounters[c].get(perf.counters)) / f;
  out->extra[0] = double(gYoga.layouts - y0.layouts) / f;
  out->extra[1] = double(gYoga.cachedLayouts - y0.cachedLayouts) / f;
  out->extra[2] = double(gYoga.measures - y0.measures) / f;
  out->extra[3] = double(jsui::gParagraphLayouts - pl0) / f;
  out->extra[4] = double(allocs) / f;
  if (const char* err = jsui_last_error(h); err && *err) std::fprintf(stderr, "[bench] %s: last error: %s\n", o.name.c_str(), err);
  jsui_destroy(h);
  return true;
}

void print(const Result& r) {
  std::printf("\n== %s  (%d frames, %d nodes)\n", r.name.c_str(), r.frames, r.nodes);
  std::printf("  %-10s %10s %10s %10s\n", "phase(us)", "mean", "p50", "p95");
  for (int p = 0; p < kPhaseCount; p++)
    std::printf("  %-10s %10.1f %10.1f %10.1f\n", kPhaseNames[p], r.phase[p].mean, r.phase[p].p50, r.phase[p].p95);
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
      double was = b.count(k) ? b[k] : NAN;
      char delta[32] = "";
      if (was > 0) std::snprintf(delta, sizeof delta, "%+.0f%%", (now / was - 1) * 100);
      std::printf("    %-18s %10.1f -> %10.1f  %s\n", k, was, now, delta);
    };
    for (int p = 0; p < kPhaseCount; p++) line(kPhaseNames[p], r.phase[p].mean);
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
    } else if (a == "-h" || a == "--help") {
      std::printf(
          "usage: jsui-bench <bundle.js> [--frames N] [--warmup N] [--width W] [--height H]\n"
          "                  [--paint incremental|full|none] [--mouse-sweep X,Y0,Y1,FRAMES]\n"
          "                  [--patch-state KEY,SLOTS] [--name NAME] [--json out.json]\n"
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
