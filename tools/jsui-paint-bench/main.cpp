// jsui-paint-bench: paint-time benchmark of bundles on the CPU raster and on
// native Ganesh GL, with deterministic simulated time.
//
//   jsui-paint-bench [--frames N] [--warmup N] [--settle-ms MS] [--width W]
//                    [--height H] [--scale S] [--backends cpu,cpu-full,gl,gl-direct]
//                    [--json out.json] name=bundle.js[@hover] ...
//
// Per scenario and backend it loads the bundle, runs --settle-ms of simulated
// time (entrance animations, image loads), then ticks --warmup + --frames
// frames at 60 Hz and times only the paint call:
//   cpu       jsui_render_rgba_incremental into one retained buffer (what game
//             hosts and the browser CPU path do): damage-only repaints;
//   cpu-full  jsui_render_rgba: the whole frame every frame;
//   gl        jsui_render_gl into a texture FBO (Ganesh) on a headless GL
//             context (Linux: EGL, Mesa's surfaceless platform works without a
//             display; macOS: CGL). Frames where jsui_needs_redraw() is false
//             are skipped like a host would. "paint" is the CPU time of the
//             render call (issuing GL), "gpu" adds glFinish().
//   gl-direct gl with JSUI_GL_NO_RETAIN: the whole frame into the target.
// `@hover` sweeps the mouse over the left part of the screen every frame.
//
// Reported per frame: mean / p95 / max paint ms, painted frames, mean pixels
// repainted, mean draw calls the painter issued (counted in a second, untimed
// pass with the painter's counting canvas), text / image draws, save layers,
// and for gl the GL draw calls, flushes, readbacks and texture uploads seen
// through the GL loader.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "input/input.h"
#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/painter.h"
#include "runtime.h"

#if defined(JSUI_GPU) && (defined(__linux__) || defined(__APPLE__))
#define BENCH_GL 1
#include <dlfcn.h>
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#endif
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  int frames = 240, warmup = 30;
  double settleMs = 1500;
  int width = 1280, height = 720;
  float scale = 1;
  std::vector<std::string> backends = {"cpu", "gl"};
  std::string json;
  bool count = true;  // --no-count skips the counting pass
};

struct Scenario {
  std::string name, bundle;
  bool hover = false;
};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= s.size()) {
    size_t b = s.find(sep, a);
    if (b == std::string::npos) b = s.size();
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

// ---------------------------------------------------------------- GL counts

struct GLCounts {
  int64_t draws = 0, flushes = 0, finishes = 0, readPixels = 0, texUploads = 0, texUploadBytes = 0,
          bufferBytes = 0, programs = 0, syncWaits = 0, links = 0, fboBinds = 0;
};
GLCounts g_gl;

#if BENCH_GL

using GLenum = unsigned;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned;
using GLsizeiptr = intptr_t;
using GLintptr = intptr_t;

void* (*g_realProc)(const char*) = nullptr;

#define REAL(name, ...) using name##_fn = __VA_ARGS__; static name##_fn name##_real = nullptr
REAL(glDrawArrays, void (*)(GLenum, GLint, GLsizei));
REAL(glDrawElements, void (*)(GLenum, GLsizei, GLenum, const void*));
REAL(glDrawArraysInstanced, void (*)(GLenum, GLint, GLsizei, GLsizei));
REAL(glDrawElementsInstanced, void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei));
REAL(glDrawRangeElements, void (*)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*));
REAL(glDrawArraysInstancedBaseInstance, void (*)(GLenum, GLint, GLsizei, GLsizei, GLuint));
REAL(glDrawElementsInstancedBaseVertexBaseInstance, void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint));
REAL(glFlush, void (*)());
REAL(glFinish, void (*)());
REAL(glReadPixels, void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*));
REAL(glTexSubImage2D, void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*));
REAL(glTexImage2D, void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
REAL(glBufferData, void (*)(GLenum, GLsizeiptr, const void*, GLenum));
REAL(glBufferSubData, void (*)(GLenum, GLintptr, GLsizeiptr, const void*));
REAL(glUseProgram, void (*)(GLuint));
REAL(glClientWaitSync, GLenum (*)(void*, unsigned, uint64_t));
REAL(glLinkProgram, void (*)(GLuint));
REAL(glBindFramebuffer, void (*)(GLenum, GLuint));
#undef REAL

void w_glDrawArrays(GLenum m, GLint f, GLsizei c) { g_gl.draws++; glDrawArrays_real(m, f, c); }
void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) { g_gl.draws++; glDrawElements_real(m, c, t, i); }
void w_glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { g_gl.draws++; glDrawArraysInstanced_real(m, f, c, n); }
void w_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n) {
  g_gl.draws++;
  glDrawElementsInstanced_real(m, c, t, i, n);
}
void w_glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i) {
  g_gl.draws++;
  glDrawRangeElements_real(m, s, e, c, t, i);
}
void w_glDrawArraysInstancedBaseInstance(GLenum m, GLint f, GLsizei c, GLsizei n, GLuint b) {
  g_gl.draws++;
  glDrawArraysInstancedBaseInstance_real(m, f, c, n, b);
}
void w_glDrawElementsInstancedBaseVertexBaseInstance(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint v,
                                                     GLuint b) {
  g_gl.draws++;
  glDrawElementsInstancedBaseVertexBaseInstance_real(m, c, t, i, n, v, b);
}
void w_glFlush() { g_gl.flushes++; glFlush_real(); }
void w_glFinish() { g_gl.finishes++; glFinish_real(); }
void w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p) {
  g_gl.readPixels++;
  glReadPixels_real(x, y, w, h, f, t, p);
}
void w_glTexSubImage2D(GLenum tg, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, const void* p) {
  g_gl.texUploads++;
  g_gl.texUploadBytes += int64_t(w) * h * 4;
  glTexSubImage2D_real(tg, l, x, y, w, h, f, t, p);
}
void w_glTexImage2D(GLenum tg, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum t, const void* p) {
  if (p) {
    g_gl.texUploads++;
    g_gl.texUploadBytes += int64_t(w) * h * 4;
  }
  glTexImage2D_real(tg, l, i, w, h, b, f, t, p);
}
void w_glBufferData(GLenum t, GLsizeiptr n, const void* p, GLenum u) {
  if (p) g_gl.bufferBytes += n;
  glBufferData_real(t, n, p, u);
}
void w_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void* p) {
  g_gl.bufferBytes += n;
  glBufferSubData_real(t, o, n, p);
}
void w_glUseProgram(GLuint p) { g_gl.programs++; glUseProgram_real(p); }
void w_glLinkProgram(GLuint p) { g_gl.links++; glLinkProgram_real(p); }
void w_glBindFramebuffer(GLenum t, GLuint f) { g_gl.fboBinds++; glBindFramebuffer_real(t, f); }
GLenum w_glClientWaitSync(void* s, unsigned f, uint64_t t) {
  g_gl.syncWaits++;
  return glClientWaitSync_real(s, f, t);
}

// The loader handed to jsui_gpu_create_gl: counting wrappers for the calls
// above (also their EXT / ARB / OES variants), the real entry point else.
void* countingProc(void*, const char* name) {
  void* real = g_realProc(name);
  if (!real) return nullptr;
  std::string n = name;
  for (const char* suf : {"EXT", "ARB", "OES", "ANGLE"}) {
    size_t l = std::strlen(suf);
    if (n.size() > l && n.compare(n.size() - l, l, suf) == 0) {
      n.resize(n.size() - l);
      break;
    }
  }
#define WRAP(fn)                                              \
  if (n == #fn) {                                             \
    fn##_real = reinterpret_cast<decltype(fn##_real)>(real);  \
    return reinterpret_cast<void*>(&w_##fn);                  \
  }
  WRAP(glDrawArrays)
  WRAP(glDrawElements)
  WRAP(glDrawArraysInstanced)
  WRAP(glDrawElementsInstanced)
  WRAP(glDrawRangeElements)
  WRAP(glDrawArraysInstancedBaseInstance)
  WRAP(glDrawElementsInstancedBaseVertexBaseInstance)
  WRAP(glFlush)
  WRAP(glFinish)
  WRAP(glReadPixels)
  WRAP(glTexSubImage2D)
  WRAP(glTexImage2D)
  WRAP(glBufferData)
  WRAP(glBufferSubData)
  WRAP(glUseProgram)
  WRAP(glClientWaitSync)
  WRAP(glLinkProgram)
  WRAP(glBindFramebuffer)
#undef WRAP
  return real;
}

constexpr unsigned GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
                   GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                   GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600,
                   GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02;

struct GLFns {
  void (*GenTextures)(int, GLuint*);
  void (*DeleteTextures)(int, const GLuint*);
  void (*BindTexture)(unsigned, GLuint);
  void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
  void (*TexParameteri)(unsigned, unsigned, int);
  void (*GenFramebuffers)(int, GLuint*);
  void (*DeleteFramebuffers)(int, const GLuint*);
  void (*BindFramebuffer)(unsigned, GLuint);
  void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, GLuint, int);
  unsigned (*CheckFramebufferStatus)(unsigned);
  void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
  void (*Finish)();
  const unsigned char* (*GetString)(unsigned);
  bool load() {
    bool ok = true;
    auto sym = [&](auto& f, const char* n) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(g_realProc(n));
      ok &= f != nullptr;
    };
    sym(GenTextures, "glGenTextures");
    sym(DeleteTextures, "glDeleteTextures");
    sym(BindTexture, "glBindTexture");
    sym(TexImage2D, "glTexImage2D");
    sym(TexParameteri, "glTexParameteri");
    sym(GenFramebuffers, "glGenFramebuffers");
    sym(DeleteFramebuffers, "glDeleteFramebuffers");
    sym(BindFramebuffer, "glBindFramebuffer");
    sym(FramebufferTexture2D, "glFramebufferTexture2D");
    sym(CheckFramebufferStatus, "glCheckFramebufferStatus");
    sym(ReadPixels, "glReadPixels");
    sym(Finish, "glFinish");
    sym(GetString, "glGetString");
    return ok;
  }
};
GLFns gl;

#if defined(__linux__)
// Headless EGL: Mesa's surfaceless platform, else the default display + a
// pbuffer. Desktop GL (compatibility) context.
struct HeadlessGL {
  void* lib = nullptr;
  void* dpy = nullptr;
  void* ctx = nullptr;
  void* surf = nullptr;
  std::string error;
  bool create() {
    lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return fail("libEGL.so.1 not found");
    auto getProc = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "eglGetProcAddress"));
    auto getDisplay = reinterpret_cast<void* (*)(void*)>(dlsym(lib, "eglGetDisplay"));
    auto initialize = reinterpret_cast<unsigned (*)(void*, int*, int*)>(dlsym(lib, "eglInitialize"));
    auto chooseConfig = reinterpret_cast<unsigned (*)(void*, const int*, void**, int, int*)>(dlsym(lib, "eglChooseConfig"));
    auto bindAPI = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(lib, "eglBindAPI"));
    auto createContext = reinterpret_cast<void* (*)(void*, void*, void*, const int*)>(dlsym(lib, "eglCreateContext"));
    auto createPbuffer = reinterpret_cast<void* (*)(void*, void*, const int*)>(dlsym(lib, "eglCreatePbufferSurface"));
    auto makeCurrent = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(dlsym(lib, "eglMakeCurrent"));
    auto queryString = reinterpret_cast<const char* (*)(void*, int)>(dlsym(lib, "eglQueryString"));
    if (!getProc || !getDisplay || !initialize || !chooseConfig || !bindAPI || !createContext || !makeCurrent)
      return fail("EGL entry points missing");
    g_realProc = getProc;
    auto platformDisplay = reinterpret_cast<void* (*)(unsigned, void*, const int*)>(getProc("eglGetPlatformDisplayEXT"));
    const char* ext = queryString ? queryString(nullptr, 0x3055) : nullptr;
    if (platformDisplay && ext && std::strstr(ext, "EGL_MESA_platform_surfaceless"))
      dpy = platformDisplay(0x31DD, nullptr, nullptr);
    if (dpy && !initialize(dpy, nullptr, nullptr)) dpy = nullptr;
    bool surfaceless = dpy != nullptr;
    if (!dpy) {
      dpy = getDisplay(nullptr);
      if (!dpy || !initialize(dpy, nullptr, nullptr)) return fail("eglInitialize failed");
    }
    const int attrs[] = {0x3040, 0x0008, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3033, 0x0001, 0x3038};
    void* cfg = nullptr;
    int n = 0;
    if (!chooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) return fail("no EGL config");
    if (!bindAPI(0x30A2)) return fail("eglBindAPI failed");
    ctx = createContext(dpy, cfg, nullptr, nullptr);
    if (!ctx) return fail("eglCreateContext failed");
    if (!surfaceless && createPbuffer) {
      const int pb[] = {0x3057, 16, 0x3056, 16, 0x3038};
      surf = createPbuffer(dpy, cfg, pb);
    }
    if (!makeCurrent(dpy, surf, surf, ctx)) return fail("eglMakeCurrent failed");
    return true;
  }
  bool fail(const char* e) {
    error = e;
    return false;
  }
};
#else
void* cglProc(const char* name) { return dlsym(RTLD_DEFAULT, name); }
struct HeadlessGL {
  CGLContextObj ctx = nullptr;
  std::string error;
  bool create() {
    g_realProc = cglProc;
    for (bool software : {false, true}) {
      std::vector<CGLPixelFormatAttribute> attrs = {kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(kCGLOGLPVersion_3_2_Core),
                                                    kCGLPFAColorSize, CGLPixelFormatAttribute(24), kCGLPFAAlphaSize,
                                                    CGLPixelFormatAttribute(8), kCGLPFAAllowOfflineRenderers};
      if (software) {
        attrs.push_back(kCGLPFARendererID);
        attrs.push_back(CGLPixelFormatAttribute(kCGLRendererGenericFloatID));
      }
      attrs.push_back(CGLPixelFormatAttribute(0));
      CGLPixelFormatObj pix = nullptr;
      GLint n = 0;
      if (CGLChoosePixelFormat(attrs.data(), &pix, &n) != kCGLNoError || !pix) continue;
      CGLError err = CGLCreateContext(pix, nullptr, &ctx);
      CGLDestroyPixelFormat(pix);
      if (err == kCGLNoError && ctx) break;
      ctx = nullptr;
    }
    if (!ctx) {
      error = "no CGL 3.2 core context";
      return false;
    }
    if (CGLSetCurrentContext(ctx) != kCGLNoError) {
      error = "CGLSetCurrentContext failed";
      return false;
    }
    return true;
  }
};
#endif

HeadlessGL* headlessGL(std::string* renderer) {
  static HeadlessGL* g = nullptr;
  static bool tried = false;
  static std::string name;
  if (!tried) {
    tried = true;
    auto* h = new HeadlessGL;
    if (h->create() && gl.load()) {
      g = h;
      const unsigned char* r = gl.GetString(GL_RENDERER);
      const unsigned char* v = gl.GetString(GL_VERSION);
      name = std::string(r ? reinterpret_cast<const char*>(r) : "?") + " / " + (v ? reinterpret_cast<const char*>(v) : "?");
    } else {
      std::fprintf(stderr, "gl: no headless GL context (%s)\n", h->error.empty() ? "GL entry points missing" : h->error.c_str());
    }
  }
  if (renderer) *renderer = name;
  return g;
}

#endif  // BENCH_GL

// ---------------------------------------------------------------- measuring

struct Sample {
  double paint = 0, gpu = 0;
  bool painted = false;
  int64_t pixels = 0;
  jsui::PaintStats stats;
  GLCounts gl;
};

struct Result {
  std::string scenario, backend;
  int frames = 0, painted = 0;
  double mean = 0, p50 = 0, p95 = 0, max = 0, gpuMean = 0, gpuP95 = 0;
  double pixels = 0;  // mean repainted pixels per frame (all frames)
  double draws = 0, textDraws = 0, imageDraws = 0, saveLayers = 0, clips = 0, nodes = 0;
  double glDraws = 0, glFlushes = 0, glReadPixels = 0, glTexUploads = 0, glTexUploadBytes = 0, glBufferBytes = 0,
         glPrograms = 0, glSyncWaits = 0, glFinishes = 0, glLinks = 0, glFboBinds = 0;
  bool ok = true;
  std::string note;
};

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = size_t(std::ceil(p * double(v.size()))) - 1;
  return v[std::min(i, v.size() - 1)];
}

std::unique_ptr<jsui::Runtime> makeRuntime(const Options& o, const Scenario& sc, double* t) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = o.width;
  cfg.height = o.height;
  cfg.pixel_ratio = o.scale;
  auto rt = std::make_unique<jsui::Runtime>(cfg, nullptr);
  rt->loadUrl(sc.bundle);
  auto settle = [&] {
    for (int round = 0; round < 5000 && rt->loader.pending() > 0; round++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt->tick(*t);
    }
  };
  *t = 0;
  rt->tick(*t);
  settle();
  for (; *t < o.settleMs; *t += 1000.0 / 60) {
    rt->tick(*t);
    if (rt->loader.pending() > 0) settle();
  }
  return rt;
}

void hoverAt(jsui::Runtime& rt, int frame, const Options& o) {
  // A slow vertical zig-zag over the left third (menus, buttons, lists).
  float y = float(80 + std::fmod(frame * 7.0, double(o.height - 160)));
  float x = float(60 + 40 * std::sin(frame * 0.05));
  rt.input->mouseMove(x, y);
}

// One pass over the frames; `count` turns the painter's counting canvas on.
std::vector<Sample> runPass(const Options& o, const Scenario& sc, const std::string& backend, bool count,
                            std::string* error) {
  std::vector<Sample> out;
  double t = 0;
  auto rt = makeRuntime(o, sc, &t);
  if (!rt->lastError.empty()) {
    *error = rt->lastError;
    return out;
  }
  jsui::setPaintCounting(rt->doc, count);
  int w = int(std::lround(o.width * o.scale)), h = int(std::lround(o.height * o.scale));
  std::vector<uint32_t> px(size_t(w) * h);

#if BENCH_GL
  JsuiGpuContext* gpu = nullptr;
  unsigned tex = 0, fbo = 0;
  if (backend.rfind("gl", 0) == 0) {
    if (!headlessGL(nullptr)) {
      *error = "no GL context";
      return out;
    }
    gpu = jsui::gpu::createGL(countingProc, nullptr);
    if (!gpu) {
      *error = "jsui_gpu_create_gl failed";
      return out;
    }
    gl.GenTextures(1, &tex);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      *error = "FBO incomplete";
      return out;
    }
  }
#else
  if (backend.rfind("gl", 0) == 0) {
    *error = "built without JSUI_GPU";
    return out;
  }
#endif

  for (int i = 0; i < o.warmup + o.frames; i++) {
    t += 1000.0 / 60;
    if (sc.hover) hoverAt(*rt, i, o);
    rt->tick(t);
    Sample s;
    if (backend == "cpu") {
      auto a = Clock::now();
      SkIRect r = rt->renderIncremental(px.data(), w, h, size_t(w) * 4);
      auto b = Clock::now();
      s.paint = std::chrono::duration<double, std::milli>(b - a).count();
      s.painted = !r.isEmpty();
    } else if (backend == "cpu-full") {
      auto a = Clock::now();
      rt->render(px.data(), w, h, size_t(w) * 4);
      auto b = Clock::now();
      s.paint = std::chrono::duration<double, std::milli>(b - a).count();
      s.painted = true;
    }
#if BENCH_GL
    else if (backend.rfind("gl", 0) == 0) {
      if (rt->needsRedraw()) {
        GLCounts before = g_gl;
        JsuiGLTarget target{};
        target.fbo = fbo;
        target.width = w;
        target.height = h;
        if (backend == "gl-direct") target.flags = JSUI_GL_NO_RETAIN;
        auto a = Clock::now();
        int rc = jsui::gpu::render(rt->main(), gpu, target);
        auto b = Clock::now();
        glFinish_real ? glFinish_real() : gl.Finish();
        auto c = Clock::now();
        if (rc != 0) {
          *error = "jsui_render_gl failed";
          break;
        }
        s.paint = std::chrono::duration<double, std::milli>(b - a).count();
        s.gpu = std::chrono::duration<double, std::milli>(c - a).count();
        s.painted = true;
        s.gl.draws = g_gl.draws - before.draws;
        s.gl.flushes = g_gl.flushes - before.flushes;
        s.gl.finishes = g_gl.finishes - before.finishes;
        s.gl.readPixels = g_gl.readPixels - before.readPixels;
        s.gl.texUploads = g_gl.texUploads - before.texUploads;
        s.gl.texUploadBytes = g_gl.texUploadBytes - before.texUploadBytes;
        s.gl.bufferBytes = g_gl.bufferBytes - before.bufferBytes;
        s.gl.programs = g_gl.programs - before.programs;
        s.gl.syncWaits = g_gl.syncWaits - before.syncWaits;
        s.gl.links = g_gl.links - before.links;
        s.gl.fboBinds = g_gl.fboBinds - before.fboBinds;
      }
    }
#endif
    if (s.painted) {
      s.stats = jsui::lastPaintStats(rt->doc);
      s.pixels = s.stats.repaintedPixels;
    }
    if (i >= o.warmup) out.push_back(s);
  }
#if BENCH_GL
  if (gpu) {
    jsui::gpu::destroy(gpu);
    gl.DeleteFramebuffers(1, &fbo);
    gl.DeleteTextures(1, &tex);
  }
#endif
  return out;
}

Result measure(const Options& o, const Scenario& sc, const std::string& backend) {
  Result r;
  r.scenario = sc.name;
  r.backend = backend;
  std::string err;
  std::vector<Sample> timed = runPass(o, sc, backend, false, &err);
  std::vector<Sample> counted;
  if (err.empty() && o.count) counted = runPass(o, sc, backend, true, &err);
  if (!err.empty() || timed.empty()) {
    r.ok = false;
    r.note = err.empty() ? "no frames" : err;
    return r;
  }
  std::vector<double> paint, gpuT;
  r.frames = int(timed.size());
  for (const Sample& s : timed) {
    paint.push_back(s.paint);
    if (s.painted) {
      r.painted++;
      gpuT.push_back(s.gpu);
    }
    r.pixels += double(s.pixels);
    r.glDraws += double(s.gl.draws);
    r.glFlushes += double(s.gl.flushes);
    r.glFinishes += double(s.gl.finishes);
    r.glReadPixels += double(s.gl.readPixels);
    r.glTexUploads += double(s.gl.texUploads);
    r.glTexUploadBytes += double(s.gl.texUploadBytes);
    r.glBufferBytes += double(s.gl.bufferBytes);
    r.glPrograms += double(s.gl.programs);
    r.glSyncWaits += double(s.gl.syncWaits);
    r.glLinks += double(s.gl.links);
    r.glFboBinds += double(s.gl.fboBinds);
  }
  for (const Sample& s : counted) {
    if (!s.painted) continue;
    r.draws += s.stats.draws;
    r.textDraws += s.stats.textDraws;
    r.imageDraws += s.stats.imageDraws;
    r.saveLayers += s.stats.saveLayers;
    r.clips += s.stats.clips;
    r.nodes += s.stats.nodesPainted;
  }
  double n = double(r.frames);
  for (double& v : paint) r.mean += v / n;
  r.p50 = pct(paint, 0.5);
  r.p95 = pct(paint, 0.95);
  r.max = pct(paint, 1.0);
  for (double v : gpuT) r.gpuMean += v / double(gpuT.size());
  r.gpuP95 = pct(gpuT, 0.95);
  r.pixels /= n;
  for (double* v : {&r.draws, &r.textDraws, &r.imageDraws, &r.saveLayers, &r.clips, &r.nodes}) *v /= n;
  for (double* v : {&r.glDraws, &r.glFlushes, &r.glFinishes, &r.glReadPixels, &r.glTexUploads, &r.glTexUploadBytes,
                    &r.glBufferBytes, &r.glPrograms, &r.glSyncWaits, &r.glLinks, &r.glFboBinds})
    *v /= n;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::vector<Scenario> scenarios;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--frames") o.frames = std::max(1, std::atoi(next().c_str()));
    else if (a == "--warmup") o.warmup = std::max(0, std::atoi(next().c_str()));
    else if (a == "--settle-ms") o.settleMs = std::atof(next().c_str());
    else if (a == "--width") o.width = std::atoi(next().c_str());
    else if (a == "--height") o.height = std::atoi(next().c_str());
    else if (a == "--scale") o.scale = float(std::atof(next().c_str()));
    else if (a == "--backends") o.backends = split(next(), ',');
    else if (a == "--json") o.json = next();
    else if (a == "--no-count") o.count = false;
    else if (a == "-h" || a == "--help") {
      std::printf("usage: jsui-paint-bench [--frames N] [--warmup N] [--settle-ms MS] [--width W] [--height H] "
                  "[--scale S] [--backends cpu,cpu-full,gl,gl-direct] [--json out.json] name=bundle.js[@hover] ...\n");
      return 0;
    } else {
      Scenario sc;
      size_t eq = a.find('=');
      sc.bundle = eq == std::string::npos ? a : a.substr(eq + 1);
      if (sc.bundle.size() > 6 && sc.bundle.compare(sc.bundle.size() - 6, 6, "@hover") == 0) {
        sc.hover = true;
        sc.bundle.resize(sc.bundle.size() - 6);
      }
      sc.name = eq == std::string::npos ? sc.bundle : a.substr(0, eq);
      scenarios.push_back(sc);
    }
  }
  if (scenarios.empty()) {
    std::fprintf(stderr, "no scenarios (see --help)\n");
    return 2;
  }
  std::string glName;
#if BENCH_GL
  for (const std::string& b : o.backends)
    if (b.rfind("gl", 0) == 0 && glName.empty()) headlessGL(&glName);
#endif
  std::printf("jsui-paint-bench %dx%d @%gx, %d frames (+%d warmup) at 60 Hz%s%s\n", o.width, o.height, o.scale, o.frames,
              o.warmup, glName.empty() ? "" : ", GL: ", glName.c_str());
  std::printf("%-20s %-8s %7s %7s %7s %7s %7s %10s %7s %6s %6s %6s %7s %7s %6s %6s %6s %7s\n", "scenario", "backend",
              "paint", "p95", "max", "gpu", "gpuP95", "px/frame", "draws", "text", "image", "layer", "nodes",
              "glDraw", "fbo", "rdback", "links", "upKB");
  std::vector<Result> results;
  int failures = 0;
  for (const Scenario& sc : scenarios) {
    for (const std::string& b : o.backends) {
      Result r = measure(o, sc, b);
      results.push_back(r);
      if (!r.ok) {
        failures++;
        std::printf("%-20s %-8s FAILED: %s\n", r.scenario.c_str(), r.backend.c_str(), r.note.c_str());
        continue;
      }
      std::printf("%-20s %-8s %7.3f %7.3f %7.3f %7.3f %7.3f %10.0f %7.1f %6.1f %6.1f %6.2f %7.1f %7.1f %6.2f %6.2f %6.2f %7.1f\n",
                  r.scenario.c_str(), r.backend.c_str(), r.mean, r.p95, r.max, r.gpuMean, r.gpuP95, r.pixels, r.draws,
                  r.textDraws, r.imageDraws, r.saveLayers, r.nodes, r.glDraws, r.glFboBinds, r.glReadPixels, r.glLinks,
                  r.glTexUploadBytes / 1024);
      std::fflush(stdout);
    }
  }
  std::printf("paint/p95/max: ms per frame of the paint call (all frames; idle ones cost ~0); gpu: with glFinish,\n"
              "painted frames only; px/frame, draws, text, image, layer, nodes, glDraw, fbo (glBindFramebuffer), rdback (glReadPixels), links\n"
              "(glLinkProgram), upKB (texture uploads): means per frame.\n");
  if (!o.json.empty()) {
    FILE* f = std::fopen(o.json.c_str(), "w");
    if (f) {
      std::fprintf(f, "{\"width\":%d,\"height\":%d,\"scale\":%g,\"frames\":%d,\"gl\":\"%s\",\"results\":[\n", o.width,
                   o.height, o.scale, o.frames, glName.c_str());
      for (size_t i = 0; i < results.size(); i++) {
        const Result& r = results[i];
        std::fprintf(f,
                     "  {\"scenario\":\"%s\",\"backend\":\"%s\",\"ok\":%s,\"frames\":%d,\"painted\":%d,\"meanMs\":%.4f,"
                     "\"p50Ms\":%.4f,\"p95Ms\":%.4f,\"maxMs\":%.4f,\"gpuMeanMs\":%.4f,\"gpuP95Ms\":%.4f,\"pixels\":%.0f,"
                     "\"draws\":%.2f,\"textDraws\":%.2f,\"imageDraws\":%.2f,\"saveLayers\":%.2f,\"clips\":%.2f,"
                     "\"nodes\":%.2f,\"glDraws\":%.2f,\"glFlushes\":%.3f,\"glFinishes\":%.3f,\"glReadPixels\":%.3f,"
                     "\"glTexUploads\":%.3f,\"glTexUploadBytes\":%.0f,\"glBufferBytes\":%.0f,\"glPrograms\":%.2f,"
                     "\"glSyncWaits\":%.3f,\"glLinks\":%.3f,\"glFboBinds\":%.2f}%s\n",
                     r.scenario.c_str(), r.backend.c_str(), r.ok ? "true" : "false", r.frames, r.painted, r.mean, r.p50,
                     r.p95, r.max, r.gpuMean, r.gpuP95, r.pixels, r.draws, r.textDraws, r.imageDraws, r.saveLayers,
                     r.clips, r.nodes, r.glDraws, r.glFlushes, r.glFinishes, r.glReadPixels, r.glTexUploads,
                     r.glTexUploadBytes, r.glBufferBytes, r.glPrograms, r.glSyncWaits, r.glLinks, r.glFboBinds,
                     i + 1 < results.size() ? "," : "");
      }
      std::fprintf(f, "]}\n");
      std::fclose(f);
    }
  }
  return failures ? 1 : 0;
}
