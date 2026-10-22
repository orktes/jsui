// Tiny threaded HTTP/1.1 server for loader / fetch / dev-reload tests
// (POSIX only). Each connection is served on its own thread; the handler
// returns the full raw response so tests can exercise chunked bodies,
// redirects and malformed replies.
#pragma once

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jsui_test {

struct HttpReq {
  std::string method, path, body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string header(const std::string& n) const {
    for (auto& [k, v] : headers) {
      if (k.size() != n.size()) continue;
      bool same = true;
      for (size_t i = 0; i < k.size(); i++)
        if (std::tolower((unsigned char)k[i]) != std::tolower((unsigned char)n[i])) same = false;
      if (same) return v;
    }
    return {};
  }
};

inline std::string httpResponse(int status, const std::string& body, const std::string& type = "text/plain",
                                const std::string& extraHeaders = "") {
  return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Status") + "\r\nContent-Type: " + type +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" +
         body;
}

class TestHttpServer {
 public:
  using Handler = std::function<std::string(const HttpReq&)>;

  explicit TestHttpServer(Handler h) : handler_(std::move(h)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd_, (sockaddr*)&a, sizeof a);
    socklen_t len = sizeof a;
    getsockname(fd_, (sockaddr*)&a, &len);
    port_ = ntohs(a.sin_port);
    ::listen(fd_, 64);
    thread_ = std::thread([this] { loop(); });
  }
  ~TestHttpServer() {
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lock(m_);
    for (auto& t : conns_) t.join();
    ::close(fd_);
  }
  int port() const { return port_; }
  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  int requests() const { return requests_; }

 private:
  void loop() {
    while (!stop_) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      std::lock_guard<std::mutex> lock(m_);
      conns_.emplace_back([this, c] { serve(c); });
    }
  }
  void serve(int c) {
    std::string buf;
    char tmp[8192];
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
      ssize_t n = ::recv(c, tmp, sizeof tmp, 0);
      if (n <= 0) {
        ::close(c);
        return;
      }
      buf.append(tmp, size_t(n));
    }
    HttpReq req;
    std::string head = buf.substr(0, end);
    buf.erase(0, end + 4);
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
    req.method = line.substr(0, s1);
    req.path = line.substr(s1 + 1, s2 - s1 - 1);
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
      size_t e = head.find("\r\n", pos);
      if (e == std::string::npos) e = head.size();
      std::string h = head.substr(pos, e - pos);
      pos = e + 2;
      auto colon = h.find(':');
      if (colon != std::string::npos) {
        std::string v = h.substr(colon + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        req.headers.emplace_back(h.substr(0, colon), v);
      }
    }
    size_t want = std::strtoul(req.header("content-length").c_str(), nullptr, 10);
    while (buf.size() < want) {
      ssize_t n = ::recv(c, tmp, sizeof tmp, 0);
      if (n <= 0) break;
      buf.append(tmp, size_t(n));
    }
    req.body = buf.substr(0, want);
    requests_++;
    std::string out = handler_(req);
    size_t off = 0;
    while (off < out.size()) {
      ssize_t n = ::send(c, out.data() + off, out.size() - off, MSG_NOSIGNAL);
      if (n <= 0) break;
      off += size_t(n);
    }
    ::shutdown(c, SHUT_WR);
    // Drain until the client closes so the response is not reset.
    pollfd p{c, POLLIN, 0};
    while (::poll(&p, 1, 200) > 0 && ::recv(c, tmp, sizeof tmp, 0) > 0) {
    }
    ::close(c);
  }

  Handler handler_;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  std::thread thread_;
  std::mutex m_;
  std::vector<std::thread> conns_;
};

}  // namespace jsui_test

#endif  // _WIN32
