// The runtime's JS-thread mode (Runtime::jsThreadMode, docs/ARCHITECTURE.md
// "Shared state", "JS thread"): the host-callback queue and proxy calls
// (HostQueue, also across real threads), the frame snapshot behind measure /
// viewport / Animated getValue / stopAnimation, measureAsync, the clipboard
// proxy, host state written by JS and the JS perf block. Plain JS bundles on
// the raw __jsui_native bridge. JS runs on its thread: a post waits until JS
// handled it (without applying what it did), a tick syncs
// (JSUI_TICK_SYNC_JS), so what JS did lands in that tick.
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/perf.h"
#include "host_queue.h"
#include "js/js_thread.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct ModeHost {
  JsuiRuntime* rt = nullptr;
  std::vector<std::string> messages, uiState, logs;
  std::string clipboard;
  double t = 0;
  explicit ModeHost(bool jsThreadMode) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 200;
    cfg.height = 100;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<ModeHost*>(u)->messages.push_back(std::string(name) + "=" + (json ? json : ""));
    };
    cfg.message_user = this;
    cfg.on_state = [](void* u, const char* key, const char* json) {
      static_cast<ModeHost*>(u)->uiState.push_back(std::string(key) + "=" + (json ? json : "<null>"));
    };
    cfg.state_user = this;
    cfg.log = [](void* u, JsuiLogLevel, const char* msg) { static_cast<ModeHost*>(u)->logs.push_back(msg); };
    cfg.log_user = this;
    rt = jsui_create(&cfg);
    jsui_set_clipboard_callbacks(
        rt, [](void* u) { return static_cast<ModeHost*>(u)->clipboard.c_str(); },
        [](void* u, const char* text) { static_cast<ModeHost*>(u)->clipboard = text; }, this);
    if (jsThreadMode) CHECK(jsui::internalRuntime(rt)->setJsThreadMode(true));
  }
  ~ModeHost() { jsui_destroy(rt); }
  jsui::Runtime& runtime() { return *jsui::internalRuntime(rt); }
  bool load(const char* src) { return jsui_load_bundle_source(rt, src, std::strlen(src), "mode.js") == 0; }
  void tick(int n = 1) {
    for (int i = 0; i < n; i++) jsui_tick_ex(rt, t += 16, JSUI_TICK_SYNC_JS);
  }
  std::string take() {
    std::string s;
    for (auto& m : messages) s += (s.empty() ? "" : " ") + m;
    messages.clear();
    return s;
  }
  // Until the JS thread handled what was posted (serving its proxied reads;
  // what it did is applied by the next tick).
  void waitJs() {
    jsui::Runtime& r = runtime();
    if (jsui::JsThread* js = r.jsThread()) js->waitIdle(-1, [&r] { r.hostQueue.drain(); });
  }
  void post(const char* name) {
    jsui_post_message(rt, name, "null");
    waitJs();
  }
};

const char* kBundle = R"JS(
  const N = __jsui_native, A = N.animated;
  const box = N.createNode('View');
  const style = (left) => ({ position: 'absolute', left, top: 20, width: 30, height: 40 });
  N.setProps(box, { style: style(10) });
  N.appendChild(N.rootId, box);
  const json = (v) => JSON.stringify(v);
  N.postMessage('asyncMeasure', String(N.asyncMeasure));
  globalThis.__jsui_onHostMessage = (name) => {
    if (name === 'measure') N.postMessage('measure', json(N.measure(box)));
    if (name === 'move') {
      N.setProps(box, { style: style(50) });
      N.postMessage('sync', json(N.measure(box)));
      N.measureAsync(box, (m) => N.postMessage('async', json(m)));
      N.measureAsync(987654, (m) => N.postMessage('gone', json(m)));
    }
    if (name === 'viewport') N.postMessage('viewport', json(N.viewport()));
    if (name === 'state') {
      N.setState('ui', '{"a":1}');
      N.postMessage('get', N.getState('ui'));
    }
    if (name === 'clip') {
      N.setClipboard('hi');
      N.postMessage('clip', N.getClipboard());
    }
    if (name === 'props') N.postMessage('left', json(N.getProps(box).style.left));
    if (name === 'log') console.log('hello');
    if (name === 'anim1') {
      A.createNode(1, { type: 'value', value: 0, offset: 0 });
      A.setValue(1, 5);
      N.postMessage('v', String(A.getValue(1)));
      A.startAnimation(7, 1, { type: 'spring', stiffness: 100, damping: 10, mass: 1, initialVelocity: 0, toValue: 100 });
      N.postMessage('fresh', json(A.stopAnimation(9)));  // never started
    }
    if (name === 'anim2') {
      const s = A.stopAnimation(7);
      N.postMessage('anim2', json([A.getValue(1) > 5, !!s && s.velocity > 0]));
    }
    if (name === 'anim3') N.postMessage('anim3', json(A.stopAnimation(7)));
    if (name === 'anim4') {
      A.startAnimation(8, 1, { type: 'spring', toValue: 0 });
      N.postMessage('anim4', json(A.stopAnimation(8)));  // started after the snapshot: velocity 0
    }
  };
)JS";

}  // namespace

TEST(host_queue_order_and_proxy) {
  jsui::HostQueue q;
  std::vector<int> order;
  q.post([&] { order.push_back(1); });
  q.post([&] {
    order.push_back(2);
    q.post([&] { order.push_back(3); });  // posted while draining: runs in the same drain
  });
  // On the owner thread a call drains first: FIFO holds.
  CHECK(q.call([&] { order.push_back(4); }, 10));
  CHECK(q.empty());
  CHECK_EQ(q.drain(), 0);
  CHECK_EQ(order.size(), size_t(4));
  for (size_t i = 0; i < order.size(); i++) CHECK_EQ(order[i], int(i) + 1);
  q.post([&] { order.push_back(5); });
  CHECK(!q.empty());
  CHECK_EQ(q.drain(), 1);
  CHECK_EQ(order.back(), 5);

  // From another thread: the owner serves the call in its drain.
  std::atomic<int> answered{0};
  int value = 0;
  std::thread js([&] {
    bool ok = q.call([&] { value = 42; }, 5000);
    answered = ok ? 1 : -1;
  });
  while (q.empty()) std::this_thread::yield();
  while (answered.load() == 0) q.drain();
  js.join();
  CHECK_EQ(answered.load(), 1);
  CHECK_EQ(value, 42);

  // Nobody drains: the call times out and never runs later.
  bool ran = false;
  std::thread late([&] { CHECK(!q.call([&] { ran = true; }, 20)); });
  late.join();
  q.drain();
  CHECK(!ran);

  // clear() releases a waiting caller at once.
  std::atomic<bool> released{false};
  std::thread waiting([&] {
    CHECK(!q.call([] {}, 60000));
    released = true;
  });
  while (q.empty()) std::this_thread::yield();
  q.clear();
  waiting.join();
  CHECK(released.load());
}

TEST(js_thread_mode_switch) {
  ModeHost h(false);
  CHECK(!h.runtime().jsThreadMode());
  CHECK(h.runtime().setJsThreadMode(true));
  CHECK(h.runtime().jsThreadMode());
  CHECK(h.load(kBundle));
  // A loaded bundle keeps its engine.
  CHECK(!h.runtime().setJsThreadMode(false));
  CHECK(h.runtime().setJsThreadMode(true));  // no change
}

TEST(js_thread_mode_host_callbacks) {
  ModeHost h(true);
  CHECK(h.load(kBundle));
  // A bundle's callbacks run when it has evaluated.
  CHECK_EQ(h.take(), std::string("asyncMeasure=true"));
  h.tick();
  // on_message waits for the tick.
  h.post("viewport");
  CHECK_EQ(h.take(), std::string());
  CHECK(jsui_wants_frame(h.rt));
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(viewport={"width":200,"height":100,"scale":1})"));
  // JS reads its own state write at once; on_state runs in the tick, with
  // the text JS wrote.
  h.post("state");
  CHECK(h.uiState.empty());
  const char* ui = jsui_get_state(h.rt, "ui");
  CHECK(ui && std::string(ui) == R"({"a":1})");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(get={"a":1})"));
  CHECK_EQ(h.uiState.size(), size_t(1));
  if (!h.uiState.empty()) CHECK_EQ(h.uiState[0], std::string(R"(ui={"a":1})"));
  // The clipboard read is a proxy call: it runs after the queued write.
  h.post("clip");
  CHECK_EQ(h.clipboard, std::string("hi"));
  h.tick();
  CHECK_EQ(h.take(), std::string("clip=hi"));
  // console.log reaches the log callback in the tick.
  size_t logs = h.logs.size();
  h.post("log");
  CHECK_EQ(h.logs.size(), logs);
  h.tick();
  CHECK_EQ(h.logs.size(), logs + 1);
  if (h.logs.size() == logs + 1) CHECK_EQ(h.logs.back(), std::string("hello"));
}

TEST(js_thread_mode_measure) {
  ModeHost h(true);
  CHECK(h.load(kBundle));
  h.tick();
  h.take();
  // The last frame's layout (snapshot).
  h.post("measure");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(measure={"x":10,"y":20,"width":30,"height":40,"pageX":10,"pageY":20})"));
  // After a change: sync measure still shows the last frame; the callback
  // form answers after the layout of this tick (unknown nodes: null).
  h.post("move");
  CHECK_EQ(h.take(), std::string());
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(sync={"x":10,"y":20,"width":30,"height":40,"pageX":10,"pageY":20} )"
                                 R"(async={"x":50,"y":20,"width":30,"height":40,"pageX":50,"pageY":20} gone=null)"));
  h.post("measure");
  h.post("props");  // a proxy read of the native tree
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(measure={"x":50,"y":20,"width":30,"height":40,"pageX":50,"pageY":20} left=50)"));
}

// measureAsync without the mode: the same answer after the next layout.
TEST(js_thread_mode_off_measure_async) {
  ModeHost h(false);
  CHECK(h.load(kBundle));
  h.tick();
  CHECK_EQ(h.take(), std::string("asyncMeasure=false"));
  h.post("move");
  CHECK_EQ(h.take(), std::string(R"(sync={"x":50,"y":20,"width":30,"height":40,"pageX":50,"pageY":20})"));
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(async={"x":50,"y":20,"width":30,"height":40,"pageX":50,"pageY":20} gone=null)"));
}

TEST(js_thread_mode_animated) {
  ModeHost h(true);
  CHECK(h.load(kBundle));
  h.tick();
  h.take();
  // A value JS set reads back before any snapshot has it; stopping an
  // animation that never ran answers null.
  h.post("anim1");
  h.tick();
  CHECK_EQ(h.take(), std::string("v=5 fresh=null"));
  h.tick(3);
  // The spring runs natively: the snapshot has its value and velocity.
  h.post("anim2");
  h.tick();
  CHECK_EQ(h.take(), std::string("anim2=[true,true]"));
  // Stopped: not running any more on the next snapshot.
  h.post("anim3");
  h.tick();
  CHECK_EQ(h.take(), std::string("anim3=null"));
  h.post("anim4");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(anim4={"velocity":0})"));
}

// JS's perf counters go to a block of its own, merged on the owner thread.
TEST(js_thread_mode_perf_block) {
  ModeHost h(true);
  CHECK(h.load(kBundle));
  h.tick();
  JsuiPerf p;
  jsui_get_perf(h.rt, &p, 1);
  h.post("move");  // one setProps
  CHECK_EQ(h.runtime().doc.perf.counters.bridgeCalls, uint64_t(0));  // still in JS's block
  jsui_get_perf(h.rt, &p, 1);
  CHECK_EQ(p.bridge_calls, 1ull);
  h.post("move");
  h.tick();
  CHECK_EQ(h.runtime().doc.perf.counters.bridgeCalls, uint64_t(1));  // merged by the tick
}
