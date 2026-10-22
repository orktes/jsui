// Minimal blocking HTTP/1.1 client (http:// only) for dev servers and LAN
// asset servers. Runs on the loader's worker threads. No TLS: https:// URLs
// fail with status 0 and a clear error unless the embedder installs a loader.
//
// Supports Content-Length, chunked and read-until-close bodies, redirects
// (301/302/303/307/308, up to maxRedirects), an overall timeout and
// cooperative cancellation (checked every few ms while waiting on sockets).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jsui::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  Headers headers;
  std::vector<uint8_t> body;
  int timeoutMs = 30000;
  int maxRedirects = 5;
  size_t maxBodyBytes = size_t(512) << 20;
};

struct HttpResponse {
  int status = 0;  // 0: network / protocol error, see `error`
  std::string statusText;
  std::string url;  // final URL after redirects
  Headers headers;
  std::vector<uint8_t> body;
  std::string error;
  bool redirected = false;
  // First value of a header (case-insensitive), or empty.
  std::string header(const std::string& name) const;
};

struct HttpUrl {
  std::string host;
  int port = 80;
  std::string target = "/";  // path + query, no fragment
};
bool parseHttpUrl(const std::string& url, HttpUrl* out, std::string* error);

HttpResponse httpRequest(const HttpRequest& req, const std::atomic<bool>* cancel = nullptr);

// Incremental chunked transfer-encoding decoder (exposed for tests).
class ChunkedDecoder {
 public:
  // Consumes bytes from `data`; returns -1 on malformed input, 1 when the
  // terminating chunk (and trailers) were read, 0 when more input is needed.
  int feed(const char* data, size_t size, std::vector<uint8_t>* out);

 private:
  enum class State { Size, Data, DataCrlf, Trailer, Done } state_ = State::Size;
  std::string line_;
  size_t remaining_ = 0;
};

}  // namespace jsui::net
