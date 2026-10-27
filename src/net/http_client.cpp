#include "net/http_client.h"
#include "net/tls.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static const SocketHandle kBadSocket = INVALID_SOCKET;
#define JSUI_POLL WSAPoll
static int lastSocketError() { return WSAGetLastError(); }
static bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
static void closeSocket(SocketHandle s) { closesocket(s); }
static void setNonBlocking(SocketHandle s) {
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
}
#elif !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle kBadSocket = -1;
#define JSUI_POLL poll
static int lastSocketError() { return errno; }
static bool inProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
static bool wouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINTR; }
static void closeSocket(SocketHandle s) { ::close(s); }
static void setNonBlocking(SocketHandle s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

namespace jsui::net {

bool iequals(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

std::string resolveLocation(const std::string& base, const std::string& loc) {
  if (loc.find("://") != std::string::npos) return loc;
  auto scheme = base.find("://");
  auto pathStart = base.find('/', scheme + 3);
  std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
  if (loc.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + loc;
  if (!loc.empty() && loc[0] == '/') return origin + loc;
  std::string path = pathStart == std::string::npos ? "/" : base.substr(pathStart);
  path = path.substr(0, path.find_first_of("?#"));
  return origin + path.substr(0, path.rfind('/') + 1) + loc;
}

std::string HttpResponse::header(const std::string& name) const {
  for (auto& [k, v] : headers)
    if (iequals(k, name.c_str())) return v;
  return {};
}

bool parseHttpUrl(const std::string& url, HttpUrl* out, std::string* error) {
  auto scheme = url.find("://");
  if (scheme == std::string::npos) {
    *error = "not an absolute URL: " + url;
    return false;
  }
  std::string s = url.substr(0, scheme);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  out->tls = s == "https";
  if (s != "http" && s != "https") {
    *error = "unsupported URL scheme '" + s + "'";
    return false;
  }
  size_t hostStart = scheme + 3;
  size_t pathStart = url.find_first_of("/?#", hostStart);
  std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  if (auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
  out->port = out->tls ? 443 : 80;
  if (!authority.empty() && authority[0] == '[') {  // [ipv6]:port
    auto close = authority.find(']');
    if (close == std::string::npos) {
      *error = "bad IPv6 host in " + url;
      return false;
    }
    out->host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') out->port = std::atoi(authority.c_str() + close + 2);
  } else {
    auto colon = authority.rfind(':');
    out->host = authority.substr(0, colon);
    if (colon != std::string::npos) out->port = std::atoi(authority.c_str() + colon + 1);
  }
  if (out->host.empty() || out->port <= 0 || out->port > 65535) {
    *error = "bad host/port in " + url;
    return false;
  }
  std::string target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  target = target.substr(0, target.find('#'));
  if (target.empty() || target[0] != '/') target = "/" + target;
  out->target = target;
  return true;
}

int ChunkedDecoder::feed(const char* data, size_t size, std::vector<uint8_t>* out) {
  size_t i = 0;
  while (i < size || state_ == State::Done) {
    switch (state_) {
      case State::Size:
      case State::DataCrlf:
      case State::Trailer: {
        // Line-oriented states.
        while (i < size && data[i] != '\n') line_ += data[i++];
        if (i >= size) {
          if (line_.size() > 4096) return -1;
          return 0;
        }
        i++;  // '\n'
        std::string line = line_;
        line_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (state_ == State::DataCrlf) {
          if (!line.empty()) return -1;
          state_ = State::Size;
        } else if (state_ == State::Size) {
          line = line.substr(0, line.find(';'));  // chunk extensions
          line = trim(line);
          if (line.empty()) return -1;
          char* end = nullptr;
          unsigned long long n = std::strtoull(line.c_str(), &end, 16);
          if (*end) return -1;
          remaining_ = size_t(n);
          state_ = n == 0 ? State::Trailer : State::Data;
        } else {  // trailer lines until an empty one
          if (line.empty()) state_ = State::Done;
        }
        break;
      }
      case State::Data: {
        size_t n = std::min(remaining_, size - i);
        out->insert(out->end(), data + i, data + i + n);
        i += n;
        remaining_ -= n;
        if (remaining_ == 0) state_ = State::DataCrlf;
        break;
      }
      case State::Done:
        return 1;
    }
  }
  return state_ == State::Done ? 1 : 0;
}

#if defined(__EMSCRIPTEN__)

HttpResponse httpRequest(const HttpRequest& req, const std::atomic<bool>*, const HttpStream*) {
  HttpResponse res;
  res.url = req.url;
  res.error = "raw sockets are not available in the browser build";
  return res;
}

#else

namespace {

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
struct WinsockInit {
  bool ok;
  WinsockInit() {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }
  ~WinsockInit() {
    if (ok) WSACleanup();
  }
};
#endif

struct Socket {
  SocketHandle fd = kBadSocket;
  ~Socket() {
    if (fd != kBadSocket) closeSocket(fd);
  }
};

// Waits for `events` on the socket in short slices so cancellation is noticed.
// Returns 1 ready, 0 timeout, -1 cancelled.
int waitFor(SocketHandle fd, short events, Clock::time_point deadline, const std::atomic<bool>* cancel) {
  for (;;) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return -1;
    auto now = Clock::now();
    if (now >= deadline) return 0;
    int ms = int(std::min<long long>(50, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1));
    pollfd p{};
    p.fd = fd;
    p.events = events;
    int r = JSUI_POLL(&p, 1, ms);
    if (r > 0) return 1;
    if (r < 0 && !wouldBlock(lastSocketError())) return 1;  // let the next I/O call report it
  }
}

class Exchange {
 public:
  Exchange(const std::atomic<bool>* cancel, Clock::time_point deadline) : cancel_(cancel), deadline_(deadline) {}

  // The deadline for the waits that follow (header / idle timeouts).
  void setDeadline(Clock::time_point d) { deadline_ = d; }

  bool connect(const HttpUrl& u, std::string* err) {
    if (u.tls && !tlsAvailable(err)) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    std::string port = std::to_string(u.port);
    // getaddrinfo blocks and cannot be cancelled.
    int gai = getaddrinfo(u.host.c_str(), port.c_str(), &hints, &list);
    if (gai != 0 || !list) {
      *err = "cannot resolve host '" + u.host + "'";
      return false;
    }
    *err = "connection refused: " + u.host + ":" + port;
    for (addrinfo* a = list; a; a = a->ai_next) {
      SocketHandle fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd == kBadSocket) continue;
      setNonBlocking(fd);
      int r = ::connect(fd, a->ai_addr, (int)a->ai_addrlen);
      bool ok = r == 0;
      if (!ok && inProgress(lastSocketError())) {
        int w = waitFor(fd, POLLOUT, deadline_, cancel_);
        if (w == 1) {
          int soErr = 0;
          socklen_t len = sizeof soErr;
          getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len);
          ok = soErr == 0;
        } else {
          *err = w == 0 ? "timed out connecting to " + u.host + ":" + port : "cancelled";
          closeSocket(fd);
          if (w < 0) break;
          continue;
        }
      }
      if (ok) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        sock_.fd = fd;
        break;
      }
      closeSocket(fd);
    }
    freeaddrinfo(list);
    if (sock_.fd == kBadSocket) return false;
    if (u.tls) return startTls(u.host, err);
    return true;
  }

  bool sendAll(const char* data, size_t size, std::string* err) {
    while (size > 0) {
      long long n;
      if (tls_) {
        n = tls_->write(data, std::min<size_t>(size, 1 << 20), err);
        if (n == TlsSession::kWantRead || n == TlsSession::kWantWrite) {
          if (!waitIo(n == TlsSession::kWantWrite ? POLLOUT : POLLIN, "timed out sending request", err)) return false;
          continue;
        }
        if (n < 0) return false;
      } else {
#if defined(MSG_NOSIGNAL)
        n = ::send(sock_.fd, data, (int)std::min<size_t>(size, 1 << 20), MSG_NOSIGNAL);
#else
        n = ::send(sock_.fd, data, (int)std::min<size_t>(size, 1 << 20), 0);
#endif
        if (n < 0 && wouldBlock(lastSocketError())) {
          if (!waitIo(POLLOUT, "timed out sending request", err)) return false;
          continue;
        }
      }
      if (n > 0) {
        data += n;
        size -= size_t(n);
        continue;
      }
      *err = "connection lost while sending request";
      return false;
    }
    return true;
  }

  // Reads more bytes into `buf`. Returns bytes read, 0 on EOF, -1 on error.
  long long recvSome(std::string* buf, std::string* err, const char* timeoutMsg = "timed out waiting for response") {
    char tmp[16384];
    for (;;) {
      if (tls_) {
        long long n = tls_->read(tmp, sizeof tmp, err);
        if (n > 0) {
          buf->append(tmp, size_t(n));
          return n;
        }
        if (n == 0) return 0;
        if (n == TlsSession::kWantRead || n == TlsSession::kWantWrite) {
          if (!waitIo(n == TlsSession::kWantWrite ? POLLOUT : POLLIN, timeoutMsg, err)) return -1;
          continue;
        }
        return -1;
      }
      auto n = ::recv(sock_.fd, tmp, sizeof tmp, 0);
      if (n > 0) {
        buf->append(tmp, size_t(n));
        return n;
      }
      if (n == 0) return 0;
      if (wouldBlock(lastSocketError())) {
        if (!waitIo(POLLIN, timeoutMsg, err)) return -1;
        continue;
      }
      *err = "connection reset";
      return -1;
    }
  }

 private:
  bool waitIo(short events, const char* timeoutMsg, std::string* err) {
    int w = waitFor(sock_.fd, events, deadline_, cancel_);
    if (w == 1) return true;
    *err = w == 0 ? timeoutMsg : "cancelled";
    return false;
  }

  bool startTls(const std::string& host, std::string* err) {
    tls_ = tlsClient(intptr_t(sock_.fd), host, err);
    if (!tls_) return false;
    for (;;) {
      int r = tls_->handshake(err);
      if (r == 1) return true;
      if (r < 0) return false;
      if (!waitIo(tls_->wantWrite() ? POLLOUT : POLLIN, "timed out in the TLS handshake", err)) return false;
    }
  }

  const std::atomic<bool>* cancel_;
  Clock::time_point deadline_;
  Socket sock_;
  std::unique_ptr<TlsSession> tls_;  // destroyed before sock_ closes the socket
};

bool bodyForbidden(const std::string& method, int status) {
  return method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
}

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

Clock::time_point after(int ms) {
  return ms > 0 ? Clock::now() + std::chrono::milliseconds(ms) : Clock::time_point::max();
}

// Where the body goes: the stream, or res->body (bounded by maxBodyBytes).
struct BodySink {
  const HttpRequest& req;
  const HttpStream* stream;
  HttpResponse* res;
  bool put(const char* data, size_t size) {
    if (!size) return true;
    if (stream) {
      if (stream->onData && !stream->onData(reinterpret_cast<const uint8_t*>(data), size)) {
        res->error = "cancelled";
        return false;
      }
      return true;
    }
    if (res->body.size() + size > req.maxBodyBytes) {
      res->error = "response body too large";
      return false;
    }
    res->body.insert(res->body.end(), data, data + size);
    return true;
  }
};

// One request/response without redirects. A redirect (with `followable`
// set) returns right after its headers: its body is never read.
void exchangeOnce(const HttpRequest& req, const std::string& url, Clock::time_point overall, bool followable,
                  const std::atomic<bool>* cancel, const HttpStream* stream, HttpResponse* res) {
  HttpUrl u;
  if (!parseHttpUrl(url, &u, &res->error)) return;
  Exchange ex(cancel, std::min(overall, after(req.headerTimeoutMs)));
  if (!ex.connect(u, &res->error)) return;

  std::string hostHeader = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
  if (u.port != (u.tls ? 443 : 80)) hostHeader += ":" + std::to_string(u.port);
  std::string head = req.method + " " + u.target + " HTTP/1.1\r\nHost: " + hostHeader + "\r\n";
  bool hasUA = false, hasAccept = false, hasCT = false;
  for (auto& [k, v] : req.headers) {
    // Framing headers are ours to set.
    if (iequals(k, "host") || iequals(k, "content-length") || iequals(k, "connection") ||
        iequals(k, "transfer-encoding") || iequals(k, "accept-encoding"))
      continue;
    if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
    hasUA |= iequals(k, "user-agent");
    hasAccept |= iequals(k, "accept");
    hasCT |= iequals(k, "content-type");
    head += k + ": " + v + "\r\n";
  }
  if (!hasUA) head += "User-Agent: jsui/0.1\r\n";
  if (!hasAccept) head += "Accept: */*\r\n";
  head += "Accept-Encoding: identity\r\nConnection: close\r\n";
  bool sendsBody = !req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH";
  if (sendsBody) {
    if (!hasCT && !req.body.empty()) head += "Content-Type: application/octet-stream\r\n";
    head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
  }
  head += "\r\n";
  if (!ex.sendAll(head.data(), head.size(), &res->error)) return;
  if (!req.body.empty() && !ex.sendAll((const char*)req.body.data(), req.body.size(), &res->error)) return;

  std::string buf;
  size_t headerEnd = std::string::npos;
  for (;;) {
    // Parse a status line + headers, skipping 1xx interim responses.
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
      if (buf.size() > (64u << 10)) {
        res->error = "response headers too large";
        return;
      }
      long long n = ex.recvSome(&buf, &res->error);
      if (n < 0) return;
      if (n == 0) {
        res->error = buf.empty() ? "server closed the connection without a response" : "truncated response headers";
        return;
      }
    }
    std::string headText = buf.substr(0, headerEnd);
    buf.erase(0, headerEnd + 4);
    size_t eol = headText.find("\r\n");
    std::string statusLine = headText.substr(0, eol);
    if (statusLine.rfind("HTTP/", 0) != 0) {
      res->error = "malformed status line: " + statusLine.substr(0, 80);
      return;
    }
    size_t sp = statusLine.find(' ');
    res->status = sp == std::string::npos ? 0 : std::atoi(statusLine.c_str() + sp + 1);
    size_t sp2 = sp == std::string::npos ? std::string::npos : statusLine.find(' ', sp + 1);
    res->statusText = sp2 == std::string::npos ? "" : statusLine.substr(sp2 + 1);
    res->headers.clear();
    size_t pos = eol == std::string::npos ? headText.size() : eol + 2;
    while (pos < headText.size()) {
      size_t e = headText.find("\r\n", pos);
      if (e == std::string::npos) e = headText.size();
      std::string line = headText.substr(pos, e - pos);
      pos = e + 2;
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      res->headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    if (res->status >= 100 && res->status < 200) continue;
    break;
  }
  if (res->status < 100 || res->status > 999) {
    res->status = 0;
    res->error = "malformed status code";
    return;
  }
  if (followable && isRedirect(res->status) && !res->header("location").empty()) return;
  if (stream && stream->onHead && !stream->onHead(*res)) {
    res->status = 0;
    res->error = "cancelled";
    return;
  }
  if (bodyForbidden(req.method, res->status)) return;

  auto fail = [&](const std::string& e) {
    res->status = 0;
    res->error = e;
    res->body.clear();
  };
  // From here on the deadline is the idle timeout, renewed per chunk.
  auto renew = [&] { ex.setDeadline(std::min(overall, after(req.idleTimeoutMs))); };
  const char* idleMsg = "timed out reading the response body";
  BodySink sink{req, stream, res};
  std::string te = res->header("transfer-encoding");
  std::transform(te.begin(), te.end(), te.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::string cl = res->header("content-length");
  if (te.find("chunked") != std::string::npos) {
    ChunkedDecoder dec;
    std::vector<uint8_t> chunk;
    int r = dec.feed(buf.data(), buf.size(), &chunk);
    for (;;) {
      if (!sink.put(reinterpret_cast<const char*>(chunk.data()), chunk.size())) return fail(res->error);
      chunk.clear();
      if (r != 0) break;
      buf.clear();
      renew();
      long long n = ex.recvSome(&buf, &res->error, idleMsg);
      if (n < 0) return fail(res->error);
      if (n == 0) return fail("truncated chunked response");
      r = dec.feed(buf.data(), buf.size(), &chunk);
    }
    if (r < 0) return fail("malformed chunked encoding");
  } else if (!cl.empty()) {
    char* end = nullptr;
    unsigned long long want = std::strtoull(cl.c_str(), &end, 10);
    if (*end) return fail("bad Content-Length");
    if (!stream && want > req.maxBodyBytes) return fail("response body too large");
    if (!stream) res->body.reserve(size_t(want));
    unsigned long long got = std::min<unsigned long long>(buf.size(), want);
    if (!sink.put(buf.data(), size_t(got))) return fail(res->error);
    while (got < want) {
      buf.clear();
      renew();
      long long n = ex.recvSome(&buf, &res->error, idleMsg);
      if (n < 0) return fail(res->error);
      if (n == 0) return fail("truncated response body (" + std::to_string(got) + " of " + cl + " bytes)");
      size_t take = size_t(std::min<unsigned long long>(buf.size(), want - got));
      if (!sink.put(buf.data(), take)) return fail(res->error);
      got += take;
    }
  } else {
    if (!sink.put(buf.data(), buf.size())) return fail(res->error);
    for (;;) {
      buf.clear();
      renew();
      long long n = ex.recvSome(&buf, &res->error, idleMsg);
      if (n < 0) return fail(res->error);
      if (n == 0) break;
      if (!sink.put(buf.data(), buf.size())) return fail(res->error);
    }
  }
}

}  // namespace

HttpResponse httpRequest(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
#if defined(_WIN32)
  WinsockInit wsa;  // ref-counted by Winsock itself
#endif
  auto overall = after(req.timeoutMs);
  HttpRequest cur = req;
  std::string url = req.url;
  HttpResponse res;
  bool followable = req.redirect != Redirect::Manual;
  for (int hop = 0;; hop++) {
    res = HttpResponse();
    res.url = url;
    res.redirected = hop > 0;
    exchangeOnce(cur, url, overall, followable, cancel, stream, &res);
    if (!followable || !isRedirect(res.status)) break;
    std::string loc = res.header("location");
    if (loc.empty()) break;
    if (req.redirect == Redirect::Error || hop >= req.maxRedirects) {
      std::string why = req.redirect == Redirect::Error ? "redirect to " + resolveLocation(url, loc) + " with redirect: 'error'"
                                                        : "too many redirects (" + std::to_string(hop) + ")";
      res = HttpResponse();
      res.url = url;
      res.error = why;
      break;
    }
    url = resolveLocation(url, loc);
    // 303 always, 301/302 for POST (browser behaviour): switch to GET without body.
    if ((res.status == 303 && cur.method != "HEAD") || ((res.status == 301 || res.status == 302) && cur.method == "POST")) {
      cur.method = "GET";
      cur.body.clear();
      for (auto it = cur.headers.begin(); it != cur.headers.end();)
        it = iequals(it->first, "content-type") ? cur.headers.erase(it) : it + 1;
    }
  }
  return res;
}

#endif  // !__EMSCRIPTEN__

#if !defined(JSUI_OS_HTTPS)
// No OS HTTP stack: https:// on this client's TLS (net/tls.h).
HttpResponse fetchUrl(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
  return httpRequest(req, cancel, stream);
}
#endif

}  // namespace jsui::net
