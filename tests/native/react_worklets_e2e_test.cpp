// react-native-worklets end to end: a real @orktes/jsui-react bundle
// (packages/cli/test/e2e/worklets.jsx, built by the CLI with its worklets
// transform) driven through the C API.
// - no worklet runtime (and no memory for one) until the first worklet;
// - runOnUI with captured values and a nested worklet, runOnJS back;
//   a worklet as an argument; runOnUISync / runOnUIAsync results and
//   errors (values JSON cannot carry among them); a throwing worklet is
//   reported with its location;
// - a UI-runtime requestAnimationFrame loop sets an Animated value every
//   frame: the box moves while JS is not ticked (jsui_tick_native) and, with
//   JS on its own thread, while JS is stalled, with no command batch applied;
// - a synchronizable shared by both runtimes, a createWorkletRuntime
//   runtime, a UI-runtime setTimeout, remote functions released.
// Built and run by the @orktes/jsui-cli suite with JSUI_REACT_WORKLETS_BUNDLE.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/node.h"
#include "core/value.h"
#include "js/worklets.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

struct WorkletsE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  bool dev = false;

  explicit WorkletsE2E(int jsThread) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.js_thread = jsThread;
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<WorkletsE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
  }
  ~WorkletsE2E() { jsui_destroy(rt); }
  jsui::Runtime& runtime() { return *jsui::internalRuntime(rt); }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    return !wait("ready").isNull();
  }
  // (With JS on its own thread, each frame waits for that frame's JS.)
  void tick() { jsui_tick_ex(rt, now += 16, JSUI_TICK_SYNC_JS); }
  void post(const char* name) { jsui_post_message(rt, name, "null"); }
  // Ticks until the bundle posts `name` (at most `frames`); null if it did not.
  Value wait(const std::string& name, int frames = 60) {
    for (int i = 0; i <= frames; i++) {
      for (auto it = messages.begin(); it != messages.end(); ++it) {
        if (it->first == name) {
          Value v = Value::parseJson(it->second);
          messages.erase(it);
          return v;
        }
      }
      if (i < frames) tick();
    }
    return Value();
  }
  Value ask(const char* name, int frames = 60) {
    post(name);
    return wait(name, frames);
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
  double boxX() {
    jsui::Node* n = find("box");
    if (!n) return NAN;
    for (const Value& t : n->props()["style"]["transform"].asArray())
      if (t.has("translateX")) return t["translateX"].asNumber(NAN);
    return NAN;
  }
};

void run(int jsThread, const char* label) {
  const char* path = std::getenv("JSUI_REACT_WORKLETS_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_WORKLETS_BUNDLE, see packages/cli/test)\n");
    return;
  }
  WorkletsE2E t(jsThread);
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  t.dev = t.ask("dev").asBool();
  jsui::Worklets& hub = *t.runtime().worklets;

  // Nothing until the first worklet.
  CHECK_EQ(hub.runtimeCount(), size_t(0));
  CHECK_EQ(hub.memoryUsage(), size_t(0));

  // ---- closures, nested worklets, runOnJS
  Value c = t.ask("closure");
  CHECK_EQ(c["kind"].asNumber(), 2.0);  // ran on the UI runtime
  CHECK_EQ(c["n"].asNumber(), 16.0);    // square(3) + K
  CHECK_EQ(c["sum"].asNumber(), 12.0);  // (1 + 2 + 3) * scale
  CHECK_EQ(c["s"].asString(), std::string("hi"));
  CHECK(c["isWorklet"].asBool());
  CHECK_EQ(hub.runtimeCount(), size_t(1));
  size_t uiBytes = hub.memoryUsage();
  CHECK(uiBytes > 0);
  CHECK_EQ(t.ask("nested").asNumber(), 81.0);

  // ---- runOnUISync / runOnUIAsync
  Value s = t.ask("sync");
  CHECK_EQ(s["sum"].asNumber(), 5.0);
  CHECK_EQ(s["kind"].asNumber(), 2.0);
  CHECK(s["u"].asBool());
  CHECK(s["nan"].asBool());
  CHECK(s["neg"].asBool());
  CHECK_EQ(s["list"].toJson(), std::string("[1,\"x\",null]"));
  CHECK_EQ(s["f32"].toJson(), std::string("[1.5,2.5]"));
  CHECK_EQ(s["map"].asNumber(), 1.0);
  CHECK(s["set"].asBool());
  CHECK_EQ(t.ask("syncThrow").asString(), std::string("TypeError: bad input"));
  t.post("async");
  CHECK_EQ(t.wait("async").asNumber(), 42.0);
  CHECK_EQ(t.wait("asyncErr").asString(), std::string("nope"));

  // ---- a throwing worklet is reported (with its location in dev builds)
  t.post("error");
  std::string reported;
  for (int i = 0; i < 30 && reported.empty(); i++) {
    t.tick();
    if (const char* e = jsui_last_error(t.rt)) reported = e;
  }
  std::printf("    %s: reported \"%s\"\n", label, reported.substr(0, reported.find('\n')).c_str());
  CHECK(reported.find("[Worklets]") != std::string::npos);
  CHECK(reported.find("kaboom") != std::string::npos);
  if (t.dev) CHECK(reported.find("worklets.jsx:") != std::string::npos);
  t.runtime().lastError.clear();

  // ---- a UI-runtime frame loop drives an Animated value
  t.post("drive");
  for (int i = 0; i < 30 && !(t.boxX() > 0); i++) t.tick();
  CHECK(t.boxX() > 0);
  // From here every frame moves it by 1.6 (16 ms / 10), JS or not.
  double last = t.boxX();
  int moved = 0, frames = 0;
  uint64_t batches = t.runtime().appliedBatches.load();
  bool threaded = jsThread == JSUI_JS_THREAD_ON;
  if (threaded) jsui_debug_stall_js(t.rt, 150);  // JS busy: no commits meanwhile
  for (int i = 0; i < 6; i++) {
    t.now += 16;
    if (threaded) jsui_tick(t.rt, t.now);
    else jsui_tick_native(t.rt, t.now);  // no JS at all
    double x = t.boxX();
    frames++;
    if (std::fabs(x - last - 1.6) < 1e-9) moved++;
    else std::fprintf(stderr, "    frame %d: %.4f -> %.4f\n", i, last, x);
    last = x;
  }
  uint64_t batchesAfter = t.runtime().appliedBatches.load();
  std::printf("    %s: box moved %d of %d frames without JS (%llu command batches applied meanwhile)\n", label, moved,
              frames, (unsigned long long)(batchesAfter - batches));
  CHECK_EQ(moved, frames);
  if (threaded) CHECK_EQ(batchesAfter, batches);
  Value end = t.wait("driveEnd", 60);
  CHECK(end.asNumber() >= 16.0);

  // ---- synchronizable, worker runtime, timers
  Value cnt = t.ask("counter");
  CHECK_EQ(cnt["ui"].asNumber(), 5.0);
  CHECK_EQ(cnt["js"].asNumber(), 5.0);
  Value w = t.ask("worker");
  CHECK_EQ(w["kind"].asNumber(), 3.0);
  CHECK_EQ(w["v"].asNumber(), 42.0);
  CHECK_EQ(hub.runtimeCount(), size_t(2));
  CHECK_EQ(t.ask("workerSync").asNumber(), 305.0);
  Value timer = t.ask("timer");
  CHECK(timer.asNumber() >= 50 && timer.asNumber() <= 50 + 16);

  // ---- remote functions are given back once the worklet runtime drops them
  for (int i = 0; i < 4; i++) t.tick();
  CHECK_EQ(t.ask("remotes").asNumber(-1), 0.0);
  // What one more runtime costs to start.
  size_t before = hub.memoryUsage();
  auto t0 = std::chrono::steady_clock::now();
  hub.create(hub.reserveRuntimeId(), "probe");
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("    %s: UI runtime heap %zu bytes, both runtimes %zu bytes; a new runtime starts in %.2f ms (+%zu bytes)\n",
              label, uiBytes, before, ms, hub.memoryUsage() - before);
  err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
}

}  // namespace

// Single-threaded (or JSUI_TEST_JS_THREAD=lockstep).
TEST(react_worklets_e2e) {
  int mode = jsui_test::jsThreadMode();
  run(mode, mode == JSUI_JS_THREAD_LOCKSTEP ? "lockstep" : "single-threaded");
}

// JS on its own thread: the drive loop goes on while JS is stalled.
TEST(react_worklets_e2e_js_thread) { run(JSUI_JS_THREAD_ON, "js thread"); }
