#include "jsui_http.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace godot;

namespace jsui_godot {
namespace {

constexpr int kHeaderTimeoutMs = 30000;
constexpr int kIdleTimeoutMs = 30000;
constexpr int kMaxRedirects = 20;
// Body read per request per frame at most (64 KB chunks): a big download
// does not stall the frame.
constexpr int kChunksPerFrame = 64;

bool iequals(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool isNetwork(const char* url) {
  std::string u = lower(std::string(url ? url : "").substr(0, 8));
  return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

struct Url {
  bool tls = false;
  std::string host;  // brackets stripped from IPv6 literals
  int port = 80;
  std::string target = "/";
};

bool parseUrl(const std::string& url, Url* out) {
  std::string l = lower(url.substr(0, 8));
  size_t rest;
  if (l.rfind("https://", 0) == 0) {
    out->tls = true;
    out->port = 443;
    rest = 8;
  } else if (l.rfind("http://", 0) == 0) {
    rest = 7;
  } else {
    return false;
  }
  size_t end = url.find_first_of("/?#", rest);
  std::string authority = url.substr(rest, end == std::string::npos ? std::string::npos : end - rest);
  if (auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
  size_t colon = std::string::npos;
  if (!authority.empty() && authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    out->host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') colon = close + 1;
  } else {
    colon = authority.rfind(':');
    out->host = authority.substr(0, colon);
  }
  if (colon != std::string::npos) {
    std::string p = authority.substr(colon + 1);
    if (!p.empty()) {
      if (p.find_first_not_of("0123456789") != std::string::npos) return false;
      out->port = std::atoi(p.c_str());
    }
  }
  if (out->host.empty() || out->port <= 0 || out->port > 65535) return false;
  if (end != std::string::npos) {
    std::string t = url.substr(end);
    t = t.substr(0, t.find('#'));
    if (t.empty()) t = "/";
    if (t[0] == '?') t = "/" + t;
    out->target = t;
  }
  return true;
}

// Location against the URL it came from (as the built-in loader does).
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

bool methodFor(const std::string& m, HTTPClient::Method* out) {
  static const std::pair<const char*, HTTPClient::Method> kMethods[] = {
      {"GET", HTTPClient::METHOD_GET},         {"HEAD", HTTPClient::METHOD_HEAD},
      {"POST", HTTPClient::METHOD_POST},       {"PUT", HTTPClient::METHOD_PUT},
      {"DELETE", HTTPClient::METHOD_DELETE},   {"OPTIONS", HTTPClient::METHOD_OPTIONS},
      {"TRACE", HTTPClient::METHOD_TRACE},     {"CONNECT", HTTPClient::METHOD_CONNECT},
      {"PATCH", HTTPClient::METHOD_PATCH},
  };
  for (auto& [name, method] : kMethods)
    if (m == name) {
      *out = method;
      return true;
    }
  return false;
}

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

std::string toStd(const String& s) {
  CharString u = s.utf8();
  return std::string(u.get_data(), size_t(u.length()));
}

uint64_t nowMs() { return Time::get_singleton()->get_ticks_msec(); }

uint64_t after(uint64_t now, int ms) { return ms > 0 ? now + uint64_t(ms) : UINT64_MAX; }

}  // namespace

ViewHttp::ViewHttp() = default;

ViewHttp::~ViewHttp() { attach(nullptr); }

void ViewHttp::attach(JsuiRuntime* rt) {
  {
    std::lock_guard<std::mutex> lock(m_);
    rt_ = rt;
    incoming_.clear();
    cancels_.clear();
  }
  for (auto& op : ops_)
    if (op->client.is_valid()) op->client->close();
  ops_.clear();
}

int ViewHttp::request(uint64_t id, const JsuiRequest* req) {
  if (!req || !isNetwork(req->url)) return 1;
  auto op = std::make_unique<Op>();
  op->id = id;
  op->url = req->url;
  op->method = req->method ? req->method : "GET";
  for (size_t i = 0; i < req->header_count; i++)
    if (req->headers[i].name) op->headers.emplace_back(req->headers[i].name, req->headers[i].value ? req->headers[i].value : "");
  if (req->body && req->body_size) {
    auto* p = static_cast<const uint8_t*>(req->body);
    op->body.assign(p, p + req->body_size);
  }
  op->timeoutMs = req->timeout_ms;
  op->redirect = req->redirect;
  std::lock_guard<std::mutex> lock(m_);
  incoming_.push_back(std::move(op));
  return 0;
}

void ViewHttp::cancel(uint64_t id) {
  std::lock_guard<std::mutex> lock(m_);
  cancels_.push_back(id);
}

int ViewHttp::active() const {
  std::lock_guard<std::mutex> lock(m_);
  return int(ops_.size() + incoming_.size());
}

void ViewHttp::update() {
  std::vector<std::unique_ptr<Op>> incoming;
  std::vector<uint64_t> cancels;
  {
    std::lock_guard<std::mutex> lock(m_);
    incoming.swap(incoming_);
    cancels.swap(cancels_);
  }
  if (!rt_) return;
  uint64_t now = nowMs();
  for (auto& op : incoming) {
    op->start = now;
    op->deadline = after(now, op->timeoutMs);
    std::string error;
    if (!connect(*op, &error)) {
      fail(*op, error);
      continue;
    }
    ops_.push_back(std::move(op));
  }
  for (uint64_t id : cancels) {
    auto it = std::find_if(ops_.begin(), ops_.end(), [&](auto& op) { return op->id == id; });
    if (it == ops_.end()) continue;
    if ((*it)->client.is_valid()) (*it)->client->close();
    ops_.erase(it);
  }
  for (size_t i = 0; i < ops_.size();) {
    if (advance(*ops_[i], now)) {
      i++;
    } else {
      if (ops_[i]->client.is_valid()) ops_[i]->client->close();
      ops_.erase(ops_.begin() + long(i));
    }
  }
}

bool ViewHttp::connect(Op& op, std::string* error) {
  Url u;
  if (!parseUrl(op.url, &u)) {
    *error = "invalid URL: " + op.url;
    return false;
  }
  if (op.method != "GET" && op.method != "HEAD") {
    HTTPClient::Method m;
    if (!methodFor(op.method, &m)) {
      *error = "HTTP method " + op.method + " is not supported by Godot's HTTPClient";
      return false;
    }
  }
  op.target = u.target;
  op.client.instantiate();
  op.client->set_blocking_mode(false);
  op.client->set_read_chunk_size(65536);
  Error err = op.client->connect_to_host(String::utf8(u.host.c_str()), u.port,
                                         u.tls ? TLSOptions::client() : Ref<TLSOptions>());
  if (err != OK) {
    *error = "could not connect to " + u.host + " (Godot error " + std::to_string(int(err)) + ")";
    return false;
  }
  op.step = Step::Connect;
  op.idleDeadline = std::min(op.deadline, after(nowMs(), kHeaderTimeoutMs));
  return true;
}

void ViewHttp::fail(Op& op, const std::string& error) {
  if (op.responded) {
    jsui_request_end(rt_, op.id, error.c_str());
  } else {
    jsui_request_respond(rt_, op.id, 0, error.c_str(), op.url.c_str(), nullptr, 0);
  }
}

bool ViewHttp::advance(Op& op, uint64_t now) {
  for (;;) {
    op.client->poll();
    HTTPClient::Status st = op.client->get_status();
    switch (op.step) {
      case Step::Connect: {
        if (st == HTTPClient::STATUS_RESOLVING || st == HTTPClient::STATUS_CONNECTING) break;
        if (st != HTTPClient::STATUS_CONNECTED) {
          Url u;
          parseUrl(op.url, &u);
          std::string why = st == HTTPClient::STATUS_CANT_RESOLVE         ? "could not resolve " + u.host
                            : st == HTTPClient::STATUS_CANT_CONNECT        ? "could not connect to " + u.host
                            : st == HTTPClient::STATUS_TLS_HANDSHAKE_ERROR ? "TLS handshake with " + u.host +
                                                                                 " failed (certificate not trusted?)"
                                                                           : "connection to " + u.host + " failed";
          fail(op, why);
          return false;
        }
        PackedStringArray headers;
        bool hasUA = false, hasAccept = false, hasCT = false;
        for (auto& [k, v] : op.headers) {
          // Framing headers are HTTPClient's to set; it cannot decode compressed bodies.
          if (iequals(k, "host") || iequals(k, "content-length") || iequals(k, "connection") ||
              iequals(k, "transfer-encoding") || iequals(k, "accept-encoding"))
            continue;
          if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
          hasUA |= iequals(k, "user-agent");
          hasAccept |= iequals(k, "accept");
          hasCT |= iequals(k, "content-type");
          headers.push_back(String::utf8((k + ": " + v).c_str()));
        }
        if (!hasUA) headers.push_back("User-Agent: jsui/0.1 (Godot)");
        if (!hasAccept) headers.push_back("Accept: */*");
        if (!hasCT && !op.body.empty()) headers.push_back("Content-Type: application/octet-stream");
        PackedByteArray body;
        if (!op.body.empty()) {
          body.resize(int64_t(op.body.size()));
          std::memcpy(body.ptrw(), op.body.data(), op.body.size());
        }
        HTTPClient::Method method = HTTPClient::METHOD_GET;
        methodFor(op.method, &method);
        Error err = op.client->request_raw(method, String::utf8(op.target.c_str()), headers, body);
        if (err != OK) {
          fail(op, "could not send the request (Godot error " + std::to_string(int(err)) + ")");
          return false;
        }
        op.step = Step::Request;
        continue;
      }
      case Step::Request: {
        if (st == HTTPClient::STATUS_REQUESTING) break;
        if (st != HTTPClient::STATUS_BODY && st != HTTPClient::STATUS_CONNECTED) {
          fail(op, "connection failed while waiting for the response");
          return false;
        }
        if (!head(op, now)) return false;
        continue;  // the body, or a redirect's next connection
      }
      case Step::Body: {
        if (st != HTTPClient::STATUS_BODY) {
          if (st == HTTPClient::STATUS_CONNECTED || st == HTTPClient::STATUS_DISCONNECTED) {
            jsui_request_end(rt_, op.id, nullptr);
          } else {
            fail(op, "connection lost while reading the response body");
          }
          return false;
        }
        for (int i = 0; i < kChunksPerFrame && op.client->get_status() == HTTPClient::STATUS_BODY; i++) {
          PackedByteArray chunk = op.client->read_response_body_chunk();
          if (chunk.is_empty()) break;
          jsui_request_body(rt_, op.id, chunk.ptr(), size_t(chunk.size()));
          op.idleDeadline = std::min(op.deadline, after(now, kIdleTimeoutMs));
        }
        st = op.client->get_status();
        if (st != HTTPClient::STATUS_BODY) continue;  // ended (or failed) this frame
        break;
      }
    }
    break;
  }
  if (now >= op.idleDeadline || now >= op.deadline) {
    fail(op, op.step == Step::Body ? "timed out reading the response body" : "timed out waiting for response");
    return false;
  }
  return true;
}

bool ViewHttp::head(Op& op, uint64_t now) {
  int status = int(op.client->get_response_code());
  std::vector<std::pair<std::string, std::string>> headers;
  std::string location;
  PackedStringArray lines = op.client->get_response_headers();
  for (int64_t i = 0; i < lines.size(); i++) {
    std::string line = toStd(lines[i]);
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    if (iequals(headers.back().first, "location") && location.empty()) location = headers.back().second;
  }
  if (isRedirect(status) && !location.empty() && op.redirect != JSUI_REDIRECT_MANUAL) {
    std::string next = resolveLocation(op.url, location);
    if (op.redirect == JSUI_REDIRECT_ERROR) {
      fail(op, "redirect to " + next + " with redirect: 'error'");
      return false;
    }
    if (op.hops >= kMaxRedirects) {
      fail(op, "too many redirects (" + std::to_string(op.hops) + ")");
      return false;
    }
    op.hops++;
    // 303 always, 301 / 302 for POST (browser behaviour): GET without a body.
    if ((status == 303 && op.method != "HEAD") || ((status == 301 || status == 302) && op.method == "POST")) {
      op.method = "GET";
      op.body.clear();
      op.headers.erase(std::remove_if(op.headers.begin(), op.headers.end(),
                                      [](auto& h) { return iequals(h.first, "content-type"); }),
                       op.headers.end());
    }
    op.client->close();
    op.url = next;
    std::string error;
    if (!connect(op, &error)) {
      fail(op, error);
      return false;
    }
    return true;
  }
  std::vector<JsuiHeader> hs;
  hs.reserve(headers.size());
  for (auto& [k, v] : headers) hs.push_back({k.c_str(), v.c_str()});
  jsui_request_respond(rt_, op.id, status, nullptr, op.url.c_str(), hs.data(), hs.size());
  op.responded = true;
  if (op.client->get_status() != HTTPClient::STATUS_BODY) {
    jsui_request_end(rt_, op.id, nullptr);
    return false;
  }
  op.step = Step::Body;
  op.idleDeadline = std::min(op.deadline, after(now, kIdleTimeoutMs));
  return true;
}

}  // namespace jsui_godot
