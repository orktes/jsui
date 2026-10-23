#include "render/gpu.h"

#if defined(JSUI_GPU)

#include <algorithm>
#include <cstdint>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
// Skia moved the Ganesh headers under include/gpu/ganesh/ after m124 (the
// native prebuilt); the WASM GPU build (m146) only has the new locations.
#if __has_include("include/gpu/ganesh/GrDirectContext.h")
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrContextOptions.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#else
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrContextOptions.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/gpu/gl/GrGLTypes.h"
#endif
#include "render/host_textures.h"
#include "render/painter.h"
#include "runtime.h"

#if defined(__linux__) && !defined(__ANDROID__)
#define JSUI_GL_DLOPEN 1
#include <dlfcn.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>

#include "include/gpu/ShaderErrorHandler.h"
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

namespace {

// A host texture (JSUI_TEXTURE_GL) of this GL context, borrowed.
sk_sp<SkImage> wrapTexture(GrDirectContext* ctx, const JsuiTexture& t) {
  if (t.kind != JSUI_TEXTURE_GL || !ctx) return nullptr;
  unsigned format = t.format ? t.format : kGL_RGBA8;
  SkColorType ct;
  if (format == kGL_RGBA8) ct = kRGBA_8888_SkColorType;
  else if (format == kGL_BGRA8) ct = kBGRA_8888_SkColorType;
  else return nullptr;
  GrGLTextureInfo info;
  info.fTarget = kGL_TEXTURE_2D;
  info.fID = unsigned(reinterpret_cast<uintptr_t>(t.handle));
  info.fFormat = format;
  GrBackendTexture bt = GrBackendTextures::MakeGL(t.width, t.height, skgpu::Mipmapped::kNo, info);
  // A bottom-left origin is flipped where it is drawn (image_node.cpp), as
  // on the backends without origins.
  return SkImages::BorrowTextureFrom(ctx, bt, kTopLeft_GrSurfaceOrigin, ct, jsui::textureAlphaType(t), nullptr);
}

#if defined(__EMSCRIPTEN__)
// WebKit marks a WebGL context lost when a call into its GPU process fails,
// in the middle of a frame: the shaders compiling then fail with "(unknown
// error)". That is the loss, which the host handles (restore, repaint), not
// a shader error. Other failures go to Skia's default handler.
struct WebGLShaderErrors final : skgpu::ShaderErrorHandler {
  using skgpu::ShaderErrorHandler::compileError;
  void compileError(const char* shader, const char* errors) override {
    if (emscripten_is_webgl_context_lost(emscripten_webgl_get_current_context())) return;
    skgpu::DefaultShaderErrorHandler()->compileError(shader, errors);
  }
};
WebGLShaderErrors webglShaderErrors;
#endif

struct GLGpu final : JsuiGpuContext {
  GLGpu() : JsuiGpuContext(Kind::GL), textures([this](const JsuiTexture& t) { return wrapTexture(ctx.get(), t); }) {}
  // Host textures drawn in this context's frames.
  jsui::TextureWrapper textures;
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
  // Retained frames, one per runtime drawn with this context (see render()).
  struct Retained {
    const jsui::Surface* rt = nullptr;
    sk_sp<SkSurface> surface;  // null until a frame is painted into it
    uintptr_t key = 0;         // identifies the surface's contents to the runtime
    uint64_t lastUsed = 0;
    int smallFrames = 0, directFrames = 0;
  };
  std::vector<Retained> retained;
  uintptr_t nextKey = 1;

  Retained* retainedEntry(const jsui::Surface* rt) {
    for (auto& e : retained)
      if (e.rt == rt) {
        e.lastUsed = frame;
        return &e;
      }
    // Runtimes that stopped drawing here: drop their frames.
    constexpr uint64_t kMaxIdle = 600;
    // Room for the surfaces of a runtime sharing one context (web: every
    // createSurface() canvas draws through one offscreen WebGL context).
    constexpr size_t kMaxRetained = 32;
    retained.erase(std::remove_if(retained.begin(), retained.end(),
                                  [&](const Retained& e) { return frame - e.lastUsed > kMaxIdle; }),
                   retained.end());
    if (retained.size() >= kMaxRetained) {
      auto oldest = std::min_element(retained.begin(), retained.end(),
                                     [](const Retained& a, const Retained& b) { return a.lastUsed < b.lastUsed; });
      retained.erase(oldest);
    }
    retained.push_back({});
    retained.back().rt = rt;
    retained.back().lastUsed = frame;
    return &retained.back();
  }

  static bool matches(const SkSurface* s, const SkImageInfo& info) {
    return s && s->width() == info.width() && s->height() == info.height() &&
           s->imageInfo().colorType() == info.colorType() &&
           SkColorSpace::Equals(s->imageInfo().colorSpace(), info.colorSpace());
  }

  // (Re)creates the entry's surface like `targetInfo`; a new key each time.
  bool makeRetainedSurface(Retained* r, const SkImageInfo& targetInfo) {
    SkImageInfo info = SkImageInfo::Make(targetInfo.width(), targetInfo.height(), targetInfo.colorType(),
                                         kPremul_SkAlphaType, targetInfo.refColorSpace());
    SkSurfaceProps props = jsui::surfaceProps();
    r->surface = SkSurfaces::RenderTarget(ctx.get(), skgpu::Budgeted::kNo, info, 1, kTopLeft_GrSurfaceOrigin, &props);
    r->key = (nextKey += 2);  // odd: never the address of a CPU frame buffer
    return r->surface != nullptr;
  }

  sk_sp<const GrGLInterface> makeInterface() const {
    if (getProc) {
      return GrGLMakeAssembledInterface(const_cast<GLGpu*>(this), [](void* self, const char name[]) {
        auto* g = static_cast<GLGpu*>(self);
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
      retained.clear();
      textures.clear();
      ctx.reset();
    }
    lost = false;
    sk_sp<const GrGLInterface> iface = makeInterface();
    if (!iface) return false;
    GrContextOptions opts;
    // Layers (opacity, filters, blend modes) otherwise split the frame's
    // render pass in two at each one: reordering renders them first and
    // keeps the frame one pass (fewer framebuffer switches).
    opts.fReduceOpsTaskSplitting = GrContextOptions::Enable::kYes;
#if defined(__EMSCRIPTEN__)
    opts.fShaderErrorHandler = &webglShaderErrors;
#endif
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
    constexpr size_t kMaxSurfaces = 16;  // e.g. one size of the shared web canvas per surface
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

GLGpu* asGL(JsuiGpuContext* g) { return g && g->kind == JsuiGpuContext::Kind::GL ? static_cast<GLGpu*>(g) : nullptr; }

}  // namespace

namespace jsui {
namespace gpu {

namespace {
unsigned retainedDebug = 0;
}  // namespace

void setRetainedDebug(unsigned flags) { retainedDebug = flags; }

bool available() { return true; }

JsuiGpuContext* createGL(JsuiGLGetProcFn getProc, void* user) {
  auto* g = new GLGpu;
  g->getProc = getProc;
  g->user = user;
  if (!g->ensure()) {
    delete g;
    return nullptr;
  }
  return g;
}

void destroy(JsuiGpuContext* gpu) {
  GLGpu* g = asGL(gpu);
  if (!g) return;
  if (g->ctx) {
    if (g->lost) g->ctx->abandonContext();
    g->surfaces.clear();
    g->retained.clear();
    g->textures.clear();
    if (!g->lost) g->ctx->flushAndSubmit(GrSyncCpu::kNo);
    g->ctx.reset();
  }
  delete g;
}

void reset(JsuiGpuContext* gpu) {
  GLGpu* g = asGL(gpu);
  if (g && g->ctx && !g->lost) g->ctx->resetContext();
}

void contextLost(JsuiGpuContext* gpu) {
  GLGpu* g = asGL(gpu);
  if (!g) return;
  g->lost = true;
  if (g->ctx) g->ctx->abandonContext();
  g->surfaces.clear();
  g->retained.clear();
  g->textures.clear();
}

void purge(JsuiGpuContext* gpu, size_t cacheBytes) {
  GLGpu* g = asGL(gpu);
  if (!g) return;
  if (cacheBytes) g->cacheBytes = cacheBytes;
  if (!g->ctx || g->lost) return;
  if (cacheBytes) g->ctx->setResourceCacheLimit(cacheBytes);
  g->surfaces.clear();
  g->retained.clear();  // the next frames repaint fully
  g->ctx->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
}

int render(Surface& rt, JsuiGpuContext* gpu, const JsuiGLTarget& t) {
  GLGpu* g = asGL(gpu);
  if (!g || t.width <= 0 || t.height <= 0) return -1;
  if (!g->ensure()) return -1;
  // The host (or a previous frame) changed GL state Skia may have cached.
  g->ctx->resetContext();
  sk_sp<SkSurface> surface = g->surfaceFor(t);
  if (!surface) return -1;
  bool clear = (t.flags & JSUI_GL_NO_CLEAR) == 0;
  // Where the frame is painted (Surface::renderCanvasTracked), like the
  // WebGPU backend:
  //   * Small damage (a spinner, a hover, a caret): only the damage, as a
  //     few scissored rects, into a retained frame owned by the context,
  //     then one draw copies it into the target (targets need not keep
  //     their contents between frames).
  //   * Most of the frame changed (an animated full-screen UI, a scrolling
  //     list): a full repaint straight into the target, one pass, no copy.
  //     The retained frame misses that frame and is repainted fully once
  //     the damage stayed small for kRetainAfter frames.
  // MSAA targets and JSUI_GL_NO_RETAIN always take the direct path (the
  // retained frame is single-sampled).
  constexpr int kRetainAfter = 3, kFreeRetainedAfter = 300;
  jsui::TextureScope textureScope(&g->textures);
  const SkImageInfo info = surface->imageInfo();
  bool allowRetain = !(t.flags & JSUI_GL_NO_RETAIN) && t.samples <= 1;
  GLGpu::Retained* r = allowRetain ? g->retainedEntry(&rt) : nullptr;
  if (r && !GLGpu::matches(r->surface.get(), info)) r->surface.reset();
  const void* key = r && r->surface && !(retainedDebug & kRetainedFullRepaint)
                        ? reinterpret_cast<const void*>(r->key)
                        : nullptr;
  bool viaRetained = false, failed = false;
  const int64_t frameArea = int64_t(t.width) * t.height;
  rt.renderCanvasTracked(key, t.width, t.height, [&](const SkIRect&, bool keyValid) -> Surface::CanvasChoice {
    int64_t area = 0;
    for (SkRegion::Iterator it(rt.trackedDamage()); !it.done(); it.next())
      area += int64_t(it.rect().width()) * it.rect().height();
    bool large = area * 2 >= frameArea;
    bool retain = false;
    if (r) {
      r->smallFrames = large ? 0 : r->smallFrames + 1;
      retain = keyValid ? !large : r->smallFrames >= kRetainAfter;
      if (!retain && ++r->directFrames > kFreeRetainedAfter) r->surface.reset();  // a frame's worth of memory
    }
    if (!retain) return {surface->getCanvas(), nullptr, clear};
    r->directFrames = 0;
    if (!r->surface && !g->makeRetainedSurface(r, info)) {
      failed = true;
      return {};
    }
    viaRetained = true;
    Surface::CanvasChoice c{r->surface->getCanvas(), reinterpret_cast<const void*>(r->key), true};
    c.splitDamage = !(retainedDebug & kRetainedOneRect);  // Ganesh: one scissor per pass, no stencil or mask
    return c;
  });
  if (failed) return -1;
  if (viaRetained) {
    if (retainedDebug & kRetainedFlushFirst) g->ctx->flushAndSubmit(r->surface.get(), GrSyncCpu::kYes);
    SkPaint paint;
    paint.setBlendMode(clear ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    r->surface->draw(surface->getCanvas(), 0, 0, SkSamplingOptions(), &paint);
  }
  // One flush and submit per frame (with the retained frame's repaint, which
  // the target's draw depends on), and no CPU wait.
  g->ctx->flushAndSubmit(GrSyncCpu::kNo);
  g->textures.endFrame();
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
int render(Surface&, JsuiGpuContext*, const JsuiGLTarget&) { return -1; }
void setRetainedDebug(unsigned) {}
}  // namespace gpu
}  // namespace jsui

#endif
