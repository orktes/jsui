// Browser fetch() backend for ResourceLoader. Compiled to nothing natively.
//
// Lifetime: JS never holds a ResourceLoader pointer. Each loader gets a small
// integer token on first use; completions look the token up and are dropped
// if the loader is gone (its fetches are also aborted), so a runtime can be
// destroyed or reloaded while requests are in flight.
//
// Copies: the response body is copied once, from the fetch ArrayBuffer
// straight into the Resource's vector (two-phase begin/end), not via a
// temporary malloc buffer.
//
// Headers cross the boundary as "name: value\n" lines (header values cannot
// contain newlines), which keeps the glue free of JSON.
#ifdef __EMSCRIPTEN__

#include "platform/loader_emscripten.h"

#include <emscripten/emscripten.h>

#include <unordered_map>

#include "platform/loader.h"

namespace jsui {

namespace {

struct Registry {
  uint32_t nextToken = 1;
  std::unordered_map<uint32_t, ResourceLoader*> loaders;
  std::unordered_map<ResourceLoader*, uint32_t> tokens;
  // Requests in flight (url known; status/body filled by fetch_begin), keyed by (token, id).
  struct Key {
    uint32_t token;
    uint64_t id;
    bool operator==(const Key& o) const { return token == o.token && id == o.id; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<uint64_t>()(k.id * 1000003u ^ k.token); }
  };
  std::unordered_map<Key, Resource, KeyHash> inflight;
};

Registry& registry() {
  static Registry r;  // single-threaded: the browser main thread (or one worker)
  return r;
}

uint32_t tokenFor(ResourceLoader* loader) {
  auto& r = registry();
  auto it = r.tokens.find(loader);
  if (it != r.tokens.end()) return it->second;
  uint32_t t = r.nextToken++;
  r.tokens[loader] = t;
  r.loaders[t] = loader;
  return t;
}

std::string encodeHeaders(const HeaderList& headers) {
  std::string out;
  for (auto& [k, v] : headers) {
    if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
    out += k + ": " + v + "\n";
  }
  return out;
}

HeaderList decodeHeaders(const char* s) {
  HeaderList out;
  if (!s) return out;
  std::string all(s);
  size_t i = 0;
  while (i < all.size()) {
    size_t nl = all.find('\n', i);
    if (nl == std::string::npos) nl = all.size();
    size_t colon = all.find(':', i);
    if (colon != std::string::npos && colon < nl) {
      size_t v = colon + 1;
      while (v < nl && all[v] == ' ') v++;
      out.emplace_back(all.substr(i, colon - i), all.substr(v, nl - v));
    }
    i = nl + 1;
  }
  return out;
}

}  // namespace

// The request id travels as a double: ids are sequential and stay far below
// 2^53, and this keeps the JS side free of BigInt.
EM_JS(void, jsui_js_fetch_start,
      (uint32_t token, double id, const char* urlPtr, const char* methodPtr, const char* headersPtr,
       const uint8_t* bodyPtr, size_t bodySize, int timeoutMs),
      {
  var url = UTF8ToString(urlPtr);
  var method = UTF8ToString(methodPtr) || "GET";
  var headers = [];
  UTF8ToString(headersPtr).split("\n").forEach(function(line) {
    var c = line.indexOf(":");
    if (c > 0) headers.push([line.slice(0, c), line.slice(c + 1).trim()]);
  });
  var body = bodySize ? HEAPU8.slice(bodyPtr, bodyPtr + bodySize) : undefined;  // copy: caller frees
  var all = Module.__jsuiFetches || (Module.__jsuiFetches = new Map());
  var set = all.get(token);
  if (!set) { set = new Set(); all.set(token, set); }
  var ctl = typeof AbortController !== "undefined" ? new AbortController() : null;
  var entry = { ctl: ctl, aborted: false, timedOut: false, timer: 0 };
  set.add(entry);
  function str(s) {
    var n = lengthBytesUTF8(s) + 1;
    var p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
  }
  function finish(status, statusText, finalUrl, headerText, contentType, bytes, error) {
    set.delete(entry);
    if (entry.timer) clearTimeout(entry.timer);
    if (entry.aborted) return;
    var n = bytes ? bytes.length : 0;
    var ps = [str(statusText), str(finalUrl), str(headerText), str(contentType), str(error)];
    var dst = _jsui_emscripten_fetch_begin(token, id, status, ps[0], ps[1], ps[2], ps[3], ps[4], n);
    ps.forEach(function(p) { _free(p); });
    if (dst && n) HEAPU8.set(bytes, dst);  // HEAPU8 re-read after possible growth in begin()
    _jsui_emscripten_fetch_end(token, id);
  }
  var init = { method: method, headers: headers, credentials: "same-origin" };
  if (body && method !== "GET" && method !== "HEAD") init.body = body;
  if (ctl) {
    init.signal = ctl.signal;
    if (timeoutMs > 0) entry.timer = setTimeout(function() { entry.timedOut = true; ctl.abort(); }, timeoutMs);
  }
  var p;
  try { p = fetch(url, init); } catch (e) { p = Promise.reject(e); }
  p.then(function(res) {
    return res.arrayBuffer().then(function(buf) {
      var h = "";
      res.headers.forEach(function(v, k) { h += k + ": " + v + "\n"; });
      finish(res.status, res.statusText || "", res.url || url, h, res.headers.get("content-type") || "",
             new Uint8Array(buf), "");
    });
  }).catch(function(err) {
    if (entry.aborted) { set.delete(entry); return; }
    var msg = entry.timedOut ? "timeout after " + timeoutMs + " ms" : String(err && err.message || err);
    finish(0, "", url, "", "", null, "fetch " + url + " failed: " + msg);
  });
});

EM_JS(void, jsui_js_fetch_abort_all, (uint32_t token), {
  var all = Module.__jsuiFetches;
  if (!all) return;
  var set = all.get(token);
  if (!set) return;
  set.forEach(function(e) { e.aborted = true; if (e.timer) clearTimeout(e.timer); if (e.ctl) e.ctl.abort(); });
  all.delete(token);
});

bool emscriptenLoad(ResourceLoader* loader, uint64_t id, const LoadRequest& req) {
  if (req.url.rfind("data:", 0) == 0) return false;  // decoded generically by ResourceLoader::load
  uint32_t t = tokenFor(loader);
  registry().inflight[{t, id}].url = req.url;
  std::string headers = encodeHeaders(req.headers);
  jsui_js_fetch_start(t, double(id), req.url.c_str(), req.method.c_str(), headers.c_str(), req.body.data(),
                      req.body.size(), req.timeoutMs);
  return true;
}

void emscriptenLoaderDestroyed(ResourceLoader* loader) {
  auto& r = registry();
  auto it = r.tokens.find(loader);
  if (it == r.tokens.end()) return;
  uint32_t t = it->second;
  jsui_js_fetch_abort_all(t);
  for (auto i = r.inflight.begin(); i != r.inflight.end();) {
    if (i->first.token == t) i = r.inflight.erase(i);
    else ++i;
  }
  r.loaders.erase(t);
  r.tokens.erase(it);
}

}  // namespace jsui

extern "C" {

// Phase 1: fill the Resource's metadata and return where JS should write
// `size` body bytes (nullptr when the loader is gone or the body is empty).
EMSCRIPTEN_KEEPALIVE uint8_t* jsui_emscripten_fetch_begin(uint32_t token, double id, int status,
                                                          const char* statusText, const char* finalUrl,
                                                          const char* headers, const char* contentType,
                                                          const char* error, size_t size) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return nullptr;
  jsui::Resource& res = it->second;
  res.status = status;
  if (statusText) res.statusText = statusText;
  if (finalUrl && *finalUrl) res.url = finalUrl;
  res.headers = jsui::decodeHeaders(headers);
  if (contentType) res.contentType = contentType;
  if (error) res.error = error;
  res.data.resize(size);
  return size ? res.data.data() : nullptr;
}

// Phase 2: hand the filled Resource to the loader (delivered on next pump()).
EMSCRIPTEN_KEEPALIVE void jsui_emscripten_fetch_end(uint32_t token, double id) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return;  // loader destroyed meanwhile
  jsui::Resource res = std::move(it->second);
  r.inflight.erase(it);
  auto l = r.loaders.find(token);
  if (l != r.loaders.end()) l->second->complete(uint64_t(id), std::move(res));
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
