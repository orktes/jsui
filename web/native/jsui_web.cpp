// Browser glue linked into jsui.wasm (not part of the jsui library).
//
// JsuiConfig holds C function pointers, which JS cannot easily provide, so
// jsui_web_create() fills them with trampolines that forward to the JS host
// object registered under `hostId` (see web/jsui-web.js). Everything else
// the web host needs is the plain C API from include/jsui/jsui.h.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "jsui/jsui.h"
#include "js/engine.h"  // JsEngine::nextKind

#if defined(JSUI_WEBGPU)
#include <webgpu/webgpu.h>
#endif

EM_JS(void, jsui_web_js_log, (int hostId, int level, const char* msg), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  var text = UTF8ToString(msg);
  if (h && h.onLog) { h.onLog(level, text); return; }
  (level >= 3 ? console.error : level == 2 ? console.warn : level == 0 ? console.debug : console.log)("[jsui] " + text);
});

EM_JS(void, jsui_web_js_message, (int hostId, const char* name, const char* json), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onMessage) h.onMessage(UTF8ToString(name), UTF8ToString(json));
});

// Game data (shared state / calls). Call ids are passed as doubles: they
// stay far below 2^53, and a double avoids BigInt in the EM_JS signatures.
EM_JS(void, jsui_web_js_state, (int hostId, const char* key, const char* json), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onState) h.onState(UTF8ToString(key), json ? UTF8ToString(json) : null);
});

EM_JS(void, jsui_web_js_call, (int hostId, double id, const char* name, const char* args), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCall) h.onCall(id, UTF8ToString(name), UTF8ToString(args));
});

EM_JS(void, jsui_web_js_call_cancel, (int hostId, double id), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCallCancel) h.onCallCancel(id);
});

EM_JS(void, jsui_web_js_call_result, (int hostId, double id, int ok, const char* result), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCallResult) h.onCallResult(id, ok, UTF8ToString(result));
});

// UI audio (jsui.h "Audio"): every JsuiAudioCommand goes to the host's
// onAudio (web/jsui-audio.js plays it with Web Audio). The voice id is a
// double like the call ids; the strings are copied (valid only during the
// call).
EM_JS(void, jsui_web_js_audio, (int hostId, int op, double voice, const char* sound, const char* bus, float volume,
                                float pitch, float pan, float fadeMs, double offsetS, int loop, int muted), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onAudio) h.onAudio({ op: op, voice: voice, sound: sound ? UTF8ToString(sound) : null,
    bus: bus ? UTF8ToString(bus) : null, volume: volume, pitch: pitch, pan: pan, fadeMs: fadeMs, offset: offsetS,
    loop: !!loop, muted: !!muted });
});

// <Video> (jsui.h "Video"): every JsuiVideoCommand goes to the host's
// onVideo (web/jsui-web.js plays it with a <video> element). Video ids are
// doubles like the voice ids.
// `sources`: OPEN's alternatives as JSON ([{ url, type }]).
EM_JS(void, jsui_web_js_video, (int hostId, int op, double video, const char* sources, const char* texture,
                                double timeS, float volume, float rate, int paused, int muted, int loop), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onVideo) h.onVideo({ op: op, video: video, sources: sources ? JSON.parse(UTF8ToString(sources)) : null,
    texture: texture ? UTF8ToString(texture) : null, time: timeS, volume: volume, rate: rate, paused: !!paused,
    muted: !!muted, loop: !!loop });
});

// Native components drawn by page JS (web/jsui-components.js): every
// callback of a class registered with jsui_web_register_component goes to
// the host's onComponent(cls, kind, view, canvas, info, a, b). kind: 0
// create, 1 destroy, 2 props (a: JSON), 3 draw (canvas, info: JsuiDrawInfo*),
// 4 command (a: name, b: args JSON).
EM_JS(void, jsui_web_js_component, (int hostId, int cls, int kind, int view, void* canvas, const void* info,
                                    const char* a, const char* b), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onComponent) h.onComponent(cls, kind, view, canvas, info, a ? UTF8ToString(a) : null,
                                        b ? UTF8ToString(b) : null);
});

namespace {

// A page-JS component class: which host, which of its classes.
struct WebComponentClass {
  int hostId = 0;
  int cls = 0;
};

void* componentCreate(void* user, JsuiRuntime*, int view) {
  auto* c = static_cast<WebComponentClass*>(user);
  jsui_web_js_component(c->hostId, c->cls, 0, view, nullptr, nullptr, nullptr, nullptr);
  return user;
}
void componentDestroy(void* inst, int view) {
  auto* c = static_cast<WebComponentClass*>(inst);
  jsui_web_js_component(c->hostId, c->cls, 1, view, nullptr, nullptr, nullptr, nullptr);
}
void componentProps(void* inst, int view, const char* json) {
  auto* c = static_cast<WebComponentClass*>(inst);
  jsui_web_js_component(c->hostId, c->cls, 2, view, nullptr, nullptr, json, nullptr);
}
void componentDraw(void* inst, JsuiCanvas* canvas, const JsuiDrawInfo* info) {
  auto* c = static_cast<WebComponentClass*>(inst);
  jsui_web_js_component(c->hostId, c->cls, 3, info->view, canvas, info, nullptr, nullptr);
}
void componentCommand(void* inst, int view, const char* name, const char* args) {
  auto* c = static_cast<WebComponentClass*>(inst);
  jsui_web_js_component(c->hostId, c->cls, 4, view, nullptr, nullptr, name, args);
}

int hostIdOf(void* user) { return int(reinterpret_cast<intptr_t>(user)); }

void stateTrampoline(void* user, const char* key, const char* json) { jsui_web_js_state(hostIdOf(user), key, json); }

void callTrampoline(void* user, uint64_t id, const char* name, const char* args) {
  jsui_web_js_call(hostIdOf(user), double(id), name ? name : "", args ? args : "null");
}

void callCancelTrampoline(void* user, uint64_t id) { jsui_web_js_call_cancel(hostIdOf(user), double(id)); }

void callResultTrampoline(void* user, uint64_t id, int ok, const char* result) {
  jsui_web_js_call_result(hostIdOf(user), double(id), ok, result ? result : "");
}

void logTrampoline(void* user, JsuiLogLevel level, const char* message) {
  jsui_web_js_log(hostIdOf(user), int(level), message ? message : "");
}

void messageTrampoline(void* user, const char* name, const char* json) {
  jsui_web_js_message(hostIdOf(user), name ? name : "", json ? json : "null");
}

void audioTrampoline(void* user, const JsuiAudioCommand* c) {
  jsui_web_js_audio(hostIdOf(user), int(c->op), double(c->voice), c->sound, c->bus, c->volume, c->pitch, c->pan,
                    c->fade_ms, c->offset_s, c->loop, c->muted);
}

void appendJsonString(std::string& out, const char* s) {
  if (!s) {
    out += "null";
    return;
  }
  out += '"';
  for (; *s; s++) {
    unsigned char ch = static_cast<unsigned char>(*s);
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += char(ch);
    } else if (ch < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", ch);
      out += buf;
    } else {
      out += char(ch);
    }
  }
  out += '"';
}

void videoTrampoline(void* user, const JsuiVideoCommand* c) {
  std::string sources;
  if (c->op == JSUI_VIDEO_OPEN) {
    sources = "[";
    for (int i = 0; i < c->source_count; i++) {
      if (i) sources += ',';
      sources += "{\"url\":";
      appendJsonString(sources, c->sources[i]);
      sources += ",\"type\":";
      appendJsonString(sources, c->source_types ? c->source_types[i] : nullptr);
      sources += '}';
    }
    sources += ']';
  }
  jsui_web_js_video(hostIdOf(user), int(c->op), double(c->video), sources.empty() ? nullptr : sources.c_str(),
                    c->texture, c->time_s, c->volume, c->rate, c->paused, c->muted, c->loop);
}

}  // namespace

extern "C" {

// Creates a runtime whose log / on_message callbacks go to JS host `hostId`.
// `baseUrl` may be NULL (relative URLs then resolve against the bundle URL).
// `audio` 1: sounds go to the host's onAudio; 0 (also when the argument is
// left out, as headless Node hosts do): audio_disabled, every voice fails.
// `js` 1: the bundle runs on the host's own JS engine (V8 / JSC /
// SpiderMonkey, src/js/engine_hostjs.cpp) when the module has it; 0 (also
// when left out): QuickJS.
EMSCRIPTEN_KEEPALIVE JsuiRuntime* jsui_web_create(int hostId, int width, int height, float pixelRatio, int devMode,
                                                  const char* baseUrl, int audio, int js) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = width;
  cfg.height = height;
  cfg.pixel_ratio = pixelRatio;
  cfg.dev_mode = devMode;
  cfg.base_url = baseUrl && *baseUrl ? baseUrl : nullptr;
  void* user = reinterpret_cast<void*>(intptr_t(hostId));
  cfg.log = logTrampoline;
  cfg.log_user = user;
  cfg.on_message = messageTrampoline;
  cfg.message_user = user;
  cfg.on_state = stateTrampoline;
  cfg.state_user = user;
  cfg.on_call = callTrampoline;
  cfg.on_call_cancel = callCancelTrampoline;
  cfg.call_user = user;
  if (audio) {
    cfg.on_audio = audioTrampoline;
    cfg.audio_user = user;
  } else {
    cfg.audio_disabled = 1;
  }
  cfg.on_video = videoTrampoline;
  cfg.video_user = user;
  jsui::JsEngine::nextKind = js == 1 ? jsui::JsEngine::Kind::Host : jsui::JsEngine::Kind::QuickJs;
  JsuiRuntime* rt = jsui_create(&cfg);
  jsui::JsEngine::nextKind = jsui::JsEngine::Kind::QuickJs;
  return rt;
}

// 1 when this module can run bundles on the host's JS engine (jsui_web_create's `js`).
EMSCRIPTEN_KEEPALIVE int jsui_web_host_js() { return jsui::JsEngine::hostAvailable() ? 1 : 0; }

// jsui_call_ui with the result delivered to JS host `hostId` (onCallResult).
EMSCRIPTEN_KEEPALIVE double jsui_web_call_ui(JsuiRuntime* rt, int hostId, const char* name, const char* argsJson) {
  return double(jsui_call_ui(rt, name, argsJson, callResultTrampoline, reinterpret_cast<void*>(intptr_t(hostId))));
}

// jsui_resolve_call / jsui_reject_call with a double id (no BigInt on the JS side).
EMSCRIPTEN_KEEPALIVE int jsui_web_resolve_call(JsuiRuntime* rt, double id, const char* json) {
  return jsui_resolve_call(rt, uint64_t(id), json);
}
EMSCRIPTEN_KEEPALIVE int jsui_web_reject_call(JsuiRuntime* rt, double id, const char* message) {
  return jsui_reject_call(rt, uint64_t(id), message);
}

// jsui_audio_voice_ended with a double voice id (no BigInt on the JS side).
EMSCRIPTEN_KEEPALIVE void jsui_web_audio_voice_ended(JsuiRuntime* rt, double voice, int status) {
  jsui_audio_voice_ended(rt, uint64_t(voice), status);
}

// jsui_video_status with a double video id; `error` may be NULL.
EMSCRIPTEN_KEEPALIVE void jsui_web_video_status(JsuiRuntime* rt, double video, int state, double duration,
                                                double position, double buffered, int width, int height,
                                                const char* error) {
  JsuiVideoStatus s{};
  s.state = state;
  s.duration_s = duration;
  s.position_s = position;
  s.buffered_s = buffered;
  s.width = width;
  s.height = height;
  s.error = error;
  jsui_video_status(rt, uint64_t(video), &s);
}

// Registers native component `name` whose callbacks go to JS host
// `hostId`'s class `cls` (web/jsui-components.js). The class record lives
// as long as the module (a few bytes per registration).
EMSCRIPTEN_KEEPALIVE int jsui_web_register_component(JsuiRuntime* rt, int hostId, int cls, const char* name,
                                                     int flags) {
  auto* c = new WebComponentClass{hostId, cls};
  JsuiComponentClass k;
  jsui_component_class_init(&k);
  k.user = c;
  k.flags = flags;
  k.create = componentCreate;
  k.destroy = componentDestroy;
  k.props = componentProps;
  k.draw = componentDraw;
  k.command = componentCommand;
  return jsui_register_component(rt, name, &k);
}

#if defined(JSUI_WEBGPU)
// web/jsui-webgpu.js imports its GPUDevice and each frame's canvas texture
// with Emdawnwebgpu's WebGPU.importJsDevice / importJsTexture, which return
// owned references; it drops them through these.
// Imported objects need an instance as their parent: Emdawnwebgpu routes the
// device's async callbacks (buffer maps, queue work done, error scopes)
// through it.
EMSCRIPTEN_KEEPALIVE WGPUInstance jsui_web_wgpu_create_instance() { return wgpuCreateInstance(nullptr); }
EMSCRIPTEN_KEEPALIVE void jsui_web_wgpu_release_instance(WGPUInstance instance) { wgpuInstanceRelease(instance); }
EMSCRIPTEN_KEEPALIVE void jsui_web_wgpu_release_texture(WGPUTexture texture) { wgpuTextureRelease(texture); }
EMSCRIPTEN_KEEPALIVE void jsui_web_wgpu_release_device(WGPUDevice device) { wgpuDeviceRelease(device); }
#endif

}  // extern "C"
