// __jsui_native members for <Video> (jsui.h "Video"; packages/react/src/video.js):
//
//   videoOpen(urls, types, texture, paused, muted, volume, loop, rate, progressMs) -> video id
//     urls: the alternatives (array, or one string), types: their MIME types (or null)
//   videoSet(video, paused, muted, volume, loop, rate)
//   videoSeek(video, timeS)                 videoClose(video)
// Reports arrive in globalThis.__jsui_onVideo([[video, state, duration,
// position, buffered, width, height, error], ...]). Requests reach the
// embedder's on_video through Runtime::toHost: in the tick in JS-thread mode.
// Video ids are taken now.
#include <string>
#include <vector>

#include "js/engine_impl.h"
#include "platform/loader.h"
#include "runtime.h"

namespace jsui {

namespace {

double num(JSContext* ctx, int argc, JSValueConst* argv, int i, double fallback) {
  double d = fallback;
  if (i < argc && !JS_IsUndefined(argv[i]) && JS_ToFloat64(ctx, &d, argv[i]) < 0) return fallback;
  return d == d ? d : fallback;  // NaN -> fallback
}

bool flag(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  return i < argc && JS_ToBool(ctx, argv[i]);
}

std::string str(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  return i < argc && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i]) ? toStdString(ctx, argv[i]) : std::string();
}

// A string array (or one string; null / undefined entries: "").
std::vector<std::string> strings(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  std::vector<std::string> out;
  if (i >= argc || JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return out;
  if (!JS_IsArray(argv[i])) {
    out.push_back(toStdString(ctx, argv[i]));
    return out;
  }
  JSValue len = JS_GetPropertyStr(ctx, argv[i], "length");
  uint32_t n = 0;
  JS_ToUint32(ctx, &n, len);
  JS_FreeValue(ctx, len);
  for (uint32_t k = 0; k < n; k++) {
    JSValue v = JS_GetPropertyUint32(ctx, argv[i], k);
    out.push_back(JS_IsUndefined(v) || JS_IsNull(v) ? std::string() : toStdString(ctx, v));
    JS_FreeValue(ctx, v);
  }
  return out;
}

JSValue js_videoOpen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  std::vector<std::string> urls, types, given = strings(ctx, argc, argv, 0), mimes = strings(ctx, argc, argv, 1);
  for (size_t i = 0; i < given.size(); i++) {
    if (given[i].empty()) continue;
    urls.push_back(ResourceLoader::resolve(rt.baseUrl, given[i]));
    types.push_back(i < mimes.size() ? mimes[i] : std::string());
  }
  std::string texture = str(ctx, argc, argv, 2);
  if (urls.empty() || texture.empty()) return JS_ThrowTypeError(ctx, "videoOpen: no source or texture");
  uint64_t id = rt.video->reserve();
  Runtime* r = &rt;
  r->toHost([r, id, urls, types, texture, paused = flag(ctx, argc, argv, 3), muted = flag(ctx, argc, argv, 4),
             volume = float(num(ctx, argc, argv, 5, 1)), loop = flag(ctx, argc, argv, 6),
             rate = float(num(ctx, argc, argv, 7, 1)), progress = num(ctx, argc, argv, 8, 250)] {
    r->video->open(id, urls, types, texture, paused, muted, volume, loop, rate, progress);
  });
  return JS_NewFloat64(ctx, double(id));
}

JSValue js_videoSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* r = &runtimeOf(ctx);
  r->toHost([r, id = uint64_t(num(ctx, argc, argv, 0, 0)), paused = flag(ctx, argc, argv, 1),
             muted = flag(ctx, argc, argv, 2), volume = float(num(ctx, argc, argv, 3, 1)),
             loop = flag(ctx, argc, argv, 4), rate = float(num(ctx, argc, argv, 5, 1))] {
    r->video->set(id, paused, muted, volume, loop, rate);
  });
  return JS_UNDEFINED;
}

JSValue js_videoSeek(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* r = &runtimeOf(ctx);
  r->toHost([r, id = uint64_t(num(ctx, argc, argv, 0, 0)), t = num(ctx, argc, argv, 1, 0)] { r->video->seek(id, t); });
  return JS_UNDEFINED;
}

JSValue js_videoClose(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime* r = &runtimeOf(ctx);
  r->toHost([r, id = uint64_t(num(ctx, argc, argv, 0, 0))] { r->video->close(id); });
  return JS_UNDEFINED;
}

}  // namespace

void installVideoBridge(JsEngine::Impl& impl, JSValue native) {
  JSContext* ctx = impl.ctx;
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"videoOpen", js_videoOpen, 9},
             {"videoSet", js_videoSet, 6},
             {"videoSeek", js_videoSeek, 2},
             {"videoClose", js_videoClose, 1}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
}

}  // namespace jsui
