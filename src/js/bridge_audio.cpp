// __jsui_native members for UI audio (jsui.h "Audio"; packages/react/src/sound.js):
//
//   audioPlay(url, bus, volume, pitch, pan, loop, fadeMs, offsetS) -> voice id
//   audioVoice(op, voice, volume, pitch, pan, fadeMs)   op: JSUI_AUDIO_STOP / PAUSE / RESUME / SET
//   audioBus(bus, volume, muted, fadeMs)                audioStopAll(bus | null, fadeMs)
//   audioPreload(url) -> absolute url                   audioUnload(url)
//   audioResolve(url) -> absolute url
// Reports arrive in globalThis.__jsui_onAudio([[0, voice, status] | [1, url, ok, duration], ...]).
#include <string>

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

std::string str(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  return i < argc && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i]) ? toStdString(ctx, argv[i]) : std::string();
}

std::string resolved(JSContext* ctx, const std::string& url) {
  return ResourceLoader::resolve(runtimeOf(ctx).baseUrl, url);
}

JSValue js_audioPlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  std::string url = str(ctx, argc, argv, 0);
  if (url.empty()) return JS_ThrowTypeError(ctx, "audioPlay: no sound");
  std::string bus = str(ctx, argc, argv, 1);
  if (bus.empty()) bus = "sfx";
  bool loop = argc > 5 && JS_ToBool(ctx, argv[5]);
  uint64_t id = rt.audio->play(resolved(ctx, url), bus, float(num(ctx, argc, argv, 2, 1)),
                               float(num(ctx, argc, argv, 3, 1)), float(num(ctx, argc, argv, 4, 0)), loop,
                               float(num(ctx, argc, argv, 6, 0)), num(ctx, argc, argv, 7, 0));
  return JS_NewFloat64(ctx, double(id));
}

JSValue js_audioVoice(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int op = int(num(ctx, argc, argv, 0, -1));
  double voice = num(ctx, argc, argv, 1, 0);
  if (voice < 1 || (op != JSUI_AUDIO_STOP && op != JSUI_AUDIO_PAUSE && op != JSUI_AUDIO_RESUME && op != JSUI_AUDIO_SET))
    return JS_UNDEFINED;
  runtimeOf(ctx).audio->voice(JsuiAudioOp(op), uint64_t(voice), float(num(ctx, argc, argv, 2, 1)),
                              float(num(ctx, argc, argv, 3, 1)), float(num(ctx, argc, argv, 4, 0)),
                              float(num(ctx, argc, argv, 5, 0)));
  return JS_UNDEFINED;
}

JSValue js_audioBus(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string bus = str(ctx, argc, argv, 0);
  if (bus.empty()) return JS_UNDEFINED;
  bool muted = argc > 2 && JS_ToBool(ctx, argv[2]);
  runtimeOf(ctx).audio->bus(bus, float(num(ctx, argc, argv, 1, 1)), muted, float(num(ctx, argc, argv, 3, 0)));
  return JS_UNDEFINED;
}

JSValue js_audioStopAll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  runtimeOf(ctx).audio->stopAll(str(ctx, argc, argv, 0), float(num(ctx, argc, argv, 1, 0)));
  return JS_UNDEFINED;
}

// magic 0: preload, 1: unload, 2: resolve only
JSValue js_audioSound(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
  std::string url = str(ctx, argc, argv, 0);
  if (url.empty()) return JS_UNDEFINED;
  std::string abs = resolved(ctx, url);
  if (magic == 0) runtimeOf(ctx).audio->preload(abs);
  else if (magic == 1) runtimeOf(ctx).audio->unload(abs);
  return JS_NewStringLen(ctx, abs.data(), abs.size());
}

}  // namespace

void installAudioBridge(JsEngine::Impl& impl, JSValue native) {
  JSContext* ctx = impl.ctx;
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"audioPlay", js_audioPlay, 8},
             {"audioVoice", js_audioVoice, 6},
             {"audioBus", js_audioBus, 4},
             {"audioStopAll", js_audioStopAll, 2}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  const char* magics[] = {"audioPreload", "audioUnload", "audioResolve"};
  for (int i = 0; i < 3; i++)
    JS_SetPropertyStr(ctx, native, magics[i],
                      JS_NewCFunctionMagic(ctx, js_audioSound, magics[i], 1, JS_CFUNC_generic_magic, i));
}

}  // namespace jsui
