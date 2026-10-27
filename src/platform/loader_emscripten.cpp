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
       const uint8_t* bodyPtr, size_t bodySize, int timeoutMs, int redirect, int stream),
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
  var map = all.get(token);
  if (!map) { map = new Map(); all.set(token, map); }
  var ctl = typeof AbortController !== "undefined" ? new AbortController() : null;
  var entry = { ctl: ctl, aborted: false, timedOut: false, timer: 0, headSent: false };
  map.set(id, entry);
  function str(s) {
    var n = lengthBytesUTF8(s) + 1;
    var p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
  }
  function done() {
    map.delete(id);
    if (entry.timer) clearTimeout(entry.timer);
  }
  function headerText(res) {
    var h = "";
    res.headers.forEach(function(v, k) { h += k + ": " + v + "\n"; });
    return h;
  }
  function finish(status, statusText, finalUrl, headerText, contentType, bytes, error) {
    done();
    if (entry.aborted) return;
    var n = bytes ? bytes.length : 0;
    var ps = [str(statusText), str(finalUrl), str(headerText), str(contentType), str(error)];
    var dst = _jsui_emscripten_fetch_begin(token, id, status, ps[0], ps[1], ps[2], ps[3], ps[4], n);
    ps.forEach(function(p) { _free(p); });
    if (dst && n) HEAPU8.set(bytes, dst);  // HEAPU8 re-read after possible growth in begin()
    _jsui_emscripten_fetch_end(token, id);
  }
  function streamEnd(error) {
    done();
    if (entry.aborted) return;
    var p = str(error);
    _jsui_emscripten_stream_end(token, id, p);
    _free(p);
  }
  function failMessage(err) {
    return entry.timedOut ? "timeout after " + timeoutMs + " ms" : String(err && err.message || err);
  }
  var init = { method: method, headers: headers, credentials: "same-origin",
               redirect: redirect == 1 ? "error" : redirect == 2 ? "manual" : "follow" };
  if (body && method !== "GET" && method !== "HEAD") init.body = body;
  if (ctl) {
    init.signal = ctl.signal;
    if (timeoutMs > 0) entry.timer = setTimeout(function() { entry.timedOut = true; ctl.abort(); }, timeoutMs);
  }
  var p;
  try { p = fetch(url, init); } catch (e) { p = Promise.reject(e); }
  p.then(function(res) {
    if (!stream || !res.body || !res.body.getReader) {
      return res.arrayBuffer().then(function(buf) {
        finish(res.status, res.statusText || "", res.url || url, headerText(res),
               res.headers.get("content-type") || "", new Uint8Array(buf), "");
      });
    }
    if (entry.aborted) return;
    var ps = [str(res.statusText || ""), str(res.url || url), str(headerText(res)),
              str(res.headers.get("content-type") || "")];
    _jsui_emscripten_stream_head(token, id, res.status, ps[0], ps[1], ps[2], ps[3]);
    ps.forEach(function(p) { _free(p); });
    entry.headSent = true;
    var reader = res.body.getReader();
    function next() {
      return reader.read().then(function(r) {
        if (entry.aborted) { reader.cancel().catch(function() {}); return; }
        if (r.done) { streamEnd(""); return; }
        var n = r.value.length;
        if (n) {
          var dst = _jsui_emscripten_stream_alloc(token, id, n);
          if (dst) HEAPU8.set(r.value, dst);
          _jsui_emscripten_stream_push(token, id);
        }
        return next();
      });
    }
    return next();
  }).catch(function(err) {
    if (entry.aborted) { done(); return; }
    var msg = "fetch " + url + " failed: " + failMessage(err);
    if (entry.headSent) streamEnd(msg);
    else finish(0, "", url, "", "", null, msg);
  });
});

EM_JS(void, jsui_js_fetch_abort, (uint32_t token, double id), {
  var all = Module.__jsuiFetches;
  var map = all && all.get(token);
  var e = map && map.get(id);
  if (!e) return;
  e.aborted = true;
  if (e.timer) clearTimeout(e.timer);
  if (e.ctl) e.ctl.abort();
  map.delete(id);
});

EM_JS(void, jsui_js_fetch_abort_all, (uint32_t token), {
  var all = Module.__jsuiFetches;
  if (!all) return;
  var map = all.get(token);
  if (!map) return;
  map.forEach(function(e) { e.aborted = true; if (e.timer) clearTimeout(e.timer); if (e.ctl) e.ctl.abort(); });
  all.delete(token);
});

bool emscriptenLoad(ResourceLoader* loader, uint64_t id, const LoadRequest& req) {
  if (req.url.rfind("data:", 0) == 0) return false;  // decoded generically by ResourceLoader::load
  uint32_t t = tokenFor(loader);
  registry().inflight[{t, id}].url = req.url;
  std::string headers = encodeHeaders(req.headers);
  jsui_js_fetch_start(t, double(id), req.url.c_str(), req.method.c_str(), headers.c_str(), req.body.data(),
                      req.body.size(), req.timeoutMs, req.redirect, req.stream ? 1 : 0);
  return true;
}

void emscriptenCancel(ResourceLoader* loader, uint64_t id) {
  auto& r = registry();
  auto it = r.tokens.find(loader);
  if (it == r.tokens.end()) return;
  if (r.inflight.erase({it->second, id})) jsui_js_fetch_abort(it->second, double(id));
}

void emscriptenCancelAll(ResourceLoader* loader) {
  auto& r = registry();
  auto it = r.tokens.find(loader);
  if (it == r.tokens.end()) return;
  uint32_t t = it->second;
  jsui_js_fetch_abort_all(t);
  for (auto i = r.inflight.begin(); i != r.inflight.end();) {
    if (i->first.token == t) i = r.inflight.erase(i);
    else ++i;
  }
}

void emscriptenLoaderDestroyed(ResourceLoader* loader) {
  auto& r = registry();
  auto it = r.tokens.find(loader);
  if (it == r.tokens.end()) return;
  uint32_t t = it->second;
  emscriptenCancelAll(loader);
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

// Streamed responses (fetch()'s res.body): the head, then each piece copied
// straight into a vector (alloc + push), then the end.
EMSCRIPTEN_KEEPALIVE void jsui_emscripten_stream_head(uint32_t token, double id, int status, const char* statusText,
                                                      const char* finalUrl, const char* headers,
                                                      const char* contentType) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return;
  jsui::Resource meta;
  meta.status = status;
  if (statusText) meta.statusText = statusText;
  meta.url = finalUrl && *finalUrl ? finalUrl : it->second.url;
  meta.headers = jsui::decodeHeaders(headers);
  if (contentType) meta.contentType = contentType;
  auto l = r.loaders.find(token);
  if (l != r.loaders.end()) l->second->head(uint64_t(id), std::move(meta));
}

EMSCRIPTEN_KEEPALIVE uint8_t* jsui_emscripten_stream_alloc(uint32_t token, double id, size_t size) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return nullptr;
  it->second.data.resize(size);
  return size ? it->second.data.data() : nullptr;
}

EMSCRIPTEN_KEEPALIVE void jsui_emscripten_stream_push(uint32_t token, double id) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return;
  auto l = r.loaders.find(token);
  if (l != r.loaders.end()) l->second->data(uint64_t(id), std::move(it->second.data));
  it->second.data = {};
}

EMSCRIPTEN_KEEPALIVE void jsui_emscripten_stream_end(uint32_t token, double id, const char* error) {
  auto& r = jsui::registry();
  auto it = r.inflight.find({token, uint64_t(id)});
  if (it == r.inflight.end()) return;
  r.inflight.erase(it);
  auto l = r.loaders.find(token);
  if (l != r.loaders.end()) l->second->end(uint64_t(id), error ? error : "");
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
