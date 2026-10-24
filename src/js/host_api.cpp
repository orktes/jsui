// __jsui_host: native helpers behind the JS prelude (console sink, error
// reporting, UTF-8 codecs, fetch transport). Not part of the public bridge;
// the prelude captures it and removes the global.
#include <cstring>

#include "js/engine_impl.h"
#include "js/inspector.h"
#include "runtime.h"

namespace jsui {

namespace {

JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int level = JSUI_LOG_INFO;
  if (argc > 0) JS_ToInt32(ctx, &level, argv[0]);
  std::string msg = argc > 1 ? toStdString(ctx, argv[1]) : "";
  runtimeOf(ctx).log(JsuiLogLevel(level & 3), msg);
  return JS_UNDEFINED;
}

// consoleApi(type, args): console.* with its arguments, for the debugger
// (Runtime.consoleAPICalled: DevTools shows them as live objects).
JSValue js_consoleApi(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if JSUI_DEBUGGER
  JsEngine::Impl& impl = implOf(ctx);
  if (!impl.inspector || argc < 2) return JS_UNDEFINED;
  std::string type = toStdString(ctx, argv[0]);
  int64_t n = 0;
  JS_GetLength(ctx, argv[1], &n);
  std::vector<JSValue> args;
  for (int64_t i = 0; i < n && i < 64; i++) args.push_back(JS_GetPropertyInt64(ctx, argv[1], i));
  impl.inspector->console(type.c_str(), args.data(), int(args.size()));
  for (JSValue v : args) JS_FreeValue(ctx, v);
#else
  (void)ctx, (void)argc, (void)argv;
#endif
  return JS_UNDEFINED;
}

JSValue js_reportError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string prefix = argc > 1 && JS_IsString(argv[1]) ? toStdString(ctx, argv[1]) : "";
#if JSUI_DEBUGGER
  if (JsEngine::Impl& impl = implOf(ctx); impl.inspector && argc > 0) impl.inspector->exception(argv[0], prefix + "Uncaught");
#endif
  runtimeOf(ctx).reportError(prefix + (argc > 0 ? formatError(ctx, argv[0]) : std::string("undefined")));
  return JS_UNDEFINED;
}

// Backing store of a fixed-length ArrayBuffer that owns a std::vector.
void* vectorBuffer(JSRuntime*, void* opaque, void*, size_t size) {
  if (size == 0) delete static_cast<std::vector<uint8_t>*>(opaque);
  return nullptr;  // never resized
}

// Takes ownership of `bytes` without copying.
JSValue newArrayBuffer(JSContext* ctx, std::vector<uint8_t>&& bytes) {
  if (bytes.empty()) return JS_NewArrayBufferCopy(ctx, nullptr, 0);
  auto* v = new std::vector<uint8_t>(std::move(bytes));
  return JS_NewArrayBuffer(ctx, v->data(), v->size(), 0, vectorBuffer, v, false);
}

// utf8Encode(string) -> Uint8Array. QuickJS encodes lone surrogates as
// 3-byte sequences (WTF-8); the Encoding spec wants U+FFFD, same length.
JSValue js_utf8Encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
  if (!s) return JS_EXCEPTION;
  JSValue arr = JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(s), len);
  JS_FreeCString(ctx, s);
  if (JS_IsException(arr)) return arr;
  size_t size = 0;
  uint8_t* p = JS_GetUint8Array(ctx, &size, arr);
  for (size_t i = 0; p && i + 2 < size; i++) {
    if (p[i] == 0xED && p[i + 1] >= 0xA0 && p[i + 1] <= 0xBF) {
      p[i] = 0xEF;
      p[i + 1] = 0xBF;
      p[i + 2] = 0xBD;
      i += 2;
    }
  }
  return arr;
}

// Validating UTF-8 decode with U+FFFD replacement (WHATWG "maximal subpart").
// Returns false on the first error when `fatal`.
bool decodeUtf8(const uint8_t* p, size_t n, bool fatal, std::string* out) {
  out->clear();
  out->reserve(n);
  static const char kRepl[] = "\xEF\xBF\xBD";
  size_t i = 0;
  while (i < n) {
    uint8_t c = p[i];
    if (c < 0x80) {
      out->push_back(char(c));
      i++;
      continue;
    }
    int need;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      if (c == 0xE0) lo = 0xA0;
      if (c == 0xED) hi = 0x9F;  // no surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      if (c == 0xF0) lo = 0x90;
      if (c == 0xF4) hi = 0x8F;
    } else {
      if (fatal) return false;
      out->append(kRepl, 3);
      i++;
      continue;
    }
    size_t j = i + 1;
    int got = 0;
    while (got < need && j < n) {
      uint8_t b = p[j];
      if (b < lo || b > hi) break;
      lo = 0x80;
      hi = 0xBF;
      j++;
      got++;
    }
    if (got < need) {
      if (fatal) return false;
      out->append(kRepl, 3);
      i = j;
      continue;
    }
    out->append(reinterpret_cast<const char*>(p + i), j - i);
    i = j;
  }
  return true;
}

// utf8Decode(bytes, fatal, ignoreBOM) -> string
JSValue js_utf8Decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::vector<uint8_t> bytes;
  if (argc > 0 && !bytesOf(ctx, argv[0], &bytes)) return JS_ThrowTypeError(ctx, "TextDecoder.decode: expected a BufferSource");
  bool fatal = argc > 1 && JS_ToBool(ctx, argv[1]);
  bool ignoreBOM = argc > 2 && JS_ToBool(ctx, argv[2]);
  size_t start = 0;
  if (!ignoreBOM && bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;
  std::string out;
  if (!decodeUtf8(bytes.data() + start, bytes.size() - start, fatal, &out))
    return JS_ThrowTypeError(ctx, "The encoded data was not valid utf-8");
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue js_resolveUrl(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  std::string url = argc > 0 ? toStdString(ctx, argv[0]) : "";
  std::string base = argc > 1 && JS_IsString(argv[1]) ? toStdString(ctx, argv[1]) : rt.baseUrl;
  std::string r = ResourceLoader::resolve(base, url);
  return JS_NewStringLen(ctx, r.data(), r.size());
}

JSValue js_baseUrl(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  const std::string& b = runtimeOf(ctx).baseUrl;
  return JS_NewStringLen(ctx, b.data(), b.size());
}

// fetch(url, method, headers[[k, v]...], body, timeoutMs, cb) -> request id
// cb(status, statusText, url, headers[[k, v]...], ArrayBuffer, error)
JSValue js_fetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto& impl = implOf(ctx);
  if (argc < 6 || !JS_IsFunction(ctx, argv[5])) return JS_ThrowTypeError(ctx, "host.fetch: bad arguments");
  LoadRequest req;
  req.url = toStdString(ctx, argv[0]);  // already resolved by the prelude (Request)
  req.method = toStdString(ctx, argv[1]);
  if (req.method.empty()) req.method = "GET";
  int64_t n = 0;
  if (JS_IsArray(argv[2]) && JS_GetLength(ctx, argv[2], &n) == 0) {
    for (int64_t i = 0; i < n; i++) {
      JSValue pair = JS_GetPropertyInt64(ctx, argv[2], i);
      JSValue k = JS_GetPropertyInt64(ctx, pair, 0), v = JS_GetPropertyInt64(ctx, pair, 1);
      req.headers.emplace_back(toStdString(ctx, k), toStdString(ctx, v));
      JS_FreeValue(ctx, k);
      JS_FreeValue(ctx, v);
      JS_FreeValue(ctx, pair);
    }
  }
  if (!bytesOf(ctx, argv[3], &req.body)) return JS_ThrowTypeError(ctx, "fetch: unsupported body type");
  int timeout = 0;
  if (JS_IsNumber(argv[4])) JS_ToInt32(ctx, &timeout, argv[4]);
  req.timeoutMs = timeout;
  JsEngine::Impl* ip = &impl;
  uint64_t id = impl.startLoad(std::move(req), [ip](uint64_t id, Resource& res) {
    auto it = ip->pending.find(id);
    if (it == ip->pending.end()) return;
    JSContext* c = ip->ctx;
    JSValue cb = it->second.resolve;
    it->second.resolve = JS_UNDEFINED;
    ip->dropPending(id);
    JSValue headers = JS_NewArray(c);
    uint32_t i = 0;
    for (auto& [k, v] : res.headers) {
      JSValue pair = JS_NewArray(c);
      JS_SetPropertyUint32(c, pair, 0, JS_NewStringLen(c, k.data(), k.size()));
      JS_SetPropertyUint32(c, pair, 1, JS_NewStringLen(c, v.data(), v.size()));
      JS_SetPropertyUint32(c, headers, i++, pair);
    }
    if (res.headers.empty() && !res.contentType.empty()) {  // files, data:, embedder loads
      JSValue pair = JS_NewArray(c);
      JS_SetPropertyUint32(c, pair, 0, JS_NewString(c, "content-type"));
      JS_SetPropertyUint32(c, pair, 1, JS_NewStringLen(c, res.contentType.data(), res.contentType.size()));
      JS_SetPropertyUint32(c, headers, 0, pair);
    }
    std::string statusText = res.statusText;
    if (statusText.empty() && res.ok()) statusText = "OK";
    JSValue args[6] = {JS_NewInt32(c, res.status),
                       JS_NewStringLen(c, statusText.data(), statusText.size()),
                       JS_NewStringLen(c, res.url.data(), res.url.size()),
                       headers,
                       newArrayBuffer(c, std::move(res.data)),
                       JS_NewStringLen(c, res.error.data(), res.error.size())};
    JS_FreeValue(c, ip->call(cb, JS_UNDEFINED, 6, args));
    for (auto& a : args) JS_FreeValue(c, a);
    JS_FreeValue(c, cb);
  });
  impl.pending[id] = {JS_DupValue(ctx, argv[5]), JS_UNDEFINED};
  return JS_NewFloat64(ctx, double(id));
}

JSValue js_cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  double d = 0;
  if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) || d <= 0) return JS_UNDEFINED;
  auto& impl = implOf(ctx);
  uint64_t id = uint64_t(d);
  impl.rt.loader.cancel(id);
  impl.dropPending(id);
  return JS_UNDEFINED;
}

}  // namespace

void installHostApi(JsEngine::Impl& impl, JSValue global, JSValue host) {
  JSContext* ctx = impl.ctx;
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"log", js_log, 2},           {"reportError", js_reportError, 2}, {"utf8Encode", js_utf8Encode, 1},
             {"utf8Decode", js_utf8Decode, 3}, {"resolveUrl", js_resolveUrl, 2}, {"baseUrl", js_baseUrl, 0},
             {"fetch", js_fetch, 6},       {"cancel", js_cancel, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, host, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, host, "devMode", JS_NewBool(ctx, impl.rt.config.dev_mode != 0));
#if JSUI_DEBUGGER
  if (impl.rt.config.dev_mode) JS_SetPropertyStr(ctx, host, "consoleApi", JS_NewCFunction(ctx, js_consoleApi, "consoleApi", 2));
#endif
  (void)global;
}

}  // namespace jsui
