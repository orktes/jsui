// Tests for the JS host (globals, timers, rAF, errors, bridge, host messages,
// fonts, watchdog) and the resource loader (files, data:, http, dev reload).
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "net/http_client.h"
#include "platform/loader.h"
#include "runtime.h"
#include "test.h"
#include "test_http_server.h"

using namespace jsui;

namespace {

struct Host {
  JsuiRuntime* rt = nullptr;
  std::vector<std::pair<std::string, std::string>> messages;
  std::vector<std::string> errors, logs;
  double t = 0;

  explicit Host(std::function<void(JsuiConfig&)> tweak = nullptr) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 64;
    cfg.height = 64;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<Host*>(u)->messages.emplace_back(name, json);
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
      auto* h = static_cast<Host*>(u);
      (level == JSUI_LOG_ERROR ? h->errors : h->logs).push_back(msg);
    };
    cfg.log_user = this;
    if (tweak) tweak(cfg);
    rt = jsui_create(&cfg);
  }
  ~Host() { jsui_destroy(rt); }
  Runtime& runtime() { return *reinterpret_cast<std::unique_ptr<Runtime>*>(rt)->get(); }

  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "test.js") == 0; }
  void tick(int n = 1, double step = 16) {
    for (int i = 0; i < n; i++) {
      t += step;
      jsui_tick(rt, t);
    }
  }
  // Ticks (simulated 16 ms, real ~1 ms) until pred() or the real-time budget runs out.
  bool until(std::function<bool()> pred, int budgetMs = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < end) {
      tick();
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
  }
  const std::string* message(const std::string& name) const {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
      if (it->first == name) return &it->second;
    return nullptr;
  }
  bool hasError(const std::string& needle) const {
    for (auto& e : errors)
      if (e.find(needle) != std::string::npos) return true;
    return false;
  }
};

}  // namespace

TEST(host_globals_smoke_bundle) {
  Host h;
  CHECK_EQ(jsui_load_bundle_url(h.rt, "tests/native/js/globals_smoke.js"), 0);
  bool done = h.until([&] { return h.message("smoke") != nullptr; });
  CHECK(done);
  if (!done) {
    for (auto& e : h.errors) std::fprintf(stderr, "    error: %s\n", e.c_str());
    return;
  }
  Value r = Value::parseJson(*h.message("smoke"));
  for (auto& f : r["failures"].asArray()) std::fprintf(stderr, "    smoke failure: %s\n", f.asString().c_str());
  CHECK_EQ(r["failures"].asArray().size(), size_t(0));
  CHECK(r["passed"].asNumber() > 60);
  // Only the deliberate console.error('Warning: %s%s', ...) of the bundle.
  for (auto& e : h.errors)
    if (e.rfind("Warning: Each child", 0) != 0) std::fprintf(stderr, "    unexpected error: %s\n", e.c_str());
  CHECK(h.errors.size() == 1);
  bool formatted = false;
  for (auto& l : h.logs)
    if (l == "smoke: bob is 42.9 years and { a: [ 1, 2 ] } extra") formatted = true;
  CHECK(formatted);
}

TEST(host_timers_order_and_args) {
  Host h;
  h.load(R"(
    const out = [];
    setTimeout((a, b) => out.push('t20:' + a + b), 20, 'x', 'y');
    setTimeout(() => out.push('t5'), 5);
    setTimeout(() => { out.push('t0'); setTimeout(() => out.push('nested0'), 0); }, 0);
    const iv = setInterval(() => { out.push('iv'); if (out.filter(x => x === 'iv').length === 2) clearInterval(iv); }, 16);
    setTimeout(() => __jsui_native.postMessage('order', out.join(',')), 100);
  )");
  h.tick(10);
  const std::string* m = h.message("order");
  CHECK(m != nullptr);
  if (m) CHECK_EQ(*m, std::string("t0,t5,iv,nested0,t20:xy,iv"));
}

TEST(host_raf_and_microtasks) {
  Host h;
  h.load(R"(
    const seen = [];
    function frame(ts) { seen.push(ts); if (seen.length < 3) requestAnimationFrame(frame); else __jsui_native.postMessage('raf', seen); }
    requestAnimationFrame(frame);
    const c = requestAnimationFrame(() => seen.push('cancelled'));
    cancelAnimationFrame(c);
    Promise.resolve(1).then(v => __jsui_native.postMessage('micro', {v, now: performance.now()}));
  )");
  CHECK(h.message("micro") != nullptr);  // drained right after eval
  h.tick(5);
  const std::string* m = h.message("raf");
  CHECK(m != nullptr);
  if (m) CHECK_EQ(*m, std::string("[16,32,48]"));
}

TEST(host_messages_both_ways) {
  Host h;
  h.load(R"(
    globalThis.__jsui_onHostMessage = (name, json) => {
      const v = JSON.parse(json);
      __jsui_native.postMessage('echo:' + name, {got: v, n: v.n * 2});
    };
  )");
  jsui_post_message(h.rt, "hello", R"({"n":21,"s":"hé"})");
  const std::string* m = h.message("echo:hello");
  CHECK(m != nullptr);
  if (m) CHECK_EQ(*m, std::string(R"({"got":{"n":21,"s":"hé"},"n":42})"));
  jsui_post_message(h.rt, "noHandlerIsFine", nullptr);
}

TEST(host_errors_reported_with_stack) {
  Host h;
  h.load(R"(
    function boom() { throw new TypeError('kaboom'); }
    setTimeout(function timerCb() { boom(); }, 0);
  )");
  h.tick();
  CHECK(h.hasError("TypeError: kaboom"));
  CHECK(h.hasError("at boom"));
  const char* last = jsui_last_error(h.rt);
  CHECK(last && std::strstr(last, "kaboom"));

  // Unhandled rejection is reported; one handled later in the same turn is not.
  h.errors.clear();
  h.load(R"(
    Promise.reject(new Error('nobody catches me'));
    const p = Promise.reject(new Error('caught later'));
    queueMicrotask(() => p.catch(() => {}));
    reportError(new RangeError('reported explicitly'));
  )");
  CHECK(h.hasError("Unhandled promise rejection: Error: nobody catches me"));
  CHECK(!h.hasError("caught later"));
  CHECK(h.hasError("Uncaught RangeError: reported explicitly"));

  // Syntax errors fail the load.
  h.errors.clear();
  CHECK(!h.load("function ("));
  CHECK(h.hasError("SyntaxError"));
  // Throwing in the event / layout callbacks must not crash.
  h.load("globalThis.__jsui_onHostMessage = () => { throw new Error('in handler'); };");
  jsui_post_message(h.rt, "x", "1");
  CHECK(h.hasError("in handler"));
}

TEST(host_watchdog_interrupts_runaway_loop) {
  Host h([](JsuiConfig& c) { c.js_timeout_ms = 100; });
  h.load("setTimeout(() => { for (;;) {} }, 0); setTimeout(() => __jsui_native.postMessage('alive', 1), 32);");
  auto start = std::chrono::steady_clock::now();
  h.tick();
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(ms < 2000);
  CHECK(h.hasError("timed out"));
  h.tick(3);
  CHECK(h.message("alive") != nullptr);  // the engine keeps working afterwards
  // Deep recursion hits the stack limit instead of crashing.
  h.errors.clear();
  h.load("function r(n) { return r(n + 1) + 1; } r(0);");
  CHECK(h.hasError("RangeError") || h.hasError("stack"));
  // The memory limit turns into an exception.
  Host small([](JsuiConfig& c) { c.js_memory_limit_mb = 16; });
  small.load("const a = []; for (;;) a.push(new Array(100000).fill(1));");
  CHECK(small.hasError("out of memory") || small.hasError("InternalError") || small.hasError("RangeError"));
}

TEST(host_bridge_measure_patchstyle_layout) {
  Host h;
  h.load(R"(
    const n = __jsui_native;
    const v = n.createNode('View');
    n.setProps(v, {style: {width: 20, height: 10, margin: 4, opacity: 0.5, zIndex: 2}, __events: ['layout']});
    n.appendChild(n.rootId, v);
    globalThis.v = v;
    globalThis.layouts = [];
    globalThis.__jsui_onLayout = (id, r) => layouts.push([id, r.x, r.y, r.width, r.height]);
    const m = n.measure(v);
    n.postMessage('measure', m);
    n.patchStyle(v, {opacity: 1, width: 30, margin: 0, zIndex: null, cursor: undefined});
    n.postMessage('afterPatch', n.measure(v));
  )");
  CHECK_EQ(*h.message("measure"), std::string(R"({"x":4,"y":4,"width":20,"height":10,"pageX":4,"pageY":4})"));
  CHECK_EQ(*h.message("afterPatch"), std::string(R"({"x":0,"y":0,"width":30,"height":10,"pageX":0,"pageY":0})"));
  Node* node = h.runtime().doc.get(2);
  CHECK(node != nullptr);
  if (node) {
    CHECK_EQ(node->props()["style"]["opacity"].asNumber(), 1.0);
    CHECK(!node->props()["style"].has("zIndex"));
    CHECK_EQ(node->props()["style"]["margin"].asNumber(), 0.0);
    CHECK_EQ(node->props()["__events"].asArray().size(), size_t(1));  // other props preserved
  }
  h.tick();
  h.load("__jsui_native.postMessage('layouts', layouts);");
  const std::string* l = h.message("layouts");
  CHECK(l && l->find("[2,0,0,30,10]") != std::string::npos);
  // Bad ids and cycles are ignored rather than crashing.
  CHECK(h.load(R"({
    const n = __jsui_native;
    n.setProps(9999, {}); n.appendChild(9999, 1); n.measure('x'); n.patchStyle(9999, {a: 1});
    const a = n.createNode('View'), b = n.createNode('View');
    n.appendChild(a, b); n.appendChild(b, a); n.insertBefore(a, a, null);
    n.destroyNode(n.rootId);
    try { n.createNode('Nope'); } catch (e) { n.postMessage('badType', e.message); }
  })"));
  CHECK(h.message("badType") && h.message("badType")->find("unknown element type") != std::string::npos);
}

TEST(host_load_font_promise) {
  Host h;
  h.load(R"(
    __jsui_native.loadFont('TestInter', 'assets/fonts/Inter-Bold.ttf', 700, false)
      .then(ok => __jsui_native.postMessage('font', 'ok:' + ok));
    __jsui_native.loadFont('Missing', 'assets/fonts/none.ttf', 400, false)
      .catch(e => __jsui_native.postMessage('fontErr', e.message));
    __jsui_native.loadFont('NotAFont', 'data:font/ttf,notafont', 400, false)
      .catch(e => __jsui_native.postMessage('fontBad', e.message));
  )");
  CHECK(h.until([&] { return h.message("font") && h.message("fontErr") && h.message("fontBad"); }));
  if (h.message("font")) CHECK_EQ(*h.message("font"), std::string("ok:true"));
  CHECK(h.message("fontErr") && h.message("fontErr")->find("status 404") != std::string::npos);
  CHECK(h.message("fontBad") && h.message("fontBad")->find("not a supported font") != std::string::npos);
}

TEST(host_reload_from_js_and_leak_free_teardown) {
  int evals = 0;
  Host h([&](JsuiConfig& c) { (void)c; });
  std::string src = "__jsui_native.postMessage('boot', 1); setTimeout(() => __jsui_native.reload(), 0);"
                    "requestAnimationFrame(() => {}); fetch('data:,x'); __jsui_native.loadFont('A', 'data:,x');"
                    "new Promise(() => {}); const keep = {self: null}; keep.self = keep;";
  h.load(src);
  for (int i = 0; i < 20; i++) h.tick();
  for (auto& [n, j] : h.messages)
    if (n == "boot") evals++;
  CHECK(evals >= 10);  // every reload re-evaluated the bundle
  // Destroying with pending timers/rAF/fetch/font promises must be clean
  // (QuickJS asserts on leaked objects in debug builds).
}

TEST(loader_data_uris) {
  Resource r;
  CHECK(ResourceLoader::decodeDataUri("data:text/plain;base64,aGVsbG8=", &r));
  CHECK_EQ(r.text(), std::string("hello"));
  CHECK_EQ(r.contentType, std::string("text/plain"));
  CHECK(ResourceLoader::decodeDataUri("data:,a%20b%2Cc", &r));
  CHECK_EQ(r.text(), std::string("a b,c"));
  CHECK_EQ(r.contentType, std::string("text/plain;charset=US-ASCII"));
  CHECK(ResourceLoader::decodeDataUri("data:image/svg+xml;charset=utf-8,%3Csvg%3E", &r));
  CHECK_EQ(r.text(), std::string("<svg>"));
  CHECK_EQ(r.contentType, std::string("image/svg+xml;charset=utf-8"));
  CHECK(!ResourceLoader::decodeDataUri("data:text/plain;base64", &r));
  CHECK(!ResourceLoader::decodeDataUri("data:;base64,@@@", &r));
}

TEST(loader_resolve_extra) {
  CHECK_EQ(ResourceLoader::resolve("http://h/a/b.js", "./c/../d.png?x=1"), std::string("http://h/a/d.png?x=1"));
  CHECK_EQ(ResourceLoader::resolve("http://h/a/b.js?v=2", "?v=3"), std::string("http://h/a/b.js?v=3"));
  CHECK_EQ(ResourceLoader::resolve("http://h/a/b.js", "//cdn/x.js"), std::string("http://cdn/x.js"));
  CHECK_EQ(ResourceLoader::resolve("file:///x/y/b.js", "img/a.png"), std::string("file:///x/y/img/a.png"));
  CHECK_EQ(ResourceLoader::resolve("http://h/a/b.js", "data:,x"), std::string("data:,x"));
}

TEST(net_chunked_decoder) {
  net::ChunkedDecoder d;
  std::vector<uint8_t> out;
  std::string body = "4\r\nWiki\r\n5;ext=1\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-Trailer: 1\r\n\r\n";
  int r = 0;
  for (char c : body) r = d.feed(&c, 1, &out);  // byte by byte
  CHECK_EQ(r, 1);
  CHECK_EQ(std::string(out.begin(), out.end()), std::string("Wikipedia in\r\n\r\nchunks."));
  net::ChunkedDecoder bad;
  CHECK_EQ(bad.feed("zz\r\n", 4, &out), -1);
}

#ifndef _WIN32

namespace {

std::string chunked(const std::vector<std::string>& parts) {
  std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n";
  for (auto& p : parts) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%zx", p.size());
    s += std::string(hex) + "\r\n" + p + "\r\n";
  }
  return s + "0\r\n\r\n";
}

std::string routes(const jsui_test::HttpReq& r) {
  using jsui_test::httpResponse;
  if (r.path == "/plain") return httpResponse(200, "plain body", "text/plain", "X-Test: yes\r\n");
  if (r.path == "/chunked") return chunked({"hello ", "chunked ", std::string(20000, 'z')});
  if (r.path == "/redirect") return "HTTP/1.1 302 Found\r\nLocation: /plain\r\nContent-Length: 0\r\n\r\n";
  if (r.path == "/redirect-abs") return "HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:" + r.header("host").substr(r.header("host").find(':') + 1) + "/redirect\r\nContent-Length: 0\r\n\r\n";
  if (r.path == "/loop") return "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
  if (r.path == "/echo")
    return httpResponse(200, r.method + " " + r.header("content-type") + " " + r.header("x-custom") + " " + r.body,
                        "application/json");
  if (r.path == "/eof") return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil close";
  if (r.path == "/slow") {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    return httpResponse(200, "late");
  }
  if (r.path == "/truncated") return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort";
  if (r.path == "/garbage") return "NOT HTTP AT ALL\r\n\r\n";
  return httpResponse(404, "not found");
}

}  // namespace

TEST(net_http_client) {
  jsui_test::TestHttpServer server(routes);
  net::HttpRequest req;
  req.url = server.url("/plain");
  auto r = net::httpRequest(req);
  CHECK_EQ(r.status, 200);
  CHECK_EQ(std::string(r.body.begin(), r.body.end()), std::string("plain body"));
  CHECK_EQ(r.header("x-test"), std::string("yes"));

  req.url = server.url("/chunked");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 200);
  CHECK_EQ(r.body.size(), size_t(6 + 8 + 20000));

  req.url = server.url("/redirect-abs");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 200);
  CHECK(r.redirected);
  CHECK_EQ(r.url, server.url("/plain"));

  req.url = server.url("/loop");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("too many redirects") != std::string::npos);

  req.url = server.url("/eof");
  r = net::httpRequest(req);
  CHECK_EQ(std::string(r.body.begin(), r.body.end()), std::string("until close"));

  req.url = server.url("/missing");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 404);

  req.url = server.url("/truncated");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("truncated") != std::string::npos);

  req.url = server.url("/garbage");
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);

  req.url = server.url("/slow");
  req.timeoutMs = 150;
  auto t0 = std::chrono::steady_clock::now();
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("timed out") != std::string::npos);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));

  req.timeoutMs = 2000;
  req.url = "https://example.com/";
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("https") != std::string::npos);
  req.url = "http://127.0.0.1:1/";  // nothing listens on port 1
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
}

TEST(host_fetch_http) {
  jsui_test::TestHttpServer server(routes);
  Host h;
  std::string base = server.url("");
  h.load("const B = '" + base + R"(';
    const post = (name, v) => __jsui_native.postMessage(name, v);
    fetch(B + '/plain').then(r => r.text().then(t => post('plain', [r.status, r.ok, r.statusText, r.headers.get('x-test'), t])));
    fetch(B + '/redirect').then(r => post('redirect', [r.status, r.redirected, r.url]));
    fetch(B + '/chunked').then(r => r.arrayBuffer()).then(b => post('chunked', b.byteLength));
    fetch(B + '/echo', {method: 'POST', headers: {'X-Custom': 'abc', 'Content-Type': 'application/json'},
                        body: JSON.stringify({a: 1})}).then(r => r.text()).then(t => post('echo', t));
    fetch(B + '/echo', {method: 'PUT', body: new Uint8Array([104, 105])}).then(r => r.text()).then(t => post('put', t));
    fetch(B + '/missing').then(r => post('missing', [r.status, r.ok]));
    fetch('http://127.0.0.1:1/x').catch(e => post('refused', e instanceof TypeError ? 'TypeError' : String(e)));
    fetch('https://example.com/').catch(e => post('https', e.message));
    const ac = new AbortController();
    fetch(B + '/slow', {signal: ac.signal}).catch(e => post('aborted', e.name));
    setTimeout(() => ac.abort(), 50);
  )");
  CHECK(h.until([&] {
    for (const char* n : {"plain", "redirect", "chunked", "echo", "put", "missing", "refused", "https", "aborted"})
      if (!h.message(n)) return false;
    return true;
  }));
  auto msg = [&](const char* n) { return h.message(n) ? *h.message(n) : std::string("<none>"); };
  CHECK_EQ(msg("plain"), std::string(R"([200,true,"OK","yes","plain body"])"));
  CHECK_EQ(msg("redirect"), std::string("[200,true,\"" + server.url("/plain") + "\"]"));
  CHECK_EQ(msg("chunked"), std::string("20014"));
  CHECK_EQ(msg("echo"), std::string(R"(POST application/json abc {"a":1})"));
  CHECK_EQ(msg("put"), std::string("PUT application/octet-stream  hi"));
  CHECK_EQ(msg("missing"), std::string("[404,false]"));
  CHECK_EQ(msg("refused"), std::string("TypeError"));
  CHECK(msg("https").find("https:// is not supported") != std::string::npos);
  CHECK_EQ(msg("aborted"), std::string("AbortError"));
  for (auto& e : h.errors) std::fprintf(stderr, "    unexpected error: %s\n", e.c_str());
  CHECK(h.errors.empty());
}

TEST(host_dev_reload_on_version_change) {
  std::mutex m;
  std::string version = "v1";
  std::string bundle =
      "const n = __jsui_native; const v = n.createNode('View');"
      "n.setProps(v, {style: {flex: 1, backgroundColor: '#ff0000'}}); n.appendChild(n.rootId, v);"
      "n.postMessage('loaded', 'one');";
  int versionHits = 0;
  jsui_test::TestHttpServer server([&](const jsui_test::HttpReq& r) {
    std::lock_guard<std::mutex> lock(m);
    if (r.path == "/__jsui/version") {
      versionHits++;
      return jsui_test::httpResponse(200, version + "\n");
    }
    if (r.path == "/app/bundle.js") return jsui_test::httpResponse(200, bundle, "text/javascript");
    return jsui_test::httpResponse(404, "");
  });
  Host h([](JsuiConfig& c) {
    c.dev_mode = 1;
    c.width = 32;
    c.height = 32;
  });
  jsui_load_bundle_url(h.rt, server.url("/app/bundle.js").c_str());
  CHECK(h.until([&] { return h.message("loaded") != nullptr; }));
  CHECK(h.until([&] {
    std::lock_guard<std::mutex> lock(m);
    return versionHits >= 2;
  }));
  CHECK(h.messages.size() == 1);  // unchanged version: no reload
  std::vector<uint8_t> px(32 * 32 * 4);
  jsui_render_rgba(h.rt, px.data(), 32, 32, 32 * 4);
  CHECK_EQ(int(px[0]), 255);

  // Bundle changes and throws: error overlay, last good frame kept.
  {
    std::lock_guard<std::mutex> lock(m);
    version = "v2";
    bundle = "throw new Error('broken bundle');";
  }
  CHECK(h.until([&] { return jsui_last_error(h.rt) != nullptr; }));
  CHECK(h.hasError("broken bundle"));
  jsui_render_rgba(h.rt, px.data(), 32, 32, 32 * 4);
  // Pixel (0,0) is outside the overlay panel: last good red frame under the tint.
  CHECK(px[0] > 100 && px[1] < 60 && px[3] == 255);
  // Escape dismisses the overlay.
  CHECK_EQ(jsui_send_key(h.rt, "Escape", "Escape", 1, 0, 0), 1);
  CHECK(jsui_last_error(h.rt) == nullptr);

  // Fixed bundle: reload and render the new content.
  {
    std::lock_guard<std::mutex> lock(m);
    version = "v3";
    bundle =
        "const n = __jsui_native; const v = n.createNode('View');"
        "n.setProps(v, {style: {flex: 1, backgroundColor: '#0000ff'}}); n.appendChild(n.rootId, v);"
        "n.postMessage('loaded', 'two');";
  }
  CHECK(h.until([&] { return h.message("loaded") && *h.message("loaded") == "two"; }));
  jsui_render_rgba(h.rt, px.data(), 32, 32, 32 * 4);
  CHECK(px[0] == 0 && px[2] == 255);
  CHECK(jsui_last_error(h.rt) == nullptr);
}

#endif  // _WIN32

// Prints engine overhead numbers (not a pass/fail benchmark; sanity bounds only).
TEST(perf_engine_overhead) {
  using Clock = std::chrono::steady_clock;
  Host h([](JsuiConfig& c) {
    c.width = 1920;
    c.height = 1080;
  });
  h.load(R"(
    const n = __jsui_native;
    globalThis.ids = [];
    for (let i = 0; i < 200; i++) {
      const v = n.createNode('View');
      n.setProps(v, {style: {position: 'absolute', width: 40, height: 40, backgroundColor: '#336699',
                              borderRadius: 4, opacity: 1, transform: [{translateX: 0}, {scale: 1}]}});
      n.appendChild(n.rootId, v);
      ids.push(v);
    }
    globalThis.animate = false;
    function frame(ts) {
      if (animate) for (let i = 0; i < ids.length; i++)
        n.patchStyle(ids[i], {opacity: 0.5 + 0.5 * Math.sin(ts / 300 + i), transform: [{translateX: (ts + i) % 100}, {scale: 1}]});
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    globalThis.bulk = () => {
      for (let i = 0; i < ids.length; i++)
        n.setProps(ids[i], {style: {position: 'absolute', left: i, top: 3, width: 40, height: 40, backgroundColor: '#336699',
          borderRadius: 4, opacity: 1, borderWidth: 1, borderColor: '#fff', transform: [{translateX: i}]}, __events: ['press']});
    };
  )");
  h.tick(5);
  auto bench = [&](int frames) {
    auto t0 = Clock::now();
    for (int i = 0; i < frames; i++) h.tick();
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / frames;
  };
  double idle = bench(2000);
  h.load("animate = true;");
  double anim = bench(500);
  h.load("animate = false;");
  auto t0 = Clock::now();
  for (int i = 0; i < 100; i++)
    h.load("for (let i = 0; i < ids.length; i++) __jsui_native.patchStyle(ids[i], {opacity: 0.5, transform: [{translateX: i}, {scale: 1}]});");
  double patchOnly = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 100;
  t0 = Clock::now();
  for (int i = 0; i < 100; i++) h.load("bulk();");
  double bulk = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 100;
  std::printf("  perf: tick idle (rAF loop, 200 nodes) %.2f us; tick with 200 patchStyle/frame %.1f us; "
              "200 patchStyle without tick %.1f us; 200 setProps (10 keys) %.1f us\n", idle, anim, patchOnly, bulk);
  CHECK(idle < 2000);
  CHECK(h.errors.empty());
}

TEST(host_multiple_runtimes_are_independent) {
  Host a, b;
  a.load("globalThis.x = 'a'; setTimeout(() => __jsui_native.postMessage('who', x), 10);");
  b.load("globalThis.x = 'b'; setTimeout(() => __jsui_native.postMessage('who', x), 10);");
  a.tick();
  b.tick();
  CHECK(a.message("who") && *a.message("who") == "a");
  CHECK(b.message("who") && *b.message("who") == "b");
  CHECK_EQ(a.messages.size(), size_t(1));
  CHECK_EQ(b.messages.size(), size_t(1));
}
