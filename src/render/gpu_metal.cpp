#include "render/gpu_metal.h"

#if defined(JSUI_METAL)

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
// Skia moved the Ganesh headers under include/gpu/ganesh/ after m124; the
// factories below are the m124+ spellings (GrDirectContexts::MakeMetal,
// GrBackendRenderTargets::MakeMtl).
#if __has_include("include/gpu/ganesh/GrDirectContext.h")
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrContextOptions.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"
#else
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrContextOptions.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/mtl/GrMtlBackendContext.h"
#include "include/gpu/mtl/GrMtlTypes.h"
#endif
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "render/painter.h"
#include "runtime.h"

namespace {

// MTLPixelFormat values (no Objective-C here: textures arrive as id<MTLTexture>
// cast to void*, which is all Ganesh's C++ API takes).
constexpr unsigned kMTLPixelFormatRGBA8Unorm = 70;
constexpr unsigned kMTLPixelFormatRGBA8Unorm_sRGB = 71;
constexpr unsigned kMTLPixelFormatBGRA8Unorm = 80;
constexpr unsigned kMTLPixelFormatBGRA8Unorm_sRGB = 81;
constexpr unsigned kMTLPixelFormatRGB10A2Unorm = 90;
constexpr unsigned kMTLPixelFormatBGR10A2Unorm = 94;
constexpr unsigned kMTLPixelFormatRGBA16Float = 115;

bool colorTypeFor(unsigned format, SkColorType* ct, sk_sp<SkColorSpace>* cs) {
  switch (format) {
    case 0:
    case kMTLPixelFormatBGRA8Unorm: *ct = kBGRA_8888_SkColorType; return true;
    case kMTLPixelFormatRGBA8Unorm: *ct = kRGBA_8888_SkColorType; return true;
    // sRGB targets encode on write: blending happens in linear light (unlike
    // the CPU path / browsers), see the GL backend.
    case kMTLPixelFormatBGRA8Unorm_sRGB:
      *ct = kBGRA_8888_SkColorType;
      *cs = SkColorSpace::MakeSRGB();
      return true;
    case kMTLPixelFormatRGBA8Unorm_sRGB:
      *ct = kSRGBA_8888_SkColorType;
      *cs = SkColorSpace::MakeSRGB();
      return true;
    case kMTLPixelFormatRGB10A2Unorm: *ct = kRGBA_1010102_SkColorType; return true;
    case kMTLPixelFormatBGR10A2Unorm: *ct = kBGRA_1010102_SkColorType; return true;
    case kMTLPixelFormatRGBA16Float:
      *ct = kRGBA_F16_SkColorType;
      *cs = SkColorSpace::MakeSRGBLinear();
      return true;
    default: return false;
  }
}

}  // namespace

struct JsuiMetalContext {
  sk_sp<GrDirectContext> ctx;
};

namespace jsui {
namespace metal {

bool available() { return true; }

JsuiMetalContext* create(void* device, void* queue) {
  if (!device || !queue) return nullptr;
  GrMtlBackendContext backend;
  backend.fDevice.retain(static_cast<GrMTLHandle>(device));
  backend.fQueue.retain(static_cast<GrMTLHandle>(queue));
  GrContextOptions opts;
  sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeMetal(backend, opts);
  if (!ctx) return nullptr;
  auto* m = new JsuiMetalContext;
  m->ctx = std::move(ctx);
  return m;
}

void destroy(JsuiMetalContext* m) {
  if (!m) return;
  if (m->ctx) {
    m->ctx->flushAndSubmit(GrSyncCpu::kYes);
    m->ctx.reset();
  }
  delete m;
}

void purge(JsuiMetalContext* m, size_t cacheBytes) {
  if (!m || !m->ctx) return;
  if (cacheBytes) m->ctx->setResourceCacheLimit(cacheBytes);
  m->ctx->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
}

int render(Runtime& rt, JsuiMetalContext* m, void* texture, unsigned format, int width, int height, int flags) {
  if (!m || !m->ctx || !texture || width <= 0 || height <= 0) return -1;
  SkColorType ct;
  sk_sp<SkColorSpace> cs;
  if (!colorTypeFor(format, &ct, &cs)) return -1;
  // Drawables rotate every frame (and their textures may be recycled), so the
  // target is wrapped per frame; wrapping is cheap.
  GrMtlTextureInfo info;
  info.fTexture.retain(static_cast<GrMTLHandle>(texture));
  GrBackendRenderTarget target = GrBackendRenderTargets::MakeMtl(width, height, info);
  SkSurfaceProps props = surfaceProps();
  sk_sp<SkSurface> surface =
      SkSurfaces::WrapBackendRenderTarget(m->ctx.get(), target, kTopLeft_GrSurfaceOrigin, ct, cs, &props);
  if (!surface) return -1;
  rt.renderCanvas(surface->getCanvas(), width, height, (flags & JSUI_GL_NO_CLEAR) == 0);
  m->ctx->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
  return 0;
}

}  // namespace metal
}  // namespace jsui

#else  // !JSUI_METAL

namespace jsui {
namespace metal {
bool available() { return false; }
JsuiMetalContext* create(void*, void*) { return nullptr; }
void destroy(JsuiMetalContext*) {}
void purge(JsuiMetalContext*, size_t) {}
int render(Runtime&, JsuiMetalContext*, void*, unsigned, int, int, int) { return -1; }
}  // namespace metal
}  // namespace jsui

#endif
