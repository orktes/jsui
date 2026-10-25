// End-to-end game data: a real @orktes/jsui-react bundle (packages/cli/test/e2e/
// host_data.jsx) driven through the C API. Shared state set before the
// bundle loads, per-key re-renders, merge patches that re-render only the
// changed memoized row, Host.call answered on a later frame (resolve, reject,
// cancelled by a reload), HostState.set reaching on_state, jsui_call_ui
// answered by Host.handle / useHostHandler, and state surviving a reload.
// Built and run by the @orktes/jsui-cli test suite with
// JSUI_REACT_HOST_DATA_BUNDLE=<path>; skipped without it. Runs three times:
// as is, with JS on its own thread (the embedder's callbacks arrive inside
// the next tick instead of while JS runs; the ticks sync, JSUI_TICK_SYNC_JS)
// and in lockstep (JSUI_JS_THREAD_LOCKSTEP: as single-threaded).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct HostDataE2E {
  std::vector<std::pair<std::string, std::string>> messages, uiState;
  std::vector<std::pair<uint64_t, std::string>> calls;  // id, name + " " + args
  std::vector<uint64_t> cancelled;
  JsuiRuntime* rt = nullptr;
  double now = 0;

  int jsThread = JSUI_JS_THREAD_OFF;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<HostDataE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    cfg.on_state = [](void* u, const char* key, const char* json) {
      static_cast<HostDataE2E*>(u)->uiState.emplace_back(key, json ? json : "<null>");
    };
    cfg.state_user = this;
    cfg.on_call = [](void* u, uint64_t id, const char* name, const char* args) {
      static_cast<HostDataE2E*>(u)->calls.emplace_back(id, std::string(name) + " " + args);
    };
    cfg.on_call_cancel = [](void* u, uint64_t id) { static_cast<HostDataE2E*>(u)->cancelled.push_back(id); };
    cfg.call_user = this;
    rt = jsui_create(&cfg);
    if (jsui_set_js_thread(rt, jsThread) != 0) return false;
    // The game's data exists before the UI does.
    jsui_set_state(rt, "gold", "100");
    jsui_set_state(rt, "inventory", R"({"items":{"sword":{"count":1},"potion":{"count":2}}})");
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~HostDataE2E() {
    if (rt) jsui_destroy(rt);
  }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick_ex(rt, now, JSUI_TICK_SYNC_JS);
    }
  }
  std::string take() {
    std::string s;
    for (auto& [n, j] : messages) s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
  // Embedder callbacks the click's handlers made before the tick ran.
  size_t callbacksBeforeTick = 0;
  void click(float x, float y) {
    size_t before = calls.size() + uiState.size();
    jsui_send_mouse_move(rt, x, y);
    jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 1, 0);
    jsui_send_mouse_button(rt, x, y, JSUI_MOUSE_LEFT, 0, 0);
    callbacksBeforeTick = calls.size() + uiState.size() - before;
    tick(16);
  }
};

struct Result {
  int calls = 0, ok = -1;
  std::string payload;
};
void onResult(void* user, uint64_t, int ok, const char* payload) {
  auto* r = static_cast<Result*>(user);
  r->calls++;
  r->ok = ok;
  r->payload = payload ? payload : "";
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

}  // namespace

static void runHostData(int jsThread) {
  bool queued = jsThread == JSUI_JS_THREAD_ON;  // embedder callbacks wait for the tick
  const char* path = std::getenv("JSUI_REACT_HOST_DATA_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_HOST_DATA_BUNDLE, see packages/cli/test)\n");
    return;
  }
  HostDataE2E t;
  t.jsThread = jsThread;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);

  // First render already shows the state set before the bundle loaded.
  std::string m = t.take();
  CHECK(has(m, "gold=100"));
  CHECK(has(m, R"(inventory={"ids":["sword","potion"],"rowRenders":2})"));
  CHECK(has(m, "ready=true"));

  // A merge patch re-renders the inventory and only the changed row; the
  // wallet (another key) does not re-render.
  CHECK_EQ(jsui_patch_state(t.rt, "inventory", R"({"items":{"potion":{"count":3}}})"), 1);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(R"(inventory={"ids":["sword","potion"],"rowRenders":3})"));
  CHECK_EQ(jsui_patch_state(t.rt, "inventory", R"({"items":{"sword":null,"shield":{"count":1}}})"), 1);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(R"(inventory={"ids":["potion","shield"],"rowRenders":4})"));
  CHECK_EQ(jsui_set_state(t.rt, "gold", "90"), 1);
  t.tick(16);
  CHECK_EQ(t.take(), std::string("gold=90"));
  CHECK_EQ(jsui_set_state(t.rt, "gold", "90"), 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string());

  // Host.call answered on a later frame.
  t.click(20, 20);
  CHECK_EQ(t.calls.size(), size_t(1));
  CHECK_EQ(t.callbacksBeforeTick, size_t(queued ? 0 : 1));  // on_call: in the tick / at once
  if (t.calls.size() != 1) return;
  CHECK_EQ(t.calls[0].second, std::string(R"(shop.buy {"item":"potion"})"));
  t.tick(100);
  CHECK_EQ(t.take(), std::string());
  CHECK_EQ(jsui_resolve_call(t.rt, t.calls[0].first, R"({"ok":true,"gold":80})"), 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(R"(bought={"ok":true,"gold":80})"));
  t.click(20, 20);
  CHECK_EQ(jsui_reject_call(t.rt, t.calls.back().first, "not enough gold"), 0);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(R"(buyFailed={"message":"not enough gold","code":null})"));

  // UI-owned state reaches on_state and jsui_get_state.
  t.click(20, 60);
  CHECK_EQ(t.uiState.size(), size_t(1));
  CHECK_EQ(t.callbacksBeforeTick, size_t(queued ? 0 : 1));
  if (!t.uiState.empty()) CHECK_EQ(t.uiState[0].second, std::string(R"({"volume":0.25})"));
  const char* settings = jsui_get_state(t.rt, "settings");
  CHECK(settings && std::string(settings) == R"({"volume":0.25})");

  // Game -> UI call handled by an async useHostHandler.
  Result prompt;
  jsui_call_ui(t.rt, "ui.prompt", R"({"text":"Quit?"})", onResult, &prompt);
  t.tick(16);
  CHECK_EQ(t.take(), std::string(R"(status="prompt:Quit?")"));
  CHECK_EQ(prompt.calls, 0);
  t.tick(80);
  CHECK_EQ(prompt.calls, 1);
  CHECK_EQ(prompt.ok, 1);
  CHECK_EQ(prompt.payload, std::string(R"({"answer":"yes","text":"Quit?"})"));
  Result missing;
  jsui_call_ui(t.rt, "ui.nope", "null", onResult, &missing);
  t.tick(16);
  CHECK_EQ(missing.ok, 0);

  // A component mounting later reads the current value immediately.
  Result late;
  jsui_call_ui(t.rt, "ui.mountLate", "null", onResult, &late);
  t.tick(16);
  CHECK_EQ(late.payload, std::string("true"));
  CHECK_EQ(t.take(), std::string("gold=90"));

  // Reload with a call in flight: the call is cancelled, the state survives.
  t.click(20, 20);
  uint64_t inflight = t.calls.back().first;
  jsui_reload(t.rt);
  CHECK_EQ(t.cancelled.size(), size_t(1));
  if (!t.cancelled.empty()) CHECK_EQ(t.cancelled[0], inflight);
  CHECK_EQ(jsui_resolve_call(t.rt, inflight, "{}"), -1);
  t.tick(32);
  m = t.take();
  CHECK(has(m, "gold=90"));
  CHECK(has(m, R"(inventory={"ids":["potion","shield"],"rowRenders":2})"));
  err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
}

TEST(react_host_data_e2e) { runHostData(JSUI_JS_THREAD_OFF); }

// The same with JS on its own thread: the embedder's callbacks (on_message,
// on_state, on_call, on_call_cancel, Host.handle results) arrive in the tick.
TEST(react_host_data_e2e_js_thread_mode) { runHostData(JSUI_JS_THREAD_ON); }

// ... and in lockstep, where everything happens when it does single-threaded.
TEST(react_host_data_e2e_js_thread_lockstep) { runHostData(JSUI_JS_THREAD_LOCKSTEP); }
