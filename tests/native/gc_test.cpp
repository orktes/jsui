// JS garbage collection (third_party/quickjs-jsui/0001-generational-gc.patch,
// QuickJsEngine::collectIdle): cycles are collected generationally, never
// while JS runs, and between frames by jsui_tick.
#include <cstdlib>
#include <cstring>
#include <string>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct Rt {
  JsuiRuntime* rt;
  double now = 0;
  Rt() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 16;
    cfg.height = 16;
    rt = jsui_create(&cfg);
  }
  ~Rt() { jsui_destroy(rt); }
  void ticks(int n) {
    for (int i = 0; i < n; i++) jsui_tick(rt, now += 16);
  }
  JsuiPerf perf(bool reset = false) {
    JsuiPerf p;
    jsui_get_perf(rt, &p, reset);
    return p;
  }
  double state(const char* key) {
    const char* j = jsui_get_state(rt, key);
    return j ? std::atof(j) : -1;
  }
};

// Each frame drops about 1 MB of cyclic garbage (objects that point at each
// other, which reference counting alone never frees). Some of it points
// into a set of live objects that grows over the frames, and some live
// objects point at objects made in the same frame: both must survive.
const char* kCycles = R"JS(
const kept = [];
let frames = 0, broken = 0;
function frame() {
  frames++;
  let prev = null;
  for (let i = 0; i < 4000; i++) {
    // garbage: a chain of two-object cycles, pointing into the kept objects
    const a = {i, next: null, keep: kept.length ? kept[i % kept.length] : null};
    const b = {i, back: a, f: () => a.i};
    a.next = b;
    if (prev) prev.peer = a;
    prev = a;
    if (i % 997 === 0) {
      // kept: its own small cycle, plus a young object hung off the last one
      const k = {v: i, self: null, young: null};
      k.self = {k, f: () => k.v};
      if (kept.length) kept[kept.length - 1].young = {v: i};
      kept.push(k);
    }
  }
  for (const k of kept)
    if (k.self.f() !== k.v || k.self.k !== k || (k.young && typeof k.young.v !== 'number')) broken++;
  __jsui_native.setState('frames', String(frames));
  __jsui_native.setState('broken', String(broken));
  if (frames < 240) requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
)JS";

}  // namespace

TEST(gc_collects_cycles_between_frames) {
  Rt t;
  CHECK_EQ(jsui_load_bundle_source(t.rt, kCycles, std::strlen(kCycles), "gc.js"), 0);
  t.perf(true);
  for (int i = 0; i < 1000 && t.state("frames") < 240; i++) t.ticks(1);
  CHECK_EQ(t.state("frames"), 240.0);
  // Old objects referenced from young garbage, and young objects only old
  // ones reference, come through every collection intact.
  CHECK_EQ(t.state("broken"), 0.0);
  JsuiPerf p = t.perf();
  // ~240 MB of cycles over the run, collected all along, mostly young.
  CHECK(p.gc_collections >= 20);
  CHECK(p.gc_collections > 2 * p.gc_full_collections);
  CHECK(p.gc_max_us > 0);
  CHECK(p.gc_us >= p.gc_max_us);
  // Reset starts new totals.
  t.perf(true);
  CHECK_EQ(t.perf().gc_collections, 0ull);
}
