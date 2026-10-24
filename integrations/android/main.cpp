// JsuiDemo: Android host for a JSUI bundle.
//
// A NativeActivity (android_native_app_glue, no Java). Frames are paced by
// AChoreographer at display rate: every vsync ticks the runtime and, when it
// has something new, renders with Skia Ganesh on GLES straight into the EGL
// window surface (jsui_render_gl_target, default framebuffer) and swaps.
// Without a usable EGL / GLES 3 context, or with `--es jsui_renderer cpu`,
// the CPU rasteriser paints into the window's buffer (ANativeWindow_lock).
// Touches (with historical samples and timestamps for gesture velocities),
// hardware keys and gamepads (W3C standard mapping) are forwarded; the
// system bar / display cutout insets are published as jsui.safeAreaInsets.
//
// Intent extras (adb shell am start -n com.orktes.jsui.demo/android.app.NativeActivity ...):
//   --es jsui_url http://10.0.2.2:5173/bundle.js   load from `jsui dev` (dev mode: live reload)
//   --es jsui_renderer cpu                          force the CPU path
// Without jsui_url the app loads ui/bundle.js from its APK assets.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/api-level.h>
#include <android/asset_manager.h>
#include <android/choreographer.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "jsui/jsui.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "jsui", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "jsui", __VA_ARGS__)

namespace {

void jsuiLog(void*, JsuiLogLevel level, const char* message) {
  static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  __android_log_print(prio[level < 4 ? level : 3], "jsui", "%s", message);
}

// CLOCK_MONOTONIC: the clock of AMotionEvent / AKeyEvent times (uptime).
double nowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

// GLES entry points: core functions from libGLESv3, extensions (and EGL's)
// through eglGetProcAddress.
void* glProc(void*, const char* name) {
  static void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
  static void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  void* p = gles ? dlsym(gles, name) : nullptr;
  if (!p && egl) p = dlsym(egl, name);
  if (!p) p = reinterpret_cast<void*>(eglGetProcAddress(name));
  return p;
}

// W3C standard gamepad buttons for Android key codes (-1: not a pad button).
int padButton(int32_t key) {
  switch (key) {
    case AKEYCODE_BUTTON_A: return 0;
    case AKEYCODE_BUTTON_B: return 1;
    case AKEYCODE_BUTTON_X: return 2;
    case AKEYCODE_BUTTON_Y: return 3;
    case AKEYCODE_BUTTON_L1: return 4;
    case AKEYCODE_BUTTON_R1: return 5;
    case AKEYCODE_BUTTON_L2: return 6;
    case AKEYCODE_BUTTON_R2: return 7;
    case AKEYCODE_BUTTON_SELECT: return 8;
    case AKEYCODE_BUTTON_START: return 9;
    case AKEYCODE_BUTTON_THUMBL: return 10;
    case AKEYCODE_BUTTON_THUMBR: return 11;
    case AKEYCODE_DPAD_UP: return 12;
    case AKEYCODE_DPAD_DOWN: return 13;
    case AKEYCODE_DPAD_LEFT: return 14;
    case AKEYCODE_DPAD_RIGHT: return 15;
    case AKEYCODE_BUTTON_MODE: return 16;
    default: return -1;
  }
}

// DOM KeyboardEvent key / code for a hardware key. Returns false for keys
// without a mapping.
bool domKey(int32_t k, bool shift, std::string* key, std::string* code) {
  if (k >= AKEYCODE_A && k <= AKEYCODE_Z) {
    char c = char((shift ? 'A' : 'a') + (k - AKEYCODE_A));
    *key = std::string(1, c);
    *code = std::string("Key") + char('A' + (k - AKEYCODE_A));
    return true;
  }
  if (k >= AKEYCODE_0 && k <= AKEYCODE_9) {
    *key = std::string(1, char('0' + (k - AKEYCODE_0)));
    *code = "Digit" + *key;
    return true;
  }
  if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) {
    *key = *code = "F" + std::to_string(1 + (k - AKEYCODE_F1));
    return true;
  }
  switch (k) {
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: *key = "Enter"; *code = "Enter"; return true;
    case AKEYCODE_ESCAPE: case AKEYCODE_BACK: *key = "Escape"; *code = "Escape"; return true;
    case AKEYCODE_DEL: *key = "Backspace"; *code = "Backspace"; return true;
    case AKEYCODE_FORWARD_DEL: *key = "Delete"; *code = "Delete"; return true;
    case AKEYCODE_TAB: *key = "Tab"; *code = "Tab"; return true;
    case AKEYCODE_SPACE: *key = " "; *code = "Space"; return true;
    case AKEYCODE_DPAD_UP: *key = "ArrowUp"; *code = "ArrowUp"; return true;
    case AKEYCODE_DPAD_DOWN: *key = "ArrowDown"; *code = "ArrowDown"; return true;
    case AKEYCODE_DPAD_LEFT: *key = "ArrowLeft"; *code = "ArrowLeft"; return true;
    case AKEYCODE_DPAD_RIGHT: *key = "ArrowRight"; *code = "ArrowRight"; return true;
    case AKEYCODE_MOVE_HOME: *key = "Home"; *code = "Home"; return true;
    case AKEYCODE_MOVE_END: *key = "End"; *code = "End"; return true;
    case AKEYCODE_PAGE_UP: *key = "PageUp"; *code = "PageUp"; return true;
    case AKEYCODE_PAGE_DOWN: *key = "PageDown"; *code = "PageDown"; return true;
    case AKEYCODE_SHIFT_LEFT: *key = "Shift"; *code = "ShiftLeft"; return true;
    case AKEYCODE_SHIFT_RIGHT: *key = "Shift"; *code = "ShiftRight"; return true;
    case AKEYCODE_CTRL_LEFT: *key = "Control"; *code = "ControlLeft"; return true;
    case AKEYCODE_CTRL_RIGHT: *key = "Control"; *code = "ControlRight"; return true;
    case AKEYCODE_ALT_LEFT: *key = "Alt"; *code = "AltLeft"; return true;
    case AKEYCODE_ALT_RIGHT: *key = "Alt"; *code = "AltRight"; return true;
    default: return false;
  }
}

int domMods(int32_t meta) {
  return ((meta & AMETA_SHIFT_ON) ? JSUI_MOD_SHIFT : 0) | ((meta & AMETA_CTRL_ON) ? JSUI_MOD_CTRL : 0) |
         ((meta & AMETA_ALT_ON) ? JSUI_MOD_ALT : 0) | ((meta & AMETA_META_ON) ? JSUI_MOD_META : 0);
}

// Last state sent per pad, so only changes reach the runtime.
struct PadState {
  int slot = 0;
  float axes[4] = {};
  float hatX = 0, hatY = 0, l2 = 0, r2 = 0;
};

struct Host {
  android_app* app = nullptr;
  JNIEnv* env = nullptr;
  JsuiRuntime* rt = nullptr;
  JsuiGpuContext* gpu = nullptr;
  bool cpu = false;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  int stencilBits = 0;
  bool hasWindow = false, resumed = false, frameScheduled = false;
  bool presented = false;  // the window shows the latest frame
  bool safeAreaKnown = false;
  int pxW = 0, pxH = 0;
  float scale = 1;
  std::unordered_map<int32_t, int> touchIds;  // Android pointer id -> JSUI touch id
  int nextTouchId = 1;
  std::unordered_map<int32_t, PadState> pads;  // by input device id
  // Frame rate, logged every few seconds while frames are drawn.
  int framesDrawn = 0;
  double fpsSince = 0;
};

// ------------------------------------------------------------------ JNI

std::string intentExtra(Host& h, const char* name) {
  JNIEnv* env = h.env;
  jobject activity = h.app->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jobject intent = env->CallObjectMethod(activity, env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;"));
  std::string out;
  if (intent) {
    jclass intentClass = env->GetObjectClass(intent);
    jstring key = env->NewStringUTF(name);
    auto value = static_cast<jstring>(env->CallObjectMethod(
        intent, env->GetMethodID(intentClass, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;"), key));
    if (value) {
      const char* s = env->GetStringUTFChars(value, nullptr);
      out = s;
      env->ReleaseStringUTFChars(value, s);
      env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(intentClass);
    env->DeleteLocalRef(intent);
  }
  env->DeleteLocalRef(activityClass);
  if (env->ExceptionCheck()) env->ExceptionClear();
  return out;
}

// System bars + display cutout, as shared state jsui.safeAreaInsets (logical
// px), which SafeAreaView and useSafeAreaInsets() read. Returns false while
// the decor view has no insets yet (before its first layout).
bool publishSafeArea(Host& h) {
  JNIEnv* env = h.env;
  jobject activity = h.app->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jobject window = env->CallObjectMethod(activity, env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;"));
  jobject decor = env->CallObjectMethod(
      window, env->GetMethodID(env->GetObjectClass(window), "getDecorView", "()Landroid/view/View;"));
  jobject insets = env->CallObjectMethod(
      decor, env->GetMethodID(env->GetObjectClass(decor), "getRootWindowInsets", "()Landroid/view/WindowInsets;"));
  bool ok = false;
  int l = 0, t = 0, r = 0, b = 0;
  if (insets && !env->ExceptionCheck()) {
    jclass insetsClass = env->GetObjectClass(insets);
    if (android_get_device_api_level() >= 30) {
      jclass type = env->FindClass("android/view/WindowInsets$Type");
      jint mask = env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "systemBars", "()I")) |
                  env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "displayCutout", "()I"));
      jobject in = env->CallObjectMethod(insets, env->GetMethodID(insetsClass, "getInsets", "(I)Landroid/graphics/Insets;"), mask);
      if (in) {
        jclass ic = env->GetObjectClass(in);
        l = env->GetIntField(in, env->GetFieldID(ic, "left", "I"));
        t = env->GetIntField(in, env->GetFieldID(ic, "top", "I"));
        r = env->GetIntField(in, env->GetFieldID(ic, "right", "I"));
        b = env->GetIntField(in, env->GetFieldID(ic, "bottom", "I"));
        ok = true;
      }
    } else {
      l = env->CallIntMethod(insets, env->GetMethodID(insetsClass, "getSystemWindowInsetLeft", "()I"));
      t = env->CallIntMethod(insets, env->GetMethodID(insetsClass, "getSystemWindowInsetTop", "()I"));
      r = env->CallIntMethod(insets, env->GetMethodID(insetsClass, "getSystemWindowInsetRight", "()I"));
      b = env->CallIntMethod(insets, env->GetMethodID(insetsClass, "getSystemWindowInsetBottom", "()I"));
      ok = true;
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    ok = false;
  }
  if (ok && h.rt) {
    char json[160];
    snprintf(json, sizeof json, "{\"top\":%g,\"right\":%g,\"bottom\":%g,\"left\":%g}", t / h.scale, r / h.scale,
             b / h.scale, l / h.scale);
    jsui_set_state(h.rt, "jsui.safeAreaInsets", json);
  }
  // (local references are freed when the frame returns to the looper)
  return ok;
}

// ------------------------------------------------------------------ assets

// asset:///<path> from the APK's assets/ (the UI bundle and its images,
// fonts, sounds); everything is read whole.
void assetLoad(void* user, JsuiRuntime* rt, uint64_t id, const char* url) {
  auto* h = static_cast<Host*>(user);
  std::string u = url;
  const std::string prefix = "asset:///";
  if (u.rfind(prefix, 0) != 0) {
    LOGW("no loader for %s (only asset:/// without jsui_url)", url);
    jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
    return;
  }
  std::string path = u.substr(prefix.size());
  path = path.substr(0, path.find_first_of("?#"));
  AAsset* a = AAssetManager_open(h->app->activity->assetManager, path.c_str(), AASSET_MODE_BUFFER);
  if (!a) {
    jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
    return;
  }
  jsui_resource_complete(rt, id, 200, nullptr, AAsset_getBuffer(a), size_t(AAsset_getLength(a)));
  AAsset_close(a);
}

// ------------------------------------------------------------------- EGL

bool initEgl(Host& h) {
  h.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (h.display == EGL_NO_DISPLAY || !eglInitialize(h.display, nullptr, nullptr)) return false;
  // RGBA8 with a stencil buffer (Skia's path rendering uses it); without
  // stencil if no such config exists.
  for (int stencil : {8, 0}) {
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, stencil, EGL_NONE};
    EGLint n = 0;
    if (eglChooseConfig(h.display, attribs, &h.config, 1, &n) && n > 0) {
      eglGetConfigAttrib(h.display, h.config, EGL_STENCIL_SIZE, &h.stencilBits);
      break;
    }
    h.config = nullptr;
  }
  if (!h.config) return false;
  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  h.context = eglCreateContext(h.display, h.config, EGL_NO_CONTEXT, ctxAttribs);
  return h.context != EGL_NO_CONTEXT;
}

void destroySurface(Host& h) {
  if (h.surface == EGL_NO_SURFACE) return;
  eglMakeCurrent(h.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(h.display, h.surface);
  h.surface = EGL_NO_SURFACE;
}

void useCpu(Host& h, const char* why) {
  LOGW("%s: using the CPU renderer", why);
  if (h.gpu) {
    // The GL context may be gone already: drop GPU objects without GL calls.
    jsui_gpu_context_lost(h.gpu);
    jsui_gpu_destroy(h.gpu);
    h.gpu = nullptr;
  }
  destroySurface(h);
  h.cpu = true;
  h.presented = false;
}

bool createSurface(Host& h) {
  if (h.cpu) {
    ANativeWindow_setBuffersGeometry(h.app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    return true;
  }
  if (h.context == EGL_NO_CONTEXT && !initEgl(h)) {
    useCpu(h, "no EGL / GLES 3 context");
    return createSurface(h);
  }
  h.surface = eglCreateWindowSurface(h.display, h.config, h.app->window, nullptr);
  if (h.surface == EGL_NO_SURFACE || !eglMakeCurrent(h.display, h.surface, h.surface, h.context)) {
    useCpu(h, "no EGL window surface");
    return createSurface(h);
  }
  eglSwapInterval(h.display, 1);
  if (!h.gpu) {
    h.gpu = jsui_gpu_create_gl(glProc, nullptr);
    if (!h.gpu) {
      useCpu(h, "Skia Ganesh GL unavailable");
      return createSurface(h);
    }
    LOGI("renderer: gl (%s, %s, stencil %d)", glGetString(GL_RENDERER), glGetString(GL_VERSION), h.stencilBits);
  }
  return true;
}

// ----------------------------------------------------------------- frame

void scheduleFrame(Host& h);

void updateViewport(Host& h, int w, int h_) {
  if (w == h.pxW && h_ == h.pxH) return;
  h.pxW = w;
  h.pxH = h_;
  h.presented = false;
  h.safeAreaKnown = false;
  if (h.rt) jsui_set_viewport(h.rt, int(std::lround(w / h.scale)), int(std::lround(h_ / h.scale)), h.scale);
}

void createRuntime(Host& h) {
  std::string url = intentExtra(h, "jsui_url");
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = int(std::lround(h.pxW / h.scale));
  cfg.height = int(std::lround(h.pxH / h.scale));
  cfg.pixel_ratio = h.scale;
  cfg.log = jsuiLog;
  cfg.dev_mode = !url.empty();
  if (url.empty()) {
    // Built-in loader for http:// (dev servers); APK assets otherwise.
    cfg.load = assetLoad;
    cfg.load_user = &h;
    url = "asset:///ui/bundle.js";
  }
  h.rt = jsui_create(&cfg);
  LOGI("loading %s (%dx%d px, scale %.2f)", url.c_str(), h.pxW, h.pxH, h.scale);
  jsui_load_bundle_url(h.rt, url.c_str());
}

void drawFrame(Host& h) {
  if (!h.rt || !h.hasWindow) return;
  int w = ANativeWindow_getWidth(h.app->window), ht = ANativeWindow_getHeight(h.app->window);
  if (!h.cpu && h.surface != EGL_NO_SURFACE) {
    EGLint ew = 0, eh = 0;
    eglQuerySurface(h.display, h.surface, EGL_WIDTH, &ew);
    eglQuerySurface(h.display, h.surface, EGL_HEIGHT, &eh);
    if (ew > 0 && eh > 0) w = ew, ht = eh;
  }
  if (w <= 0 || ht <= 0) return;
  updateViewport(h, w, ht);
  if (!h.safeAreaKnown) h.safeAreaKnown = publishSafeArea(h);

  jsui_tick(h.rt, nowMs());
  if (h.presented && !jsui_needs_redraw(h.rt)) return;  // the screen already shows this frame
  if (!h.cpu) {
    JsuiGLTarget t{};
    t.fbo = 0;
    t.format = 0x8058;  // GL_RGBA8
    t.width = w;
    t.height = ht;
    t.stencil_bits = h.stencilBits;
    if (jsui_render_gl_target(h.rt, h.gpu, &t) != 0) {
      useCpu(h, "GL render failed");
      createSurface(h);
    } else if (!eglSwapBuffers(h.display, h.surface)) {
      EGLint err = eglGetError();
      if (err == EGL_CONTEXT_LOST) {
        // Recreate everything GL on the next frame.
        jsui_gpu_context_lost(h.gpu);
        destroySurface(h);
        eglDestroyContext(h.display, h.context);
        h.context = EGL_NO_CONTEXT;
        createSurface(h);
      }
      return;
    }
  }
  if (h.cpu) {
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(h.app->window, &buf, nullptr) != 0) return;
    jsui_render_rgba(h.rt, buf.bits, buf.width, buf.height, size_t(buf.stride) * 4);
    ANativeWindow_unlockAndPost(h.app->window);
  }
  h.presented = true;
  if (const char* err = jsui_last_error(h.rt)) LOGW("%s", err);

  h.framesDrawn++;
  double now = nowMs();
  if (h.fpsSince == 0) h.fpsSince = now;
  if (now - h.fpsSince >= 5000) {
    LOGI("%.1f frames drawn / s (%s)", h.framesDrawn * 1000.0 / (now - h.fpsSince), h.cpu ? "cpu" : "gl");
    h.framesDrawn = 0;
    h.fpsSince = now;
  }
}

void onVsync(int64_t, void* data) {
  auto& h = *static_cast<Host*>(data);
  h.frameScheduled = false;
  if (!h.hasWindow || !h.resumed) return;
  drawFrame(h);
  scheduleFrame(h);
}

void scheduleFrame(Host& h) {
  if (h.frameScheduled || !h.hasWindow || !h.resumed) return;
  h.frameScheduled = true;
  AChoreographer_postFrameCallback64(AChoreographer_getInstance(), onVsync, &h);
}

// ------------------------------------------------------------------ input

int32_t touchEvent(Host& h, AInputEvent* e) {
  int32_t action = AMotionEvent_getAction(e);
  int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
  size_t index = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  float s = h.scale;
  auto send = [&](size_t i, int phase, float x, float y) {
    int32_t pid = AMotionEvent_getPointerId(e, i);
    auto it = h.touchIds.find(pid);
    int id;
    if (it == h.touchIds.end()) {
      if (phase != 0) return;
      id = h.nextTouchId++;
      h.touchIds[pid] = id;
    } else {
      id = it->second;
    }
    jsui_send_touch(h.rt, id, phase, x / s, y / s);
    if (phase >= 2) h.touchIds.erase(pid);
  };
  switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
      jsui_set_event_time(h.rt, AMotionEvent_getEventTime(e) / 1e6);
      send(index, 0, AMotionEvent_getX(e, index), AMotionEvent_getY(e, index));
      break;
    case AMOTION_EVENT_ACTION_MOVE: {
      // Samples batched since the last event (input arrives faster than
      // frames), oldest first, then the current positions.
      size_t pointers = AMotionEvent_getPointerCount(e);
      size_t history = AMotionEvent_getHistorySize(e);
      for (size_t k = 0; k < history; k++) {
        jsui_set_event_time(h.rt, AMotionEvent_getHistoricalEventTime(e, k) / 1e6);
        for (size_t i = 0; i < pointers; i++)
          send(i, 1, AMotionEvent_getHistoricalX(e, i, k), AMotionEvent_getHistoricalY(e, i, k));
      }
      jsui_set_event_time(h.rt, AMotionEvent_getEventTime(e) / 1e6);
      for (size_t i = 0; i < pointers; i++) send(i, 1, AMotionEvent_getX(e, i), AMotionEvent_getY(e, i));
      break;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
      jsui_set_event_time(h.rt, AMotionEvent_getEventTime(e) / 1e6);
      send(index, 2, AMotionEvent_getX(e, index), AMotionEvent_getY(e, index));
      break;
    case AMOTION_EVENT_ACTION_CANCEL:
      for (size_t i = 0, n = AMotionEvent_getPointerCount(e); i < n; i++)
        send(i, 3, AMotionEvent_getX(e, i), AMotionEvent_getY(e, i));
      h.touchIds.clear();
      break;
    default: return 0;
  }
  return 1;
}

PadState& pad(Host& h, int32_t device) {
  auto it = h.pads.find(device);
  if (it != h.pads.end()) return it->second;
  PadState& p = h.pads[device];
  p.slot = int(h.pads.size()) - 1;
  jsui_set_gamepad_connected(h.rt, p.slot, 1, "Android gamepad (STANDARD GAMEPAD)");
  return p;
}

int32_t joystickEvent(Host& h, AInputEvent* e) {
  PadState& p = pad(h, AInputEvent_getDeviceId(e));
  const int32_t axes[4] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ};
  for (int i = 0; i < 4; i++) {
    float v = AMotionEvent_getAxisValue(e, axes[i], 0);
    if (v != p.axes[i]) jsui_send_gamepad_axis(h.rt, p.slot, i, p.axes[i] = v);
  }
  // Pads that report the D-pad as a hat and triggers as axes.
  auto button = [&](float* last, float v, int b) {
    if (v == *last) return;
    *last = v;
    jsui_send_gamepad_button(h.rt, p.slot, b, v > 0.5f, v);
  };
  float hatX = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
  float hatY = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
  if (hatX != p.hatX) {
    jsui_send_gamepad_button(h.rt, p.slot, 14, hatX < -0.5f, hatX < -0.5f ? 1.f : 0.f);
    jsui_send_gamepad_button(h.rt, p.slot, 15, hatX > 0.5f, hatX > 0.5f ? 1.f : 0.f);
    p.hatX = hatX;
  }
  if (hatY != p.hatY) {
    jsui_send_gamepad_button(h.rt, p.slot, 12, hatY < -0.5f, hatY < -0.5f ? 1.f : 0.f);
    jsui_send_gamepad_button(h.rt, p.slot, 13, hatY > 0.5f, hatY > 0.5f ? 1.f : 0.f);
    p.hatY = hatY;
  }
  button(&p.l2, std::fmax(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0)), 6);
  button(&p.r2, std::fmax(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0)), 7);
  return 1;
}

int32_t keyEvent(Host& h, AInputEvent* e) {
  int32_t action = AKeyEvent_getAction(e);
  if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 0;
  bool down = action == AKEY_EVENT_ACTION_DOWN;
  int32_t code = AKeyEvent_getKeyCode(e);
  int32_t source = AInputEvent_getSource(e);
  jsui_set_event_time(h.rt, AKeyEvent_getEventTime(e) / 1e6);
  int b = padButton(code);
  if (b >= 0 && ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                 (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK)) {
    if (AKeyEvent_getRepeatCount(e) == 0)
      jsui_send_gamepad_button(h.rt, pad(h, AInputEvent_getDeviceId(e)).slot, b, down, down ? 1.f : 0.f);
    return 1;
  }
  int32_t meta = AKeyEvent_getMetaState(e);
  std::string key, dom;
  if (!domKey(code, (meta & AMETA_SHIFT_ON) != 0, &key, &dom)) return 0;
  int mods = domMods(meta);
  int handled = jsui_send_key(h.rt, key.c_str(), dom.c_str(), down, AKeyEvent_getRepeatCount(e) > 0, mods);
  if (down && key.size() == 1 && (unsigned char)key[0] >= 0x20 && !(mods & (JSUI_MOD_CTRL | JSUI_MOD_META)))
    handled |= jsui_send_text(h.rt, key.c_str());
  // Back the UI did not use (no open menu to close) leaves the app.
  if (code == AKEYCODE_BACK) return handled ? 1 : 0;
  return 1;
}

int32_t onInput(android_app* app, AInputEvent* e) {
  auto& h = *static_cast<Host*>(app->userData);
  if (!h.rt) return 0;
  int32_t type = AInputEvent_getType(e);
  int32_t source = AInputEvent_getSource(e);
  int32_t r = 0;
  if (type == AINPUT_EVENT_TYPE_MOTION) {
    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) r = joystickEvent(h, e);
    else r = touchEvent(h, e);
  } else if (type == AINPUT_EVENT_TYPE_KEY) {
    r = keyEvent(h, e);
  }
  scheduleFrame(h);
  return r;
}

// -------------------------------------------------------------- lifecycle

void onCmd(android_app* app, int32_t cmd) {
  auto& h = *static_cast<Host*>(app->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (!app->window) break;
      h.hasWindow = true;
      createSurface(h);
      if (!h.rt) {
        h.pxW = ANativeWindow_getWidth(app->window);
        h.pxH = ANativeWindow_getHeight(app->window);
        createRuntime(h);
      }
      h.presented = false;
      scheduleFrame(h);
      break;
    case APP_CMD_TERM_WINDOW:
      h.hasWindow = false;
      destroySurface(h);
      break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_WINDOW_REDRAW_NEEDED:
      h.presented = false;
      h.safeAreaKnown = false;
      scheduleFrame(h);
      break;
    case APP_CMD_RESUME:
      h.resumed = true;
      scheduleFrame(h);
      break;
    case APP_CMD_PAUSE:
      h.resumed = false;
      break;
    case APP_CMD_LOW_MEMORY:
      if (h.gpu && h.surface != EGL_NO_SURFACE) jsui_gpu_purge(h.gpu, 0);
      break;
    default: break;
  }
}

}  // namespace

void android_main(android_app* app) {
  Host h;
  h.app = app;
  app->userData = &h;
  app->onAppCmd = onCmd;
  app->onInputEvent = onInput;
  app->activity->vm->AttachCurrentThread(&h.env, nullptr);
  h.scale = std::fmax(1.f, AConfiguration_getDensity(app->config) / 160.f);
  h.cpu = intentExtra(h, "jsui_renderer") == "cpu";

  while (!app->destroyRequested) {
    android_poll_source* source = nullptr;
    int result = ALooper_pollOnce(-1, nullptr, nullptr, reinterpret_cast<void**>(&source));
    if (result == ALOOPER_POLL_ERROR) break;
    if (source) source->process(app, source);
  }

  if (h.rt) jsui_destroy(h.rt);
  if (h.gpu) {
    if (h.surface == EGL_NO_SURFACE) jsui_gpu_context_lost(h.gpu);
    jsui_gpu_destroy(h.gpu);
  }
  destroySurface(h);
  if (h.context != EGL_NO_CONTEXT) eglDestroyContext(h.display, h.context);
  if (h.display != EGL_NO_DISPLAY) eglTerminate(h.display);
  app->activity->vm->DetachCurrentThread();
}
