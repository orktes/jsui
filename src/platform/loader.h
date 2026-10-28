// Asynchronous resource loading (bundle, fetch(), images, fonts).
//
// Order per load: data: URIs are decoded inline on every platform; a
// container child forwards to its parent (`forward`); the host's
// JsuiRequestFn takes the request or declines it; then the host's JsuiLoadFn
// (URL only), else the built-in backend: files and http(s):// on a small
// worker pool (native, net/http_client.h) or the browser's fetch
// (Emscripten).
//
// Answers are events: head (status, headers), body pieces, end. A plain
// load() gets them gathered into one Resource; loadStream() gets each as it
// comes (fetch()'s res.body). Producers (workers, the host's network
// thread) queue them from any thread; pump(), called by jsui_tick(),
// delivers them on the runtime thread in order.
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
  int timeoutMs = 0;  // whole request; 0: none (http: 30 s to the headers, 30 s between body pieces)
  int redirect = JSUI_REDIRECT_FOLLOW;  // JsuiRedirectMode
  bool stream = false;  // set by loadStream()
  // Not counted by pending() / busy() (jsui_pending_loads): a long poll the
  // runtime keeps open in the background (dev session).
  bool background = false;
  // What asked for it, for the debugger's Network panel: "fetch", "image",
  // "font", "script", ... (empty: guessed from the response).
  std::string kind;
};

// One step of a streamed answer (loadStream()).
struct LoadEvent {
  enum Kind { Head, Data, End } kind;
  // Head: status, statusText, url, headers, contentType, error (status 0:
  // failed; no Data / End follow). Data: a body piece in `data`. End: `error`
  // empty when the body is complete, else why it failed part way.
  Resource res;
};

// Sees every load as it happens (the JS debugger's Network panel): a
// request starting, and each answer in the order pump() delivers them.
// Called on the thread that calls load() / pump().
struct LoadObserver {
  virtual ~LoadObserver() = default;
  virtual void loadStarted(uint64_t id, const LoadRequest& req) = 0;
  // kind: a LoadEvent::Kind, or 3 for a whole answer at once (complete()).
  virtual void loadAnswered(uint64_t id, int kind, const Resource& res) = 0;
};

class ResourceLoader {
 public:
  using Callback = std::function<void(Resource)>;
  using StreamCallback = std::function<void(LoadEvent&)>;

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
  // A load whose answer arrives in pieces (Head, Data..., End).
  void loadStream(uint64_t id, LoadRequest req, StreamCallback cb);
  // The host's JsuiRequestFn (JsuiConfig.on_request).
  void setHostFetch(JsuiRequestFn fetch, JsuiRequestCancelFn cancel, void* user) {
    hostFetch_ = fetch;
    hostCancel_ = cancel;
    hostUser_ = user;
  }

  // Answers, from any thread. complete() is the whole answer at once (the
  // embedder's jsui_resource_complete, files, buffered http); head / data /
  // end stream it (jsui_request_*, streamed http).
  void complete(uint64_t id, Resource res);
  void head(uint64_t id, Resource meta);
  void data(uint64_t id, std::vector<uint8_t> bytes);
  void end(uint64_t id, std::string error);
  // Delivers finished loads. Returns the number of callbacks run.
  int pump();
  // Drops the callback of one request and aborts it if it is in flight.
  void cancel(uint64_t id);
  // Drops callbacks of in-flight requests (on reload / destroy).
  void cancelAll();
  // Set while a debugger shows the Network panel; null: none.
  std::atomic<LoadObserver*> observer{nullptr};
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
  void start(uint64_t id, LoadRequest req);
  void builtinLoad(uint64_t id, LoadRequest req);
  bool deliver(uint64_t id, int kind, Resource& res);

  JsuiRuntime* rt_;
  JsuiLoadFn custom_;
  void* customUser_;
  JsuiRequestFn hostFetch_ = nullptr;
  JsuiRequestCancelFn hostCancel_ = nullptr;
  void* hostUser_ = nullptr;
  std::atomic<uint64_t> nextId_{1};
  struct PendingLoad {
    Callback cb;                           // gathered answer, or
    std::shared_ptr<StreamCallback> scb;   // streamed (shared: it may cancel itself)
    std::string url;
    bool background = false;
    bool host = false;      // with the host's JsuiRequestFn (cancel tells it)
    bool headSeen = false;
    Resource acc;           // gathered answer so far
  };
  std::unordered_map<uint64_t, PendingLoad> pending_;
  size_t background_ = 0;  // background entries of pending_
  void erasePending(std::unordered_map<uint64_t, PendingLoad>::iterator it);
  mutable std::mutex mutex_;
  // Queued answers: kind is a LoadEvent::Kind, or kWhole (complete()).
  static constexpr int kWhole = 3;
  struct Answer {
    uint64_t id;
    int kind;
    Resource res;
  };
  std::vector<Answer> done_, doneScratch_;
  struct Worker;
  std::unique_ptr<Worker> worker_;
};

}  // namespace jsui
