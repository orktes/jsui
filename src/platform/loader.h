// Asynchronous resource loading (bundle, fetch(), images, fonts).
//
// The built-in backend reads files and plain http:// URLs on a worker thread
// (native) or uses the browser's fetch (Emscripten). An embedder-supplied
// JsuiLoadFn replaces it. Completions are queued and delivered on the
// runtime thread from pump(), called by jsui_tick().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"

namespace jsui {

struct Resource {
  int status = 0;  // HTTP-like: 200 ok, 404 missing, 0 network error
  std::string contentType;
  std::string url;
  std::vector<uint8_t> data;
  bool ok() const { return status >= 200 && status < 300; }
  std::string text() const { return std::string(data.begin(), data.end()); }
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
  // Called by the embedder (jsui_resource_complete) or the built-in backend.
  void complete(uint64_t id, Resource res);
  // Delivers finished loads. Returns the number of callbacks run.
  int pump();
  // Drops callbacks of in-flight requests (on reload / destroy).
  void cancelAll();
  bool busy() const;

 private:
  void builtinLoad(uint64_t id, const std::string& url);

  JsuiRuntime* rt_;
  JsuiLoadFn custom_;
  void* customUser_;
  uint64_t nextId_ = 1;
  std::unordered_map<uint64_t, Callback> pending_;
  mutable std::mutex mutex_;
  std::vector<std::pair<uint64_t, Resource>> done_;
  struct Worker;
  std::unique_ptr<Worker> worker_;
};

}  // namespace jsui
