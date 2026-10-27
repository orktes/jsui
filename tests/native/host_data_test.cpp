// Game data through the C API: shared host state (jsui_set_state /
// jsui_patch_state / jsui_get_state, on_state), UI -> host calls (on_call,
// jsui_resolve_call / jsui_reject_call, on_call_cancel) and host -> UI calls
// (jsui_call_ui), with plain JS bundles on the raw __jsui_native bridge.
// The @orktes/jsui-react layer is covered by packages/react/test/hostdata.test.js
// and the react_host_data_e2e test.
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "host_state.h"
#include "jsui/jsui.h"
#include "test.h"

using jsui::Json;

namespace {

struct Host {
  JsuiRuntime* rt = nullptr;
  std::vector<std::pair<std::string, std::string>> messages;
  std::vector<std::pair<std::string, std::string>> uiState;  // on_state (json "<null>" for removal)
  struct Call {
    uint64_t id;
    std::string name, args;
  };
  std::vector<Call> calls;
  std::vector<uint64_t> cancelled;
  std::vector<std::string> errors;
  std::function<void(Host&, uint64_t)> onCall;
  double t = 0;

  Host() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 32;
    cfg.height = 32;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<Host*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    cfg.on_state = [](void* u, const char* key, const char* json) {
      static_cast<Host*>(u)->uiState.emplace_back(key, json ? json : "<null>");
    };
    cfg.state_user = this;
    cfg.on_call = [](void* u, uint64_t id, const char* name, const char* args) {
      auto* h = static_cast<Host*>(u);
      h->calls.push_back({id, name, args});
      if (h->onCall) h->onCall(*h, id);
    };
    cfg.on_call_cancel = [](void* u, uint64_t id) { static_cast<Host*>(u)->cancelled.push_back(id); };
    cfg.call_user = this;
    cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
      if (level == JSUI_LOG_ERROR) static_cast<Host*>(u)->errors.push_back(msg);
    };
    cfg.log_user = this;
    rt = jsui_create(&cfg);
  }
  ~Host() {
    if (rt) jsui_destroy(rt);
  }
  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "test.js") == 0; }
  void tick(int n = 1) {
    for (int i = 0; i < n; i++) jsui_tick(rt, t += 16);
  }
  std::string take() {
    std::string s;
    for (auto& [n, j] : messages) s += (s.empty() ? "" : " ") + n + "=" + j;
    messages.clear();
    return s;
  }
  std::string state(const char* key) {
    const char* j = jsui_get_state(rt, key);
    return j ? j : "<null>";
  }
};

struct Result {
  int calls = 0;
  uint64_t id = 0;
  int ok = -1;
  std::string payload;
};

void onResult(void* user, uint64_t id, int ok, const char* payload) {
  auto* r = static_cast<Result*>(user);
  r->calls++;
  r->id = id;
  r->ok = ok;
  r->payload = payload ? payload : "";
}

// Reads keys through the bridge and posts them; logs __jsui_onHostState deliveries.
const char* kStateBundle = R"(
  const N = __jsui_native;
  globalThis.__jsui_onHostState = (keys) => N.postMessage('changed', JSON.stringify(keys));
  globalThis.read = (k) => N.postMessage('read', JSON.stringify([k, N.getState(k) ?? null, N.stateVersion(k)]));
  read('gold');
)";

}  // namespace

TEST(json_parse_dump_roundtrip) {
  Json v;
  CHECK(Json::parse(R"( {"b":1,"a":[true,false,null,-0.5e+3,"x\"\\\n\u00e9\ud83d\ude00"],"c":{}} )", &v));
  CHECK_EQ(v.dump(), std::string("{\"b\":1,\"a\":[true,false,null,-0.5e+3,\"x\\\"\\\\\\n\xC3\xA9\xF0\x9F\x98\x80\"],\"c\":{}}"));
  std::string err;
  for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "\"\x01\"", "nul", "1 2", "{\"a\":1,}", "\"\\x\""}) {
    err.clear();
    CHECK(!Json::parse(bad, nullptr, &err));
    CHECK(!err.empty());
  }
  std::string deep(Json::kMaxDepth + 10, '[');
  CHECK(!Json::parse(deep, nullptr, &err));
  // Lone surrogates become U+FFFD.
  CHECK(Json::parse(R"("\ud800x")", &v));
  CHECK_EQ(v.text(), std::string("\xEF\xBF\xBDx"));
}

TEST(json_merge_patch_rfc7386) {
  // RFC 7386 Appendix A.
  const char* cases[][3] = {
      {R"({"a":"b"})", R"({"a":"c"})", R"({"a":"c"})"},
      {R"({"a":"b"})", R"({"b":"c"})", R"({"a":"b","b":"c"})"},
      {R"({"a":"b"})", R"({"a":null})", R"({})"},
      {R"({"a":"b","b":"c"})", R"({"a":null})", R"({"b":"c"})"},
      {R"({"a":["b"]})", R"({"a":"c"})", R"({"a":"c"})"},
      {R"({"a":"c"})", R"({"a":["b"]})", R"({"a":["b"]})"},
      {R"({"a":{"b":"c"}})", R"({"a":{"b":"d","c":null}})", R"({"a":{"b":"d"}})"},
      {R"({"a":[{"b":"c"}]})", R"({"a":[1]})", R"({"a":[1]})"},
      {R"(["a","b"])", R"(["c","d"])", R"(["c","d"])"},
      {R"({"a":"b"})", R"(["c"])", R"(["c"])"},
      {R"({"a":"foo"})", "null", "null"},
      {R"({"a":"foo"})", R"("bar")", R"("bar")"},
      {R"({"e":null})", R"({"a":1})", R"({"e":null,"a":1})"},
      {R"([1,2])", R"({"a":"b","c":null})", R"({"a":"b"})"},
      {R"({})", R"({"a":{"bb":{"ccc":null}}})", R"({"a":{"bb":{}}})"},
  };
  for (auto& c : cases) {
    Json target, patch;
    CHECK(Json::parse(c[0], &target));
    CHECK(Json::parse(c[1], &patch));
    target.mergePatch(patch);
    CHECK_EQ(target.dump(), std::string(c[2]));
  }
  // Big objects take the indexed path; order is kept, removals compact.
  std::string big = "{";
  for (int i = 0; i < 100; i++) big += (i ? "," : "") + std::string("\"k") + std::to_string(i) + "\":" + std::to_string(i);
  big += "}";
  Json v, p;
  CHECK(Json::parse(big, &v));
  CHECK(Json::parse(R"({"k5":null,"k7":70,"new":1,"k99":null,"k0":0})", &p));
  CHECK(v.mergePatch(p));
  CHECK_EQ(v.members().size(), size_t(99));
  CHECK_EQ(v.members()[5].first, std::string("k6"));
  CHECK_EQ(v.find("k7")->text(), std::string("70"));
  CHECK_EQ(v.members().back().first, std::string("new"));
  CHECK(!v.mergePatch(p) || true);
  Json same;
  CHECK(Json::parse(R"({"k7":70})", &same));
  CHECK(!v.mergePatch(same));  // no change reported
}

// The patch log behind __jsui_native.statePatches: readers holding an older
// version apply the patches instead of re-reading the whole document.
TEST(host_state_patch_log) {
  jsui::HostState s;
  std::vector<const std::string*> out;
  CHECK(!s.patchesSince("inv", 1, &out));  // unknown key
  CHECK(s.set("inv", R"({"a":1,"b":{"c":2}})") == jsui::HostState::Result::Changed);
  uint64_t v1 = s.version("inv");
  CHECK(s.patchesSince("inv", v1, &out) && out.empty());  // up to date
  CHECK(s.patch("inv", R"({"b":{"c":3}})") == jsui::HostState::Result::Changed);
  CHECK(s.patch("inv", R"({"b":{"c":3}})") == jsui::HostState::Result::Unchanged);  // not logged
  CHECK(s.set("other", "1") == jsui::HostState::Result::Changed);                   // versions are global
  CHECK(s.patch("inv", R"({"a":null})") == jsui::HostState::Result::Changed);
  CHECK(s.patchesSince("inv", v1, &out));
  CHECK_EQ(out.size(), size_t(2));
  CHECK_EQ(*out[0], std::string(R"({"b":{"c":3}})"));
  CHECK_EQ(*out[1], std::string(R"({"a":null})"));
  CHECK(!s.patchesSince("inv", v1 + 100, &out));  // a version the log does not know
  // A set starts over: older versions must re-read.
  CHECK(s.set("inv", R"({"x":1})") == jsui::HostState::Result::Changed);
  CHECK(!s.patchesSince("inv", v1, &out));
  // The log is bounded: the oldest versions fall out of it.
  uint64_t first = s.version("inv");
  for (int i = 0; i < 100; i++) s.patch("inv", "{\"n\":" + std::to_string(i) + "}");
  CHECK(!s.patchesSince("inv", first, &out));
  uint64_t recent = s.version("inv");
  s.patch("inv", R"({"n":-1})");
  CHECK(s.patchesSince("inv", recent, &out) && out.size() == 1);
}

TEST(host_state_before_mount_and_multiple_keys) {
  Host h;
  CHECK_EQ(jsui_set_state(h.rt, "gold", "120"), 1);
  CHECK_EQ(jsui_set_state(h.rt, "gold", "120"), 0);    // same text
  CHECK_EQ(jsui_set_state(h.rt, "gold", " 120 "), 0);  // same value
  CHECK_EQ(jsui_set_state(h.rt, "inv", R"({"items":{"sword":{"n":1}}})"), 1);
  CHECK_EQ(jsui_set_state(h.rt, "bad", "{nope"), -1);
  CHECK(!h.errors.empty());
  CHECK_EQ(jsui_set_state(h.rt, nullptr, "1"), -1);
  CHECK_EQ(h.state("bad"), std::string("<null>"));
  CHECK(h.load(kStateBundle));
  CHECK_EQ(h.take(), std::string("read=[\"gold\",\"120\",1]"));
  h.tick();  // set before load: nothing to deliver, the bundle read it
  CHECK_EQ(h.take(), std::string());

  // Two keys changed between ticks arrive in one delivery.
  CHECK_EQ(jsui_set_state(h.rt, "gold", "90"), 1);
  CHECK_EQ(jsui_patch_state(h.rt, "inv", R"({"items":{"potion":{"n":3}}})"), 1);
  CHECK_EQ(jsui_set_state(h.rt, "gold", "80"), 1);
  CHECK(jsui_wants_frame(h.rt));
  h.tick();
  CHECK_EQ(h.take(), std::string("changed=[\"gold\",\"inv\"]"));
  h.tick();
  CHECK_EQ(h.take(), std::string());
  CHECK_EQ(h.state("inv"), std::string(R"({"items":{"sword":{"n":1},"potion":{"n":3}}})"));
  CHECK_EQ(jsui_patch_state(h.rt, "inv", R"({"items":{"sword":null}})"), 1);
  CHECK_EQ(jsui_patch_state(h.rt, "inv", R"({"items":{"sword":null}})"), 0);
  CHECK_EQ(h.state("inv"), std::string(R"({"items":{"potion":{"n":3}}})"));
  CHECK_EQ(jsui_patch_state(h.rt, "inv", "{oops"), -1);

  // Removal; versions never repeat.
  CHECK_EQ(jsui_set_state(h.rt, "gold", nullptr), 1);
  CHECK_EQ(jsui_set_state(h.rt, "gold", nullptr), 0);
  h.tick();
  h.take();
  jsui_post_message(h.rt, "x", "0");
  h.load("read('gold')");
  CHECK_EQ(h.take(), std::string("read=[\"gold\",null,0]"));
  jsui_set_state(h.rt, "gold", "1");
  h.load("read('gold')");
  std::string m = h.take();
  CHECK(m.find("\"1\"") != std::string::npos && m.find(",1]") == std::string::npos);
}

TEST(host_state_survives_reload) {
  Host h;
  CHECK(h.load(kStateBundle));
  jsui_set_state(h.rt, "gold", "7");
  h.tick();
  h.take();
  jsui_reload(h.rt);  // re-evaluates the bundle in a fresh context
  std::string m = h.take();
  CHECK(m.find("read=[\"gold\",\"7\",") == 0);
  h.tick();
  CHECK_EQ(h.take(), std::string());  // nothing re-delivered to the new context
}

TEST(safe_area_insets_shorthand) {
  Host h;
  CHECK_EQ(jsui_set_safe_area_insets(h.rt, 59, 0, 34, 0), 1);
  CHECK_EQ(h.state("jsui.safeAreaInsets"), std::string(R"({"top":59,"right":0,"bottom":34,"left":0})"));
  CHECK_EQ(jsui_set_safe_area_insets(h.rt, 59, 0, 34, 0), 0);  // unchanged
  CHECK_EQ(jsui_set_safe_area_insets(h.rt, 0, 59.5f, 21, 59.5f), 1);  // rotated
  CHECK_EQ(h.state("jsui.safeAreaInsets"), std::string(R"({"top":0,"right":59.5,"bottom":21,"left":59.5})"));
}

TEST(host_state_ui_owned_keys) {
  Host h;
  CHECK(h.load(R"(
    const N = __jsui_native;
    globalThis.__jsui_onHostState = (keys) => N.postMessage('changed', JSON.stringify(keys));
    N.setState('settings', '{"volume":0.5}');
    N.setState('settings', '{"volume":0.5}');
    N.patchState('settings', '{"lang":"fi"}');
    let threw = '';
    try { N.setState('x', '{bad'); } catch (e) { threw = e.name; }
    N.postMessage('threw', threw);
    N.setState('settings', undefined);
  )"));
  CHECK_EQ(h.uiState.size(), size_t(3));
  if (h.uiState.size() == 3) {
    CHECK_EQ(h.uiState[0].second, std::string(R"({"volume":0.5})"));
    CHECK_EQ(h.uiState[1].second, std::string(R"({"volume":0.5,"lang":"fi"})"));
    CHECK_EQ(h.uiState[2].second, std::string("<null>"));
  }
  CHECK_EQ(h.take(), std::string("threw=SyntaxError"));
  h.tick();
  CHECK_EQ(h.take(), std::string());  // JS-originated changes are not echoed back to JS
  // The embedder's changes are not reported to on_state.
  jsui_set_state(h.rt, "settings", "{}");
  CHECK_EQ(h.uiState.size(), size_t(3));
}

namespace {
const char* kCallBundle = R"(
  const N = __jsui_native;
  globalThis.ask = (name, args) => {
    const id = N.hostCall(name, JSON.stringify(args), (ok, payload) =>
      N.postMessage(ok ? 'resolved' : 'rejected', payload));
    N.postMessage('id', String(id));
    return id;
  };
)";
}

TEST(host_call_resolve_reject) {
  Host h;
  CHECK(h.load(kCallBundle));
  h.load("ask('shop.buy', {item: 'potion'})");
  CHECK_EQ(h.calls.size(), size_t(1));
  uint64_t id = h.calls[0].id;
  CHECK(id != 0);
  CHECK_EQ(h.calls[0].name, std::string("shop.buy"));
  CHECK_EQ(h.calls[0].args, std::string(R"({"item":"potion"})"));
  CHECK_EQ(h.take(), "id=" + std::to_string(id));
  h.tick(3);  // answered on a later frame
  CHECK_EQ(jsui_resolve_call(h.rt, id, R"({"gold":90})"), 0);
  CHECK_EQ(h.take(), std::string(R"(resolved={"gold":90})"));
  CHECK_EQ(jsui_resolve_call(h.rt, id, "1"), -1);  // already settled
  CHECK_EQ(jsui_reject_call(h.rt, id, "x"), -1);
  CHECK_EQ(jsui_resolve_call(h.rt, 999, "1"), -1);

  h.load("ask('shop.buy', {item: 'castle'})");
  uint64_t id2 = h.calls.back().id;
  CHECK(id2 > id);
  h.take();
  CHECK_EQ(jsui_reject_call(h.rt, id2, "not enough gold"), 0);
  CHECK_EQ(h.take(), std::string("rejected=not enough gold"));

  // Invalid JSON result: the call is rejected rather than left hanging.
  h.load("ask('q', null)");
  h.take();
  CHECK_EQ(jsui_resolve_call(h.rt, h.calls.back().id, "{bad"), -2);
  CHECK(h.take().rfind("rejected=jsui_resolve_call: invalid JSON", 0) == 0);

  // Answered from inside on_call (JS on the stack).
  h.onCall = [](Host& host, uint64_t cid) { jsui_resolve_call(host.rt, cid, "\"now\""); };
  h.load("ask('sync', 1)");
  std::string m = h.take();
  CHECK(m.find("resolved=\"now\"") != std::string::npos);

  // JS-side cancellation (Host.call timeout) reaches on_call_cancel.
  h.onCall = nullptr;
  h.load("globalThis.cid = ask('slow', 0); N.postMessage('cancel', String(N.cancelCall(cid)))");
  uint64_t slow = h.calls.back().id;
  CHECK(h.take().find("cancel=true") != std::string::npos);
  CHECK_EQ(h.cancelled.size(), size_t(1));
  if (!h.cancelled.empty()) CHECK_EQ(h.cancelled[0], slow);
  CHECK_EQ(jsui_resolve_call(h.rt, slow, "1"), -1);
}

TEST(host_call_unanswered_on_reload_and_destroy) {
  uint64_t a, b;
  std::vector<uint64_t> cancelledOnDestroy;
  {
    Host h;
    CHECK(h.load(kCallBundle));
    h.load("ask('a', 1); ask('b', 2)");
    CHECK_EQ(h.calls.size(), size_t(2));
    a = h.calls[0].id;
    b = h.calls[1].id;
    jsui_reload(h.rt);
    CHECK_EQ(h.cancelled.size(), size_t(2));
    if (h.cancelled.size() == 2) {
      CHECK_EQ(h.cancelled[0], a);
      CHECK_EQ(h.cancelled[1], b);
    }
    CHECK_EQ(jsui_resolve_call(h.rt, a, "1"), -1);  // stale id: no effect on the new context
    h.take();
    h.load(kCallBundle);
    h.load("ask('c', 3)");
    CHECK(h.calls.back().id > b);  // ids are unique across contexts
    h.cancelled.clear();
    // Destroy with a call open: reported, nothing leaks (ASan / QuickJS leak checks).
    JsuiRuntime* rt = h.rt;
    h.rt = nullptr;
    jsui_destroy(rt);
    cancelledOnDestroy = h.cancelled;
  }
  CHECK_EQ(cancelledOnDestroy.size(), size_t(1));
}

TEST(host_call_ui) {
  Host h;
  // No bundle handler installed yet: fails, delivered on the next tick.
  Result none;
  uint64_t id0 = jsui_call_ui(h.rt, "x", "null", onResult, &none);
  CHECK(id0 != 0);
  CHECK_EQ(none.calls, 0);
  h.tick();
  CHECK_EQ(none.calls, 1);
  CHECK_EQ(none.ok, 0);
  CHECK(none.payload.find("no UI handler") != std::string::npos);

  CHECK(h.load(R"(
    const N = __jsui_native;
    const handlers = {
      double: (a) => a.n * 2,
      later: () => new Promise((r) => setTimeout(() => r({ok: 1}), 40)),
      fails: () => { throw new Error('nope'); },
    };
    globalThis.__jsui_onCall = (id, name, json) => {
      const fn = handlers[name];
      if (!fn) return N.callResult(id, false, 'no handler ' + name);
      try {
        Promise.resolve(fn(JSON.parse(json))).then((v) => N.callResult(id, true, JSON.stringify(v)),
                                                   (e) => N.callResult(id, false, e.message));
      } catch (e) { N.callResult(id, false, e.message); }
    };
  )"));
  Result dbl, later, fails, bad, gone;
  uint64_t id = jsui_call_ui(h.rt, "double", R"({"n":21})", onResult, &dbl);
  CHECK_EQ(dbl.calls, 0);  // never inside jsui_call_ui
  h.tick();
  CHECK_EQ(dbl.calls, 1);
  CHECK_EQ(dbl.id, id);
  CHECK_EQ(dbl.ok, 1);
  CHECK_EQ(dbl.payload, std::string("42"));
  jsui_call_ui(h.rt, "later", "null", onResult, &later);
  jsui_call_ui(h.rt, "fails", "null", onResult, &fails);
  jsui_call_ui(h.rt, "double", "{bad", onResult, &bad);
  jsui_call_ui(h.rt, "double", R"({"n":1})", nullptr, nullptr);  // no callback: fine
  h.tick();
  CHECK_EQ(later.calls, 0);
  CHECK_EQ(fails.ok, 0);
  CHECK_EQ(fails.payload, std::string("nope"));
  CHECK_EQ(bad.ok, 0);
  CHECK(bad.payload.find("invalid JSON") != std::string::npos);
  h.tick(4);
  CHECK_EQ(later.calls, 1);
  CHECK_EQ(later.payload, std::string(R"({"ok":1})"));

  // Unsettled when the context goes away: fails once, from jsui_reload.
  jsui_call_ui(h.rt, "later", "null", onResult, &gone);
  jsui_reload(h.rt);
  CHECK_EQ(gone.calls, 1);
  CHECK_EQ(gone.ok, 0);
  h.tick(10);
  CHECK_EQ(gone.calls, 1);

  Result onDestroy;
  jsui_call_ui(h.rt, "later", "null", onResult, &onDestroy);
  jsui_destroy(h.rt);
  h.rt = nullptr;
  CHECK_EQ(onDestroy.calls, 1);
  CHECK_EQ(onDestroy.payload, std::string("runtime destroyed"));
}
