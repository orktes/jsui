// Frame data through the C API (jsui_set_buffer / jsui_map_buffer /
// jsui_commit_buffer / jsui_remove_buffer / jsui_get_buffer) with plain JS
// bundles on the raw __jsui_native bridge: typed arrays over the runtime's
// storage, delivery once per tick, detaching when storage moves, buffers
// surviving reloads, and hostBuffer native Animated nodes moving a view
// without JS (jsui_tick_native). The @orktes/jsui-react layer is covered by
// packages/react/test/hostbuffer.test.js and the react_host_buffers_e2e test.
// JS-thread mode: published copies (copy-on-publish), read from another
// thread while the owner keeps writing, and the JS view of them.
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "host_buffers.h"
#include "js/js_thread.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct BufHost {
  JsuiRuntime* rt = nullptr;
  std::vector<std::string> messages, errors;
  double t = 0;
  explicit BufHost(bool jsThreadMode = false) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 200;
    cfg.height = 100;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<BufHost*>(u)->messages.push_back(std::string(name) + "=" + (json ? json : ""));
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
      if (level == JSUI_LOG_ERROR) static_cast<BufHost*>(u)->errors.push_back(msg);
    };
    cfg.log_user = this;
    rt = jsui_create(&cfg);
    if (jsThreadMode) jsui::internalRuntime(rt)->setJsThreadMode(true);
  }
  ~BufHost() { jsui_destroy(rt); }
  bool load(const char* src) { return jsui_load_bundle_source(rt, src, std::strlen(src), "buf.js") == 0; }
  void tick(int n = 1) {
    for (int i = 0; i < n; i++) jsui_tick_ex(rt, t += 16, JSUI_TICK_SYNC_JS);
  }
  std::string take() {
    std::string s;
    for (auto& m : messages) s += (s.empty() ? "" : " ") + m;
    messages.clear();
    return s;
  }
  // A post is handled before it returns (JS thread: waited for, without
  // applying what JS did; that waits for the tick).
  void post(const char* name) {
    jsui_post_message(rt, name, "null");
    jsui::Runtime& r = *jsui::internalRuntime(rt);
    if (jsui::JsThread* js = r.jsThread()) js->waitIdle(-1, [&r] { r.hostQueue.drain(); });
  }
};

// read(key) posts [type, contents, version > 0]; deliveries are logged.
const char* kReadBundle = R"JS(
  const N = __jsui_native;
  const T = [Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array];
  let keep = null, keepBuffer = null;
  const view = (k) => { const i = N.buffer(k); return i && new T[i[1]](i[0], 0, i[2]); };
  globalThis.__jsui_onHostBuffer = (keys) => N.postMessage('changed', JSON.stringify(keys));
  globalThis.__jsui_onHostMessage = (name) => {
    if (name === 'read') { const v = view('pos'); N.postMessage('pos', JSON.stringify(v ? [v.constructor.name, Array.from(v), N.bufferVersion('pos') > 0] : null)); }
    if (name === 'keep') { keep = view('pos'); keepBuffer = N.buffer('pos')[0]; }
    if (name === 'kept') N.postMessage('kept', JSON.stringify([keep.length, N.buffer('pos') ? N.buffer('pos')[0] === keepBuffer : null]));
    if (name === 'keptValues') N.postMessage('keptValues', JSON.stringify(Array.from(keep)));
    if (name === 'keys') N.postMessage('keys', JSON.stringify(N.bufferKeys().sort()));
  };
)JS";

}  // namespace

TEST(host_buffers_store) {
  jsui::HostBuffers b;
  std::vector<std::string> released;
  b.onRelease = [&](const std::string& k) { released.push_back(k); };
  float a[3] = {1, 2, 3};
  CHECK_EQ(b.set("p", JSUI_BUFFER_F32, a, 3), 1);
  CHECK_EQ(b.set("p", JSUI_BUFFER_F32, a, 3), 0);  // same contents: no change
  uint64_t v = b.version("p");
  CHECK(v > 0);
  CHECK_EQ(b.find("p")->at(2), 3.0);
  CHECK_EQ(b.find("p")->at(3), 0.0);  // out of range
  CHECK_EQ(b.set("p", JSUI_BUFFER_I32, a, 3), 1);  // same bytes, other type
  CHECK_EQ(b.set("p", JsuiBufferType(42), a, 3), -1);
  auto dirty = b.takeDirty();
  CHECK_EQ(dirty.size(), size_t(1));
  CHECK(!b.hasDirty());
  // map keeps the contents up to the new length, zero-fills the rest, and is
  // invisible until committed.
  float* m = static_cast<float*>(b.map("q", JSUI_BUFFER_F32, 4));
  CHECK(m != nullptr);
  CHECK(b.find("q") == nullptr);
  m[0] = 5;
  CHECK(b.commit("q"));
  CHECK_EQ(b.find("q")->at(0), 5.0);
  CHECK_EQ(b.find("q")->at(1), 0.0);
  CHECK(!b.commit("nope"));
  // Growing past the capacity moves the storage (released first); shrinking does not.
  const uint8_t* before = b.find("q")->data();
  b.map("q", JSUI_BUFFER_F32, 2);
  CHECK(b.find("q")->data() == before);
  CHECK(released.empty());
  m = static_cast<float*>(b.map("q", JSUI_BUFFER_F32, 1000));
  CHECK_EQ(released.size(), size_t(1));
  CHECK_EQ(m[0], 5.0f);
  CHECK_EQ(m[999], 0.0f);
  uint64_t gen = b.generation();
  CHECK(b.remove("q"));
  CHECK(b.generation() != gen);
  CHECK(!b.remove("q"));
  CHECK_EQ(b.version("q"), uint64_t(0));
}

TEST(host_buffers_c_api) {
  BufHost h;
  float pos[4] = {1.5f, 2, 3, 4};
  CHECK_EQ(jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 4), 1);
  CHECK_EQ(jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 4), 0);
  CHECK_EQ(jsui_set_buffer(h.rt, nullptr, JSUI_BUFFER_F32, pos, 4), -1);
  JsuiBufferType type = JSUI_BUFFER_U8;
  size_t count = 0;
  const float* got = static_cast<const float*>(jsui_get_buffer(h.rt, "pos", &type, &count));
  CHECK(got != nullptr);
  CHECK_EQ(type, JSUI_BUFFER_F32);
  CHECK_EQ(count, size_t(4));
  if (got) CHECK_EQ(got[0], 1.5f);
  CHECK(jsui_get_buffer(h.rt, "nope", nullptr, nullptr) == nullptr);
  uint16_t* ids = static_cast<uint16_t*>(jsui_map_buffer(h.rt, "ids", JSUI_BUFFER_U16, 2));
  CHECK(ids != nullptr);
  ids[0] = 7;
  ids[1] = 9;
  CHECK_EQ(jsui_commit_buffer(h.rt, "ids"), 1);
  CHECK_EQ(jsui_commit_buffer(h.rt, "nope"), -1);
  CHECK_EQ(jsui_remove_buffer(h.rt, "ids"), 1);
  CHECK_EQ(jsui_remove_buffer(h.rt, "ids"), 0);
  CHECK(jsui_map_buffer(h.rt, "x", JsuiBufferType(-1), 1) == nullptr);
  CHECK_EQ(h.errors.size(), size_t(1));
}

TEST(host_buffers_js_views) {
  BufHost h;
  float pos[2] = {1, 2};
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);  // before the bundle: no delivery needed
  CHECK(h.load(kReadBundle));
  h.tick();
  CHECK_EQ(h.take(), std::string());
  h.post("read");
  CHECK_EQ(h.take(), std::string(R"(pos=["Float32Array",[1,2],true])"));
  // One delivery per tick for any number of changes.
  h.post("keep");
  pos[0] = 5;
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);
  pos[1] = 6;
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);
  CHECK(jsui_wants_frame(h.rt));
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(changed=["pos"])"));
  h.tick();
  CHECK_EQ(h.take(), std::string());
  // A kept view sees the new contents in place (same ArrayBuffer).
  h.post("kept");
  CHECK_EQ(h.take(), std::string("kept=[2,true]"));
  h.post("read");
  CHECK_EQ(h.take(), std::string(R"(pos=["Float32Array",[5,6],true])"));
  // Zero-copy writes, another type.
  int32_t* p = static_cast<int32_t*>(jsui_map_buffer(h.rt, "pos", JSUI_BUFFER_I32, 3));
  p[0] = -1;
  p[1] = 2;
  p[2] = 3;
  jsui_commit_buffer(h.rt, "pos");
  h.post("read");
  CHECK_EQ(h.take(), std::string(R"(pos=["Int32Array",[-1,2,3],true])"));
  // Growing moves the storage: the kept view is detached, not dangling.
  std::vector<float> big(5000, 1.f);
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, big.data(), big.size());
  h.post("kept");
  CHECK_EQ(h.take(), std::string("kept=[0,false]"));
  h.post("keys");
  CHECK_EQ(h.take(), std::string(R"(keys=["pos"])"));
  jsui_remove_buffer(h.rt, "pos");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(changed=["pos"])"));
  h.post("read");
  CHECK_EQ(h.take(), std::string("pos=null"));
  // Buffers belong to the runtime: they survive a reload.
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);
  h.post("keep");
  jsui_reload(h.rt);
  h.tick();
  h.take();
  h.post("read");
  CHECK_EQ(h.take(), std::string(R"(pos=["Float32Array",[5,6],true])"));
}

// Animated.hostBuffer's native node: the runtime moves a view from the
// buffer every frame without running JS.
TEST(host_buffers_native_animated) {
  BufHost h;
  const char* src = R"JS(
    const N = __jsui_native, A = N.animated;
    const box = N.createNode('View');
    N.setProps(box, { style: { position: 'absolute', width: 10, height: 10, backgroundColor: '#fff' } });
    N.appendChild(N.rootId, box);
    A.createNode(1, { type: 'hostBuffer', key: 'enemies', index: 2 });
    A.createNode(2, { type: 'hostBuffer', key: 'enemies', index: 3 });
    A.createNode(3, { type: 'transform', transforms: [{ property: 'translateX', nodeTag: 1 }, { property: 'translateY', nodeTag: 2 }] });
    A.connect(1, 3); A.connect(2, 3);
    A.createNode(4, { type: 'style', style: { transform: 3 } });
    A.connect(3, 4);
    A.createNode(5, { type: 'props', props: { style: 4 } });
    A.connect(4, 5);
    A.connectToView(5, box);
    globalThis.__jsui_onHostMessage = (name) => { if (name === 'drop') { A.dropNode(1); A.dropNode(2); } };
    N.postMessage('box', String(box));
  )JS";
  CHECK(h.load(src));
  h.tick();
  std::string m = h.take();
  CHECK(m.rfind("box=", 0) == 0);
  jsui::Runtime& r = *jsui::internalRuntime(h.rt);
  jsui::Node* box = r.findNode(uint32_t(std::stoul(m.substr(4))));
  CHECK(box != nullptr);
  if (!box) return;
  auto tx = [&](size_t i) -> jsui::Value {
    const auto& a = box->props()["style"]["transform"].asArray();
    return i < a.size() ? a[i] : jsui::Value();
  };
  // No buffer yet: 0.
  CHECK_EQ(tx(0)["translateX"].asNumber(), 0.0);
  float data[8] = {0, 0, 40, 25, 0, 0, 0, 0};
  jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 8);
  for (int f = 1; f <= 5; f++) {
    data[2] = 40.f + float(f);
    data[3] = 25.f - float(f);
    jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 8);
    jsui_tick_native(h.rt, h.t += 16);  // JS does not run
    CHECK_EQ(tx(0)["translateX"].asNumber(), 40.0 + f);
    CHECK_EQ(tx(1)["translateY"].asNumber(), 25.0 - f);
  }
  // A shorter buffer: the element is gone -> 0. Removal -> 0.
  jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 3);
  h.tick();
  CHECK_EQ(tx(0)["translateX"].asNumber(), 45.0);
  CHECK_EQ(tx(1)["translateY"].asNumber(), 0.0);
  jsui_remove_buffer(h.rt, "enemies");
  h.tick();
  CHECK_EQ(tx(0)["translateX"].asNumber(), 0.0);
  // Unchanged buffer: nothing is written.
  jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 8);
  h.tick();
  uint32_t pv = box->paintVersion;
  jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 8);
  h.tick();
  CHECK_EQ(box->paintVersion, pv);
  // Dropped nodes stop following.
  h.post("drop");
  data[2] = 99;
  jsui_set_buffer(h.rt, "enemies", JSUI_BUFFER_F32, data, 8);
  h.tick();
  CHECK(tx(0)["translateX"].asNumber() != 99.0);
}

TEST(host_buffers_published_copies) {
  jsui::HostBuffers b;
  float a[3] = {1, 2, 3};
  b.set("p", JSUI_BUFFER_F32, a, 3);
  CHECK(b.published("p") == nullptr);
  CHECK_EQ(b.publishedVersion("p"), uint64_t(0));
  // Versions are published for every key; contents only for subscribed ones.
  b.publish();
  CHECK_EQ(b.publishedVersion("p"), b.version("p"));
  CHECK(b.published("p") == nullptr);
  b.subscribe("p");
  b.publish();
  std::shared_ptr<const jsui::HostBuffers::Copy> c = b.published("p");
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->count, size_t(3));
  CHECK_EQ(c->bytes.size(), size_t(12));
  CHECK_EQ(reinterpret_cast<const float*>(c->bytes.data())[2], 3.0f);
  b.publish();  // unchanged: the same copy
  CHECK(b.published("p") == c);
  // A change is published at the next publish(); a copy a reader holds keeps
  // its contents.
  a[0] = 9;
  b.set("p", JSUI_BUFFER_F32, a, 3);
  CHECK(b.publishedVersion("p") < b.version("p"));
  CHECK(b.published("p") == c);
  b.publish();
  std::shared_ptr<const jsui::HostBuffers::Copy> d = b.published("p");
  CHECK(d != c);
  CHECK_EQ(reinterpret_cast<const float*>(c->bytes.data())[0], 1.0f);
  CHECK_EQ(reinterpret_cast<const float*>(d->bytes.data())[0], 9.0f);
  // A copy nobody holds is rewritten in place.
  const void* where = d.get();
  c.reset();
  d.reset();
  a[1] = 7;
  b.set("p", JSUI_BUFFER_F32, a, 3);
  b.publish();
  CHECK(b.published("p").get() == where);
  CHECK_EQ(reinterpret_cast<const float*>(b.published("p")->bytes.data())[1], 7.0f);
  CHECK_EQ(b.publishedKeys().size(), size_t(1));
  b.remove("p");
  CHECK(b.published("p") != nullptr);
  b.publish();
  CHECK(b.published("p") == nullptr);
  CHECK_EQ(b.publishedVersion("p"), uint64_t(0));
  CHECK(b.publishedKeys().empty());
}

// A reader on another thread (what the JS thread will be) takes copies while
// the owner thread keeps rewriting, growing and publishing the buffer: every
// copy it sees is one whole frame of data.
TEST(host_buffers_snapshot_reader) {
  jsui::HostBuffers b;
  b.subscribe("m");
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0}, torn{0};
  std::thread reader([&] {
    uint64_t last = 0;
    while (!stop.load()) {
      std::shared_ptr<const jsui::HostBuffers::Copy> c = b.published("m");
      if (!c) continue;
      reads++;
      const float* f = reinterpret_cast<const float*>(c->bytes.data());
      int frame = int(f[0]);
      if (c->count != size_t(frame % 64 + 1) || c->bytes.size() != c->count * 4 || c->version < last) torn++;
      for (size_t i = 0; i < c->count; i++)
        if (f[i] != float(frame)) torn++;
      last = c->version;
    }
  });
  for (int frame = 1; frame <= 3000; frame++) {
    std::vector<float> data(size_t(frame % 64 + 1), float(frame));
    b.set("m", JSUI_BUFFER_F32, data.data(), data.size());
    b.publish();
  }
  // Let the reader see the last frame too.
  while (reads.load() == 0) std::this_thread::yield();
  stop = true;
  reader.join();
  CHECK_EQ(torn.load(), 0);
  CHECK(reads.load() > 0);
  std::shared_ptr<const jsui::HostBuffers::Copy> last = b.published("m");
  CHECK(last && reinterpret_cast<const float*>(last->bytes.data())[0] == 3000.0f);
}

// The JS side of JS-thread mode: JS reads the copy published at the start of
// the tick (a change waits for the next one), and a view JS keeps holds its
// frame instead of being detached when the storage moves.
TEST(host_buffers_js_thread_mode) {
  BufHost h(true);
  float pos[2] = {1, 2};
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);
  CHECK(h.load(kReadBundle));
  h.tick();
  CHECK_EQ(h.take(), std::string());
  h.post("read");
  CHECK_EQ(h.take(), std::string());  // on_message runs in the tick
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(pos=["Float32Array",[1,2],true])"));
  h.post("keep");
  pos[0] = 5;
  pos[1] = 6;
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, pos, 2);
  h.post("read");  // before the tick publishes it: the last frame's copy
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(pos=["Float32Array",[1,2],true] changed=["pos"])"));
  h.post("kept");
  h.post("keptValues");
  h.post("read");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(kept=[2,false] keptValues=[1,2] pos=["Float32Array",[5,6],true])"));
  // Growing moves the storage; the kept copy is not detached.
  std::vector<float> big(5000, 1.f);
  jsui_set_buffer(h.rt, "pos", JSUI_BUFFER_F32, big.data(), big.size());
  h.tick();
  h.take();
  h.post("keptValues");
  h.post("keys");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(keptValues=[1,2] keys=["pos"])"));
  jsui_remove_buffer(h.rt, "pos");
  h.tick();
  CHECK_EQ(h.take(), std::string(R"(changed=["pos"])"));
  h.post("read");
  h.tick();
  CHECK_EQ(h.take(), std::string("pos=null"));
  CHECK(h.errors.empty());
}
