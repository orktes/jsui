// Minimal blocking HTTP/1.1 client for the built-in loader. Runs on the
// loader's worker threads. http:// goes over plain sockets; https:// uses the
// platform's TLS (net/https.h): OpenSSL loaded at run time on Linux, while
// Apple and Windows hand https:// to the OS HTTP stack instead (fetchUrl()).
//
// Supports Content-Length, chunked and read-until-close bodies, redirects
// (301/302/303/307/308, up to maxRedirects), timeouts (overall, until the
// response headers, and between body chunks), streamed bodies and
// cooperative cancellation (checked every few ms while waiting on sockets).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jsui::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

enum class Redirect { Follow, Error, Manual };

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  Headers headers;
  std::vector<uint8_t> body;
  int timeoutMs = 0;            // whole request, body included; 0: none
  int headerTimeoutMs = 30000;  // until the response headers arrive
  int idleTimeoutMs = 30000;    // between body chunks
  int maxRedirects = 20;
  Redirect redirect = Redirect::Follow;
  size_t maxBodyBytes = size_t(512) << 20;  // buffered (not streamed) bodies only
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

// Streamed delivery. onHead runs once with the final response's status,
// headers and URL (no body); onData runs per body chunk. Either returning
// false stops the request (the result then has status 0, error "cancelled").
// The returned HttpResponse carries no body; status 0 + error after onHead
// means the body failed part way. Both run on the calling (worker) thread.
struct HttpStream {
  std::function<bool(const HttpResponse& head)> onHead;
  std::function<bool(const uint8_t* data, size_t size)> onData;
};

struct HttpUrl {
  bool tls = false;  // https://
  std::string host;
  int port = 80;
  std::string target = "/";  // path + query, no fragment
};
bool parseHttpUrl(const std::string& url, HttpUrl* out, std::string* error);

// http:// and https:// on this client's sockets (https:// needs
// tlsAvailable()). Without `stream` the body is buffered into the result.
HttpResponse httpRequest(const HttpRequest& req, const std::atomic<bool>* cancel = nullptr,
                         const HttpStream* stream = nullptr);

// The built-in loader's entry point: https:// on the OS HTTP stack where
// there is one (NSURLSession on Apple, WinHTTP on Windows), else
// httpRequest(). Same contract as httpRequest.
HttpResponse fetchUrl(const HttpRequest& req, const std::atomic<bool>* cancel = nullptr,
                      const HttpStream* stream = nullptr);

// Resolves a Location header against the request URL.
std::string resolveLocation(const std::string& base, const std::string& loc);
// Case-insensitive ASCII compare.
bool iequals(const std::string& a, const char* b);

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
