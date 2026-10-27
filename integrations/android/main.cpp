// JsuiDemo: Android host for a JSUI bundle.
//
// A NativeActivity (android_native_app_glue, no Java). Frames are paced by
// AChoreographer at display rate: every vsync ticks the runtime and, when it
// has something new, renders it:
//   * Skia Graphite on Vulkan (default): into a retained image of the host's
//     own (only the damage is repainted, jsui_render_gpu_target with
//     JSUI_GPU_INCREMENTAL), which is copied into the acquired swapchain
//     image and presented. Pipelines and the driver's pipeline cache are
//     kept in the app's cache directory (jsui_set_cache_dir).
//   * Skia Ganesh on GLES 3 when Vulkan is missing or anything in its setup
//     fails (or `--es jsui_renderer gl`): straight into the EGL window
//     surface (jsui_render_gl_target, default framebuffer), then a swap.
//   * Without a usable EGL / GLES 3 context either, or with
//     `--es jsui_renderer cpu`, the CPU rasteriser paints into the window's
//     buffer (ANativeWindow_lock).
// Touches (with historical samples and timestamps for gesture velocities),
// hardware keys and gamepads (W3C standard mapping) are forwarded; the
// system bar / display cutout insets are published as jsui.safeAreaInsets.
// fetch(), images and fonts load from the APK's assets (asset:///) or, for
// http:// and https://, through java.net.HttpURLConnection over JNI
// (JsuiConfig.on_request): JSUI itself has no TLS on Android.
//
// Intent extras (adb shell am start -n com.orktes.jsui.demo/android.app.NativeActivity ...):
//   --es jsui_url http://10.0.2.2:5173/bundle.js   load from `jsui dev` (dev mode: live reload)
//   --es jsui_renderer gl|cpu                       force Ganesh GLES / the CPU path (default: vulkan)
//   --es jsui_js_thread off                         run JS on the frame thread (default: on, its own
//                                                   thread as in React Native; or lockstep)
// Without jsui_url the app loads ui/bundle.js from its APK assets.
// JS runs on its own thread by default (JsuiConfig.js_thread): the vsync
// frames never wait for it. They are scheduled every vsync anyway, so no
// on_js_ready wake-up is needed.

#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"
#include "present_spv.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "jsui", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "jsui", __VA_ARGS__)

namespace {

void jsuiLog(void*, JsuiLogLevel level, const char* message) {
  static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  __android_log_print(prio[int(level) < 4 ? int(level) : 3], "jsui", "%s", message);
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


// The Vulkan device and window swapchain (Graphite). Entry points come from
// libvulkan.so at run time, so the app also starts where there is none.
struct Vk {
  void* lib = nullptr;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
#define JSUI_VK_FNS(X)                                                                                          \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                         \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR)                \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                        \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define JSUI_VK_DEVICE_FNS(X)                                                                                   \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)  \
  X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImage) X(vkDestroyImage)  \
  X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) \
  X(vkDestroyImageView) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)             \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence)            \
  X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateRenderPass)         \
  X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule)                \
  X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines)   \
  X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
  X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler)          \
  X(vkDestroySampler) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                      \
  X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)
#define JSUI_VK_DECLARE(name) PFN_##name name = nullptr;
  JSUI_VK_FNS(JSUI_VK_DECLARE)
  JSUI_VK_DEVICE_FNS(JSUI_VK_DECLARE)
#undef JSUI_VK_DECLARE
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  uint32_t apiVersion = VK_API_VERSION_1_1;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  std::vector<const char*> instanceExtensions, deviceExtensions;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;  // the last present pass'
  VkSemaphore acquired = VK_NULL_HANDLE, drawn = VK_NULL_HANDLE;
  bool inFlight = false;
  // The present pass: the frame drawn into the swapchain image, pre-rotated
  // and opaque (present_spv.h).
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;  // for passFormat
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkFormat passFormat = VK_FORMAT_UNDEFINED;
  // Per window.
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  int rotation = 0;  // the swapchain's pre-rotation: 0, 90, 180, 270 degrees clockwise as 0..3
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;
  bool outOfDate = false, suboptimalLogged = false;
  // The retained frame JSUI renders into (window-sized, survives swapchain
  // changes of the same size): RGBA8, left in SHADER_READ_ONLY_OPTIMAL for
  // the present pass.
  VkImage frame = VK_NULL_HANDLE;
  VkDeviceMemory frameMemory = VK_NULL_HANDLE;
  VkImageView frameView = VK_NULL_HANDLE;
  int frameW = 0, frameH = 0;
  bool frameValid = false;  // holds the last frame JSUI rendered
};

enum class Renderer { Vulkan, GL, Cpu };

namespace net {
struct Net;
}

struct Host {
  android_app* app = nullptr;
  JNIEnv* env = nullptr;
  JsuiRuntime* rt = nullptr;
  // The Choreographer's vsync time of the frame being drawn (ms, the
  // CLOCK_MONOTONIC of nowMs and input events): flings and resampled touch
  // drags step evenly however late the callback runs.
  double frameTime = 0;
  JsuiGpuContext* gpu = nullptr;
  Renderer renderer = Renderer::Vulkan;
  bool cpu = false;  // renderer == Cpu
  Vk vk;
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
  double renderMs = 0;     // summed time of the frames' renders + present / swap calls
  double renderMaxMs = 0;  // ... the longest
  std::shared_ptr<net::Net> net;  // http(s):// on HttpURLConnection; null if JNI setup failed
};

bool createVkSurface(Host& h);
void destroyVkSurface(Vk& v);
void destroyVulkan(Host& h);

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

// Context.getCacheDir(): where the pipeline caches and the Vulkan driver's
// pipeline cache are kept between launches.
std::string cacheDir(Host& h) {
  JNIEnv* env = h.env;
  jobject activity = h.app->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jobject dir = env->CallObjectMethod(activity, env->GetMethodID(activityClass, "getCacheDir", "()Ljava/io/File;"));
  std::string out;
  if (dir && !env->ExceptionCheck()) {
    auto path = static_cast<jstring>(
        env->CallObjectMethod(dir, env->GetMethodID(env->GetObjectClass(dir), "getAbsolutePath", "()Ljava/lang/String;")));
    if (path && !env->ExceptionCheck()) {
      const char* p = env->GetStringUTFChars(path, nullptr);
      out = p;
      env->ReleaseStringUTFChars(path, p);
    }
  }
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
  if (ok && h.rt) jsui_set_safe_area_insets(h.rt, t / h.scale, r / h.scale, b / h.scale, l / h.scale);
  // (local references are freed when the frame returns to the looper)
  return ok;
}

// ------------------------------------------------------- assets + network
//
// JsuiConfig.on_request. asset:/// URLs (the bundle and everything relative
// to it) come from the APK's assets; http:// and https:// go to
// java.net.HttpURLConnection, i.e. the platform's TLS, trust store, proxy
// and network security config (JSUI's own loader has no TLS on Android:
// apps may not load the system's libssl). Anything else (file paths) is
// declined to the built-in loader.
//
// Requests run on a few worker threads attached to the VM. A watchdog
// thread answers the ones that time out and has those and cancelled ones
// stopped with disconnect(), which aborts a blocked connect or read. Answers are sent
// under the Net's lock and shutdown() (before jsui_destroy) closes it, so
// nothing reaches a destroyed runtime; the threads hold the Net, not the
// Host, and wind down on their own.

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// asset:///<path> from the APK's assets/ (the UI bundle and its images,
// fonts, sounds); everything is read whole.
void serveAsset(Host& h, JsuiRuntime* rt, uint64_t id, const std::string& url) {
  std::string path = url.substr(std::strlen("asset:///"));
  path = path.substr(0, path.find_first_of("?#"));
  AAsset* a = AAssetManager_open(h.app->activity->assetManager, path.c_str(), AASSET_MODE_BUFFER);
  if (!a) {
    jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
    return;
  }
  jsui_resource_complete(rt, id, 200, nullptr, AAsset_getBuffer(a), size_t(AAsset_getLength(a)));
  AAsset_close(a);
}

namespace net {

using Clock = std::chrono::steady_clock;
constexpr int kStepTimeoutMs = 30000;  // connect, and each read (as JSUI's own loader)
constexpr int kMaxRedirects = 20;
constexpr int kMaxThreads = 4;
constexpr jsize kChunk = 64 << 10;

// System classes and methods, looked up once on the main thread (a native
// thread's FindClass only sees the system class loader; enough for these,
// but there is no need to repeat the lookups per request).
struct Jni {
  bool ok = false;
  jclass url = nullptr, conn = nullptr;
  jmethodID urlInit, urlInitContext, urlToString, openConnection;
  jmethodID setRequestMethod, setFollowRedirects, setConnectTimeout, setReadTimeout, setUseCaches, setDoOutput,
      setFixedLength, addRequestProperty, getOutputStream, getResponseCode, getResponseMessage, getHeaderFieldKey,
      getHeaderFieldAt, getHeaderField, getInputStream, getErrorStream, disconnect;
  jmethodID read, inClose, write, outClose, throwableToString;
};
Jni jni;

bool initJni(JNIEnv* env) {
  if (jni.ok) return true;
  auto global = [&](const char* name) {
    jclass local = env->FindClass(name);
    if (!local) return jclass(nullptr);
    auto g = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return g;
  };
  jni.url = global("java/net/URL");
  jni.conn = global("java/net/HttpURLConnection");
  jclass in = env->FindClass("java/io/InputStream"), out = env->FindClass("java/io/OutputStream"),
         thr = env->FindClass("java/lang/Throwable");
  if (env->ExceptionCheck() || !jni.url || !jni.conn || !in || !out || !thr) {
    env->ExceptionClear();
    return false;
  }
  jclass c = jni.conn;
  jni.urlInit = env->GetMethodID(jni.url, "<init>", "(Ljava/lang/String;)V");
  jni.urlInitContext = env->GetMethodID(jni.url, "<init>", "(Ljava/net/URL;Ljava/lang/String;)V");
  jni.urlToString = env->GetMethodID(jni.url, "toString", "()Ljava/lang/String;");
  jni.openConnection = env->GetMethodID(jni.url, "openConnection", "()Ljava/net/URLConnection;");
  jni.setRequestMethod = env->GetMethodID(c, "setRequestMethod", "(Ljava/lang/String;)V");
  jni.setFollowRedirects = env->GetMethodID(c, "setInstanceFollowRedirects", "(Z)V");
  jni.setConnectTimeout = env->GetMethodID(c, "setConnectTimeout", "(I)V");
  jni.setReadTimeout = env->GetMethodID(c, "setReadTimeout", "(I)V");
  jni.setUseCaches = env->GetMethodID(c, "setUseCaches", "(Z)V");
  jni.setDoOutput = env->GetMethodID(c, "setDoOutput", "(Z)V");
  jni.setFixedLength = env->GetMethodID(c, "setFixedLengthStreamingMode", "(J)V");
  jni.addRequestProperty = env->GetMethodID(c, "addRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
  jni.getOutputStream = env->GetMethodID(c, "getOutputStream", "()Ljava/io/OutputStream;");
  jni.getResponseCode = env->GetMethodID(c, "getResponseCode", "()I");
  jni.getResponseMessage = env->GetMethodID(c, "getResponseMessage", "()Ljava/lang/String;");
  jni.getHeaderFieldKey = env->GetMethodID(c, "getHeaderFieldKey", "(I)Ljava/lang/String;");
  jni.getHeaderFieldAt = env->GetMethodID(c, "getHeaderField", "(I)Ljava/lang/String;");
  jni.getHeaderField = env->GetMethodID(c, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;");
  jni.getInputStream = env->GetMethodID(c, "getInputStream", "()Ljava/io/InputStream;");
  jni.getErrorStream = env->GetMethodID(c, "getErrorStream", "()Ljava/io/InputStream;");
  jni.disconnect = env->GetMethodID(c, "disconnect", "()V");
  jni.read = env->GetMethodID(in, "read", "([BII)I");
  jni.inClose = env->GetMethodID(in, "close", "()V");
  jni.write = env->GetMethodID(out, "write", "([BII)V");
  jni.outClose = env->GetMethodID(out, "close", "()V");
  jni.throwableToString = env->GetMethodID(thr, "toString", "()Ljava/lang/String;");
  env->DeleteLocalRef(in);
  env->DeleteLocalRef(out);
  env->DeleteLocalRef(thr);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  jni.ok = true;
  return true;
}

// UTF-8 <-> Java strings (NewStringUTF wants modified UTF-8, which breaks on
// 4-byte sequences and on malformed input).
jstring toJ(JNIEnv* env, const std::string& s) {
  std::u16string u;
  u.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    auto c = static_cast<unsigned char>(s[i]);
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    bool valid = len > 0 && i + len <= s.size();
    for (int k = 1; valid && k < len; k++) {
      auto cc = static_cast<unsigned char>(s[i + k]);
      valid = (cc & 0xc0) == 0x80;
      cp = cp << 6 | (cc & 0x3f);
    }
    if (!valid || cp > 0x10ffff) {
      cp = 0xfffd;
      len = 1;
    }
    i += size_t(len);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      u.push_back(char16_t(0xd800 + (cp >> 10)));
      u.push_back(char16_t(0xdc00 + (cp & 0x3ff)));
    } else {
      u.push_back(char16_t(cp));
    }
  }
  return env->NewString(reinterpret_cast<const jchar*>(u.data()), jsize(u.size()));
}

std::string fromJ(JNIEnv* env, jstring s) {
  std::string out;
  if (!s) return out;
  jsize n = env->GetStringLength(s);
  const jchar* c = env->GetStringChars(s, nullptr);
  if (!c) return out;
  out.reserve(size_t(n));
  for (jsize i = 0; i < n; i++) {
    uint32_t cp = c[i];
    if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && c[i + 1] >= 0xdc00 && c[i + 1] < 0xe000)
      cp = 0x10000 + ((cp - 0xd800) << 10) + (c[++i] - 0xdc00);
    else if (cp >= 0xd800 && cp < 0xe000)
      cp = 0xfffd;
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xc0 | cp >> 6);
      out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += char(0xe0 | cp >> 12);
      out += char(0x80 | ((cp >> 6) & 0x3f));
      out += char(0x80 | (cp & 0x3f));
    } else {
      out += char(0xf0 | cp >> 18);
      out += char(0x80 | ((cp >> 12) & 0x3f));
      out += char(0x80 | ((cp >> 6) & 0x3f));
      out += char(0x80 | (cp & 0x3f));
    }
  }
  env->ReleaseStringChars(s, c);
  return out;
}

// The pending Java exception as text ("java.net.UnknownHostException: ..."),
// cleared; empty when there is none.
std::string takeException(JNIEnv* env) {
  jthrowable t = env->ExceptionOccurred();
  if (!t) return {};
  env->ExceptionClear();
  auto s = static_cast<jstring>(env->CallObjectMethod(t, jni.throwableToString));
  std::string out = "java exception";
  if (env->ExceptionCheck()) env->ExceptionClear();
  else if (s) out = fromJ(env, s);
  if (s) env->DeleteLocalRef(s);
  env->DeleteLocalRef(t);
  return out;
}

JNIEnv* attachThread(JavaVM* vm, const char* name) {
  JavaVMAttachArgs args{JNI_VERSION_1_6, name, nullptr};
  JNIEnv* env = nullptr;
  return vm->AttachCurrentThread(&env, &args) == JNI_OK ? env : nullptr;
}

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct Request {
  JsuiRuntime* rt = nullptr;
  uint64_t id = 0;
  std::string url, method;
  HeaderList headers;  // names in lower case
  std::vector<uint8_t> body;
  int redirect = JSUI_REDIRECT_FOLLOW;
  Clock::time_point deadline = Clock::time_point::max();  // JsuiRequest.timeout_ms
  // Under Net::mu:
  bool cancelled = false;  // by the UI, the host's shutdown, or answered by the watchdog: no more answers
  bool timedOut = false;
  bool headSent = false;
  bool kicked = false;     // a kicker() runs for it
  jobject conn = nullptr;  // global ref to the open HttpURLConnection
};

struct Net {
  JavaVM* vm = nullptr;
  std::mutex mu;
  std::condition_variable work, watch;
  std::deque<std::shared_ptr<Request>> queue;
  std::unordered_map<uint64_t, std::shared_ptr<Request>> live;  // queued or running
  int threads = 0, idle = 0;
  bool watching = false, closed = false;
};

// Runs `send` (a jsui_request_* call) unless the UI cancelled the request or
// the host shut down; false then. Under the lock, so shutdown() never
// returns while an answer is on its way into the runtime.
template <class F>
bool answer(Net& n, Request& r, F&& send) {
  std::lock_guard<std::mutex> lock(n.mu);
  if (n.closed || r.cancelled) return false;
  send();
  return true;
}

// Makes `conn` the connection the watchdog disconnects; false if the request
// is already stopping.
bool attachConnection(Net& n, Request& r, JNIEnv* env, jobject conn) {
  std::lock_guard<std::mutex> lock(n.mu);
  if (n.closed || r.cancelled || r.timedOut) return false;
  r.conn = env->NewGlobalRef(conn);
  return true;
}

void releaseConnection(Net& n, Request& r, JNIEnv* env, bool disconnect) {
  jobject c;
  {
    std::lock_guard<std::mutex> lock(n.mu);
    c = r.conn;
    r.conn = nullptr;
  }
  if (!c) return;
  if (disconnect) {
    env->CallVoidMethod(c, jni.disconnect);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  env->DeleteGlobalRef(c);
}

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// Pops the hop's local references however it ends.
struct LocalFrame {
  JNIEnv* env;
  bool ok;
  explicit LocalFrame(JNIEnv* e) : env(e), ok(e->PushLocalFrame(32) == 0) {
    if (!ok) env->ExceptionClear();
  }
  ~LocalFrame() {
    if (ok) env->PopLocalFrame(nullptr);
  }
};

// One request on a worker thread: each hop is one HttpURLConnection (JSUI
// follows redirects itself, so http:// <-> https:// hops work and the
// 'error' / 'manual' modes behave as in the built-in loader).
void run(Net& n, Request& r, JNIEnv* env) {
  bool headSent = false;
  std::string url = r.url, method = r.method;
  HeaderList headers = r.headers;
  std::vector<uint8_t> body = r.body;
  // Ends the request with `err` (empty: success). Nothing is sent for a
  // stopped one (the watchdog already answered a timeout).
  auto finish = [&](const std::string& err) {
    if (env) releaseConnection(n, r, env, !err.empty());
    if (headSent)
      answer(n, r, [&] { jsui_request_end(r.rt, r.id, err.empty() ? nullptr : err.c_str()); });
    else
      answer(n, r, [&] { jsui_request_respond(r.rt, r.id, 0, err.c_str(), url.c_str(), nullptr, 0); });
  };
  if (!env) return finish("cannot attach a thread to the Java VM");

  for (int hop = 0;; hop++) {
    LocalFrame frame(env);
    if (!frame.ok) return finish("out of memory");
    jobject u = env->NewObject(jni.url, jni.urlInit, toJ(env, url));
    jobject c = u ? env->CallObjectMethod(u, jni.openConnection) : nullptr;
    std::string err = takeException(env);
    if (!err.empty() || !c || !env->IsInstanceOf(c, jni.conn))
      return finish(err.empty() ? "not an http(s) URL: " + url : err);
    if (!attachConnection(n, r, env, c)) return finish("cancelled");

    // Each call checked before the next (CheckJNI insists).
    env->CallVoidMethod(c, jni.setFollowRedirects, JNI_FALSE);
    if (!env->ExceptionCheck()) env->CallVoidMethod(c, jni.setUseCaches, JNI_FALSE);
    if (!env->ExceptionCheck()) env->CallVoidMethod(c, jni.setConnectTimeout, jint(kStepTimeoutMs));
    if (!env->ExceptionCheck()) env->CallVoidMethod(c, jni.setReadTimeout, jint(kStepTimeoutMs));
    // A method HttpURLConnection does not know throws ProtocolException (it
    // knows GET, HEAD, POST, PUT, DELETE, OPTIONS, TRACE and, on Android, PATCH).
    if (!env->ExceptionCheck()) env->CallVoidMethod(c, jni.setRequestMethod, toJ(env, method));
    if (env->ExceptionCheck()) return finish(takeException(env));
    bool hasAccept = false, hasType = false;
    auto add = [&](const std::string& k, const std::string& v) {
      jstring jk = toJ(env, k), jv = toJ(env, v);
      env->CallVoidMethod(c, jni.addRequestProperty, jk, jv);
      if (env->ExceptionCheck()) env->ExceptionClear();  // an invalid header is left out
      env->DeleteLocalRef(jk);
      env->DeleteLocalRef(jv);
    };
    for (auto& [k, v] : headers) {
      // Framing is the connection's. Without an Accept-Encoding of ours it
      // asks for gzip and unpacks it, dropping Content-Encoding / -Length.
      if (k == "host" || k == "content-length" || k == "connection" || k == "transfer-encoding" ||
          k == "accept-encoding")
        continue;
      hasAccept |= k == "accept";
      hasType |= k == "content-type";
      add(k, v);
    }
    if (!hasAccept) add("Accept", "*/*");
    bool sendsBody = method != "GET" && method != "HEAD" &&
                     (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH");
    if (sendsBody) {
      if (!hasType && !body.empty()) add("Content-Type", "application/octet-stream");
      env->CallVoidMethod(c, jni.setDoOutput, JNI_TRUE);
      if (!env->ExceptionCheck()) env->CallVoidMethod(c, jni.setFixedLength, jlong(body.size()));
      jobject out = env->ExceptionCheck() ? nullptr : env->CallObjectMethod(c, jni.getOutputStream);  // connects
      if (out && !env->ExceptionCheck()) {
        jbyteArray chunk = env->NewByteArray(jsize(std::min(body.size(), size_t(kChunk))));
        for (size_t off = 0; chunk && off < body.size() && !env->ExceptionCheck();) {
          auto len = jsize(std::min(body.size() - off, size_t(kChunk)));
          env->SetByteArrayRegion(chunk, 0, len, reinterpret_cast<const jbyte*>(body.data() + off));
          env->CallVoidMethod(out, jni.write, chunk, 0, len);
          off += size_t(len);
        }
        if (!env->ExceptionCheck()) env->CallVoidMethod(out, jni.outClose);
      }
      if (env->ExceptionCheck()) return finish(takeException(env));
    }

    jint status = env->CallIntMethod(c, jni.getResponseCode);  // connects, sends, reads the headers
    if (env->ExceptionCheck()) return finish(takeException(env));
    if (status < 100 || status > 999) return finish("malformed response from " + url);

    if (isRedirect(status) && r.redirect != JSUI_REDIRECT_MANUAL) {
      auto loc = static_cast<jstring>(env->CallObjectMethod(c, jni.getHeaderField, toJ(env, "Location")));
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (loc) {
        jobject next = env->NewObject(jni.url, jni.urlInitContext, u, loc);  // resolved against this URL
        auto to = next ? static_cast<jstring>(env->CallObjectMethod(next, jni.urlToString)) : nullptr;
        err = takeException(env);
        if (!err.empty() || !to) return finish("bad redirect location: " + err);
        std::string target = fromJ(env, to);
        if (r.redirect == JSUI_REDIRECT_ERROR) return finish("redirect to " + target + " with redirect: 'error'");
        if (hop >= kMaxRedirects) return finish("too many redirects (" + std::to_string(hop) + ")");
        releaseConnection(n, r, env, true);
        // 303 always, 301/302 for POST (browser behaviour): GET without a body.
        if ((status == 303 && method != "HEAD") || ((status == 301 || status == 302) && method == "POST")) {
          method = "GET";
          body.clear();
          for (auto it = headers.begin(); it != headers.end();) it = it->first == "content-type" ? headers.erase(it) : it + 1;
        }
        url = target;
        continue;
      }
    }

    std::string message;
    auto jmessage = static_cast<jstring>(env->CallObjectMethod(c, jni.getResponseMessage));
    if (env->ExceptionCheck()) env->ExceptionClear();
    else message = fromJ(env, jmessage);
    HeaderList resHeaders;
    for (jint i = 0; i < 1000; i++) {
      // By index, in the server's order (index 0 may be the status line, without a key).
      auto v = static_cast<jstring>(env->CallObjectMethod(c, jni.getHeaderFieldAt, i));
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (!v) break;
      auto k = static_cast<jstring>(env->CallObjectMethod(c, jni.getHeaderFieldKey, i));
      if (env->ExceptionCheck()) env->ExceptionClear();
      if (k) {
        std::string name = fromJ(env, k);
        // The connection's own bookkeeping (X-Android-Sent-Millis, ...) is not the server's.
        if (lower(name).rfind("x-android-", 0) != 0) resHeaders.emplace_back(name, fromJ(env, v));
        env->DeleteLocalRef(k);
      }
      env->DeleteLocalRef(v);
    }
    std::vector<JsuiHeader> list;
    for (auto& [k, v] : resHeaders) list.push_back({k.c_str(), v.c_str()});
    if (!answer(n, r, [&] {
          jsui_request_respond(r.rt, r.id, status, message.c_str(), url.c_str(), list.data(), list.size());
          r.headSent = true;  // under the lock (the watchdog reads it)
        }))
      return finish("cancelled");
    headSent = true;
    if (method == "HEAD" || status == 204 || status == 304) return finish("");

    // 4xx / 5xx bodies come from the error stream (getInputStream throws).
    jobject in = env->CallObjectMethod(c, status >= 400 ? jni.getErrorStream : jni.getInputStream);
    if (env->ExceptionCheck()) return finish(takeException(env));
    if (!in) return finish("");  // no body
    jbyteArray buf = env->NewByteArray(kChunk);
    if (!buf) return finish(takeException(env));
    std::vector<uint8_t> bytes(static_cast<size_t>(kChunk));
    for (;;) {
      jint got = env->CallIntMethod(in, jni.read, buf, 0, kChunk);
      if (env->ExceptionCheck()) return finish(takeException(env));
      if (got < 0) break;
      if (got == 0) continue;
      env->GetByteArrayRegion(buf, 0, got, reinterpret_cast<jbyte*>(bytes.data()));
      if (!answer(n, r, [&] { jsui_request_body(r.rt, r.id, bytes.data(), size_t(got)); })) return finish("cancelled");
    }
    env->CallVoidMethod(in, jni.inClose);  // hands the connection back to the pool
    if (env->ExceptionCheck()) env->ExceptionClear();
    return finish("");
  }
}

void worker(std::shared_ptr<Net> n) {
  JNIEnv* env = attachThread(n->vm, "jsui-net");
  std::unique_lock<std::mutex> lock(n->mu);
  for (;;) {
    if (n->queue.empty()) {
      if (n->closed) break;
      n->idle++;
      bool woken = n->work.wait_for(lock, std::chrono::seconds(30), [&] { return !n->queue.empty() || n->closed; });
      n->idle--;
      if (!woken) break;  // idle for a while: let the thread go
      continue;
    }
    std::shared_ptr<Request> r = std::move(n->queue.front());
    n->queue.pop_front();
    lock.unlock();
    run(*n, *r, env);
    lock.lock();
    n->live.erase(r->id);
    n->watch.notify_one();
  }
  n->threads--;
  lock.unlock();
  if (env) n->vm->DetachCurrentThread();
}

// Calls disconnect() on a stopping request's connection, again every 50 ms
// until the worker lets go of it (one that came before the connection
// started connecting does not stop it). A thread of its own per request: a
// disconnect() may block until the read it aborts gives up.
void kicker(std::shared_ptr<Net> n, std::shared_ptr<Request> r) {
  JNIEnv* env = attachThread(n->vm, "jsui-net-stop");
  if (!env) return;
  for (;;) {
    jobject c = nullptr;
    {
      std::lock_guard<std::mutex> lock(n->mu);
      if (r->conn) c = env->NewGlobalRef(r->conn);
    }
    if (!c) break;
    env->CallVoidMethod(c, jni.disconnect);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  n->vm->DetachCurrentThread();
}

// Times requests out (answering them at once: the UI does not wait for Java
// to give up) and has cancelled and timed-out ones disconnected.
void watchdog(std::shared_ptr<Net> n) {
  std::unique_lock<std::mutex> lock(n->mu);
  for (;;) {
    auto now = Clock::now();
    auto next = Clock::time_point::max();
    for (auto& [id, r] : n->live) {
      if (!r->cancelled && !r->timedOut && r->deadline <= now) {
        r->timedOut = true;
        if (!n->closed) {
          if (r->headSent) jsui_request_end(r->rt, r->id, "timed out reading the response body");
          else jsui_request_respond(r->rt, r->id, 0, "timed out waiting for response", r->url.c_str(), nullptr, 0);
        }
        r->cancelled = true;  // answered: the worker stays silent
      }
      if (r->cancelled && r->conn && !r->kicked) {
        r->kicked = true;
        std::thread(kicker, n, r).detach();
      }
      if (!r->cancelled) next = std::min(next, r->deadline);
    }
    if (n->closed && n->live.empty()) break;
    if (next == Clock::time_point::max()) n->watch.wait(lock);
    else n->watch.wait_until(lock, next);
  }
  n->watching = false;
}

// Queues a request; false once the host shut down.
bool start(const std::shared_ptr<Net>& n, std::shared_ptr<Request> r) {
  std::lock_guard<std::mutex> lock(n->mu);
  if (n->closed) return false;
  n->live[r->id] = r;
  n->queue.push_back(std::move(r));
  if (!n->watching) {
    n->watching = true;
    std::thread(watchdog, n).detach();
  }
  if (n->queue.size() > size_t(n->idle) && n->threads < kMaxThreads) {
    n->threads++;
    std::thread(worker, n).detach();
  } else {
    n->work.notify_one();
  }
  n->watch.notify_one();  // a new deadline
  return true;
}

void cancel(Net& n, uint64_t id) {
  std::lock_guard<std::mutex> lock(n.mu);
  auto it = n.live.find(id);
  if (it == n.live.end()) return;
  it->second->cancelled = true;
  n.watch.notify_one();
}

// Before jsui_destroy: no answer is sent after this returns; requests on the
// way are disconnected and their threads exit.
void shutdown(Net& n) {
  std::lock_guard<std::mutex> lock(n.mu);
  n.closed = true;
  for (auto& r : n.queue) n.live.erase(r->id);
  n.queue.clear();
  for (auto& [id, r] : n.live) r->cancelled = true;
  n.work.notify_all();
  n.watch.notify_all();
}

}  // namespace net

int onRequest(void* user, JsuiRuntime* rt, uint64_t id, const JsuiRequest* req) {
  auto& h = *static_cast<Host*>(user);
  std::string url = req->url ? req->url : "";
  if (url.rfind("asset:///", 0) == 0) {
    serveAsset(h, rt, id, url);
    return 0;
  }
  std::string scheme = lower(url.substr(0, url.find(':')));
  if ((scheme != "http" && scheme != "https") || !h.net) return 1;  // the built-in loader
  auto r = std::make_shared<net::Request>();
  r->rt = rt;
  r->id = id;
  r->url = url;
  r->method = req->method ? req->method : "GET";
  for (size_t i = 0; i < req->header_count; i++)
    if (req->headers[i].name)
      r->headers.emplace_back(lower(req->headers[i].name), req->headers[i].value ? req->headers[i].value : "");
  if (req->body && req->body_size)
    r->body.assign(static_cast<const uint8_t*>(req->body), static_cast<const uint8_t*>(req->body) + req->body_size);
  r->redirect = req->redirect;
  if (req->timeout_ms > 0) r->deadline = net::Clock::now() + std::chrono::milliseconds(req->timeout_ms);
  return net::start(h.net, std::move(r)) ? 0 : 1;
}

void onRequestCancel(void* user, JsuiRuntime*, uint64_t id) {
  auto& h = *static_cast<Host*>(user);
  if (h.net) net::cancel(*h.net, id);
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
  if (h.renderer == Renderer::Vulkan) destroyVulkan(h);
  if (h.gpu) {
    // The GL context may be gone already: drop GPU objects without GL calls.
    jsui_gpu_context_lost(h.gpu);
    jsui_gpu_destroy(h.gpu);
    h.gpu = nullptr;
  }
  destroySurface(h);
  h.renderer = Renderer::Cpu;
  h.cpu = true;
  h.presented = false;
}

// Vulkan is missing or failed: Ganesh on GLES from now on.
void useGl(Host& h, const char* why) {
  LOGW("%s: using Ganesh GLES", why);
  destroyVulkan(h);
  h.renderer = Renderer::GL;
  h.presented = false;
}

bool createSurface(Host& h) {
  if (h.renderer == Renderer::Vulkan) {
    if (createVkSurface(h)) {
      h.presented = false;
      return true;
    }
    useGl(h, "no usable Vulkan device / swapchain");
  }
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


// ---------------------------------------------------------------- Vulkan

constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kFrameUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;

VkShaderModule shaderModule(Vk& v, const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  v.vkCreateShaderModule(v.device, &ci, nullptr, &m);
  return m;
}

// The instance, a device with a graphics queue, and what every frame
// reuses. False when there is no Vulkan 1.1 or no such device (the caller
// destroys what was made).
bool initVulkan(Host& h) {
  Vk& v = h.vk;
  if (!v.lib) v.lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!v.lib) return false;
  v.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(v.lib, "vkGetInstanceProcAddr"));
  if (!v.getInstanceProcAddr) return false;
  auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      v.getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (enumerateVersion) enumerateVersion(&loaderVersion);
  if (loaderVersion < VK_API_VERSION_1_1) return false;  // Graphite needs 1.1
  auto createInstance =
      reinterpret_cast<PFN_vkCreateInstance>(v.getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
  v.instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  appInfo.pApplicationName = "JsuiDemo";
  appInfo.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &appInfo;
  ici.enabledExtensionCount = uint32_t(v.instanceExtensions.size());
  ici.ppEnabledExtensionNames = v.instanceExtensions.data();
  if (!createInstance || createInstance(&ici, nullptr, &v.instance) != VK_SUCCESS) return false;
#define JSUI_VK_LOAD(name) v.name = reinterpret_cast<PFN_##name>(v.getInstanceProcAddr(v.instance, #name));
  JSUI_VK_FNS(JSUI_VK_LOAD)
#undef JSUI_VK_LOAD

  // The first device that has Vulkan 1.1 and a graphics queue (Android
  // surfaces can be presented from any queue family that supports graphics).
  uint32_t n = 0;
  v.vkEnumeratePhysicalDevices(v.instance, &n, nullptr);
  std::vector<VkPhysicalDevice> devices(n);
  v.vkEnumeratePhysicalDevices(v.instance, &n, devices.data());
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties props;
    v.vkGetPhysicalDeviceProperties(d, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) continue;
    uint32_t qn = 0;
    v.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> families(qn);
    v.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, families.data());
    for (uint32_t i = 0; i < qn; i++) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        v.physical = d;
        v.family = i;
        LOGI("vulkan: %s (api %u.%u.%u, driver 0x%x)", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
        break;
      }
    }
    if (v.physical) break;
  }
  if (!v.physical) return false;

  // Every feature the device has but robust buffer access (bounds checks
  // on every access), handed to Skia as they were enabled.
  v.vkGetPhysicalDeviceFeatures2(v.physical, &v.features);
  v.features.features.robustBufferAccess = VK_FALSE;
  v.deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  float priority = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = v.family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &v.features;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(v.deviceExtensions.size());
  dci.ppEnabledExtensionNames = v.deviceExtensions.data();
  if (v.vkCreateDevice(v.physical, &dci, nullptr, &v.device) != VK_SUCCESS) return false;
#define JSUI_VK_LOAD(name) v.name = reinterpret_cast<PFN_##name>(v.vkGetDeviceProcAddr(v.device, #name));
  JSUI_VK_DEVICE_FNS(JSUI_VK_LOAD)
#undef JSUI_VK_LOAD
  v.vkGetDeviceQueue(v.device, v.family, 0, &v.queue);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = v.family;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  if (v.vkCreateCommandPool(v.device, &pci, nullptr, &v.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = v.pool;
  cai.commandBufferCount = 1;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  if (v.vkAllocateCommandBuffers(v.device, &cai, &v.cmd) != VK_SUCCESS ||
      v.vkCreateFence(v.device, &fci, nullptr, &v.fence) != VK_SUCCESS ||
      v.vkCreateSemaphore(v.device, &sci, nullptr, &v.acquired) != VK_SUCCESS ||
      v.vkCreateSemaphore(v.device, &sci, nullptr, &v.drawn) != VK_SUCCESS)
    return false;

  // The present pass' sampler (texelFetch: no filtering), descriptor set and
  // layouts; its render pass and pipeline follow the swapchain format.
  VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  smp.magFilter = smp.minFilter = VK_FILTER_NEAREST;
  smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       nullptr};
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl.bindingCount = 1;
  dsl.pBindings = &binding;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &size;
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &push;
  if (v.vkCreateSampler(v.device, &smp, nullptr, &v.sampler) != VK_SUCCESS ||
      v.vkCreateDescriptorSetLayout(v.device, &dsl, nullptr, &v.setLayout) != VK_SUCCESS ||
      v.vkCreateDescriptorPool(v.device, &dpi, nullptr, &v.descriptorPool) != VK_SUCCESS)
    return false;
  pli.pSetLayouts = &v.setLayout;
  VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsa.descriptorPool = v.descriptorPool;
  dsa.descriptorSetCount = 1;
  dsa.pSetLayouts = &v.setLayout;
  return v.vkCreatePipelineLayout(v.device, &pli, nullptr, &v.pipelineLayout) == VK_SUCCESS &&
         v.vkAllocateDescriptorSets(v.device, &dsa, &v.set) == VK_SUCCESS;
}

// The present pass' render pass and pipeline for swapchain images of `format`.
bool createPresentPass(Vk& v, VkFormat format) {
  if (v.passFormat == format && v.pipeline) return true;
  if (v.pipeline) v.vkDestroyPipeline(v.device, v.pipeline, nullptr);
  if (v.renderPass) v.vkDestroyRenderPass(v.device, v.renderPass, nullptr);
  v.pipeline = VK_NULL_HANDLE;
  v.renderPass = VK_NULL_HANDLE;
  VkAttachmentDescription att{};
  att.format = format;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  // The layout transition waits for the acquire (its semaphore waits at
  // this stage).
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  rpi.dependencyCount = 1;
  rpi.pDependencies = &dep;
  if (v.vkCreateRenderPass(v.device, &rpi, nullptr, &v.renderPass) != VK_SUCCESS) return false;

  VkShaderModule vert = shaderModule(v, kPresentVert, sizeof kPresentVert);
  VkShaderModule frag = shaderModule(v, kPresentFrag, sizeof kPresentFrag);
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &ds;
  gp.layout = v.pipelineLayout;
  gp.renderPass = v.renderPass;
  VkResult r = vert && frag ? v.vkCreateGraphicsPipelines(v.device, VK_NULL_HANDLE, 1, &gp, nullptr, &v.pipeline)
                            : VK_ERROR_INITIALIZATION_FAILED;
  if (vert) v.vkDestroyShaderModule(v.device, vert, nullptr);
  if (frag) v.vkDestroyShaderModule(v.device, frag, nullptr);
  if (r != VK_SUCCESS) return false;
  v.passFormat = format;
  return true;
}

// The last present pass done, so its command buffer and semaphores can be
// used again.
void waitVk(Vk& v) {
  if (!v.inFlight) return;
  v.vkWaitForFences(v.device, 1, &v.fence, VK_TRUE, UINT64_MAX);
  v.vkResetFences(v.device, 1, &v.fence);
  v.inFlight = false;
}

void destroyFrameImage(Vk& v) {
  if (v.frameView) v.vkDestroyImageView(v.device, v.frameView, nullptr);
  if (v.frame) v.vkDestroyImage(v.device, v.frame, nullptr);
  if (v.frameMemory) v.vkFreeMemory(v.device, v.frameMemory, nullptr);
  v.frameView = VK_NULL_HANDLE;
  v.frame = VK_NULL_HANDLE;
  v.frameMemory = VK_NULL_HANDLE;
  v.frameW = v.frameH = 0;
  v.frameValid = false;
}

// The retained frame image of the window's size and its view in the
// present pass' descriptor set. A new one starts invalid: the next render
// is a full repaint.
bool createFrameImage(Vk& v, int w, int h) {
  if (v.frame && v.frameW == w && v.frameH == h) return true;
  waitVk(v);
  destroyFrameImage(v);
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = kFrameFormat;
  ici.extent = {uint32_t(w), uint32_t(h), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = kFrameUsage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (v.vkCreateImage(v.device, &ici, nullptr, &v.frame) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  v.vkGetImageMemoryRequirements(v.device, v.frame, &req);
  VkPhysicalDeviceMemoryProperties mem;
  v.vkGetPhysicalDeviceMemoryProperties(v.physical, &mem);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
    if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      type = i;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = kFrameFormat;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (type == UINT32_MAX || v.vkAllocateMemory(v.device, &mai, nullptr, &v.frameMemory) != VK_SUCCESS ||
      v.vkBindImageMemory(v.device, v.frame, v.frameMemory, 0) != VK_SUCCESS ||
      (vci.image = v.frame, v.vkCreateImageView(v.device, &vci, nullptr, &v.frameView)) != VK_SUCCESS) {
    destroyFrameImage(v);
    return false;
  }
  v.frameW = w;
  v.frameH = h;
  VkDescriptorImageInfo info{v.sampler, v.frameView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = v.set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &info;
  v.vkUpdateDescriptorSets(v.device, 1, &write, 0, nullptr);
  return true;
}

void destroySwapchainImages(Vk& v) {
  for (VkFramebuffer f : v.framebuffers) v.vkDestroyFramebuffer(v.device, f, nullptr);
  for (VkImageView iv : v.views) v.vkDestroyImageView(v.device, iv, nullptr);
  v.framebuffers.clear();
  v.views.clear();
  v.images.clear();
}

void destroySwapchain(Vk& v) {
  if (!v.device) return;
  waitVk(v);
  v.vkDeviceWaitIdle(v.device);
  destroySwapchainImages(v);
  if (v.swapchain) v.vkDestroySwapchainKHR(v.device, v.swapchain, nullptr);
  v.swapchain = VK_NULL_HANDLE;
}

void destroyVkSurface(Vk& v) {
  destroySwapchain(v);
  if (v.surface) v.vkDestroySurfaceKHR(v.instance, v.surface, nullptr);
  v.surface = VK_NULL_HANDLE;
}

// Everything Vulkan; the JSUI context (made on this device) goes first.
void destroyVulkan(Host& h) {
  Vk& v = h.vk;
  if (v.device) {
    waitVk(v);
    v.vkDeviceWaitIdle(v.device);
  }
  if (h.gpu && h.renderer == Renderer::Vulkan) {
    jsui_gpu_destroy(h.gpu);
    h.gpu = nullptr;
  }
  if (v.device) {
    destroyVkSurface(v);
    destroyFrameImage(v);
    if (v.pipeline) v.vkDestroyPipeline(v.device, v.pipeline, nullptr);
    if (v.renderPass) v.vkDestroyRenderPass(v.device, v.renderPass, nullptr);
    if (v.pipelineLayout) v.vkDestroyPipelineLayout(v.device, v.pipelineLayout, nullptr);
    if (v.descriptorPool) v.vkDestroyDescriptorPool(v.device, v.descriptorPool, nullptr);
    if (v.setLayout) v.vkDestroyDescriptorSetLayout(v.device, v.setLayout, nullptr);
    if (v.sampler) v.vkDestroySampler(v.device, v.sampler, nullptr);
    if (v.acquired) v.vkDestroySemaphore(v.device, v.acquired, nullptr);
    if (v.drawn) v.vkDestroySemaphore(v.device, v.drawn, nullptr);
    if (v.fence) v.vkDestroyFence(v.device, v.fence, nullptr);
    if (v.pool) v.vkDestroyCommandPool(v.device, v.pool, nullptr);
    v.vkDestroyDevice(v.device, nullptr);
  } else if (v.surface && v.vkDestroySurfaceKHR) {
    v.vkDestroySurfaceKHR(v.instance, v.surface, nullptr);
  }
  if (v.instance && v.vkDestroyInstance) v.vkDestroyInstance(v.instance, nullptr);
  void* lib = v.lib;
  v = Vk{};
  v.lib = lib;  // kept loaded (the driver may still hold references to it)
}

// The swapchain for the window's current size, pre-rotated to the display's
// current transform (so the compositor does not rotate it: the present pass
// does), with its framebuffers.
bool createSwapchain(Host& h) {
  Vk& v = h.vk;
  VkSurfaceCapabilitiesKHR caps;
  if (v.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.physical, v.surface, &caps) != VK_SUCCESS) return false;
  uint32_t n = 0;
  v.vkGetPhysicalDeviceSurfaceFormatsKHR(v.physical, v.surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  v.vkGetPhysicalDeviceSurfaceFormatsKHR(v.physical, v.surface, &n, formats.data());
  VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (const VkSurfaceFormatKHR& f : formats)
    if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && (format.format == VK_FORMAT_UNDEFINED || f.format == kFrameFormat))
      format = f;
  if (format.format == VK_FORMAT_UNDEFINED || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return false;
  VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
  int rotation = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? 1
                 : transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
                 : transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                        : 0;
  if (rotation == 0) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;  // (mirrored transforms: left to the compositor)
  // `extent`: the window as the UI sees it; the images are in the display's
  // natural orientation (the other way round after a quarter turn).
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == 0xFFFFFFFF)
    extent = {uint32_t(ANativeWindow_getWidth(h.app->window)), uint32_t(ANativeWindow_getHeight(h.app->window))};
  if (extent.width == 0 || extent.height == 0) return false;
  VkExtent2D imageExtent = extent;
  if (rotation & 1) std::swap(imageExtent.width, imageExtent.height);
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = v.surface;
  sci.minImageCount = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = format.format;
  sci.imageColorSpace = format.colorSpace;
  sci.imageExtent = imageExtent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = transform;
  sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                           ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                           : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = v.swapchain;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkResult r = v.vkCreateSwapchainKHR(v.device, &sci, nullptr, &swapchain);
  destroySwapchain(v);
  if (r != VK_SUCCESS || !createPresentPass(v, format.format)) {
    if (swapchain) v.vkDestroySwapchainKHR(v.device, swapchain, nullptr);
    return false;
  }
  v.swapchain = swapchain;
  v.extent = imageExtent;
  v.rotation = rotation;
  v.outOfDate = false;
  LOGI("vulkan swapchain: %ux%u, format %d, rotation %d", imageExtent.width, imageExtent.height, int(format.format),
       rotation * 90);
  v.vkGetSwapchainImagesKHR(v.device, v.swapchain, &n, nullptr);
  v.images.resize(n);
  v.vkGetSwapchainImagesKHR(v.device, v.swapchain, &n, v.images.data());
  for (VkImage image : v.images) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (v.vkCreateImageView(v.device, &vci, nullptr, &view) != VK_SUCCESS) return false;
    v.views.push_back(view);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = v.renderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &view;
    fci.width = imageExtent.width;
    fci.height = imageExtent.height;
    fci.layers = 1;
    if (v.vkCreateFramebuffer(v.device, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    v.framebuffers.push_back(fb);
  }
  // The UI's frame: the window's size as the UI sees it.
  return createFrameImage(v, int(extent.width), int(extent.height));
}

// A surface + swapchain for the window (the device and JSUI context are
// made the first time). False when any of it fails: the caller falls back.
bool createVkSurface(Host& h) {
  Vk& v = h.vk;
  if (!v.device && !initVulkan(h)) {
    destroyVulkan(h);
    return false;
  }
  if (!h.gpu) {
    JsuiVulkanDevice d{};
    d.instance = v.instance;
    d.physical_device = v.physical;
    d.device = v.device;
    d.queue = v.queue;
    d.queue_family = v.family;
    d.api_version = VK_API_VERSION_1_1;  // what the device was created for
    d.get_instance_proc_addr = reinterpret_cast<void*>(v.getInstanceProcAddr);
    d.instance_extensions = v.instanceExtensions.data();
    d.instance_extension_count = int(v.instanceExtensions.size());
    d.device_extensions = v.deviceExtensions.data();
    d.device_extension_count = int(v.deviceExtensions.size());
    d.device_features2 = &v.features;
    h.gpu = jsui_gpu_create_vulkan(&d);
    if (!h.gpu) {
      LOGW("Skia Graphite rejected the Vulkan device");
      destroyVulkan(h);
      return false;
    }
    jsui_gpu_prepare_target(h.gpu, kFrameFormat);
    LOGI("renderer: vulkan (Skia Graphite)");
  }
  VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  asci.window = h.app->window;
  if (v.vkCreateAndroidSurfaceKHR(v.instance, &asci, nullptr, &v.surface) != VK_SUCCESS) return false;
  VkBool32 present = VK_FALSE;
  v.vkGetPhysicalDeviceSurfaceSupportKHR(v.physical, v.family, v.surface, &present);
  return present && createSwapchain(h);
}

// Renders into the retained frame (only the damage once it holds the last
// one), draws that into the next swapchain image and presents. 1:
// presented, 0: skipped (the swapchain is recreated next frame), -1: failed.
int vkFrame(Host& h, int w, int ht) {
  Vk& v = h.vk;
  if (w != v.frameW || ht != v.frameH) v.outOfDate = true;
  if (v.outOfDate && !createSwapchain(h)) return -1;
  waitVk(v);
  uint32_t index = 0;
  VkResult r = v.vkAcquireNextImageKHR(v.device, v.swapchain, UINT64_MAX, v.acquired, VK_NULL_HANDLE, &index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    v.outOfDate = true;
    return 0;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return -1;

  JsuiGpuTarget t{};
  t.texture = v.frame;
  t.format = kFrameFormat;
  t.width = v.frameW;
  t.height = v.frameH;
  t.flags = v.frameValid ? JSUI_GPU_INCREMENTAL : 0;
  t.usage = kFrameUsage;
  t.layout = v.frameValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  t.final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // Submitted (between full barriers) before it returns: the pass below,
  // submitted after it on the same queue, reads the finished frame.
  bool rendered = jsui_render_gpu_target(h.rt, h.gpu, &t, nullptr) == 0;
  v.frameValid = rendered;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  v.vkBeginCommandBuffer(v.cmd, &bi);
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = v.renderPass;
  rp.framebuffer = v.framebuffers[index];
  rp.renderArea = {{0, 0}, v.extent};
  v.vkCmdBeginRenderPass(v.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, 0, float(v.extent.width), float(v.extent.height), 0, 1};
  VkRect2D scissor{{0, 0}, v.extent};
  v.vkCmdSetViewport(v.cmd, 0, 1, &viewport);
  v.vkCmdSetScissor(v.cmd, 0, 1, &scissor);
  v.vkCmdBindPipeline(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipeline);
  v.vkCmdBindDescriptorSets(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipelineLayout, 0, 1, &v.set, 0, nullptr);
  int32_t rotation = v.rotation;
  v.vkCmdPushConstants(v.cmd, v.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rotation, &rotation);
  v.vkCmdDraw(v.cmd, 3, 1, 0, 0);
  v.vkCmdEndRenderPass(v.cmd);
  v.vkEndCommandBuffer(v.cmd);
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &v.acquired;
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &v.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &v.drawn;
  if (v.vkQueueSubmit(v.queue, 1, &si, v.fence) != VK_SUCCESS) return -1;
  v.inFlight = true;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &v.drawn;
  pi.swapchainCount = 1;
  pi.pSwapchains = &v.swapchain;
  pi.pImageIndices = &index;
  r = v.vkQueuePresentKHR(v.queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    v.outOfDate = true;
  } else if (r == VK_SUBOPTIMAL_KHR) {
    // Still presented; recreating every frame would cost more than it helps.
    if (!v.suboptimalLogged) LOGW("vulkan: swapchain suboptimal");
    v.suboptimalLogged = true;
  } else if (r != VK_SUCCESS) {
    return -1;
  }
  return rendered ? 1 : -1;
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
  std::string jsThread = intentExtra(h, "jsui_js_thread");
  cfg.js_thread = jsThread == "off" ? JSUI_JS_THREAD_OFF : jsThread == "lockstep" ? JSUI_JS_THREAD_LOCKSTEP : JSUI_JS_THREAD_ON;
  // APK assets and http(s):// (onRequest); file paths go to the built-in loader.
  if (!h.net && net::initJni(h.env)) {
    h.net = std::make_shared<net::Net>();
    h.net->vm = h.app->activity->vm;
  }
  if (!h.net) LOGW("network: JNI setup failed; https:// is unavailable");
  cfg.on_request = onRequest;
  cfg.on_request_cancel = onRequestCancel;
  cfg.request_user = &h;
  if (url.empty()) url = "asset:///ui/bundle.js";
  h.rt = jsui_create(&cfg);
  LOGI("loading %s (%dx%d px, scale %.2f)", url.c_str(), h.pxW, h.pxH, h.scale);
  jsui_load_bundle_url(h.rt, url.c_str());
}

void drawFrame(Host& h) {
  if (!h.rt || !h.hasWindow) return;
  int w = ANativeWindow_getWidth(h.app->window), ht = ANativeWindow_getHeight(h.app->window);
  if (h.renderer == Renderer::GL && h.surface != EGL_NO_SURFACE) {
    EGLint ew = 0, eh = 0;
    eglQuerySurface(h.display, h.surface, EGL_WIDTH, &ew);
    eglQuerySurface(h.display, h.surface, EGL_HEIGHT, &eh);
    if (ew > 0 && eh > 0) w = ew, ht = eh;
  }
  if (w <= 0 || ht <= 0) return;
  updateViewport(h, w, ht);
  if (!h.safeAreaKnown) h.safeAreaKnown = publishSafeArea(h);

  jsui_tick(h.rt, h.frameTime);
  if (h.presented && !jsui_needs_redraw(h.rt)) return;  // the screen already shows this frame
  double t0 = nowMs();
  if (h.renderer == Renderer::Vulkan) {
    int r = h.vk.swapchain ? vkFrame(h, w, ht) : -1;
    if (r < 0) {
      useGl(h, "Vulkan frame failed");
      createSurface(h);
      return;
    }
    if (r == 0) return;  // swapchain out of date: recreated next frame
  } else if (!h.cpu) {
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
  double took = nowMs() - t0;
  h.renderMs += took;
  h.renderMaxMs = std::max(h.renderMaxMs, took);
  h.presented = true;
  if (const char* err = jsui_last_error(h.rt)) LOGW("%s", err);

  h.framesDrawn++;
  double now = nowMs();
  if (h.fpsSince == 0) h.fpsSince = now;
  if (now - h.fpsSince >= 5000) {
    static const char* names[] = {"vulkan", "gl", "cpu"};
    LOGI("%.1f frames drawn / s, %.2f ms render + present each, %.1f ms the longest (%s)",
         h.framesDrawn * 1000.0 / (now - h.fpsSince), h.renderMs / h.framesDrawn, h.renderMaxMs, names[int(h.renderer)]);
    h.framesDrawn = 0;
    h.renderMs = h.renderMaxMs = 0;
    h.fpsSince = now;
  }
}

void onVsync(int64_t frameTimeNanos, void* data) {
  auto& h = *static_cast<Host*>(data);
  h.frameScheduled = false;
  h.frameTime = std::max(h.frameTime, double(frameTimeNanos) / 1e6);
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
      if (h.renderer == Renderer::Vulkan) destroyVkSurface(h.vk);
      else destroySurface(h);
      break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_WINDOW_REDRAW_NEEDED:
      h.vk.outOfDate = h.vk.swapchain != VK_NULL_HANDLE;  // a rotation keeps the size
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
      // Also saves the Vulkan driver's pipeline cache (the process may not
      // come back).
      if (h.gpu && h.renderer == Renderer::Vulkan) jsui_gpu_purge(h.gpu, 0);
      break;
    case APP_CMD_LOW_MEMORY:
      if (h.gpu && (h.renderer == Renderer::Vulkan || h.surface != EGL_NO_SURFACE)) jsui_gpu_purge(h.gpu, 0);
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
  std::string renderer = intentExtra(h, "jsui_renderer");
  h.renderer = renderer == "cpu" ? Renderer::Cpu : renderer == "gl" ? Renderer::GL : Renderer::Vulkan;
  h.cpu = h.renderer == Renderer::Cpu;
  std::string cache = cacheDir(h);
  if (!cache.empty()) jsui_set_cache_dir(cache.c_str());

  while (!app->destroyRequested) {
    android_poll_source* source = nullptr;
    int result = ALooper_pollOnce(-1, nullptr, nullptr, reinterpret_cast<void**>(&source));
    if (result == ALOOPER_POLL_ERROR) break;
    if (source) source->process(app, source);
  }

  if (h.net) net::shutdown(*h.net);  // nothing answers the runtime after this
  if (h.rt) jsui_destroy(h.rt);
  if (h.renderer == Renderer::Vulkan) destroyVulkan(h);
  if (h.gpu) {
    if (h.surface == EGL_NO_SURFACE) jsui_gpu_context_lost(h.gpu);
    jsui_gpu_destroy(h.gpu);
  }
  destroySurface(h);
  if (h.context != EGL_NO_CONTEXT) eglDestroyContext(h.display, h.context);
  if (h.display != EGL_NO_DISPLAY) eglTerminate(h.display);
  app->activity->vm->DetachCurrentThread();
}
