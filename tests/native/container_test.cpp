// JSUIContainer (render/container_view.h): a child runtime running inside a
// node of the parent. Raw-bridge bundles on both sides (the React component
// is tested in packages/react/test/container.test.js):
// - the child's frame is painted into the node's content box (padding and
//   border excluded) and repaints the parent when it changes;
// - inline source (`load` command) and bundleSrc through the parent's loader;
// - host messaging both ways: the child's postMessage / Host.call / state
//   writes become containerevent events, the parent's post / call / resolve /
//   state commands reach the child;
// - pointer input over the node goes to the child in its coordinates, and a
//   press the child consumes is not the parent's.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/value.h"
#include "jsui/jsui.h"
#include "platform/loader.h"
#include "render/container_view.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

constexpr int kW = 160, kH = 100;

// The child: a View filling its root, red until a 'ping' turns it blue;
// reports presses, answers calls and talks to its host.
const char kChild[] = R"JS(
const N = __jsui_native;
const v = N.createNode('View');
N.setProps(v, {style: {flex: 1, backgroundColor: '#ff0000'}, __events: ['pointerDown']});
N.appendChild(N.rootId, v);
globalThis.__jsui_dispatchEvent = (e) => {
  if (e.type === 'pointerdown') { N.postMessage('down', {x: e.localX, y: e.localY}); return 1; }
  return 0;
};
globalThis.__jsui_onHostMessage = (name, json) => {
  if (name !== 'ping') return;
  N.setProps(v, {style: {flex: 1, backgroundColor: '#0000ff'}, __events: ['pointerDown']});
  N.postMessage('pong', {n: JSON.parse(json).n + 1, theme: JSON.parse(N.getState('theme') || 'null')});
};
globalThis.__jsui_onCall = (id, name, json) => {
  const args = typeof json === 'string' ? JSON.parse(json) : json;
  N.callResult(id, true, JSON.stringify({doubled: args.n * 2}));
};
N.setState('ready', JSON.stringify(true));
N.hostCall('add', JSON.stringify({q: 21}), (ok, payload) => N.postMessage('answer', {ok, payload: JSON.parse(payload)}));
N.postMessage('hello', {from: 'child'});
)JS";

// The parent: one container at (10, 20), 100 x 50 with 5 px padding; its
// events go to the embedder ('ev'), its Host.calls are answered at once, and
// embedder messages named 'cmd' are container commands.
const char kParent[] = R"JS(
const N = __jsui_native;
const c = N.createNode('JSUIContainer');
N.setProps(c, {style: {position: 'absolute', left: 10, top: 20, width: 100, height: 50, padding: 5,
                       backgroundColor: '#00ff00'},
               __events: ['containerEvent']});
N.appendChild(N.rootId, c);
globalThis.__jsui_dispatchEvent = (e) => {
  if (e.type !== 'containerevent') return 0;
  N.postMessage('ev', e);
  if (e.kind === 'call') N.containerCommand(c, {op: 'resolve', callId: e.callId, value: {sum: e.args.q * 2}});
  return 1;
};
globalThis.__jsui_onHostMessage = (name, json) => {
  if (name === 'cmd') N.containerCommand(c, JSON.parse(json));
  if (name === 'props') N.setProps(c, Object.assign(JSON.parse(json), {__events: ['containerEvent']}));
};
N.postMessage('id', c);
)JS";

struct Parent {
  JsuiRuntime* rt = nullptr;
  std::vector<std::pair<std::string, std::string>> messages;
  std::vector<std::string> errors;
  std::map<std::string, std::string> files;  // what the embedder's loader serves
  std::vector<std::string> loaded;
  double t = 0;
  uint32_t node = 0;
  std::vector<uint32_t> px;

  Parent() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = kW;
    cfg.height = kH;
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<Parent*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
      if (level == JSUI_LOG_ERROR) static_cast<Parent*>(u)->errors.push_back(msg);
    };
    cfg.log_user = this;
    cfg.load = [](void* u, JsuiRuntime* rt, uint64_t id, const char* url) {
      auto* p = static_cast<Parent*>(u);
      p->loaded.push_back(url);
      auto it = p->files.find(url);
      if (it == p->files.end()) jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
      else jsui_resource_complete(rt, id, 200, "text/javascript", it->second.data(), it->second.size());
    };
    cfg.load_user = this;
    cfg.base_url = "game://ui/";
    rt = jsui_create(&cfg);
  }
  ~Parent() { jsui_destroy(rt); }

  bool start() {
    if (jsui_load_bundle_source(rt, kParent, sizeof(kParent) - 1, "parent.js") != 0) return false;
    if (!until([&] { return has("id"); })) return false;
    node = uint32_t(take("id").back().asNumber());
    return node != 0;
  }
  void tick() {
    t += 16;
    jsui_tick(rt, t);
    jsui_sync(rt, 1000);
  }
  bool until(const std::function<bool()>& pred, int budgetMs = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
      tick();
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
  }
  void post(const char* name, const Value& v) { jsui_post_message(rt, name, v.toJson().c_str()); }
  void command(Value::Object cmd) { post("cmd", Value(std::move(cmd))); }
  bool has(const std::string& name) const {
    for (auto& m : messages)
      if (m.first == name) return true;
    return false;
  }
  std::vector<Value> take(const std::string& name) {
    std::vector<Value> out;
    for (auto it = messages.begin(); it != messages.end();) {
      if (it->first == name) {
        out.push_back(Value::parseJson(it->second));
        it = messages.erase(it);
      } else {
        ++it;
      }
    }
    return out;
  }
  // A container event of `kind` (and `name`, for messages) arrived.
  const Value* event(const std::string& kind, const std::string& name = "") {
    static Value found;
    for (auto& m : messages) {
      if (m.first != "ev") continue;
      Value v = Value::parseJson(m.second);
      if (v["kind"].asString() == kind && (name.empty() || v["name"].asString() == name)) {
        found = v;
        return &found;
      }
    }
    return nullptr;
  }
  JsuiRuntime* child() { return jsui::container::child(jsui::internalRuntime(rt)->findNode(node)); }
  void render() {
    px.assign(size_t(kW) * kH, 0);
    jsui_render_rgba(rt, px.data(), kW, kH, size_t(kW) * 4);
  }
  uint32_t at(int x, int y) const { return px[size_t(y) * kW + x]; }
};

constexpr uint32_t kRed = 0xff0000ff, kBlue = 0xffff0000, kGreen = 0xff00ff00;

}  // namespace

TEST(container_inline_bundle_paints_and_talks) {
  Parent p;
  CHECK(p.start());
  Value::Object load{{"op", Value("load")}, {"data", Value(std::string(kChild))}};
  p.command(load);
  CHECK(p.until([&] { return p.event("message", "hello") && p.event("message", "answer"); }));
  CHECK(p.child() != nullptr);
  if (!p.child()) return;
  // The child's Host.call reached the parent and its answer came back.
  const Value* call = p.event("call");
  CHECK(call && call->operator[]("name").asString() == "add");
  const Value* answer = p.event("message", "answer");
  CHECK(answer && answer->operator[]("payload")["ok"].asBool());
  CHECK(answer && answer->operator[]("payload")["payload"]["sum"].asNumber() == 42);
  // Its state write.
  const Value* state = p.event("state");
  CHECK(state && state->operator[]("key").asString() == "ready" && state->operator[]("value").asBool());

  // Painted into the content box: (15, 25) .. (105, 65).
  p.render();
  CHECK_EQ(p.at(50, 45), kRed);
  CHECK_EQ(p.at(16, 26), kRed);
  CHECK_EQ(p.at(104, 64), kRed);
  CHECK_EQ(p.at(12, 22), kGreen);  // padding: the container's own background
  CHECK_EQ(p.at(5, 45), 0u);
  CHECK_EQ(p.at(120, 45), 0u);
  // The child's viewport is the content box.
  float w = 0, h = 0;
  jsui_surface_get_size(p.child(), &w, &h);
  CHECK_NEAR(w, 90, 0.01);
  CHECK_NEAR(h, 40, 0.01);

  // Parent -> child: hostState via a state command, then a message; the
  // child repaints the parent.
  p.command({{"op", Value("state")}, {"key", Value("theme")}, {"value", Value("dark")}});
  p.command({{"op", Value("post")}, {"name", Value("ping")}, {"payload", Value(Value::Object{{"n", Value(1)}})}});
  CHECK(p.until([&] { return p.event("message", "pong") != nullptr; }));
  const Value* pong = p.event("message", "pong");
  CHECK(pong && pong->operator[]("payload")["n"].asNumber() == 2);
  CHECK(pong && pong->operator[]("payload")["theme"].asString() == "dark");
  CHECK(jsui_needs_redraw(p.rt));
  p.render();
  CHECK_EQ(p.at(50, 45), kBlue);
  CHECK_EQ(p.at(12, 22), kGreen);

  // Parent -> child call (Host.handle side).
  p.command({{"op", Value("call")}, {"seq", Value(7)}, {"name", Value("double")},
             {"args", Value(Value::Object{{"n", Value(4)}})}});
  CHECK(p.until([&] { return p.event("callResult") != nullptr; }));
  const Value* result = p.event("callResult");
  CHECK(result && result->operator[]("ok").asBool() && result->operator[]("seq").asNumber() == 7);
  CHECK(result && result->operator[]("result")["doubled"].asNumber() == 8);
  CHECK(p.errors.empty());
  for (auto& e : p.errors) std::fprintf(stderr, "    error: %s\n", e.c_str());
}

TEST(container_input_goes_to_the_child) {
  Parent p;
  CHECK(p.start());
  p.command({{"op", Value("load")}, {"data", Value(std::string(kChild))}});
  CHECK(p.until([&] { return p.event("message", "hello") != nullptr; }));
  p.render();
  // A press in the content box: the child's, at child coordinates.
  CHECK(jsui_send_mouse_move(p.rt, 30, 40));
  CHECK(jsui_send_mouse_button(p.rt, 30, 40, JSUI_MOUSE_LEFT, 1, 0));
  CHECK(jsui_send_mouse_button(p.rt, 30, 40, JSUI_MOUSE_LEFT, 0, 0));
  CHECK(p.until([&] { return p.event("message", "down") != nullptr; }));
  const Value* down = p.event("message", "down");
  CHECK(down && std::abs(down->operator[]("payload")["x"].asNumber() - 15) < 0.01);
  CHECK(down && std::abs(down->operator[]("payload")["y"].asNumber() - 15) < 0.01);
  CHECK_EQ(jsui_hit_test(p.rt, 30, 40), JSUI_HIT_POINTER);
  // Outside the container nothing captures (the game gets it).
  CHECK_EQ(jsui_hit_test(p.rt, 140, 90), 0);
  CHECK(!jsui_send_mouse_button(p.rt, 140, 90, JSUI_MOUSE_LEFT, 1, 0));
  jsui_send_mouse_button(p.rt, 140, 90, JSUI_MOUSE_LEFT, 0, 0);
}

TEST(container_bundle_src_through_the_parent_loader) {
  Parent p;
  p.files["game://ui/apps/child.js"] = kChild;
  CHECK(p.start());
  p.post("props", Value(Value::Object{
                      {"bundleSrc", Value("apps/child.js")},
                      {"style", Value(Value::Object{{"position", Value("absolute")}, {"width", Value(50)},
                                                    {"height", Value(50)}})}}));
  CHECK(p.until([&] { return p.event("message", "hello") != nullptr; }));
  bool viaParent = false;
  for (auto& u : p.loaded)
    if (u == "game://ui/apps/child.js") viaParent = true;
  CHECK(viaParent);
  // A missing source: an error event, no child bundle.
  p.post("props", Value(Value::Object{{"bundleSrc", Value("apps/missing.js")}}));
  CHECK(p.until([&] { return p.event("error") != nullptr; }));
}

TEST(container_fetch_keeps_method_headers_and_body) {
  Parent p;
  // The parent's loader, whole requests (the embedder's load callback only
  // sees URLs): what the child's fetch() hands its parent.
  std::vector<jsui::LoadRequest> seen;
  jsui::ResourceLoader& loader = jsui::internalRuntime(p.rt)->loader;
  loader.forward = [&](uint64_t id, jsui::LoadRequest req) {
    seen.push_back(req);
    jsui::Resource r;
    r.status = 201;
    r.statusText = "Created";
    r.headers.push_back({"x-reply", "yes"});
    std::string body = req.url.find("score") != std::string::npos ? "stored" : "";
    if (req.url.find("child.js") != std::string::npos) body = kChild;
    r.data.assign(body.begin(), body.end());
    if (body.empty()) r.status = 404;
    loader.complete(id, std::move(r));
  };
  CHECK(p.start());
  const char child[] = R"JS(
    fetch('api/score', {method: 'POST', headers: {'X-Token': 'abc'}, body: 'points=7'})
      .then((r) => r.text().then((t) => __jsui_native.postMessage('fetched', {status: r.status, text: t,
                                                                             reply: r.headers.get('x-reply')})));
  )JS";
  p.command({{"op", Value("load")}, {"data", Value(std::string(child))}});
  CHECK(p.until([&] { return p.event("message", "fetched") != nullptr; }));
  const Value* got = p.event("message", "fetched");
  CHECK(got && got->operator[]("payload")["status"].asNumber() == 201);
  CHECK(got && got->operator[]("payload")["text"].asString() == "stored");
  CHECK(got && got->operator[]("payload")["reply"].asString() == "yes");
  const jsui::LoadRequest* post = nullptr;
  for (auto& r : seen)
    if (r.url == "game://ui/api/score") post = &r;
  CHECK(post != nullptr);
  if (!post) return;
  CHECK_EQ(post->method, std::string("POST"));
  CHECK_EQ(std::string(post->body.begin(), post->body.end()), std::string("points=7"));
  bool token = false;
  for (auto& [k, v] : post->headers)
    if ((k == "X-Token" || k == "x-token") && v == "abc") token = true;
  CHECK(token);
}
