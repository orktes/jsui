// https:// on macOS and iOS through NSURLSession: the OS's TLS, certificate
// trust and HTTP/2, nothing bundled. http:// stays on our own socket client
// (httpRequest()), which keeps plain-http dev servers working under App
// Transport Security.
//
// One ephemeral session (no cookies, no cache, no credential storage) serves
// every request; its delegate runs on a serial operation queue and hands
// events to the blocking fetchUrl() call on the loader's worker thread, which
// waits on a condition variable, checks `cancel` and the timeouts every few
// ms and runs the HttpStream callbacks on its own thread.
#if defined(__APPLE__)

#import <Foundation/Foundation.h>

#include "net/http_client.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace jsui::net {
namespace {

using Clock = std::chrono::steady_clock;

// Above this many undelivered bytes the task is suspended until the worker
// catches up (a slow res.body reader does not buffer the whole download).
constexpr size_t kMaxQueuedBytes = size_t(8) << 20;

// One request in flight. The delegate fills it, the worker drains it.
struct Op {
  std::mutex m;
  std::condition_variable cv;
  Redirect redirect = Redirect::Follow;
  int maxRedirects = 20;
  int hops = 0;
  bool haveHead = false;
  HttpResponse head;
  std::deque<std::vector<uint8_t>> chunks;
  size_t queued = 0;
  bool suspended = false;
  bool done = false;
  std::string error;  // set by the delegate (redirect policy) or the completion
};

std::mutex g_opsMutex;
std::unordered_map<NSUInteger, std::shared_ptr<Op>> g_ops;

std::shared_ptr<Op> findOp(NSURLSessionTask* task) {
  std::lock_guard<std::mutex> lock(g_opsMutex);
  auto it = g_ops.find(task.taskIdentifier);
  return it == g_ops.end() ? nullptr : it->second;
}

const char* reasonPhrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
  }
}

std::string utf8(NSString* s) { return s ? std::string(s.UTF8String ?: "") : std::string(); }

std::string describe(NSError* e) {
  return utf8(e.localizedDescription) + " (" + utf8(e.domain) + " " + std::to_string(long(e.code)) + ")";
}

}  // namespace
}  // namespace jsui::net

@interface JsuiHttpsDelegate : NSObject <NSURLSessionDataDelegate>
@end

@implementation JsuiHttpsDelegate

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
  using namespace jsui::net;
  auto op = findOp(task);
  if (!op) {
    completionHandler(nil);
    return;
  }
  std::string why;
  {
    std::lock_guard<std::mutex> lock(op->m);
    if (op->redirect == Redirect::Manual) {
      // nil: the 3xx itself becomes the response, body and all.
      completionHandler(nil);
      return;
    }
    if (op->redirect == Redirect::Error)
      why = "redirect to " + utf8(request.URL.absoluteString) + " with redirect: 'error'";
    else if (op->hops >= op->maxRedirects)
      why = "too many redirects (" + std::to_string(op->hops) + ")";
    else
      op->hops++;
    if (!why.empty()) op->error = why;
  }
  if (!why.empty()) {
    completionHandler(nil);
    [task cancel];
    return;
  }
  completionHandler(request);
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  using namespace jsui::net;
  auto op = findOp(dataTask);
  if (!op) {
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  HttpResponse head;
  head.url = utf8(response.URL.absoluteString);
  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
    auto* http = (NSHTTPURLResponse*)response;
    head.status = int(http.statusCode);
    head.statusText = reasonPhrase(head.status);
    // NSURLSession asks for gzip / deflate / br and decodes the body itself:
    // the reported encoding and length would describe bytes JS never sees.
    NSString* enc = [http valueForHTTPHeaderField:@"Content-Encoding"];
    bool decoded = enc && ![enc.lowercaseString isEqualToString:@"identity"];
    Headers* headers = &head.headers;
    [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
      std::string k = utf8([key description]);
      if (decoded && (iequals(k, "content-encoding") || iequals(k, "content-length"))) return;
      headers->emplace_back(std::move(k), utf8([value description]));
    }];
  } else {
    head.status = 200;
    head.statusText = "OK";
  }
  {
    std::lock_guard<std::mutex> lock(op->m);
    head.redirected = op->hops > 0;
    op->head = std::move(head);
    op->haveHead = true;
  }
  op->cv.notify_all();
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
  using namespace jsui::net;
  auto op = findOp(dataTask);
  if (!op) return;
  {
    std::lock_guard<std::mutex> lock(op->m);
    Op* o = op.get();
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL*) {
      auto* p = static_cast<const uint8_t*>(bytes);
      o->chunks.emplace_back(p, p + range.length);
      o->queued += range.length;
    }];
    if (op->queued > kMaxQueuedBytes && !op->suspended) {
      op->suspended = true;
      [dataTask suspend];
    }
  }
  op->cv.notify_all();
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  using namespace jsui::net;
  auto op = findOp(task);
  if (!op) return;
  {
    std::lock_guard<std::mutex> lock(op->m);
    if (error && op->error.empty())
      op->error = [error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled
                      ? std::string("cancelled")
                      : describe(error);
    op->done = true;
  }
  op->cv.notify_all();
}

@end

namespace jsui::net {
namespace {

NSURLSession* session() {
  static NSURLSession* s = [] {
    NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.HTTPCookieStorage = nil;
    cfg.HTTPShouldSetCookies = NO;
    cfg.URLCache = nil;
    cfg.URLCredentialStorage = nil;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    // fetchUrl() enforces the request's own timeouts; these only back them up.
    cfg.timeoutIntervalForRequest = 24 * 3600;
    cfg.timeoutIntervalForResource = 7 * 24 * 3600;
    cfg.HTTPMaximumConnectionsPerHost = 6;
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    queue.name = @"jsui.https";
    return [NSURLSession sessionWithConfiguration:cfg delegate:[[JsuiHttpsDelegate alloc] init] delegateQueue:queue];
  }();
  return s;
}

Clock::time_point after(Clock::time_point from, int ms) {
  return ms > 0 ? from + std::chrono::milliseconds(ms) : Clock::time_point::max();
}

bool isHttps(const std::string& url) {
  return url.size() >= 8 && iequals(url.substr(0, 8), "https://");
}

HttpResponse fetchHttps(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
  HttpResponse res;
  res.url = req.url;
  NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:req.url.c_str()]];
  if (!url) {
    res.error = "invalid URL: " + req.url;
    return res;
  }
  NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:url];
  r.HTTPMethod = [NSString stringWithUTF8String:req.method.c_str()];
  r.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  r.timeoutInterval = 24 * 3600;
  r.HTTPShouldHandleCookies = NO;
  bool hasUA = false, hasAccept = false, hasCT = false;
  for (auto& [k, v] : req.headers) {
    // Framing headers are the session's to set (it also asks for compression).
    if (iequals(k, "host") || iequals(k, "content-length") || iequals(k, "connection") ||
        iequals(k, "transfer-encoding") || iequals(k, "accept-encoding"))
      continue;
    if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
    hasUA |= iequals(k, "user-agent");
    hasAccept |= iequals(k, "accept");
    hasCT |= iequals(k, "content-type");
    [r addValue:[NSString stringWithUTF8String:v.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:k.c_str()]];
  }
  if (!hasUA) [r setValue:@"jsui/0.1" forHTTPHeaderField:@"User-Agent"];
  if (!hasAccept) [r setValue:@"*/*" forHTTPHeaderField:@"Accept"];
  if (!req.body.empty()) {
    if (!hasCT) [r setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
    r.HTTPBody = [NSData dataWithBytes:req.body.data() length:req.body.size()];
  } else if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
    r.HTTPBody = [NSData data];
  }

  auto op = std::make_shared<Op>();
  op->redirect = req.redirect;
  op->maxRedirects = req.maxRedirects;
  NSURLSessionDataTask* task = [session() dataTaskWithRequest:r];
  {
    std::lock_guard<std::mutex> lock(g_opsMutex);
    g_ops[task.taskIdentifier] = op;
  }
  [task resume];

  auto start = Clock::now();
  auto overall = after(start, req.timeoutMs);
  auto headerDeadline = std::min(overall, after(start, req.headerTimeoutMs));
  auto idleDeadline = Clock::time_point::max();
  bool headDelivered = false;
  bool bodyAllowed = true;
  std::string fail;  // a failure decided here (timeout, cancel, size, callback)
  for (;;) {
    std::deque<std::vector<uint8_t>> chunks;
    bool done = false, gotHead = false, resume = false;
    {
      std::unique_lock<std::mutex> lock(op->m);
      op->cv.wait_for(lock, std::chrono::milliseconds(15), [&] {
        return op->done || !op->chunks.empty() || (op->haveHead && !headDelivered);
      });
      if (op->haveHead && !headDelivered) {
        res.status = op->head.status;
        res.statusText = op->head.statusText;
        res.url = op->head.url;
        res.headers = op->head.headers;
        res.redirected = op->head.redirected;
        gotHead = true;
      }
      chunks.swap(op->chunks);
      op->queued = 0;
      done = op->done && op->chunks.empty();
      if (op->suspended) {
        op->suspended = false;
        resume = true;
      }
    }
    if (resume) [task resume];
    auto now = Clock::now();
    if (gotHead) {
      headDelivered = true;
      bodyAllowed = !(req.method == "HEAD" || res.status == 204 || res.status == 304);
      idleDeadline = std::min(overall, after(now, req.idleTimeoutMs));
      if (stream && stream->onHead && !stream->onHead(res)) fail = "cancelled";
    }
    if (fail.empty() && !chunks.empty()) {
      idleDeadline = std::min(overall, after(now, req.idleTimeoutMs));
      for (auto& c : chunks) {
        if (!bodyAllowed || c.empty()) continue;
        if (stream) {
          if (stream->onData && !stream->onData(c.data(), c.size())) {
            fail = "cancelled";
            break;
          }
        } else if (res.body.size() + c.size() > req.maxBodyBytes) {
          fail = "response body too large";
          break;
        } else {
          res.body.insert(res.body.end(), c.begin(), c.end());
        }
      }
    }
    if (fail.empty() && done) break;
    if (fail.empty() && cancel && cancel->load()) fail = "cancelled";
    if (fail.empty() && now >= (headDelivered ? idleDeadline : headerDeadline))
      fail = headDelivered ? "timed out reading the response body" : "timed out waiting for response";
    if (!fail.empty()) {
      [task cancel];
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_opsMutex);
    g_ops.erase(task.taskIdentifier);
  }
  std::string error;
  {
    std::lock_guard<std::mutex> lock(op->m);
    error = fail.empty() ? op->error : fail;
  }
  if (!error.empty()) {
    res.status = 0;
    res.error = error;
    res.body.clear();
    if (!headDelivered) {
      res.statusText.clear();
      res.headers.clear();
    }
  }
  return res;
}

}  // namespace

HttpResponse fetchUrl(const HttpRequest& req, const std::atomic<bool>* cancel, const HttpStream* stream) {
  if (!isHttps(req.url)) return httpRequest(req, cancel, stream);
  @autoreleasepool {
    return fetchHttps(req, cancel, stream);
  }
}

}  // namespace jsui::net

#endif  // __APPLE__
