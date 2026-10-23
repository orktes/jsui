// GPU backend tests: render scenes with Skia Ganesh into a real GL context
// and compare with the CPU raster output.
//
// Linux: the GL context comes from EGL (loaded with dlopen, so the test
// binary has no GL link dependency): Mesa's surfaceless platform (llvmpipe
// works without a display or GPU), else the default display with a pbuffer.
// Desktop GL (default / compatibility), a 3.2 core profile (what macOS
// gives, with no default vertex array object) and GLES 3 / 2 are tried.
// macOS: a headless CGL 3.2 core profile context (hardware, else Apple's
// software renderer). Without a usable context (Windows, minimal
// containers) every case prints SKIP and passes. JSUI_GPU_TEST_REQUIRE=1
// turns the skip into a failure (CI jobs with a known GL setup set it, so a
// broken setup cannot silently skip).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "render/gpu.h"
#include "render_util.h"
#include "test.h"

#if defined(__linux__) && defined(JSUI_GPU)
#define JSUI_GPU_TESTS 1
#define JSUI_GPU_EGL 1
#include <dlfcn.h>
#elif defined(__APPLE__) && defined(JSUI_GPU)
#define JSUI_GPU_TESTS 1
#define JSUI_GPU_CGL 1
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>  // CGL; jsui links the OpenGL framework on macOS
#include <dlfcn.h>
#endif

using rtest::Scene;

namespace {

bool requireGpu() {
  const char* v = std::getenv("JSUI_GPU_TEST_REQUIRE");
  return v && *v && std::strcmp(v, "0") != 0;
}

void skip(const char* test, const char* why) {
  if (requireGpu()) {
    std::fprintf(stderr, "  %s: no GL context (%s) but JSUI_GPU_TEST_REQUIRE is set\n", test, why);
    jsui_test::failures()++;
    return;
  }
  std::printf("SKIP %s: %s\n", test, why);
}

#if JSUI_GPU_TESTS

enum class Api { GL, GLCore, GLES3, GLES2 };
const char* apiName(Api a) {
  return a == Api::GL ? "GL" : a == Api::GLCore ? "GLCore" : a == Api::GLES3 ? "GLES3" : "GLES2";
}

#endif  // JSUI_GPU_TESTS

#if JSUI_GPU_EGL

// ---------------------------------------------------------------------- EGL

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint = int32_t;
using EGLenum = unsigned;
constexpr EGLint EGL_NONE = 0x3038, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_BIT = 0x0008,
                 EGL_OPENGL_ES2_BIT = 0x0004, EGL_OPENGL_ES3_BIT = 0x0040, EGL_SURFACE_TYPE = 0x3033,
                 EGL_PBUFFER_BIT = 0x0001, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056,
                 EGL_CONTEXT_MAJOR_VERSION = 0x3098, EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023,
                 EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021, EGL_CONTEXT_MINOR_VERSION = 0x30FB,
                 EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x0001,
                 EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE = 0x31B1, EGL_TRUE = 1;
constexpr EGLenum EGL_OPENGL_API = 0x30A2, EGL_OPENGL_ES_API = 0x30A0, EGL_PLATFORM_SURFACELESS_MESA = 0x31DD,
                  EGL_EXTENSIONS = 0x3055;

struct Egl {
  void* lib = nullptr;
  void* (*getProcAddress)(const char*) = nullptr;
  EGLDisplay (*getDisplay)(void*) = nullptr;
  EGLDisplay (*getPlatformDisplayEXT)(EGLenum, void*, const EGLint*) = nullptr;
  unsigned (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
  unsigned (*terminate)(EGLDisplay) = nullptr;
  unsigned (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
  unsigned (*bindAPI)(EGLenum) = nullptr;
  EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
  unsigned (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
  EGLSurface (*createPbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
  unsigned (*destroySurface)(EGLDisplay, EGLSurface) = nullptr;
  unsigned (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
  const char* (*queryString)(EGLDisplay, EGLint) = nullptr;

  bool load() {
    lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return false;
    auto sym = [&](auto& f, const char* n) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, n));
      return f != nullptr;
    };
    bool ok = sym(getProcAddress, "eglGetProcAddress") && sym(getDisplay, "eglGetDisplay") &&
              sym(initialize, "eglInitialize") && sym(terminate, "eglTerminate") &&
              sym(chooseConfig, "eglChooseConfig") && sym(bindAPI, "eglBindAPI") &&
              sym(createContext, "eglCreateContext") && sym(destroyContext, "eglDestroyContext") &&
              sym(createPbufferSurface, "eglCreatePbufferSurface") && sym(destroySurface, "eglDestroySurface") &&
              sym(makeCurrent, "eglMakeCurrent") && sym(queryString, "eglQueryString");
    if (ok) getPlatformDisplayEXT = reinterpret_cast<decltype(getPlatformDisplayEXT)>(getProcAddress("eglGetPlatformDisplayEXT"));
    return ok;
  }
};

#endif  // JSUI_GPU_EGL

#if JSUI_GPU_TESTS

// ----------------------------------------------------------------------- GL

// The few GL entry points the test itself needs (loaded per context).
struct GL {
  using GLuint = unsigned;
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
  void (*PixelStorei)(unsigned, int);
  void (*ClearColor)(float, float, float, float);
  void (*Clear)(unsigned);
  void (*Disable)(unsigned);
  void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
  void (*Finish)();
  const unsigned char* (*GetString)(unsigned);

  bool load(void* (*getProc)(const char*)) {
    bool ok = true;
    auto sym = [&](auto& f, const char* n) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(getProc(n));
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
    sym(PixelStorei, "glPixelStorei");
    sym(ClearColor, "glClearColor");
    sym(Clear, "glClear");
    sym(Disable, "glDisable");
    sym(ColorMask, "glColorMask");
    sym(Finish, "glFinish");
    sym(GetString, "glGetString");
    return ok;
  }
};
constexpr unsigned GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
                   GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                   GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600,
                   GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_COLOR_BUFFER_BIT = 0x4000, GL_SCISSOR_TEST = 0x0C11,
                   GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01;

#endif  // JSUI_GPU_TESTS

#if JSUI_GPU_EGL

// An EGL context made current on construction (ok() false if impossible).
struct GLContext {
  Egl& egl;
  EGLDisplay dpy = nullptr;
  EGLContext ctx = nullptr;
  EGLSurface surf = nullptr;
  GL gl{};
  std::string error;
  std::string renderer;

  GLContext(Egl& e, Api api) : egl(e) {
    if (egl.getPlatformDisplayEXT) {
      const char* ext = egl.queryString(nullptr, EGL_EXTENSIONS);  // client extensions
      if (ext && std::strstr(ext, "EGL_MESA_platform_surfaceless"))
        dpy = egl.getPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    }
    if (dpy && !egl.initialize(dpy, nullptr, nullptr)) dpy = nullptr;
    bool surfaceless = dpy != nullptr;
    if (!dpy) {
      dpy = egl.getDisplay(nullptr);
      if (!dpy || !egl.initialize(dpy, nullptr, nullptr)) {
        dpy = nullptr;
        error = "eglInitialize failed";
        return;
      }
    }
    bool desktop = api == Api::GL || api == Api::GLCore;
    EGLint renderable = desktop ? EGL_OPENGL_BIT : api == Api::GLES3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
    EGLint attrs[] = {EGL_RENDERABLE_TYPE, renderable, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                      EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};  // default is WINDOW_BIT
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!egl.chooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) {
      error = std::string("no EGL config for ") + apiName(api);
      return;
    }
    if (!egl.bindAPI(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API)) {
      error = "eglBindAPI failed";
      return;
    }
    EGLint esAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, api == Api::GLES3 ? 3 : 2, EGL_NONE};
    // Same request as jsui-viewer on macOS: 3.2 core, forward compatible.
    EGLint coreAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE, EGL_TRUE, EGL_NONE};
    ctx = egl.createContext(dpy, cfg, nullptr, api == Api::GL ? nullptr : api == Api::GLCore ? coreAttrs : esAttrs);
    if (!ctx) {
      error = std::string("eglCreateContext failed for ") + apiName(api);
      return;
    }
    if (!surfaceless) {
      EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
      surf = egl.createPbufferSurface(dpy, cfg, pb);
    }
    if (!egl.makeCurrent(dpy, surf, surf, ctx)) {
      error = "eglMakeCurrent failed";
      egl.destroyContext(dpy, ctx);
      ctx = nullptr;
      return;
    }
    if (!gl.load(egl.getProcAddress)) {
      error = "missing GL entry points";
      return;
    }
    const unsigned char* r = gl.GetString(GL_RENDERER);
    const unsigned char* v = gl.GetString(GL_VERSION);
    renderer = std::string(r ? reinterpret_cast<const char*>(r) : "?") + " / " +
               (v ? reinterpret_cast<const char*>(v) : "?");
  }
  ~GLContext() {
    if (dpy) {
      egl.makeCurrent(dpy, nullptr, nullptr, nullptr);
      if (ctx) egl.destroyContext(dpy, ctx);
      if (surf) egl.destroySurface(dpy, surf);
      // Not terminated: the display is shared by the other contexts of the run.
    }
  }
  bool ok() const { return ctx && error.empty(); }
};

Egl* egl() {
  static Egl e;
  static bool loaded = e.load();
  return loaded ? &e : nullptr;
}

void* eglProc(void*, const char* name) { return egl()->getProcAddress(name); }

#endif  // JSUI_GPU_EGL

#if JSUI_GPU_CGL

// ---------------------------------------------------------------------- CGL

void* cglGetProc(const char* name) { return dlsym(RTLD_DEFAULT, name); }
void* cglProc(void*, const char* name) { return cglGetProc(name); }

// A headless CGL context made current on construction. macOS only offers
// legacy 2.1 (which Skia's shaders cannot use) or core profiles; ask for
// 3.2 core like jsui-viewer does, on any renderer, else the software one.
struct GLContext {
  CGLContextObj ctx = nullptr;
  GL gl{};
  std::string error;
  std::string renderer;

  explicit GLContext(Api) {
    for (bool software : {false, true}) {
      std::vector<CGLPixelFormatAttribute> attrs = {
          kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(kCGLOGLPVersion_3_2_Core),
          kCGLPFAColorSize,     CGLPixelFormatAttribute(24),
          kCGLPFAAlphaSize,     CGLPixelFormatAttribute(8),
          kCGLPFAAllowOfflineRenderers};
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
      error = "no CGL 3.2 core pixel format / context";
      return;
    }
    if (CGLSetCurrentContext(ctx) != kCGLNoError) {
      error = "CGLSetCurrentContext failed";
      return;
    }
    if (!gl.load(cglGetProc)) {
      error = "missing GL entry points";
      return;
    }
    const unsigned char* r = gl.GetString(GL_RENDERER);
    const unsigned char* v = gl.GetString(GL_VERSION);
    renderer = std::string(r ? reinterpret_cast<const char*>(r) : "?") + " / " +
               (v ? reinterpret_cast<const char*>(v) : "?");
  }
  ~GLContext() {
    if (ctx) {
      CGLSetCurrentContext(nullptr);
      CGLDestroyContext(ctx);
    }
  }
  bool ok() const { return ctx && error.empty(); }
};

#endif  // JSUI_GPU_CGL

#if JSUI_GPU_TESTS

// A texture-backed FBO the test reads back.
struct RenderTexture {
  GL& gl;
  unsigned tex = 0, fbo = 0;
  int w, h;
  RenderTexture(GL& g, int width, int height) : gl(g), w(width), h(height) {
    gl.GenTextures(1, &tex);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  }
  ~RenderTexture() {
    gl.DeleteFramebuffers(1, &fbo);
    gl.DeleteTextures(1, &tex);
  }
  bool complete() {
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }
  // Rows as stored (row 0 = GL's bottom row), RGBA8888 premul as uint32.
  std::vector<uint32_t> read(bool flip) {
    gl.Finish();
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    std::vector<uint32_t> px(size_t(w) * h);
    gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (flip) {
      for (int y = 0; y < h / 2; y++)
        std::swap_ranges(px.begin() + long(y) * w, px.begin() + long(y + 1) * w, px.begin() + long(h - 1 - y) * w);
    }
    return px;
  }
};

// ------------------------------------------------------------- comparison

struct Diff {
  int maxDiff = 0;
  double mean = 0;     // mean absolute channel difference (0..255)
  double over = 0;     // fraction of pixels with any channel off by more than `tol`
};

Diff compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int tol) {
  Diff d;
  if (a.size() != b.size() || a.empty()) {
    d.maxDiff = 255;
    d.mean = 255;
    d.over = 1;
    return d;
  }
  uint64_t sum = 0, over = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int m = 0;
    for (int c = 0; c < 4; c++) {
      int x = int((a[i] >> (c * 8)) & 255), y = int((b[i] >> (c * 8)) & 255);
      int v = x > y ? x - y : y - x;
      sum += uint64_t(v);
      if (v > m) m = v;
    }
    if (m > d.maxDiff) d.maxDiff = m;
    if (m > tol) over++;
  }
  d.mean = double(sum) / double(a.size() * 4);
  d.over = double(over) / double(a.size());
  return d;
}

void savePng(const char* name, const std::vector<uint32_t>& px, int w, int h) {
  const char* dir = std::getenv("JSUI_TEST_OUT");
  if (!dir) return;
  SkPixmap pm(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType), px.data(), size_t(w) * 4);
  SkFILEWStream f((std::string(dir) + "/" + name + ".png").c_str());
  SkPngEncoder::Encode(&f, pm, {});
}

std::string base64(const void* data, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out += t[(v >> 18) & 63];
    out += t[(v >> 12) & 63];
    out += i + 1 < n ? t[(v >> 6) & 63] : '=';
    out += i + 2 < n ? t[v & 63] : '=';
  }
  return out;
}

// A 32x32 PNG: quadrants red / green / blue / translucent white.
std::string quadPngUri() {
  SkBitmap bm;
  bm.allocN32Pixels(32, 32);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      SkColor c = y < 16 ? (x < 16 ? SK_ColorRED : SK_ColorGREEN) : (x < 16 ? SK_ColorBLUE : SkColorSetARGB(128, 255, 255, 255));
      *bm.getAddr32(x, y) = SkPreMultiplyColor(c);
    }
  SkDynamicMemoryWStream s;
  SkPngEncoder::Encode(&s, bm.pixmap(), {});
  sk_sp<SkData> d = s.detachAsData();
  return "data:image/png;base64," + base64(d->data(), d->size());
}

// Everything the painter does that could differ on a GPU surface: rounded
// rects, borders (solid, dashed, per-side colours), cached and uncached
// (rotated) blurred shadows, inset shadows, linear / radial / repeating
// gradients, opacity / filter / blend-mode layers (saveLayer), rounded
// overflow clips, transforms, images (cover, rounded, tinted) and text.
void buildScene(Scene& s, const std::string& png) {
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"#202430"}})J");
  s.add(R"J({"style":{"position":"absolute","left":12,"top":12,"width":90,"height":60,"backgroundColor":"#3366ff",
            "borderRadius":14,"boxShadow":"0 6px 12px rgba(0,0,0,0.6)"}})J");
  s.add(R"J({"style":{"position":"absolute","left":120,"top":14,"width":70,"height":56,"backgroundColor":"#ffcc00",
            "borderRadius":8,"boxShadow":"4px 4px 10px rgba(255,64,64,0.8)","transform":[{"rotate":"12deg"}]}})J");
  s.add(R"J({"style":{"position":"absolute","left":210,"top":12,"width":96,"height":60,"borderWidth":3,
            "borderColor":"#ffffff","borderStyle":"dashed","borderRadius":10,
            "boxShadow":"inset 0 0 12px rgba(0,255,200,0.9)"}})J");
  s.add(R"J({"style":{"position":"absolute","left":12,"top":90,"width":90,"height":50,"borderRadius":6,
            "backgroundImage":"linear-gradient(135deg, #ff0080, #7928ca 60%, #2af598)"}})J");
  s.add(R"J({"style":{"position":"absolute","left":120,"top":90,"width":70,"height":50,
            "backgroundImage":"radial-gradient(circle closest-side, white, rgba(0,0,255,0.2))"}})J");
  s.add(R"J({"style":{"position":"absolute","left":210,"top":90,"width":96,"height":50,
            "backgroundImage":"repeating-linear-gradient(90deg, #e33 0px, #e33 6px, #333 6px, #333 12px)",
            "borderTopWidth":4,"borderTopColor":"lime","borderLeftWidth":2,"borderLeftColor":"orange"}})J");
  // Layers: opacity, blur + drop-shadow filter, grayscale, multiply blend.
  s.add(R"J({"style":{"position":"absolute","left":12,"top":156,"width":60,"height":40,"backgroundColor":"#ff3366",
            "opacity":0.5,"borderRadius":20}})J");
  s.add(R"J({"style":{"position":"absolute","left":82,"top":156,"width":50,"height":40,"backgroundColor":"#33ffcc",
            "filter":"blur(3px)"}})J");
  s.add(R"J({"style":{"position":"absolute","left":142,"top":156,"width":50,"height":40,"backgroundColor":"#ff9933",
            "filter":"grayscale(1) drop-shadow(2px 2px 3px black)"}})J");
  s.add(R"J({"style":{"position":"absolute","left":120,"top":150,"width":60,"height":30,"backgroundColor":"#00aaff",
            "mixBlendMode":"multiply"}})J");
  // Rounded overflow clip with an image (cover) and a tinted round image.
  s.add(R"J({"style":{"position":"absolute","left":210,"top":152,"width":50,"height":50,"borderRadius":12,
            "overflow":"hidden","backgroundColor":"#444"},
            "children":[{"type":"Image","props":{"resizeMode":"cover","source":{"uri":")J" + png +
        R"J("}},"style":{"width":70,"height":50}}]})J");
  s.add(R"J({"type":"Image","props":{"source":{"uri":")J" + png +
        R"J("}},"style":{"position":"absolute","left":270,"top":156,"width":40,"height":40,"borderRadius":20,
            "tintColor":"#ffee88"}})J");
  // Text: solid, shadowed and transformed.
  s.add(R"J({"type":"Text","style":{"position":"absolute","left":14,"top":204,"fontSize":18,"color":"#ffffff",
            "fontWeight":"700"},"text":"GPU vs CPU"})J");
  s.add(R"J({"type":"Text","style":{"position":"absolute","left":130,"top":206,"fontSize":14,"color":"#ffd0a0",
            "textShadowColor":"rgba(0,0,0,0.9)","textShadowOffset":{"width":1,"height":1},"textShadowRadius":2,
            "transform":[{"rotate":"-4deg"}]},"text":"Ganesh text"})J");
  // SkSL: a ShaderView fill (gradient from its uniforms) and one that
  // post-processes its own children (mirrored, channels swapped).
  s.add(R"J({"type":"ShaderView","props":{"source":"uniform float2 resolution; layout(color) uniform half4 a; layout(color) uniform half4 b; half4 main(float2 p) { return mix(a, b, half(p.x / resolution.x)); }",
            "uniforms":{"a":"#ff2060","b":[0.1,0.8,1,1]}},"style":{"position":"absolute","left":262,"top":204,"width":26,"height":30,"borderRadius":6}})J");
  s.add(R"J({"type":"ShaderView","props":{"source":"uniform shader content; uniform float2 resolution; half4 main(float2 p) { return content.eval(float2(resolution.x - p.x, p.y)).brga; }"},
            "style":{"position":"absolute","left":292,"top":204,"width":26,"height":30},
            "children":[{"style":{"width":10,"height":30,"backgroundColor":"#ff8800"}}]})J");
}

// Runs fn(ctx, api) once per GL API that works here; SKIP when none does.
// A context that exists but that jsui cannot build a Ganesh context on is a
// failure, not a skip.
template <class F>
void forEachContext(const char* test, F fn) {
#if JSUI_GPU_EGL
  Egl* e = egl();
  if (!e) {
    skip(test, "libEGL.so.1 not found");
    return;
  }
  const Api apis[] = {Api::GL, Api::GLCore, Api::GLES3, Api::GLES2};
  JsuiGLGetProcFn proc = eglProc;
#else
  const Api apis[] = {Api::GLCore};
  JsuiGLGetProcFn proc = cglProc;
#endif
  int ran = 0;
  std::string why;
  for (Api api : apis) {
#if JSUI_GPU_EGL
    GLContext c(*e, api);
#else
    GLContext c(api);
#endif
    if (!c.ok()) {
      why += (why.empty() ? "" : "; ") + c.error;
      continue;
    }
    JsuiGpuContext* gpu = jsui_gpu_create_gl(proc, nullptr);
    if (!gpu) {
      std::fprintf(stderr, "  %s [%s] %s: jsui_gpu_create_gl failed\n", test, apiName(api), c.renderer.c_str());
      jsui_test::failures()++;
      continue;
    }
    std::printf("  [%s] %s\n", apiName(api), c.renderer.c_str());
    fn(c, gpu, api);
    jsui_gpu_destroy(gpu);
    ran++;
  }
  if (!ran) skip(test, why.c_str());
}

#endif  // JSUI_GPU_TESTS

}  // namespace

TEST(gpu_backend_available) {
#if defined(JSUI_GPU)
  CHECK(jsui::gpu::available());
#else
  CHECK(!jsui::gpu::available());
  CHECK(jsui_gpu_create_gl(nullptr, nullptr) == nullptr);
#endif
  // No GL context is current here: creation must fail cleanly, and every
  // entry point must accept NULL.
  CHECK(jsui_gpu_create_gl(nullptr, nullptr) == nullptr);
  jsui_gpu_destroy(nullptr);
  jsui_gpu_reset(nullptr);
  jsui_gpu_context_lost(nullptr);
  jsui_gpu_purge(nullptr, 0);
  JsuiRuntime* rt = jsui_create(nullptr);
  CHECK_EQ(jsui_render_gl(rt, nullptr, 0, 64, 64, 0), -1);
  CHECK_EQ(jsui_render_gl_target(rt, nullptr, nullptr), -1);
  jsui_destroy(rt);
}

#if JSUI_GPU_TESTS

// Two GPU frames of the same content. Mesa reproduces them bit for bit;
// Apple's software GL renderer (the macOS CI runner) rounds a few pixels by
// one level from frame to frame, so allow +-1 per channel and nothing more.
bool sameFrame(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  return a.size() == b.size() && compare(a, b, 1).maxDiff <= 1;
}

// The full scene, GPU (texture target, top-left origin) vs CPU, at 1x and 2x.
TEST(gpu_matches_cpu_scene) {
  forEachContext("gpu_matches_cpu_scene", [](GLContext& c, JsuiGpuContext* gpu, Api api) {
    std::string png = quadPngUri();
    for (float dpr : {1.f, 2.f}) {
      Scene s(320, 240, dpr);
      buildScene(s, png);
      s.render();  // CPU reference (also loads the data: images)
      CHECK(!s.rt->needsRedraw());
      int w = s.dw(), h = s.dh();
      RenderTexture rtex(c.gl, w, h);
      CHECK(rtex.complete());
      JsuiGLTarget t{};
      t.texture = rtex.tex;
      t.width = w;
      t.height = h;
      t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
      s.doc().markPaintDirty();
      CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, t), 0);
      CHECK(!s.rt->needsRedraw());
      std::vector<uint32_t> out = rtex.read(false);
      Diff d = compare(out, s.px, 24);
      std::printf("  [%s] scene @%gx: max %d, mean %.3f, >24: %.3f%%\n", apiName(api), dpr, d.maxDiff, d.mean,
                  d.over * 100);
      std::string name = std::string("gpu_scene_") + apiName(api) + (dpr > 1 ? "_2x" : "_1x");
      savePng(name.c_str(), out, w, h);
      s.save((name + "_cpu").c_str());
      CHECK(d.mean < 1.0);
      CHECK(d.over < 0.01);
      // Second frame on the same context (cached surface, cached image
      // textures, cached shadow masks) is identical to the first.
      s.doc().markPaintDirty();
      CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, t), 0);
      std::vector<uint32_t> again = rtex.read(false);
      if (again != out) {
        Diff d2 = compare(again, out, 0);
        std::printf("  [%s] second frame differs from first: max %d, mean %.4f, changed %.3f%%\n", apiName(api), d2.maxDiff,
                    d2.mean, d2.over * 100);
        savePng((name + "_again").c_str(), again, w, h);
      }
      CHECK(sameFrame(again, out));
    }
  });
}

// Applies a style patch like Animated / patchStyle does.
void patchStyle(jsui::Node* n, const std::string& json) {
  using jsui::Value;
  std::string err;
  Value p = Value::parseJson(json, &err);
  Value::Object style = n->props()["style"].asObject();
  for (auto& [k, v] : p.asObject()) style[k] = v;
  Value::Object props = n->props().asObject();
  props["style"] = Value(style);
  n->setProps(Value(props));
}

// The retained frame (damage-only repaints, one rect or a few scissored
// passes, then one draw into the target) matches drawing the whole frame
// into the target every frame. Perf guard: it repaints only the damage.
struct RetainedRun {
  int worst = 0;
  double over = 0, repainted = 0, passes = 0;
  int steps = 0;
};

// Runs the probes over the scene with jsui::gpu::setRetainedDebug(debug):
// scene a through the retained frame, b direct, and a CPU copy as the
// reference. On the first step off by more than rounding, prints where,
// how the frame was painted and which of the two GPU frames left the CPU
// reference there (saved as PNGs with JSUI_TEST_OUT).
RetainedRun runRetained(GLContext& c, JsuiGpuContext* gpu, Api api, unsigned debug) {
  jsui::gpu::setRetainedDebug(debug);
  std::string png = quadPngUri();
  Scene a(320, 240), b(320, 240), cpu(320, 240);  // a: retained, b: direct, cpu: reference
  Scene* scenes[3] = {&a, &b, &cpu};
  jsui::Node* probes[3][2];
  for (int k = 0; k < 3; k++) {
    Scene& s = *scenes[k];
    buildScene(s, png);
    probes[k][0] = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":12,"height":10,"borderRadius":4,
      "backgroundColor":"rgba(255,255,255,0.6)","boxShadow":"0 2px 4px rgba(0,0,0,0.7)"}})J");
    probes[k][1] = s.add(R"J({"style":{"position":"absolute","left":300,"top":220,"width":8,"height":8,
      "backgroundColor":"#ff00ff"}})J");
    s.render();  // loads the data: images
  }
  RenderTexture ta(c.gl, 320, 240), tb(c.gl, 320, 240);
  JsuiGLTarget t{};
  t.width = 320;
  t.height = 240;
  t.texture = ta.tex;
  t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
  CHECK_EQ(jsui::gpu::render(a.rt->main(), gpu, t), 0);
  uint32_t seed = 777;
  auto rnd = [&](float lo, float hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
  };
  RetainedRun run;
  bool reported = false;
  const int steps = 40;
  for (int i = 0; i < steps; i++) {
    // Two probes far apart (two passes), a corner one only every 4th step.
    std::string p0 = "{\"left\":" + std::to_string(rnd(0, 140)) + ",\"top\":" + std::to_string(rnd(0, 100)) + "}";
    std::string p1 = "{\"left\":" + std::to_string(rnd(250, 310)) + ",\"top\":" + std::to_string(rnd(180, 230)) + "}";
    for (int k = 0; k < 3; k++) {
      patchStyle(probes[k][0], p0);
      if (i % 4 == 0) patchStyle(probes[k][1], p1);
    }
    t.texture = ta.tex;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
    CHECK_EQ(jsui::gpu::render(a.rt->main(), gpu, t), 0);
    jsui::PaintStats st = jsui::lastPaintStats(a.doc());
    run.repainted += double(st.repaintedPixels);
    run.passes += st.damageRects;
    t.texture = tb.tex;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN | JSUI_GL_NO_RETAIN;
    b.doc().markPaintDirty();
    CHECK_EQ(jsui::gpu::render(b.rt->main(), gpu, t), 0);
    std::vector<uint32_t> ra = ta.read(false), rb = tb.read(false);
    Diff d = compare(ra, rb, 2);
    run.worst = std::max(run.worst, d.maxDiff);
    run.over = std::max(run.over, d.over);
    if (d.over >= 0.0005 && !reported) {
      reported = true;
      cpu.render();
      SkIRect box = SkIRect::MakeEmpty();
      int offA = 0, offB = 0;  // pixels in the box more than 24 off the CPU frame
      for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
          size_t k = size_t(y) * 320 + x;
          auto ch = [](uint32_t v, int c) { return int((v >> (c * 8)) & 255); };
          int dab = 0, da = 0, db = 0;
          for (int c = 0; c < 4; c++) {
            dab = std::max(dab, std::abs(ch(ra[k], c) - ch(rb[k], c)));
            da = std::max(da, std::abs(ch(ra[k], c) - ch(cpu.px[k], c)));
            db = std::max(db, std::abs(ch(rb[k], c) - ch(cpu.px[k], c)));
          }
          if (dab <= 2) continue;
          box.join(SkIRect::MakeXYWH(x, y, 1, 1));
          offA += da > 24;
          offB += db > 24;
        }
      std::fprintf(stderr,
                   "  [%s] debug %u: step %d off: %.3f%% of pixels, max %d, in [%d,%d %dx%d]; frame %s, %d passes, "
                   "%lld px; of those pixels %d (retained) / %d (direct) are >24 off the CPU frame\n",
                   apiName(api), debug, i, d.over * 100, d.maxDiff, box.left(), box.top(), box.width(), box.height(),
                   st.repaintedPixels == 320 * 240 ? "full" : "partial", st.damageRects,
                   (long long)st.repaintedPixels, offA, offB);
      std::fprintf(stderr, "    damage:");
      int shown = 0;
      for (SkRegion::Iterator it(a.rt->main().trackedDamage()); !it.done() && shown < 12; it.next(), shown++)
        std::fprintf(stderr, " [%d,%d %dx%d]", it.rect().left(), it.rect().top(), it.rect().width(), it.rect().height());
      std::fprintf(stderr, "\n");
      std::string tag = std::string("_") + apiName(api) + "_debug" + std::to_string(debug);
      savePng(("gpu_retained" + tag).c_str(), ra, 320, 240);
      savePng(("gpu_direct" + tag).c_str(), rb, 320, 240);
      cpu.save(("gpu_cpu" + tag).c_str());
    }
  }
  run.steps = steps;
  jsui::gpu::setRetainedDebug(0);
  return run;
}

TEST(gpu_retained_frame_matches_direct) {
  forEachContext("gpu_retained_frame_matches_direct", [](GLContext& c, JsuiGpuContext* gpu, Api api) {
    RetainedRun r = runRetained(c, gpu, api, 0);
    std::printf("  [%s] retained vs direct: max %d, >2: %.4f%%; repainted %.0f px per frame in %.2f passes\n",
                apiName(api), r.worst, r.over * 100, r.repainted / r.steps, r.passes / r.steps);
    CHECK(r.over < 0.0005);
    // Perf guard: the probes' old + new rects with their shadow (and the
    // rounded boxes they cross), in at most a few passes, never the whole
    // frame.
    CHECK(r.repainted / r.steps < 320 * 240 / 4);
    CHECK(r.passes / r.steps <= 2.5);
    if (r.over < 0.0005) return;
    // Which part of the retained path differs on this driver.
    using namespace jsui::gpu;
    for (unsigned debug : {unsigned(kRetainedOneRect), unsigned(kRetainedFlushFirst),
                           unsigned(kRetainedOneRect | kRetainedFlushFirst), unsigned(kRetainedFullRepaint)}) {
      RetainedRun v = runRetained(c, gpu, api, debug);
      std::printf("  [%s] debug %u (1 one rect, 2 flush first, 4 full repaints): max %d, >2: %.4f%%\n", apiName(api),
                  debug, v.worst, v.over * 100);
    }
  });
}

// Default origin (bottom-left) into an FBO: GL row 0 is the bottom of the UI.
TEST(gpu_fbo_bottom_left_and_no_clear) {
  forEachContext("gpu_fbo_bottom_left_and_no_clear", [](GLContext& c, JsuiGpuContext* gpu, Api) {
    Scene s(64, 48);
    s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":64,"height":8,"backgroundColor":"#ff0000"}})J");
    s.add(R"J({"style":{"position":"absolute","left":8,"top":20,"width":20,"height":20,"backgroundColor":"rgba(0,0,255,0.5)"}})J");
    s.render();
    RenderTexture rtex(c.gl, 64, 48);
    CHECK(rtex.complete());
    s.doc().markPaintDirty();
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, JsuiGLTarget{rtex.fbo, 0, 0, 64, 48, 0, 0, 0}), 0);
    std::vector<uint32_t> out = rtex.read(true);
    Diff d = compare(out, s.px, 2);
    CHECK_EQ(d.maxDiff, 0);
    // NO_CLEAR: the UI is composited over what the FBO holds (opaque green).
    c.gl.BindFramebuffer(GL_FRAMEBUFFER, rtex.fbo);
    c.gl.Disable(GL_SCISSOR_TEST);
    c.gl.ColorMask(1, 1, 1, 1);
    c.gl.ClearColor(0, 1, 0, 1);
    c.gl.Clear(GL_COLOR_BUFFER_BIT);
    jsui_gpu_reset(gpu);  // we touched GL state behind Skia's back
    s.doc().markPaintDirty();
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, JsuiGLTarget{rtex.fbo, 0, 0, 64, 48, 0, 0, JSUI_GL_NO_CLEAR}), 0);
    out = rtex.read(true);
    auto px = [&](int x, int y) { return out[size_t(y) * 64 + x]; };
    CHECK_EQ(px(40, 30), 0xff00ff00u);  // untouched: green
    CHECK_EQ(px(10, 2), 0xff0000ffu);   // red bar (ABGR in memory order)
    uint32_t blend = px(15, 30);        // 50% blue over green
    CHECK(int(blend & 255) < 4);
    CHECK_NEAR(double((blend >> 8) & 255), 128, 3);
    CHECK_NEAR(double((blend >> 16) & 255), 128, 3);
    CHECK_EQ(int(blend >> 24), 255);
  });
}

// Host textures (jsui_set_texture, JSUI_TEXTURE_GL): a texture of the GL
// context drawn in place, matching the same pixels given as CPU pixels;
// new contents (jsui_texture_updated) show in the next frame; a
// bottom-left origin is flipped.
TEST(gpu_host_texture_gl) {
  forEachContext("gpu_host_texture_gl", [](GLContext& c, JsuiGpuContext* gpu, Api api) {
    // 4x2, row 0 (the top as uploaded) red, row 1 blue.
    std::vector<uint32_t> rows(8);
    for (int x = 0; x < 4; x++) {
      rows[x] = 0xff0000ffu;
      rows[4 + x] = 0xffff0000u;
    }
    unsigned src = 0;
    c.gl.GenTextures(1, &src);
    c.gl.BindTexture(GL_TEXTURE_2D, src);
    c.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    c.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    c.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    c.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    jsui_gpu_reset(gpu);
    Scene s(64, 48);
    jsui::Node* v = s.add(R"J({"type":"Image","props":{"texture":"gl","resizeMode":"stretch"},
                               "style":{"position":"absolute","left":8,"top":8,"width":40,"height":32}})J");
    (void)v;
    // The CPU reference: the same pixels as CPU pixels.
    s.rt->textures.setPixels("gl", rows.data(), 4, 2, 0, 0);
    s.render();
    JsuiTexture t{};
    t.kind = JSUI_TEXTURE_GL;
    t.handle = reinterpret_cast<void*>(uintptr_t(src));
    t.width = 4;
    t.height = 2;
    CHECK_EQ(s.rt->textures.set("gl", &t), 1);
    RenderTexture rtex(c.gl, 64, 48);
    JsuiGLTarget target{};
    target.texture = rtex.tex;
    target.width = 64;
    target.height = 48;
    target.flags = JSUI_GL_TOP_LEFT_ORIGIN | JSUI_GL_NO_RETAIN;
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, target), 0);
    std::vector<uint32_t> out = rtex.read(false);
    savePng((std::string("gpu_host_texture_") + apiName(api)).c_str(), out, 64, 48);
    auto px = [&](int x, int y) { return out[size_t(y) * 64 + x]; };
    CHECK_EQ(px(28, 12), 0xff0000ffu);  // top half red
    CHECK_EQ(px(28, 36), 0xffff0000u);  // bottom half blue
    CHECK_EQ(px(2, 2), 0u);
    // Away from the red/blue edge (rows 16..31: the stretched texels are 16 px
    // tall, so filtering blends there and its precision differs by driver,
    // e.g. Apple's software renderer) the frame matches the CPU pixels.
    std::vector<uint32_t> gpuRows = out, cpuRows = s.px;
    for (int y = 16; y < 32; y++)
      for (int x = 0; x < 64; x++) gpuRows[size_t(y) * 64 + x] = cpuRows[size_t(y) * 64 + x] = 0;
    Diff d = compare(gpuRows, cpuRows, 8);
    CHECK(d.over < 0.02);
    if (d.over >= 0.02) std::printf("  host texture vs CPU pixels: max %d, mean %.3f, over %.3f\n", d.maxDiff, d.mean, d.over);
    // New contents in the same texture: green.
    std::vector<uint32_t> green(8, 0xff00ff00u);
    c.gl.BindTexture(GL_TEXTURE_2D, src);
    c.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green.data());
    CHECK(!s.rt->needsRedraw());
    CHECK(s.rt->textures.updated("gl"));
    CHECK(s.rt->needsRedraw());
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, target), 0);
    out = rtex.read(false);
    CHECK_EQ(px(28, 12), 0xff00ff00u);
    // A bottom-left texture (rows bottom first) is shown upright.
    c.gl.BindTexture(GL_TEXTURE_2D, src);
    c.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    t.flags = JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN;
    CHECK_EQ(s.rt->textures.set("gl", &t), 1);
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, target), 0);
    out = rtex.read(false);
    CHECK_EQ(px(28, 12), 0xffff0000u);
    CHECK_EQ(px(28, 36), 0xff0000ffu);
    s.rt->textures.remove("gl");
    c.gl.DeleteTextures(1, &src);
  });
}

// Context loss: abandon, then the next render recreates the Ganesh context
// (images are uploaded again from the runtime's CPU copies).
TEST(gpu_context_lost_and_recreated) {
  forEachContext("gpu_context_lost_and_recreated", [](GLContext& c, JsuiGpuContext* gpu, Api) {
    std::string png = quadPngUri();
    Scene s(320, 240);
    buildScene(s, png);
    s.render();
    RenderTexture rtex(c.gl, 320, 240);
    JsuiGLTarget t{};
    t.texture = rtex.tex;
    t.width = 320;
    t.height = 240;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, t), 0);
    std::vector<uint32_t> before = rtex.read(false);
    jsui_gpu_context_lost(gpu);
    jsui_gpu_purge(gpu, 0);  // no-op while lost
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, t), 0);
    std::vector<uint32_t> after = rtex.read(false);
    CHECK(sameFrame(after, before));
    jsui_gpu_purge(gpu, 64u << 20);
    CHECK_EQ(jsui::gpu::render(s.rt->main(), gpu, t), 0);
    CHECK(sameFrame(rtex.read(false), before));
  });
}

// The C API end to end: a bundle drawn with jsui_render_gl_target, and the
// platform-default loader (NULL get_proc: EGL / CGL is current).
TEST(gpu_capi_bundle) {
  forEachContext("gpu_capi_bundle", [](GLContext& c, JsuiGpuContext*, Api) {
    JsuiGpuContext* gpu = jsui_gpu_create_gl(nullptr, nullptr);
    CHECK(gpu != nullptr);
    if (!gpu) return;
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 100;
    cfg.height = 80;
    JsuiRuntime* rt = jsui_create(&cfg);
    const char* src =
        "const n = __jsui_native; const v = n.createNode('View');"
        "n.setProps(v, {style: {position: 'absolute', left: 10, top: 10, width: 50, height: 40,"
        " backgroundColor: '#00ff00', borderRadius: 8}});"
        "n.appendChild(n.rootId, v);";
    CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "gpu.js"), 0);
    jsui_tick(rt, 0);
    CHECK(jsui_needs_redraw(rt));
    std::vector<uint32_t> cpu(100 * 80);
    jsui_render_rgba(rt, cpu.data(), 100, 80, 400);
    RenderTexture rtex(c.gl, 100, 80);
    JsuiGLTarget t{};
    t.texture = rtex.tex;
    t.width = 100;
    t.height = 80;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
    CHECK_EQ(jsui_render_gl_target(rt, gpu, &t), 0);
    CHECK(!jsui_needs_redraw(rt));
    Diff d = compare(rtex.read(false), cpu, 16);
    CHECK(d.over < 0.005);
    CHECK_EQ(cpu[size_t(30) * 100 + 30], 0xff00ff00u);
    // The default framebuffer form (fbo argument) into our FBO.
    CHECK_EQ(jsui_render_gl(rt, gpu, rtex.fbo, 100, 80, 0), 0);
    std::vector<uint32_t> flipped = rtex.read(true);
    CHECK_EQ(flipped[size_t(30) * 100 + 30], 0xff00ff00u);
    CHECK_EQ(flipped[size_t(75) * 100 + 90], 0u);
    // A CPU incremental render after GPU frames repaints fully.
    std::vector<uint32_t> buf(100 * 80, 0xdeadbeef);
    JsuiRect dirty;
    CHECK_EQ(jsui_render_rgba_incremental(rt, buf.data(), 100, 80, 400, &dirty), 1);
    CHECK(buf == cpu);
    jsui_destroy(rt);
    jsui_gpu_destroy(gpu);
  });
}

// A shadow drawn approximately while its node scales (from the mask of a
// nearby size, painter.cpp quantizedScale) is repainted exactly in the
// frame after the animation stops, and while it runs the surface still goes
// idle after each frame (jsui_needs_redraw), as with any other animation.
// Another surface of the runtime, with a static shadow, is not repainted.
TEST(gpu_scaling_shadow_exact_after_animation) {
  forEachContext("gpu_scaling_shadow_exact_after_animation", [](GLContext& c, JsuiGpuContext* gpu, Api api) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 160;
    cfg.height = 120;
    JsuiRuntime* rt = jsui_create(&cfg);
    JsuiRuntime* other = jsui_surface_create(rt, "Static", nullptr, 160, 120, 1);
    const char* src = R"JS(
      const N = __jsui_native, A = N.animated;
      globalThis.__jsui_runSurfaces = (list) => {
        for (const [tag] of list) {
          const v = N.createNode('View', tag);
          N.setProps(v, { style: { margin: 30, flex: 1, borderRadius: 8, backgroundColor: '#36a',
            boxShadow: '0 4px 12px rgba(0,0,0,0.8)' } });
          N.appendChild(tag, v);
        }
      };
      const box = N.createNode('View');
      N.setProps(box, { style: { position: 'absolute', left: 50, top: 40, width: 60, height: 40, borderRadius: 8,
        backgroundColor: '#3a6', boxShadow: '0 4px 12px rgba(0,0,0,0.8)' } });
      N.appendChild(N.rootId, box);
      A.createNode(1, { type: 'value', value: 1, offset: 0 });
      A.createNode(2, { type: 'transform', transforms: [{ property: 'scale', nodeTag: 1 }] });
      A.connect(1, 2);
      A.createNode(3, { type: 'style', style: { transform: 2 } });
      A.connect(2, 3);
      A.createNode(4, { type: 'props', props: { style: 3 } });
      A.connect(3, 4);
      A.connectToView(4, box);
      globalThis.__jsui_onHostMessage = () =>
        A.startAnimation(5, 1, { type: 'frames', frames: [0, 0.25, 0.5, 0.75, 1], duration: 160, toValue: 1.3, iterations: 1 });
    )JS";
    CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "shadow.js"), 0);
    jsui::Runtime& r = *jsui::internalRuntime(rt);
    RenderTexture rtex(c.gl, 160, 120);
    JsuiGLTarget t{};
    t.texture = rtex.tex;
    t.width = 160;
    t.height = 120;
    t.flags = JSUI_GL_TOP_LEFT_ORIGIN;
    RenderTexture otex(c.gl, 160, 120);
    JsuiGLTarget ot = t;
    ot.texture = otex.tex;
    double now = 0;
    jsui_tick(rt, now += 16);
    CHECK_EQ(jsui_render_gl_target(rt, gpu, &t), 0);
    CHECK(jsui_needs_redraw(other));
    CHECK_EQ(jsui_render_gl_target(other, gpu, &ot), 0);
    jsui_post_message(rt, "start", "null");
    int frames = 0, approximate = 0, busy = 0, otherRenders = 0;
    while (frames < 30 && (jsui_wants_frame(rt) || jsui_needs_redraw(rt))) {
      frames++;
      jsui_tick(rt, now += 16);
      if (jsui_needs_redraw(rt)) CHECK_EQ(jsui_render_gl_target(rt, gpu, &t), 0);
      if (jsui_needs_redraw(other)) {
        otherRenders++;
        CHECK_EQ(jsui_render_gl_target(other, gpu, &ot), 0);
      }
      if (r.doc.exactRepaintPending()) approximate++;
      if (r.animatingFrames(r.main()) && jsui_needs_redraw(rt)) busy++;
    }
    std::printf("  [%s] %d frames, %d approximate, %d busy while animating, other surface repainted %d\n",
                apiName(api), frames, approximate, busy, otherRenders);
    CHECK(approximate > 0);
    CHECK_EQ(busy, 0);
    CHECK_EQ(otherRenders, 0);
    CHECK(frames < 30 && !jsui_needs_redraw(rt) && !jsui_needs_redraw(other));
    // The retained frame equals a full repaint (another target) of the end state.
    std::vector<uint32_t> retained = rtex.read(false);
    RenderTexture full(c.gl, 160, 120);
    JsuiGLTarget f = t;
    f.texture = full.tex;
    CHECK_EQ(jsui_render_gl_target(rt, gpu, &f), 0);
    Diff d = compare(retained, full.read(false), 2);
    CHECK(d.over < 0.0005);
    jsui_destroy(rt);
  });
}

#endif  // JSUI_GPU_TESTS
