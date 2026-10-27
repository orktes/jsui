// The built-in loader's https:// (net::fetchUrl) against real servers:
// NSURLSession on Apple, OpenSSL on Linux. Needs the internet; each test
// SKIPs when the network is unreachable or https:// is not available on this
// platform, and JSUI_TEST_NETWORK=0 skips them outright.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "net/http_client.h"
#include "test.h"

using namespace jsui;

#ifndef __EMSCRIPTEN__

namespace {

std::string text(const net::HttpResponse& r) { return std::string(r.body.begin(), r.body.end()); }

// Whether https://example.com answers; prints SKIP otherwise.
bool online(const char* test) {
  const char* env = std::getenv("JSUI_TEST_NETWORK");
  if (env && std::strcmp(env, "0") == 0) {
    std::printf("  SKIP %s: JSUI_TEST_NETWORK=0\n", test);
    return false;
  }
  static int state = -1;
  static std::string why;
  if (state < 0) {
    net::HttpRequest req;
    req.url = "https://example.com/";
    req.timeoutMs = 10000;
    auto r = net::fetchUrl(req);
    state = r.status == 200 ? 1 : 0;
    why = r.status ? "status " + std::to_string(r.status) : r.error;
  }
  if (!state) std::printf("  SKIP %s: https://example.com unreachable (%s)\n", test, why.c_str());
  return state == 1;
}

// httpbin.org is flaky at times (502 / 503): skip the rest of a test then.
bool httpbinUp(const net::HttpResponse& r, const char* test) {
  if (r.status >= 500 || (r.status == 0 && r.error.find("timed out") != std::string::npos)) {
    std::printf("  SKIP %s: httpbin.org unavailable (%d %s)\n", test, r.status, r.error.c_str());
    return false;
  }
  return true;
}

}  // namespace

TEST(net_https_buffered_and_streamed) {
  if (!online("net_https_buffered_and_streamed")) return;
  net::HttpRequest req;
  req.url = "https://example.com/";
  req.timeoutMs = 15000;
  auto r = net::fetchUrl(req);
  CHECK_EQ(r.status, 200);
  CHECK_EQ(r.statusText, std::string("OK"));
  CHECK_EQ(r.url, std::string("https://example.com/"));
  CHECK(!r.redirected);
  CHECK(r.header("content-type").find("text/html") != std::string::npos);
  CHECK(text(r).find("Example Domain") != std::string::npos);

  int heads = 0;
  size_t bytes = 0;
  std::string body;
  net::HttpStream s;
  s.onHead = [&](const net::HttpResponse& h) {
    heads++;
    CHECK_EQ(h.status, 200);
    CHECK(h.body.empty());
    return true;
  };
  s.onData = [&](const uint8_t* d, size_t n) {
    CHECK_EQ(heads, 1);
    bytes += n;
    body.append(reinterpret_cast<const char*>(d), n);
    return true;
  };
  auto sr = net::fetchUrl(req, nullptr, &s);
  CHECK_EQ(sr.status, 200);
  CHECK(sr.body.empty());
  CHECK_EQ(heads, 1);
  CHECK(bytes > 0);
  CHECK(body.find("Example Domain") != std::string::npos);
}

TEST(net_https_bad_certificates_fail) {
  if (!online("net_https_bad_certificates_fail")) return;
  for (const char* url : {"https://wrong.host.badssl.com/", "https://self-signed.badssl.com/",
                          "https://expired.badssl.com/", "https://untrusted-root.badssl.com/"}) {
    net::HttpRequest req;
    req.url = url;
    req.timeoutMs = 15000;
    auto r = net::fetchUrl(req);
    if (r.status != 0) std::fprintf(stderr, "    %s answered %d\n", url, r.status);
    CHECK_EQ(r.status, 0);
    CHECK(!r.error.empty());
    CHECK(r.body.empty());
  }
}

TEST(net_https_methods_headers_redirects) {
  const char* name = "net_https_methods_headers_redirects";
  if (!online(name)) return;
  net::HttpRequest req;
  req.method = "POST";
  req.url = "https://httpbin.org/post";
  req.timeoutMs = 20000;
  req.headers = {{"x-custom", "abc"}, {"content-type", "application/json"}};
  std::string json = R"({"a":1})";
  req.body.assign(json.begin(), json.end());
  auto r = net::fetchUrl(req);
  if (!httpbinUp(r, name)) return;
  CHECK_EQ(r.status, 200);
  CHECK(text(r).find(R"("X-Custom": "abc")") != std::string::npos);
  CHECK(text(r).find(R"("json": {)") != std::string::npos);
  CHECK(text(r).find(R"("User-Agent": "jsui/0.1")") != std::string::npos);

#if defined(__APPLE__)
  // NSURLSession decodes gzip itself: no Content-Encoding reaches JS. (The
  // socket client asks for identity, which httpbin's /gzip ignores.)
  req = net::HttpRequest();
  req.url = "https://httpbin.org/gzip";
  req.timeoutMs = 20000;
  r = net::fetchUrl(req);
  if (!httpbinUp(r, name)) return;
  CHECK_EQ(r.status, 200);
  CHECK(text(r).find(R"("gzipped": true)") != std::string::npos);
  CHECK(r.header("content-encoding").empty());
#endif

  req = net::HttpRequest();
  req.timeoutMs = 20000;
  req.url = "https://httpbin.org/redirect/2";
  r = net::fetchUrl(req);
  if (!httpbinUp(r, name)) return;
  CHECK_EQ(r.status, 200);
  CHECK(r.redirected);
  CHECK_EQ(r.url, std::string("https://httpbin.org/get"));

  req.maxRedirects = 1;
  r = net::fetchUrl(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("too many redirects") != std::string::npos);
  req.maxRedirects = 20;

  req.redirect = net::Redirect::Error;
  r = net::fetchUrl(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("redirect: 'error'") != std::string::npos);

  req.redirect = net::Redirect::Manual;
  r = net::fetchUrl(req);
  CHECK_EQ(r.status, 302);
  CHECK(!r.redirected);
  CHECK(!r.header("location").empty());

  req = net::HttpRequest();
  req.url = "https://httpbin.org/status/404";
  req.timeoutMs = 20000;
  r = net::fetchUrl(req);
  if (!httpbinUp(r, name)) return;
  CHECK_EQ(r.status, 404);
  // httpbin sends "NOT FOUND"; NSURLSession gives none and the standard one
  // is filled in.
  CHECK(net::iequals(r.statusText, "Not Found"));

  req.method = "HEAD";
  req.url = "https://httpbin.org/get";
  r = net::fetchUrl(req);
  CHECK_EQ(r.status, 200);
  CHECK(r.body.empty());
}

TEST(net_https_timeouts_and_cancel) {
  const char* name = "net_https_timeouts_and_cancel";
  if (!online(name)) return;
  using Clock = std::chrono::steady_clock;
  net::HttpRequest req;
  req.url = "https://httpbin.org/delay/5";
  req.timeoutMs = 800;
  auto t0 = Clock::now();
  auto r = net::fetchUrl(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("timed out") != std::string::npos);
  CHECK(Clock::now() - t0 < std::chrono::milliseconds(2000));

  req.timeoutMs = 0;
  req.headerTimeoutMs = 800;
  t0 = Clock::now();
  r = net::fetchUrl(req);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("timed out") != std::string::npos);
  CHECK(Clock::now() - t0 < std::chrono::milliseconds(2000));

  // /drip sends a byte a second after the headers: the idle timeout trips.
  req = net::HttpRequest();
  req.url = "https://httpbin.org/drip?duration=10&numbytes=5&delay=0";
  req.idleTimeoutMs = 600;
  int heads = 0;
  net::HttpStream s;
  s.onHead = [&](const net::HttpResponse& h) {
    heads++;
    return true;
  };
  s.onData = [&](const uint8_t*, size_t) { return true; };
  t0 = Clock::now();
  r = net::fetchUrl(req, nullptr, &s);
  if (heads == 0 && !httpbinUp(r, name)) return;
  CHECK_EQ(heads, 1);
  CHECK_EQ(r.status, 0);
  CHECK(r.error.find("timed out reading") != std::string::npos);
  CHECK(Clock::now() - t0 < std::chrono::milliseconds(5000));

  // Cancellation is seen within a few ms, mid body.
  req.idleTimeoutMs = 30000;
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    cancel = true;
  });
  t0 = Clock::now();
  r = net::fetchUrl(req, &cancel, &s);
  auto took = Clock::now() - t0;
  canceller.join();
  CHECK_EQ(r.status, 0);
  CHECK_EQ(r.error, std::string("cancelled"));
  CHECK(took < std::chrono::milliseconds(2500));

  // onData returning false stops the request.
  int chunks = 0;
  s.onData = [&](const uint8_t*, size_t) { return ++chunks < 2; };
  r = net::fetchUrl(req, nullptr, &s);
  CHECK_EQ(r.status, 0);
  CHECK_EQ(r.error, std::string("cancelled"));
  CHECK_EQ(chunks, 2);
}

#endif  // !__EMSCRIPTEN__
