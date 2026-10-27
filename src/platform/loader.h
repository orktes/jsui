// Asynchronous resource loading (bundle, fetch(), images, fonts).
//
// The built-in backend reads files and plain http:// URLs on a small worker
// pool (native) or uses the browser's fetch (Emscripten). data: URIs are
// decoded inline on every platform. An embedder-supplied JsuiLoadFn replaces
// the file/http backends (it only sees the URL; method/body are http-only).
// Completions are queued and delivered on the runtime thread from pump(),
// called by jsui_tick().
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jsui/jsui.h"

namespace jsui {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct Resource {
  int status = 0;  // HTTP-like: 200 ok, 404 missing, 0 network error
  std::string contentType;
  std::string url;  // final URL (after redirects)
  std::vector<uint8_t> data;
  // Additive fields (fetch): response headers, status text, error for status 0.
  std::string statusText;
  HeaderList headers;
  std::string error;
  bool ok() const { return status >= 200 && status < 300; }
  std::string text() const { return std::string(data.begin(), data.end()); }
};

// A load with HTTP options. Files and data: URIs ignore method/headers/body.
struct LoadRequest {
  std::string url;
  std::string method = "GET";
  HeaderList headers;
  std::vector<uint8_t> body;
  int timeoutMs = 0;  // 0: default (30 s for http)
  // Not counted by pending() / busy() (jsui_pending_loads): a long poll the
  // runtime keeps open in the background (dev session).
  bool background = false;
};

class ResourceLoader {
 public:
  using Callback = std::function<void(Resource)>;

  ResourceLoader(JsuiRuntime* rt, JsuiLoadFn custom, void* customUser);
  ~ResourceLoader();

  // Resolves `url` against `base` (RFC 3986 subset: absolute URLs, //host,
  // /path, relative paths with ./ and ../; plain file paths).
  static std::string resolve(const std::string& base, const std::string& url);

  // Starts a load; `cb` runs on the runtime thread during pump().
  uint64_t load(const std::string& url, Callback cb);
  uint64_t load(LoadRequest req, Callback cb);
  // An id for a load started later with it (any thread: JS on its own
  // thread names its request before the owner thread starts it).
  uint64_t reserveId() { return nextId_.fetch_add(1); }
  // load() under a reserveId() id.
  void load(uint64_t id, LoadRequest req, Callback cb);
  // Called by the embedder (jsui_resource_complete) or the built-in backend.
  // Thread-safe.
  void complete(uint64_t id, Resource res);
  // Delivers finished loads. Returns the number of callbacks run.
  int pump();
  // Drops the callback of one request and aborts it if it is in flight.
  void cancel(uint64_t id);
  // Drops callbacks of in-flight requests (on reload / destroy).
  void cancelAll();
  bool busy() const;
  size_t pending() const { return pending_.size() - background_; }

  // Takes every load but data: URIs (before the embedder's loader and the
  // built-in one), with the whole request: a JSUIContainer's child loads
  // through its parent's loader this way (render/container_view.h). The
  // load is answered with complete(id, ...).
  std::function<void(uint64_t id, LoadRequest req)> forward;

  // Decodes a data: URI (base64 or percent-encoded). Exposed for tests.
  static bool decodeDataUri(const std::string& uri, Resource* out);

 private:
  void builtinLoad(uint64_t id, LoadRequest req);

  JsuiRuntime* rt_;
  JsuiLoadFn custom_;
  void* customUser_;
  std::atomic<uint64_t> nextId_{1};
  struct PendingLoad {
    Callback cb;
    std::string url;
    bool background = false;
  };
  std::unordered_map<uint64_t, PendingLoad> pending_;
  size_t background_ = 0;  // background entries of pending_
  void erasePending(std::unordered_map<uint64_t, PendingLoad>::iterator it);
  mutable std::mutex mutex_;
  std::vector<std::pair<uint64_t, Resource>> done_, doneScratch_;
  struct Worker;
  std::unique_ptr<Worker> worker_;
};

}  // namespace jsui
