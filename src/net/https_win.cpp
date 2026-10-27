// Windows: https:// on WinHTTP (the OS HTTP stack: Schannel, the system's
// certificate store and proxy settings), so nothing is linked in for TLS.
// http:// stays on the built-in client's sockets (httpRequest()).
//
// WinHTTP runs in asynchronous mode: every step (send, headers, each body
// read) completes through the status callback, and the calling worker
// thread waits for it in short slices. That way the overall, header and
// idle timeouts and the cancel flag are noticed within ~50 ms, and stopping
// is always the same: close the request handle and wait for its
// HANDLE_CLOSING notification (the last callback WinHTTP makes with it).
#if defined(_WIN32)

#include "net/http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4  // Windows 8.1+
#endif

namespace jsui::net {

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point after(int ms) {
  return ms > 0 ? Clock::now() + std::chrono::milliseconds(ms) : Clock::time_point::max();
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

std::string narrow(const wchar_t* s, size_t len) {
  if (!len) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s, int(len), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, int(len), &out[0], n, nullptr, nullptr);
  return out;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A WinHTTP (or system) error code as text, e.g. "the server's certificate
// is not trusted (WinHTTP error 12175)".
std::string winhttpError(DWORD code, DWORD secureFlags = 0) {
  std::string msg;
  if (code == ERROR_WINHTTP_SECURE_FAILURE && secureFlags) {
    if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA) msg = "the server's certificate is not trusted";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID) msg = "the server's certificate is for another host name";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID) msg = "the server's certificate has expired or is not yet valid";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED) msg = "the server's certificate was revoked";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED) msg = "the server's certificate revocation could not be checked";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT) msg = "the server's certificate is invalid";
    else if (secureFlags & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR) msg = "TLS handshake failed";
  }
  if (msg.empty()) {
    switch (code) {
      case ERROR_WINHTTP_NAME_NOT_RESOLVED: msg = "cannot resolve host"; break;
      case ERROR_WINHTTP_CANNOT_CONNECT: msg = "cannot connect to the server"; break;
      case ERROR_WINHTTP_CONNECTION_ERROR: msg = "connection reset"; break;
      case ERROR_WINHTTP_TIMEOUT: msg = "timed out"; break;
      case ERROR_WINHTTP_REDIRECT_FAILED: msg = "too many redirects"; break;
      case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: msg = "invalid response from the server"; break;
      case ERROR_WINHTTP_SECURE_FAILURE: msg = "TLS / certificate error"; break;
      default: {
        wchar_t* buf = nullptr;
        DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                 GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
        if (n && buf) msg = trim(narrow(buf, n));
        if (buf) LocalFree(buf);
        if (msg.empty()) msg = "request failed";
      }
    }
  }
  return "https://: " + msg + " (WinHTTP error " + std::to_string(code) + ")";
}

// One request handle's callback state and the memory WinHTTP reads or
// writes asynchronously (request headers and body, the read buffer). It
// lives until WinHTTP's HANDLE_CLOSING for the handle, which can come after
// the exchange closed it and returned (a cancelled step still finishing):
// whichever of the two comes last frees it.
struct Op {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;    // the pending step completed (or failed: error)
  DWORD bytes = 0;      // DATA_AVAILABLE / READ_COMPLETE
  DWORD error = 0;      // REQUEST_ERROR
  DWORD secureFlags = 0;
  bool redirected = false;
  std::string url;          // the request's URL, then each redirect's target
  bool closed = false;      // HANDLE_CLOSING arrived
  bool orphaned = false;    // the exchange is gone: the closing callback frees it
  std::wstring headers;
  std::vector<uint8_t> body;
  char buf[65536];  // ReadData target
};

void CALLBACK statusCallback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLen) {
  auto* op = reinterpret_cast<Op*>(context);
  if (!op) return;  // the connect handle, or a request before its context was set
  if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
    std::unique_lock<std::mutex> lock(op->m);
    op->closed = true;
    if (op->orphaned) {
      lock.unlock();
      delete op;
    }
    return;
  }
  std::lock_guard<std::mutex> lock(op->m);
  switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
      op->done = true;
      break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
      op->bytes = info ? *static_cast<DWORD*>(info) : 0;
      op->done = true;
      break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
      op->bytes = infoLen;
      op->done = true;
      break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
      op->error = info ? static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
      if (!op->error) op->error = ERROR_WINHTTP_INTERNAL_ERROR;
      op->done = true;
      break;
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
      if (info) op->secureFlags = *static_cast<DWORD*>(info);
      break;
    case WINHTTP_CALLBACK_STATUS_REDIRECT:
      op->redirected = true;
      // The new URL (absolute or not, depending on the WinHTTP version).
      if (info) {
        auto* to = static_cast<const wchar_t*>(info);
        op->url = resolveLocation(op->url, narrow(to, std::wcslen(to)));
      }
      break;
    default:
      return;
  }
  op->cv.notify_all();
}

// The process-wide async session (never closed: worker threads may use it
// until the process exits).
HINTERNET session(std::string* err) {
  static std::once_flag once;
  static HINTERNET s = nullptr;
  static std::string why;
  std::call_once(once, [] {
    // Automatic proxy (WPAD / PAC and the system settings) needs Windows 8.1+.
    s = WinHttpOpen(L"jsui/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                    WINHTTP_FLAG_ASYNC);
    if (!s)
      s = WinHttpOpen(L"jsui/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                      WINHTTP_FLAG_ASYNC);
    if (!s) {
      why = winhttpError(GetLastError());
      return;
    }
    DWORD flags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES |
                  WINHTTP_CALLBACK_FLAG_REDIRECT | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE;
    if (WinHttpSetStatusCallback(s, statusCallback, flags, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      why = winhttpError(GetLastError());
      WinHttpCloseHandle(s);
      s = nullptr;
      return;
    }
    // TLS versions and cipher suites: the system's (Schannel's) defaults.
    // HTTP/1.1 (WinHTTP's default; HTTP/2 is opt-in): responses keep the
    // server's reason phrase (statusText), as on the socket client.
  });
  if (!s && err) *err = why;
  return s;
}

// The connect + request handles of one exchange and its Op. Closing does
// not wait: the Op outlives the exchange until HANDLE_CLOSING if need be.
class Exchange {
 public:
  explicit Exchange(const std::atomic<bool>* cancel) : cancel_(cancel) {}
  ~Exchange() { close(); }

  bool open(HINTERNET s, const HttpUrl& u, const std::string& method, std::string* err) {
    connect_ = WinHttpConnect(s, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
    if (!connect_) {
      *err = winhttpError(GetLastError());
      return false;
    }
    request_ = WinHttpOpenRequest(connect_, widen(method).c_str(), widen(u.target).c_str(), nullptr,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request_) {
      *err = winhttpError(GetLastError());
      return false;
    }
    op_ = new Op();
    DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(op_);
    if (!WinHttpSetOption(request_, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof ctx)) {
      *err = winhttpError(GetLastError());
      // No callback will name this Op: close the handle without waiting.
      WinHttpCloseHandle(request_);
      request_ = nullptr;
      delete op_;
      op_ = nullptr;
      return false;
    }
    return true;
  }

  HINTERNET handle() const { return request_; }
  Op& op() { return *op_; }

  // Before a step: forget the last completion.
  void arm() {
    std::lock_guard<std::mutex> lock(op_->m);
    op_->done = false;
    op_->bytes = 0;
    op_->error = 0;
  }

  // Waits for the armed step. 1 completed (check op().error), 0 deadline,
  // -1 cancelled.
  int wait(Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(op_->m);
    for (;;) {
      if (op_->done) return 1;
      if (cancel_ && cancel_->load(std::memory_order_relaxed)) return -1;
      auto now = Clock::now();
      if (now >= deadline) return 0;
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1);
      op_->cv.wait_for(lock, std::min<std::chrono::milliseconds>(left, std::chrono::milliseconds(50)));
    }
  }

  // Runs one asynchronous step: `start` issues the call; the result is the
  // empty string on success or why it failed.
  template <class F>
  std::string step(F&& start, Clock::time_point deadline, const char* timeoutMsg) {
    arm();
    if (!start()) return winhttpError(GetLastError());
    int w = wait(deadline);
    if (w == 0) return timeoutMsg;
    if (w < 0) return "cancelled";
    std::lock_guard<std::mutex> lock(op_->m);
    if (op_->error) return winhttpError(op_->error, op_->secureFlags);
    return {};
  }

  void close() {
    if (request_) {
      // A pending step fails with ERROR_WINHTTP_OPERATION_CANCELLED, then
      // HANDLE_CLOSING arrives (possibly on this thread, inside the call).
      WinHttpCloseHandle(request_);
      request_ = nullptr;
      std::unique_lock<std::mutex> lock(op_->m);
      if (op_->closed) {
        lock.unlock();
        delete op_;
      } else {
        op_->orphaned = true;
      }
      op_ = nullptr;
    }
    if (connect_) {
      WinHttpCloseHandle(connect_);
      connect_ = nullptr;
    }
  }

 private:
  const std::atomic<bool>* cancel_;
  HINTERNET connect_ = nullptr;
  HINTERNET request_ = nullptr;
  Op* op_ = nullptr;
};

bool queryNumber(HINTERNET h, DWORD what, DWORD* out) {
  DWORD len = sizeof *out;
  return WinHttpQueryHeaders(h, what | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, out, &len,
                             WINHTTP_NO_HEADER_INDEX) != FALSE;
}

std::string queryString(HINTERNET h, DWORD what) {
  DWORD bytes = 0;
  WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
  std::wstring w(bytes / sizeof(wchar_t) + 1, L'\0');
  if (!WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, &w[0], &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
  return narrow(w.data(), bytes / sizeof(wchar_t));
}

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

bool bodyForbidden(const std::string& method, int status) {
  return method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
}

HttpResponse winhttpRequest(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
  auto overall = after(req.timeoutMs);
  HttpResponse res;
  res.url = req.url;
  HttpUrl u;
  if (!parseHttpUrl(req.url, &u, &res.error)) return res;
  HINTERNET s = session(&res.error);
  if (!s) return res;
  Exchange ex(cancel);
  if (!ex.open(s, u, req.method, &res.error)) return res;
  HINTERNET h = ex.handle();

  // Redirects: WinHTTP follows them itself (to http:// too, as browsers do)
  // up to maxRedirects; with 'error' / 'manual' it hands the 3xx back. No
  // cookies (the built-in client keeps none either).
  DWORD features = WINHTTP_DISABLE_COOKIES;
  if (req.redirect == Redirect::Follow) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    DWORD maxHops = DWORD(std::max(0, req.maxRedirects));
    WinHttpSetOption(h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxHops, sizeof maxHops);
  } else {
    features |= WINHTTP_DISABLE_REDIRECTS;
  }
  WinHttpSetOption(h, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
  // WinHTTP's own timeouts as a backstop (ours below are the ones that
  // apply): -1 waits forever.
  int headerMs = req.headerTimeoutMs > 0 ? req.headerTimeoutMs : -1;
  int recvMs = req.headerTimeoutMs > 0 && req.idleTimeoutMs > 0 ? std::max(req.headerTimeoutMs, req.idleTimeoutMs) : -1;
  WinHttpSetTimeouts(h, 0, headerMs, headerMs, recvMs);

  std::string headers;
  bool hasAccept = false, hasCT = false;
  for (auto& [k, v] : req.headers) {
    // Framing headers are WinHTTP's; identity encoding as in the built-in client.
    if (iequals(k, "host") || iequals(k, "content-length") || iequals(k, "connection") ||
        iequals(k, "transfer-encoding") || iequals(k, "accept-encoding"))
      continue;
    if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
    if (iequals(k, "user-agent")) {
      // Replaces the session's "jsui/0.1" (a header would go out next to it).
      std::wstring ua = widen(v);
      if (!ua.empty() && WinHttpSetOption(h, WINHTTP_OPTION_USER_AGENT, &ua[0], DWORD(ua.size()))) continue;
    }
    hasAccept |= iequals(k, "accept");
    hasCT |= iequals(k, "content-type");
    headers += k + ": " + v + "\r\n";
  }
  if (!hasAccept) headers += "Accept: */*\r\n";
  headers += "Accept-Encoding: identity\r\n";
  if (!hasCT && !req.body.empty()) headers += "Content-Type: application/octet-stream\r\n";
  Op& op = ex.op();
  op.url = req.url;  // no callback runs before WinHttpSendRequest
  op.headers = widen(headers);
  op.body = req.body;

  auto headerDeadline = std::min(overall, after(req.headerTimeoutMs));
  const char* headMsg = "timed out waiting for response";
  DWORD bodySize = DWORD(op.body.size());
  void* body = op.body.empty() ? WINHTTP_NO_REQUEST_DATA : op.body.data();
  std::string err = ex.step(
      [&] {
        // Its context argument becomes the handle's context: keep it the Op.
        return WinHttpSendRequest(h, op.headers.c_str(), DWORD(op.headers.size()), body, bodySize, bodySize,
                                  reinterpret_cast<DWORD_PTR>(&op)) != FALSE;
      },
      headerDeadline, headMsg);
  if (err.empty()) err = ex.step([&] { return WinHttpReceiveResponse(h, nullptr) != FALSE; }, headerDeadline, headMsg);
  if (!err.empty()) {
    res.error = err;
    return res;
  }

  DWORD code = 0;
  if (!queryNumber(h, WINHTTP_QUERY_STATUS_CODE, &code) || code < 100 || code > 999) {
    res.error = "malformed status code";
    return res;
  }
  res.status = int(code);
  res.statusText = queryString(h, WINHTTP_QUERY_STATUS_TEXT);
  {
    std::lock_guard<std::mutex> lock(op.m);
    res.redirected = op.redirected;
    if (op.redirected) res.url = op.url;
  }
  std::string raw = queryString(h, WINHTTP_QUERY_RAW_HEADERS_CRLF);
  size_t pos = raw.find("\r\n");  // past the status line
  pos = pos == std::string::npos ? raw.size() : pos + 2;
  while (pos < raw.size()) {
    size_t e = raw.find("\r\n", pos);
    if (e == std::string::npos) e = raw.size();
    std::string line = raw.substr(pos, e - pos);
    pos = e + 2;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    res.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
  }

  if (req.redirect == Redirect::Follow && isRedirect(res.status) && !res.header("location").empty()) {
    // WinHTTP stopped following (past maxRedirects, on versions that hand
    // the last 3xx back instead of ERROR_WINHTTP_REDIRECT_FAILED).
    std::string url = res.url;
    res = HttpResponse();
    res.url = url;
    res.error = "too many redirects (" + std::to_string(req.maxRedirects) + ")";
    return res;
  }
  if (req.redirect == Redirect::Error && isRedirect(res.status) && !res.header("location").empty()) {
    std::string to = resolveLocation(res.url, res.header("location"));
    std::string url = res.url;
    res = HttpResponse();
    res.url = url;
    res.error = "redirect to " + to + " with redirect: 'error'";
    return res;
  }
  if (stream && stream->onHead && !stream->onHead(res)) {
    res.status = 0;
    res.error = "cancelled";
    return res;
  }
  if (bodyForbidden(req.method, res.status)) return res;

  auto fail = [&](const std::string& e) {
    res.status = 0;
    res.error = e;
    res.body.clear();
    return res;
  };
  if (!stream) {
    std::string cl = res.header("content-length");
    if (!cl.empty()) {
      unsigned long long want = std::strtoull(cl.c_str(), nullptr, 10);
      if (want > req.maxBodyBytes) return fail("response body too large");
      res.body.reserve(size_t(want));
    }
  }
  const char* idleMsg = "timed out reading the response body";
  for (;;) {
    // Waits for data (the idle timeout), then copies what is there.
    auto idle = std::min(overall, after(req.idleTimeoutMs));
    err = ex.step([&] { return WinHttpQueryDataAvailable(h, nullptr) != FALSE; }, idle, idleMsg);
    if (!err.empty()) return fail(err);
    DWORD avail;
    {
      std::lock_guard<std::mutex> lock(op.m);
      avail = op.bytes;
    }
    if (avail == 0) break;  // end of the body
    DWORD want = std::min<DWORD>(avail, DWORD(sizeof op.buf));
    err = ex.step([&] { return WinHttpReadData(h, op.buf, want, nullptr) != FALSE; }, idle, idleMsg);
    if (!err.empty()) return fail(err);
    DWORD got;
    {
      std::lock_guard<std::mutex> lock(op.m);
      got = op.bytes;
    }
    if (got == 0) break;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(op.buf);
    if (stream) {
      if (stream->onData && !stream->onData(data, got)) return fail("cancelled");
    } else {
      if (res.body.size() + got > req.maxBodyBytes) return fail("response body too large");
      res.body.insert(res.body.end(), data, data + got);
    }
  }
  return res;
}

}  // namespace

// http:// on the built-in client's sockets, https:// on WinHTTP.
HttpResponse fetchUrl(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
  HttpUrl u;
  std::string err;
  if (!parseHttpUrl(req.url, &u, &err) || !u.tls) return httpRequest(req, cancel, stream);
  return winhttpRequest(req, cancel, stream);
}

}  // namespace jsui::net

#endif  // _WIN32
