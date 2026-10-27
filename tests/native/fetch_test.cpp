// Tests for fetch() beyond plain loads: streamed bodies (native http and the
// host's JsuiConfig.on_request), redirect modes, the host request API, and
// the JS side (Streams, Blob, FormData) through fetch_smoke.js.
//
// JSUI_NET_TESTS=1 also runs the live https:// checks (public internet).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/value.h"
#include "jsui/jsui.h"
#include "net/http_client.h"
#include "test.h"
#include "test_http_server.h"

using namespace jsui;

namespace {

struct FetchHost {
  JsuiRuntime* rt = nullptr;
  std::vector<std::pair<std::string, std::string>> messages;
  std::vector<std::string> errors;
  double t = 0;

  explicit FetchHost(std::function<void(JsuiConfig&)> tweak = nullptr) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 64;
    cfg.height = 64;
    cfg.js_thread = jsui_test::jsThreadMode();
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<FetchHost*>(u)->messages.emplace_back(name, json);
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
      if (level == JSUI_LOG_ERROR) static_cast<FetchHost*>(u)->errors.push_back(msg);
    };
    cfg.log_user = this;
    if (tweak) tweak(cfg);
    rt = jsui_create(&cfg);
  }
  ~FetchHost() { jsui_destroy(rt); }

  bool load(const std::string& src) { return jsui_load_bundle_source(rt, src.data(), src.size(), "test.js") == 0; }
  void tick() {
    t += 16;
    jsui_tick(rt, t);
    jsui_sync(rt, 50);
  }
  bool until(std::function<bool()> pred, int budgetMs = 4000) {
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
  std::string msg(const std::string& name) const { return message(name) ? *message(name) : std::string("<none>"); }
  int count(const std::string& name) const {
    int n = 0;
    for (auto& [k, v] : messages) n += k == name;
    return n;
  }
  void printErrors() const {
    for (auto& e : errors) std::fprintf(stderr, "    unexpected error: %s\n", e.c_str());
  }
};

const char* kPost = "const post = (name, v) => __jsui_native.postMessage(name, v);\n";

}  // namespace

TEST(fetch_smoke_bundle) {
  FetchHost h;
  CHECK_EQ(jsui_load_bundle_url(h.rt, "tests/native/js/fetch_smoke.js"), 0);
  bool done = h.until([&] { return h.message("smoke") != nullptr; });
  CHECK(done);
  if (!done) {
    h.printErrors();
    return;
  }
  Value r = Value::parseJson(*h.message("smoke"));
  for (auto& f : r["failures"].asArray()) std::fprintf(stderr, "    smoke failure: %s\n", f.asString().c_str());
  CHECK_EQ(r["failures"].asArray().size(), size_t(0));
  CHECK(r["passed"].asNumber() > 50);
  h.printErrors();
  CHECK(h.errors.empty());
}

// ------------------------------------------------- the host's on_request

namespace {

// A host that serves game://... itself (streamed by hand from the test) and
// declines everything else.
struct NetHost {
  struct Req {
    uint64_t id;
    std::string url, method, body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout, redirect, streaming;
  };
  std::vector<Req> requests;
  std::vector<uint64_t> cancelled;
  int declined = 0;

  static int onRequest(void* u, JsuiRuntime*, uint64_t id, const JsuiRequest* r) {
    auto* self = static_cast<NetHost*>(u);
    if (std::strncmp(r->url, "game://", 7) != 0) {
      self->declined++;
      return 1;
    }
    Req q{id, r->url, r->method, std::string(static_cast<const char*>(r->body ? r->body : ""), r->body_size), {},
          r->timeout_ms, r->redirect, r->streaming};
    for (size_t i = 0; i < r->header_count; i++) q.headers.emplace_back(r->headers[i].name, r->headers[i].value);
    self->requests.push_back(std::move(q));
    return 0;
  }
  static void onCancel(void* u, JsuiRuntime*, uint64_t id) { static_cast<NetHost*>(u)->cancelled.push_back(id); }
  void install(JsuiConfig& c) {
    c.on_request = onRequest;
    c.on_request_cancel = onCancel;
    c.request_user = this;
  }
  const Req* find(const std::string& url) const {
    for (auto& r : requests)
      if (r.url == url) return &r;
    return nullptr;
  }
};

}  // namespace

TEST(fetch_host_request_api) {
  NetHost net;
  FetchHost h([&](JsuiConfig& c) { net.install(c); });
  h.load(std::string(kPost) + R"(
    // Streamed: each piece is seen as it arrives.
    fetch('game://api/stream', {method: 'POST', headers: {'X-Token': 't1'}, body: 'payload', redirect: 'manual',
                                timeout: 1234})
      .then(async (r) => {
        post('head', [r.status, r.statusText, r.url, r.headers.get('content-type'), r.redirected]);
        const reader = r.body.getReader();
        const dec = new TextDecoder();
        for (;;) {
          const {value, done} = await reader.read();
          if (done) break;
          post('piece', dec.decode(value));
        }
        post('stream-done', true);
      });
    // Whole answer through jsui_resource_complete.
    fetch('game://api/whole').then((r) => r.json()).then((j) => post('whole', j));
    // Network error, and a body that fails part way.
    fetch('game://api/fail').catch((e) => post('fail', [e instanceof TypeError, e.message]));
    fetch('game://api/broken').then((r) => r.text()).catch((e) => post('broken', e.message));
    // Aborted after the head: the host is told, the body errors.
    const ac = new AbortController();
    fetch('game://api/abort', {signal: ac.signal}).then((r) => {
      post('abort-head', r.status);
      r.text().catch((e) => post('abort-body', e.name));
      ac.abort();
    });
    // Declined: the built-in loader answers (a data: URI never reaches the host).
    fetch('tests/native/js/fetch_smoke.js').then((r) => post('declined', r.status));
  )");
  CHECK(h.until([&] { return net.requests.size() == 5 && net.declined >= 1; }));
  const NetHost::Req* s = net.find("game://api/stream");
  CHECK(s != nullptr);
  if (!s) return;
  CHECK_EQ(s->method, std::string("POST"));
  CHECK_EQ(s->body, std::string("payload"));
  CHECK_EQ(s->redirect, int(JSUI_REDIRECT_MANUAL));
  CHECK_EQ(s->timeout, 1234);
  CHECK_EQ(s->streaming, 1);
  bool token = false, type = false;
  for (auto& [k, v] : s->headers) {
    token |= k == "x-token" && v == "t1";
    type |= k == "content-type" && v.rfind("text/plain", 0) == 0;
  }
  CHECK(token);
  CHECK(type);

  // Streamed answer, from another thread like a network thread would.
  JsuiHeader hdrs[] = {{"Content-Type", "text/event-stream"}, {"X-Extra", "1"}};
  std::thread([&] { jsui_request_respond(h.rt, s->id, 200, "OK", "game://api/stream?final", hdrs, 2); }).join();
  CHECK(h.until([&] { return h.message("head") != nullptr; }));
  CHECK_EQ(h.msg("head"), std::string(R"([200,"OK","game://api/stream?final","text/event-stream",true])"));
  jsui_request_body(h.rt, s->id, "first ", 6);
  CHECK(h.until([&] { return h.count("piece") == 1; }));
  CHECK(!h.message("stream-done"));
  jsui_request_body(h.rt, s->id, "second", 6);
  CHECK(h.until([&] { return h.count("piece") == 2; }));
  CHECK_EQ(h.msg("piece"), std::string("second"));
  jsui_request_end(h.rt, s->id, nullptr);
  CHECK(h.until([&] { return h.message("stream-done") != nullptr; }));

  const char* json = R"({"a":[1,2]})";
  jsui_resource_complete(h.rt, net.find("game://api/whole")->id, 200, "application/json", json, std::strlen(json));
  jsui_request_respond(h.rt, net.find("game://api/fail")->id, 0, "host offline", nullptr, nullptr, 0);
  uint64_t broken = net.find("game://api/broken")->id;
  jsui_request_respond(h.rt, broken, 200, "OK", nullptr, nullptr, 0);
  jsui_request_body(h.rt, broken, "par", 3);
  jsui_request_end(h.rt, broken, "connection reset");
  uint64_t aborted = net.find("game://api/abort")->id;
  jsui_request_respond(h.rt, aborted, 200, "OK", nullptr, nullptr, 0);
  CHECK(h.until([&] {
    return h.message("whole") && h.message("fail") && h.message("broken") && h.message("abort-body") &&
           h.message("declined");
  }));
  CHECK_EQ(h.msg("whole"), std::string(R"({"a":[1,2]})"));
  CHECK(h.msg("fail").find("host offline") != std::string::npos);
  CHECK(h.msg("fail").rfind("[true,", 0) == 0);
  CHECK(h.msg("broken").find("connection reset") != std::string::npos);
  CHECK_EQ(h.msg("abort-head"), std::string("200"));
  CHECK_EQ(h.msg("abort-body"), std::string("AbortError"));
  CHECK_EQ(net.cancelled.size(), size_t(1));
  if (!net.cancelled.empty()) CHECK_EQ(net.cancelled[0], aborted);
  CHECK_EQ(h.msg("declined"), std::string("200"));
  // Answering a cancelled request is harmless.
  jsui_request_body(h.rt, aborted, "late", 4);
  jsui_request_end(h.rt, aborted, nullptr);
  h.tick();
  h.printErrors();
  CHECK(h.errors.empty());
}

// The host's on_request also serves the runtime's own loads (the bundle,
// images): those want the whole body (streaming = 0) and get it gathered.
TEST(fetch_host_request_serves_bundle) {
  NetHost net;
  FetchHost h([&](JsuiConfig& c) { net.install(c); });
  CHECK_EQ(jsui_load_bundle_url(h.rt, "game://ui/bundle.js"), 0);
  CHECK(h.until([&] { return !net.requests.empty(); }));
  CHECK_EQ(net.requests[0].streaming, 0);
  const char* src = "__jsui_native.postMessage('ran', 1);";
  size_t half = std::strlen(src) / 2;
  jsui_request_respond(h.rt, net.requests[0].id, 200, "OK", nullptr, nullptr, 0);
  jsui_request_body(h.rt, net.requests[0].id, src, half);
  jsui_request_body(h.rt, net.requests[0].id, src + half, std::strlen(src) - half);
  jsui_request_end(h.rt, net.requests[0].id, nullptr);
  CHECK(h.until([&] { return h.message("ran") != nullptr; }));
}

// ------------------------------------------------ the built-in http client

#ifndef _WIN32

namespace {

std::string chunkedPaused(const std::vector<std::string>& parts) {
  std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n";
  for (auto& p : parts) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%zx", p.size());
    s += std::string(hex) + "\r\n" + p + "\r\n" + jsui_test::kPause;
  }
  return s + "0\r\n\r\n";
}

std::string netRoutes(const jsui_test::HttpReq& r) {
  using jsui_test::httpResponse;
  if (r.path == "/plain") return httpResponse(200, "plain body", "text/plain");
  if (r.path == "/stream") return chunkedPaused({"one;", "two;", "three"});
  if (r.path == "/redirect") return "HTTP/1.1 302 Found\r\nLocation: /plain\r\nContent-Length: 0\r\n\r\n";
  if (r.path == "/see-other")
    return "HTTP/1.1 303 See Other\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n";
  if (r.path == "/echo")
    return httpResponse(200, r.method + " " + r.header("content-type") + " " + std::to_string(r.body.size()) + " " +
                                 r.body.substr(0, 1000));
  if (r.path == "/big") return httpResponse(200, std::string(300000, 'b'), "application/octet-stream");
  return httpResponse(404, "not found");
}

}  // namespace

TEST(net_http_stream) {
  jsui_test::TestHttpServer server(netRoutes);
  net::HttpRequest req;
  req.url = server.url("/stream");
  std::vector<std::string> pieces;
  int heads = 0;
  net::HttpStream stream;
  stream.onHead = [&](const net::HttpResponse& h) {
    heads++;
    return h.status == 200;
  };
  stream.onData = [&](const uint8_t* d, size_t n) {
    pieces.emplace_back(reinterpret_cast<const char*>(d), n);
    return true;
  };
  auto r = net::httpRequest(req, nullptr, &stream);
  CHECK_EQ(r.status, 200);
  CHECK_EQ(heads, 1);
  CHECK(r.body.empty());
  std::string all;
  for (auto& p : pieces) all += p;
  CHECK_EQ(all, std::string("one;two;three"));
  CHECK(pieces.size() >= 3);  // the server pauses between chunks

  // onData returning false stops the request.
  req.url = server.url("/big");
  size_t got = 0;
  stream.onData = [&](const uint8_t*, size_t n) {
    got += n;
    return false;
  };
  r = net::httpRequest(req, nullptr, &stream);
  CHECK_EQ(r.status, 0);
  CHECK_EQ(r.error, std::string("cancelled"));
  CHECK(got > 0 && got < 300000);

  // Redirect modes.
  req = net::HttpRequest();
  req.url = server.url("/redirect");
  req.redirect = net::Redirect::Manual;
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 302);
  CHECK_EQ(r.header("location"), std::string("/plain"));
  req.redirect = net::Redirect::Error;
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("redirect") != std::string::npos);
  req.redirect = net::Redirect::Follow;
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 200);
  CHECK(r.redirected);
  // 303 turns a POST into a GET without a body.
  req.url = server.url("/see-other");
  req.method = "POST";
  req.body = {'x', 'y'};
  req.headers = {{"Content-Type", "text/plain"}};
  r = net::httpRequest(req);
  CHECK_EQ(std::string(r.body.begin(), r.body.end()), std::string("GET  0 "));

  // The idle timeout counts between pieces, not over the whole body: the
  // server pauses 300 ms per chunk (~900 ms in all).
  req = net::HttpRequest();
  req.url = server.url("/stream");
  req.idleTimeoutMs = 600;
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 200);
  req.idleTimeoutMs = 100;
  r = net::httpRequest(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("timed out") != std::string::npos);
}

TEST(fetch_http_streams_and_forms) {
  jsui_test::TestHttpServer server(netRoutes);
  FetchHost h;
  h.load("const B = '" + server.url("") + "';\n" + kPost + R"(
    fetch(B + '/stream').then(async (r) => {
      const dec = new TextDecoderStream();
      const seen = [];
      for await (const s of r.body.pipeThrough(dec)) {
        seen.push(s);
        post('piece', seen.length);
      }
      post('stream', seen.join(''));
    });
    fetch(B + '/redirect', {redirect: 'manual'}).then((r) => post('manual', [r.status, r.headers.get('location')]));
    fetch(B + '/redirect', {redirect: 'error'}).catch((e) => post('error-mode', e instanceof TypeError));
    const fd = new FormData();
    fd.append('name', 'jsui');
    fd.append('file', new File(['abc'], 'a.txt', {type: 'text/plain'}));
    fetch(B + '/echo', {method: 'POST', body: fd}).then((r) => r.text()).then((t) => post('form', t));
    fetch(B + '/echo', {method: 'POST', body: new Blob(['blob!'], {type: 'x/blob'})}).then((r) => r.text())
      .then((t) => post('blob', t));
    fetch(B + '/big').then((r) => r.blob()).then((b) => post('big', [b.size, b.type]));
    fetch(B + '/stream').then((r) => r.body.cancel()).then(() => post('cancelled', true));
  )");
  CHECK(h.until([&] {
    for (const char* n : {"stream", "manual", "error-mode", "form", "blob", "big", "cancelled"})
      if (!h.message(n)) return false;
    return true;
  }));
  CHECK_EQ(h.msg("stream"), std::string("one;two;three"));
  CHECK(h.count("piece") >= 3);
  CHECK_EQ(h.msg("manual"), std::string(R"([302,"/plain"])"));
  CHECK_EQ(h.msg("error-mode"), std::string("true"));
  std::string form = h.msg("form");
  CHECK(form.find("POST multipart/form-data; boundary=") != std::string::npos);
  CHECK(form.find("name=\"name\"\r\n\r\njsui") != std::string::npos);
  CHECK(form.find("filename=\"a.txt\"") != std::string::npos);
  CHECK_EQ(h.msg("blob"), std::string("POST x/blob 5 blob!"));
  CHECK_EQ(h.msg("big"), std::string(R"([300000,"application/octet-stream"])"));
  h.printErrors();
  CHECK(h.errors.empty());
}

#endif  // !_WIN32

// ------------------------------------------- live https (JSUI_NET_TESTS=1)

TEST(fetch_https_live) {
  const char* on = std::getenv("JSUI_NET_TESTS");
  if (!on || std::strcmp(on, "1") != 0) {
    std::printf("  SKIP fetch_https_live (set JSUI_NET_TESTS=1)\n");
    return;
  }
  net::HttpRequest req;
  req.url = "https://example.com/";
  auto r = net::fetchUrl(req);
  if (r.status == 0) std::fprintf(stderr, "    https error: %s\n", r.error.c_str());
  CHECK_EQ(r.status, 200);
  CHECK(std::string(r.body.begin(), r.body.end()).find("Example Domain") != std::string::npos);
  // Certificates are checked (status 0; a filtering proxy may answer instead).
  req.url = "https://self-signed.badssl.com/";
  r = net::fetchUrl(req);
  CHECK(r.status != 200);
  req.url = "https://wrong.host.badssl.com/";
  r = net::fetchUrl(req);
  CHECK(r.status != 200);

  FetchHost h;
  h.load(std::string(kPost) + R"(
    fetch('https://example.com/').then(async (r) => {
      let n = 0;
      for await (const c of r.body) n += c.byteLength;
      post('live', [r.status, n > 100]);
    }).catch((e) => post('live', String(e)));
  )");
  CHECK(h.until([&] { return h.message("live") != nullptr; }, 15000));
  CHECK_EQ(h.msg("live"), std::string("[200,true]"));
}
