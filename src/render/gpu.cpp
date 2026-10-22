#include "render/gpu.h"

#if defined(JSUI_GPU)

#include <cstdint>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrContextOptions.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/gpu/gl/GrGLTypes.h"
#include "render/painter.h"
#include "runtime.h"

#if defined(__linux__) && !defined(__ANDROID__)
#define JSUI_GL_DLOPEN 1
#include <dlfcn.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

namespace {

constexpr unsigned kGL_TEXTURE_2D = 0x0DE1;
constexpr unsigned kGL_RGBA8 = 0x8058;
constexpr unsigned kGL_RGB8 = 0x8051;
constexpr unsigned kGL_BGRA8 = 0x93A1;
constexpr unsigned kGL_SRGB8_ALPHA8 = 0x8C43;
constexpr unsigned kGL_RGB10_A2 = 0x8059;
constexpr unsigned kGL_RGBA16F = 0x881A;

struct SurfaceKey {
  unsigned fbo, texture, format;
  int width, height, samples, stencil;
  bool topLeft;
  bool operator==(const SurfaceKey& o) const {
    return fbo == o.fbo && texture == o.texture && format == o.format && width == o.width && height == o.height &&
           samples == o.samples && stencil == o.stencil && topLeft == o.topLeft;
  }
};

#if JSUI_GL_DLOPEN
// Linux: no link-time GL dependency. Resolve through whichever of EGL / GLX
// has a current context (SDL, GLFW and engines use either), falling back to
// symbols already loaded into the process.
struct LinuxGL {
  using GetCurrent = void* (*)();
  using GetProc = void* (*)(const char*);
  GetCurrent eglGetCurrentContext = nullptr, glXGetCurrentContext = nullptr;
  GetProc eglGetProcAddress = nullptr, glXGetProcAddress = nullptr;
  LinuxGL() {
    void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (egl) {
      eglGetCurrentContext = reinterpret_cast<GetCurrent>(dlsym(egl, "eglGetCurrentContext"));
      eglGetProcAddress = reinterpret_cast<GetProc>(dlsym(egl, "eglGetProcAddress"));
    }
    void* glx = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!glx) glx = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (glx) {
      glXGetCurrentContext = reinterpret_cast<GetCurrent>(dlsym(glx, "glXGetCurrentContext"));
      glXGetProcAddress = reinterpret_cast<GetProc>(dlsym(glx, "glXGetProcAddressARB"));
    }
  }
  static LinuxGL& get() {
    static LinuxGL gl;
    return gl;
  }
};

}  // namespace

// The prebuilt Skia's Ganesh GL references GLX (its default native interface,
// pulled in with GrDirectContexts::MakeGL). Satisfy those references here,
// forwarding to libGL when it exists at run time, so linking the GPU backend
// adds no libGL dependency (headless servers keep running jsui-render).
// Hidden: nothing outside this link unit binds to them.
extern "C" {
__attribute__((weak, visibility("hidden"))) void* glXGetCurrentContext() {
  auto f = LinuxGL::get().glXGetCurrentContext;
  return f ? f() : nullptr;
}
__attribute__((weak, visibility("hidden"))) void (*glXGetProcAddress(const unsigned char* name))() {
  auto f = LinuxGL::get().glXGetProcAddress;
  return f ? reinterpret_cast<void (*)()>(f(reinterpret_cast<const char*>(name))) : nullptr;
}
}

namespace {

GrGLFuncPtr linuxEglProc(void*, const char name[]) {
  if (void* p = LinuxGL::get().eglGetProcAddress(name)) return reinterpret_cast<GrGLFuncPtr>(p);
  return reinterpret_cast<GrGLFuncPtr>(dlsym(RTLD_DEFAULT, name));
}
// Skia asks for eglQueryString / eglGetCurrentDisplay to read EGL
// extensions and calls them when non-null. GLX's glXGetProcAddress returns
// a dispatch stub for *any* name (so does SDL_GL_GetProcAddress on X11), and
// calling those crashes: answer egl* names only when an EGL context is current.
bool eglName(const char* name) { return name[0] == 'e' && name[1] == 'g' && name[2] == 'l'; }
GrGLFuncPtr eglOnly(const char* name) {
  LinuxGL& gl = LinuxGL::get();
  if (gl.eglGetCurrentContext && gl.eglGetProcAddress && gl.eglGetCurrentContext())
    return reinterpret_cast<GrGLFuncPtr>(gl.eglGetProcAddress(name));
  return nullptr;
}

GrGLFuncPtr linuxGlxProc(void*, const char name[]) {
  if (eglName(name)) return eglOnly(name);
  if (void* p = LinuxGL::get().glXGetProcAddress(name)) return reinterpret_cast<GrGLFuncPtr>(p);
  return reinterpret_cast<GrGLFuncPtr>(dlsym(RTLD_DEFAULT, name));
}
#endif

}  // namespace

struct JsuiGpuContext {
  JsuiGLGetProcFn getProc = nullptr;
  void* user = nullptr;
  sk_sp<GrDirectContext> ctx;
  bool lost = false;
  size_t cacheBytes = 0;
  struct Cached {
    SurfaceKey key;
    sk_sp<SkSurface> surface;
    uint64_t lastUsed;
  };
  std::vector<Cached> surfaces;  // wrapped framebuffers, most recently used kept
  uint64_t frame = 0;

  sk_sp<const GrGLInterface> makeInterface() const {
    if (getProc) {
      return GrGLMakeAssembledInterface(const_cast<JsuiGpuContext*>(this), [](void* self, const char name[]) {
        auto* g = static_cast<JsuiGpuContext*>(self);
#if JSUI_GL_DLOPEN
        if (eglName(name)) return eglOnly(name);
#endif
        return reinterpret_cast<GrGLFuncPtr>(g->getProc(g->user, name));
      });
    }
#if JSUI_GL_DLOPEN
    LinuxGL& gl = LinuxGL::get();
    if (gl.eglGetCurrentContext && gl.eglGetProcAddress && gl.eglGetCurrentContext())
      return GrGLMakeAssembledInterface(nullptr, linuxEglProc);
    if (gl.glXGetCurrentContext && gl.glXGetProcAddress && gl.glXGetCurrentContext())
      return GrGLMakeAssembledInterface(nullptr, linuxGlxProc);
    return nullptr;
#else
    // Skia's native interface queries GL right away: make sure a context is
    // current (Windows' WGL interface checks this itself).
#if defined(__APPLE__)
    using CGLGetCurrent = void* (*)();
    auto cglCurrent = reinterpret_cast<CGLGetCurrent>(dlsym(RTLD_DEFAULT, "CGLGetCurrentContext"));
    if (!cglCurrent || !cglCurrent()) return nullptr;
#elif defined(__EMSCRIPTEN__)
    if (emscripten_webgl_get_current_context() == 0) return nullptr;
#endif
    return GrGLMakeNativeInterface();
#endif
  }

  bool ensure() {
    if (ctx && !lost && !ctx->abandoned()) return true;
    if (ctx) {
      ctx->abandonContext();
      surfaces.clear();
      ctx.reset();
    }
    lost = false;
    sk_sp<const GrGLInterface> iface = makeInterface();
    if (!iface) return false;
    GrContextOptions opts;
    ctx = GrDirectContexts::MakeGL(std::move(iface), opts);
    if (ctx && cacheBytes) ctx->setResourceCacheLimit(cacheBytes);
    return ctx != nullptr;
  }

  sk_sp<SkSurface> surfaceFor(const JsuiGLTarget& t) {
    SurfaceKey key{t.fbo, t.texture, t.format ? t.format : kGL_RGBA8, t.width, t.height,
                   t.samples > 1 ? t.samples : 0, t.texture ? 0 : (t.stencil_bits > 0 ? t.stencil_bits : 0),
                   (t.flags & JSUI_GL_TOP_LEFT_ORIGIN) != 0};
    frame++;
    for (auto& c : surfaces) {
      if (c.key == key) {
        c.lastUsed = frame;
        return c.surface;
      }
    }
    SkColorType ct;
    sk_sp<SkColorSpace> cs;
    switch (key.format) {
      case kGL_RGBA8: ct = kRGBA_8888_SkColorType; break;
      case kGL_BGRA8: ct = kBGRA_8888_SkColorType; break;
      case kGL_RGB8: ct = kRGB_888x_SkColorType; break;
      case kGL_SRGB8_ALPHA8:
        // The target encodes on write: draw sRGB colours into a linear-blending
        // sRGB surface (blending then happens in linear light, unlike the CPU
        // path / browsers).
        ct = kSRGBA_8888_SkColorType;
        cs = SkColorSpace::MakeSRGB();
        break;
      case kGL_RGB10_A2: ct = kRGBA_1010102_SkColorType; break;
      case kGL_RGBA16F:
        ct = kRGBA_F16_SkColorType;
        cs = SkColorSpace::MakeSRGBLinear();
        break;
      default: return nullptr;
    }
    GrSurfaceOrigin origin = key.topLeft ? kTopLeft_GrSurfaceOrigin : kBottomLeft_GrSurfaceOrigin;
    SkSurfaceProps props = jsui::surfaceProps();
    sk_sp<SkSurface> surface;
    if (key.texture) {
      GrGLTextureInfo info{kGL_TEXTURE_2D, key.texture, key.format};
      GrBackendTexture tex = GrBackendTextures::MakeGL(key.width, key.height, skgpu::Mipmapped::kNo, info);
      surface = SkSurfaces::WrapBackendTexture(ctx.get(), tex, origin, 1, ct, cs, &props);
    } else {
      GrGLFramebufferInfo info{key.fbo, key.format};
      GrBackendRenderTarget rt = GrBackendRenderTargets::MakeGL(key.width, key.height, key.samples, key.stencil, info);
      surface = SkSurfaces::WrapBackendRenderTarget(ctx.get(), rt, origin, ct, cs, &props);
    }
    // Texture targets are wrapped per frame: Skia attaches the texture to an
    // FBO of its own, which would keep drawing into the old storage if the
    // host deleted the texture and reused its name. Framebuffers are looked
    // up by name on every bind, so their wrappers are safe to keep.
    if (!surface || key.texture) return surface;
    constexpr size_t kMaxSurfaces = 4;
    if (surfaces.size() >= kMaxSurfaces) {
      size_t oldest = 0;
      for (size_t i = 1; i < surfaces.size(); i++)
        if (surfaces[i].lastUsed < surfaces[oldest].lastUsed) oldest = i;
      surfaces.erase(surfaces.begin() + long(oldest));
    }
    surfaces.push_back({key, surface, frame});
    return surface;
  }
};

namespace jsui {
namespace gpu {

bool available() { return true; }

JsuiGpuContext* createGL(JsuiGLGetProcFn getProc, void* user) {
  auto* g = new JsuiGpuContext;
  g->getProc = getProc;
  g->user = user;
  if (!g->ensure()) {
    delete g;
    return nullptr;
  }
  return g;
}

void destroy(JsuiGpuContext* g) {
  if (!g) return;
  if (g->ctx) {
    if (g->lost) g->ctx->abandonContext();
    g->surfaces.clear();
    if (!g->lost) g->ctx->flushAndSubmit(GrSyncCpu::kNo);
    g->ctx.reset();
  }
  delete g;
}

void reset(JsuiGpuContext* g) {
  if (g && g->ctx && !g->lost) g->ctx->resetContext();
}

void contextLost(JsuiGpuContext* g) {
  if (!g) return;
  g->lost = true;
  if (g->ctx) g->ctx->abandonContext();
  g->surfaces.clear();
}

void purge(JsuiGpuContext* g, size_t cacheBytes) {
  if (!g) return;
  if (cacheBytes) g->cacheBytes = cacheBytes;
  if (!g->ctx || g->lost) return;
  if (cacheBytes) g->ctx->setResourceCacheLimit(cacheBytes);
  g->surfaces.clear();
  g->ctx->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
}

int render(Runtime& rt, JsuiGpuContext* g, const JsuiGLTarget& t) {
  if (!g || t.width <= 0 || t.height <= 0) return -1;
  if (!g->ensure()) return -1;
  // The host (or a previous frame) changed GL state Skia may have cached.
  g->ctx->resetContext();
  sk_sp<SkSurface> surface = g->surfaceFor(t);
  if (!surface) return -1;
  rt.renderCanvas(surface->getCanvas(), t.width, t.height, (t.flags & JSUI_GL_NO_CLEAR) == 0);
  g->ctx->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
  return g->ctx->abandoned() ? -1 : 0;
}

}  // namespace gpu
}  // namespace jsui

#else  // !JSUI_GPU

namespace jsui {
namespace gpu {
bool available() { return false; }
JsuiGpuContext* createGL(JsuiGLGetProcFn, void*) { return nullptr; }
void destroy(JsuiGpuContext*) {}
void reset(JsuiGpuContext*) {}
void contextLost(JsuiGpuContext*) {}
void purge(JsuiGpuContext*, size_t) {}
int render(Runtime&, JsuiGpuContext*, const JsuiGLTarget&) { return -1; }
}  // namespace gpu
}  // namespace jsui

#endif
