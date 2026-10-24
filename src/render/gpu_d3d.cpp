// Skia Ganesh on Direct3D 12 (jsui_gpu_create_d3d12 / _d3d11,
// jsui_render_gpu_target in jsui.h). Graphite has no Direct3D backend.
//
// D3D12: Ganesh on the host's ID3D12Device and DIRECT ID3D12CommandQueue.
// The host's resource is wrapped per frame with the state it is in
// (JsuiGpuTarget.layout); Ganesh records its own transitions (to
// RENDER_TARGET, COPY_SOURCE for destination reads, ...) and tracks the
// state in the GrBackendTexture's shared GrD3DResourceState, but it cannot
// leave a resource in a state of the caller's choosing (its flush ignores
// a new MutableTextureState on D3D; only kPresent moves to PRESENT). So
// after Skia's submit the render reads the state Skia left the resource in
// and executes one command list of its own on the same queue with the
// transitions to `final_layout` (and those that return sampled host
// textures to their home state, below). Everything is executed on the
// host's queue before the render returns, no CPU wait: queue order and the
// barriers order the render after what the host executed before and
// before what it executes after.
//
// Frames go through Surface::renderCanvasTracked like the GL backend
// (gpu.cpp): with JSUI_GPU_INCREMENTAL the target holds our last frame and
// only the damage is repainted into it (as in gpu_graphite.cpp). Without
// it, small damage goes into a retained frame the context owns (one per
// runtime) that is then drawn into the target, and most-of-the-frame
// damage is a full repaint straight into the target.
//
// Host textures (JSUI_TEXTURE_D3D12) are wrapped with their home state
// (JsuiTexture.usage, 0: PIXEL_SHADER_RESOURCE). Ganesh moves a texture it
// samples to PIXEL_SHADER_RESOURCE; the render's own command list moves it
// back home, so the host's state tracking stays valid.
//
// D3D11 (jsui_gpu_create_d3d11): Ganesh draws on a D3D12 device of its own,
// created on the adapter of the host's ID3D11Device (same LUID), with its
// own DIRECT queue. Each runtime draws into a shared texture (created in
// D3D12 with D3D12_HEAP_FLAG_SHARED and ALLOW_SIMULTANEOUS_ACCESS, opened in
// D3D11 with OpenSharedResource1), which doubles as that runtime's
// retained frame: only the damage is ever repainted into it. One shared
// fence (created in D3D12, opened in D3D11) orders the two APIs:
//
//     D3D11: Signal(v)   [after the previous frame's copies out of it]
//     D3D12: Wait(v), Skia's command lists (the texture decays back to
//            COMMON after them: simultaneous access), Signal(v + 1)
//     D3D11: Wait(v + 1), CopySubresourceRegion(host texture <- shared)
//
// The copy is the damage when the host's texture holds our last frame
// (JSUI_GPU_INCREMENTAL), else the whole frame. The shared texture is
// typed UNORM (RGBA or BGRA, after the host's format family) and the copy
// moves raw bits, so an sRGB host texture receives sRGB-encoded values
// blended like the CPU raster (not in linear light as the D3D12 sRGB path
// does). JSUI_GPU_NO_CLEAR copies the host's texture into the shared one
// first and draws the full frame over it. All D3D11 calls go to the
// device's immediate context, so the render must run on its thread.
#include "render/gpu.h"

#if defined(JSUI_D3D)

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrContextOptions.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DDirectContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#include "render/host_textures.h"
#include "render/painter.h"
#include "runtime.h"

namespace {

// The DXGI formats JSUI draws: how Skia sees them. TYPELESS resources are
// drawn through UNORM views. BGRA8 sRGB has no Skia Ganesh D3D format.
struct Format {
  DXGI_FORMAT draw = DXGI_FORMAT_UNKNOWN;  // the view format Skia uses
  SkColorType ct = kUnknown_SkColorType;
  sk_sp<SkColorSpace> cs;
  bool bgra = false;  // the B8G8R8A8 family (else R8G8B8A8)
};

bool d3d12Format(DXGI_FORMAT f, Format* out) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      out->draw = DXGI_FORMAT_R8G8B8A8_UNORM;
      out->ct = kRGBA_8888_SkColorType;
      return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      // The target encodes on write: blending happens in linear light (as on
      // GL / Vulkan sRGB targets).
      out->draw = f;
      out->ct = kSRGBA_8888_SkColorType;
      out->cs = SkColorSpace::MakeSRGB();
      return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      out->draw = DXGI_FORMAT_B8G8R8A8_UNORM;
      out->ct = kBGRA_8888_SkColorType;
      out->bgra = true;
      return true;
    default: return false;
  }
}

// D3D11 host textures: any format of the two families the copy accepts.
bool d3d11Family(DXGI_FORMAT f, bool* bgra) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *bgra = false; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: *bgra = true; return true;
    default: return false;
  }
}

GrBackendTexture wrapResource(ID3D12Resource* res, int w, int h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                              uint32_t levels) {
  GrD3DTextureResourceInfo info;
  info.fResource.retain(res);  // gr_cp(T*) would adopt the host's reference
  info.fResourceState = state;
  info.fFormat = format;
  info.fSampleCount = 1;
  // The resource's own (0 for single-sampled); the default is
  // DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN, which then goes into Skia's
  // pipelines and mismatches the render target view.
  info.fSampleQualityPattern = res->GetDesc().SampleDesc.Quality;
  info.fLevelCount = levels ? levels : 1;
  return GrBackendTextures::MakeD3D(w, h, info);
}

D3D12_RESOURCE_STATES stateOf(const GrBackendTexture& bt) {
  return GrBackendTextures::GetD3DTextureResourceInfo(bt).fResourceState;
}

// Transition barriers of the render's own, executed after Skia's submit on
// the same queue. Command lists are recycled once the GPU finished them
// (their fence); the resources they name are referenced until then.
class Transitions {
 public:
  bool init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_.retain(device);
    queue_.retain(queue);
    return SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
  }
  void add(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    pending_.push_back(b);
    gr_cp<ID3D12Resource> ref;
    ref.retain(res);
    pendingRefs_.push_back(std::move(ref));
  }
  bool empty() const { return pending_.empty(); }
  // Executes the pending barriers (if any) on the queue.
  bool execute() {
    if (pending_.empty()) return true;
    Slot* slot = freeSlot();
    bool ok = slot && SUCCEEDED(slot->alloc->Reset()) && SUCCEEDED(slot->list->Reset(slot->alloc.get(), nullptr));
    if (ok) {
      slot->list->ResourceBarrier(UINT(pending_.size()), pending_.data());
      ok = SUCCEEDED(slot->list->Close());
    }
    if (ok) {
      ID3D12CommandList* lists[] = {slot->list.get()};
      queue_->ExecuteCommandLists(1, lists);
      ok = SUCCEEDED(queue_->Signal(fence_.get(), ++value_));
      slot->done = value_;
      slot->refs = std::move(pendingRefs_);
    }
    pending_.clear();
    pendingRefs_.clear();
    return ok;
  }
  // Waits for every list executed so far (teardown).
  void wait() {
    if (!fence_ || fence_->GetCompletedValue() >= value_) return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event && SUCCEEDED(fence_->SetEventOnCompletion(value_, event))) WaitForSingleObject(event, INFINITE);
    if (event) CloseHandle(event);
  }
  void release() {
    wait();
    slots_.clear();
    fence_.reset();
    queue_.reset();
    device_.reset();
  }

 private:
  struct Slot {
    gr_cp<ID3D12CommandAllocator> alloc;
    gr_cp<ID3D12GraphicsCommandList> list;
    uint64_t done = 0;
    std::vector<gr_cp<ID3D12Resource>> refs;
  };
  Slot* freeSlot() {
    const uint64_t completed = fence_->GetCompletedValue();
    for (auto& s : slots_)
      if (s->done <= completed) {
        s->refs.clear();
        return s.get();
      }
    // Every list still in flight (the host renders far ahead): a few more,
    // then wait for the oldest.
    constexpr size_t kMaxSlots = 8;
    if (slots_.size() < kMaxSlots) {
      auto s = std::make_unique<Slot>();
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->alloc))) ||
          FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc.get(), nullptr,
                                            IID_PPV_ARGS(&s->list))) ||
          FAILED(s->list->Close()))
        return nullptr;
      slots_.push_back(std::move(s));
      return slots_.back().get();
    }
    Slot* oldest = slots_[0].get();
    for (auto& s : slots_)
      if (s->done < oldest->done) oldest = s.get();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return nullptr;
    if (SUCCEEDED(fence_->SetEventOnCompletion(oldest->done, event))) WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    oldest->refs.clear();
    return oldest;
  }

  gr_cp<ID3D12Device> device_;
  gr_cp<ID3D12CommandQueue> queue_;
  gr_cp<ID3D12Fence> fence_;
  uint64_t value_ = 0;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<D3D12_RESOURCE_BARRIER> pending_;
  std::vector<gr_cp<ID3D12Resource>> pendingRefs_;
};

struct D3DGpu final : JsuiGpuContext {
  explicit D3DGpu(bool on11)
      : JsuiGpuContext(Kind::D3D), d3d11(on11), textures([this](const JsuiTexture& t) { return wrapTexture(t); }) {}
  ~D3DGpu() override { release(); }

  const bool d3d11;  // jsui_gpu_create_d3d11: a D3D12 device of our own
  gr_cp<ID3D12Device> device;
  gr_cp<ID3D12CommandQueue> queue;
  sk_sp<GrDirectContext> ctx;
  Transitions transitions;
  bool lost = false;
  size_t cacheBytes = 0;
  uint64_t frame = 0;
  // Host textures drawn in this context's frames (D3D12 contexts only).
  jsui::TextureWrapper textures;
  // The wraps of host textures still cached by `textures`, with the state
  // each lives in between renders (JsuiTexture.usage).
  struct HostWrap {
    sk_sp<SkImage> image;
    GrBackendTexture bt;  // shares Skia's state tracking with the image
    D3D12_RESOURCE_STATES home;
  };
  std::vector<HostWrap> hostWraps;

  // D3D11 interop (d3d11 only).
  gr_cp<ID3D11Device5> device11;
  gr_cp<ID3D11DeviceContext4> context11;
  gr_cp<ID3D12Fence> fence12;  // shared
  gr_cp<ID3D11Fence> fence11;  // the same fence, opened on D3D11
  uint64_t fenceValue = 0;

  // Retained frames, one per runtime drawn with this context. D3D12: the
  // small-damage frame of non-incremental targets (gpu.cpp). D3D11: the
  // shared texture every frame is drawn into.
  struct Retained {
    const jsui::Surface* rt = nullptr;
    sk_sp<SkSurface> surface;  // null until a frame is painted into it
    uintptr_t key = 0;         // identifies the surface's contents to the runtime
    uint64_t lastUsed = 0;
    int smallFrames = 0, directFrames = 0;
    // D3D11: the shared texture behind `surface`.
    gr_cp<ID3D12Resource> shared12;
    gr_cp<ID3D11Texture2D> shared11;
    GrBackendTexture bt;
    bool bgra = false;
    bool used11 = false;  // D3D11 work reading / writing it was queued since D3D12 last waited
  };
  std::vector<std::unique_ptr<Retained>> retained;
  uintptr_t nextKey = 1;

  Retained* retainedEntry(const jsui::Surface* rt) {
    for (auto& e : retained)
      if (e->rt == rt) {
        e->lastUsed = frame;
        return e.get();
      }
    // Runtimes that stopped drawing here: drop their frames.
    constexpr uint64_t kMaxIdle = 600;
    constexpr size_t kMaxRetained = 32;
    retained.erase(std::remove_if(retained.begin(), retained.end(),
                                  [&](const std::unique_ptr<Retained>& e) { return frame - e->lastUsed > kMaxIdle; }),
                   retained.end());
    if (retained.size() >= kMaxRetained) {
      auto oldest = std::min_element(
          retained.begin(), retained.end(),
          [](const std::unique_ptr<Retained>& a, const std::unique_ptr<Retained>& b) { return a->lastUsed < b->lastUsed; });
      retained.erase(oldest);
    }
    retained.push_back(std::make_unique<Retained>());
    retained.back()->rt = rt;
    retained.back()->lastUsed = frame;
    return retained.back().get();
  }

  static bool matches(const SkSurface* s, const SkImageInfo& info) {
    return s && s->width() == info.width() && s->height() == info.height() &&
           s->imageInfo().colorType() == info.colorType() &&
           SkColorSpace::Equals(s->imageInfo().colorSpace(), info.colorSpace());
  }

  // D3D12: (re)creates the entry's surface like `targetInfo`; a new key each time.
  bool makeRetainedSurface(Retained* r, const SkImageInfo& targetInfo) {
    SkImageInfo info = SkImageInfo::Make(targetInfo.width(), targetInfo.height(), targetInfo.colorType(),
                                         kPremul_SkAlphaType, targetInfo.refColorSpace());
    SkSurfaceProps props = jsui::surfaceProps();
    r->surface = SkSurfaces::RenderTarget(ctx.get(), skgpu::Budgeted::kNo, info, 1, kTopLeft_GrSurfaceOrigin, &props);
    r->key = (nextKey += 2);  // odd: never the address of a texture or CPU frame buffer
    return r->surface != nullptr;
  }

  // D3D11: (re)creates the entry's shared texture.
  bool makeShared(Retained* r, int w, int h, bool bgra) {
    r->surface.reset();
    r->shared11.reset();
    r->shared12.reset();
    r->bt = {};
    const DXGI_FORMAT format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = UINT64(w);
    desc.Height = UINT(h);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&r->shared12)))) {
      SkDebugf("jsui: D3D11: could not create a %dx%d shared texture\n", w, h);
      return false;
    }
    HANDLE handle = nullptr;
    if (FAILED(device->CreateSharedHandle(r->shared12.get(), nullptr, GENERIC_ALL, nullptr, &handle))) return false;
    HRESULT hr = device11->OpenSharedResource1(handle, IID_PPV_ARGS(&r->shared11));
    CloseHandle(handle);
    if (FAILED(hr)) {
      SkDebugf("jsui: D3D11 could not open the shared texture (0x%08lx)\n", (unsigned long)hr);
      return false;
    }
    r->bt = wrapResource(r->shared12.get(), w, h, format, D3D12_RESOURCE_STATE_COMMON, 1);
    SkSurfaceProps props = jsui::surfaceProps();
    r->surface = SkSurfaces::WrapBackendTexture(ctx.get(), r->bt, kTopLeft_GrSurfaceOrigin, 1,
                                                bgra ? kBGRA_8888_SkColorType : kRGBA_8888_SkColorType, nullptr,
                                                &props);
    r->key = (nextKey += 2);
    r->bgra = bgra;
    r->used11 = false;
    return r->surface != nullptr;
  }

  // A host texture (JSUI_TEXTURE_D3D12) of this device, sampled in place.
  sk_sp<SkImage> wrapTexture(const JsuiTexture& t) {
    if (d3d11 || !ctx || t.kind != JSUI_TEXTURE_D3D12) return nullptr;
    auto* res = static_cast<ID3D12Resource*>(t.handle);
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) return nullptr;
    Format f;
    // sRGB views would decode: host textures hold sRGB-encoded values
    // sampled as is (jsui.h), as UNORM / TYPELESS.
    DXGI_FORMAT format = t.format ? DXGI_FORMAT(t.format) : desc.Format;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || !d3d12Format(format, &f)) return nullptr;
    const D3D12_RESOURCE_STATES home =
        t.usage ? D3D12_RESOURCE_STATES(t.usage) : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    GrBackendTexture bt = wrapResource(res, t.width, t.height, f.draw, home, desc.MipLevels);
    sk_sp<SkImage> image = SkImages::BorrowTextureFrom(ctx.get(), bt, kTopLeft_GrSurfaceOrigin, f.ct,
                                                       jsui::textureAlphaType(t), nullptr);
    if (image) hostWraps.push_back({image, bt, home});
    return image;
  }

  // After Skia's submit: sampled host textures go back to their home state.
  void restoreHostTextures() {
    for (auto& w : hostWraps) {
      const D3D12_RESOURCE_STATES now = stateOf(w.bt);
      if (now == w.home) continue;
      transitions.add(GrBackendTextures::GetD3DTextureResourceInfo(w.bt).fResource.get(), now, w.home);
      GrBackendTextures::SetD3DResourceState(&w.bt, w.home);
    }
    // Wraps the TextureWrapper let go of.
    hostWraps.erase(std::remove_if(hostWraps.begin(), hostWraps.end(),
                                   [](const HostWrap& w) { return w.image->unique(); }),
                    hostWraps.end());
  }

  void clearCaches() {
    retained.clear();
    textures.clear();
    hostWraps.clear();
  }

  bool init(const char* what) {
    gr_cp<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    GrD3DBackendContext backend;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&backend.fAdapter)))) {
      SkDebugf("jsui: %s: no DXGI adapter with the device's LUID\n", what);
      return false;
    }
    backend.fDevice = device;
    backend.fQueue = queue;
    GrContextOptions opts;
    // Layers (opacity, filters, blend modes) otherwise split the frame's
    // render pass at each one (as on GL).
    opts.fReduceOpsTaskSplitting = GrContextOptions::Enable::kYes;
    ctx = GrDirectContexts::MakeD3D(backend, opts);
    if (!ctx) {
      SkDebugf("jsui: %s: Skia rejected the Direct3D 12 device\n", what);
      return false;
    }
    return transitions.init(device.get(), queue.get());
  }

  // False once the device is removed (the context is abandoned then).
  bool alive() {
    if (lost || !ctx) return false;
    bool removed = FAILED(device->GetDeviceRemovedReason()) ||
                   (device11 && FAILED(device11->GetDeviceRemovedReason()));
    if (!removed) return true;
    SkDebugf("jsui: the Direct3D device was removed (0x%08lx)\n", (unsigned long)device->GetDeviceRemovedReason());
    lost = true;
    ctx->abandonContext();
    clearCaches();
    return false;
  }

  void release() {
    if (ctx) {
      clearCaches();
      if (!lost) ctx->flushAndSubmit(GrSyncCpu::kYes);
      // Skia waits for its own command lists when it is destroyed.
      ctx.reset();
    }
    transitions.release();
    fence11.reset();
    fence12.reset();
    context11.reset();
    device11.reset();
    queue.reset();
    device.reset();
  }
};

D3DGpu* asD3D(JsuiGpuContext* g) { return g && g->kind == JsuiGpuContext::Kind::D3D ? static_cast<D3DGpu*>(g) : nullptr; }

// Where a frame is painted, for both kinds of context (Surface::
// renderCanvasTracked); see the top of the file.
constexpr int kRetainAfter = 3, kFreeRetainedAfter = 300;

int render12(jsui::Surface& rt, D3DGpu* g, const JsuiGpuTarget& t, JsuiRect* dirty) {
  auto* res = static_cast<ID3D12Resource*>(t.texture);
  const D3D12_RESOURCE_DESC desc = res->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
      desc.Width < UINT64(t.width) || desc.Height < UINT(t.height)) {
    SkDebugf("jsui: D3D12 target must be a single-sampled 2D texture of at least %dx%d\n", t.width, t.height);
    return -1;
  }
  if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
    SkDebugf("jsui: D3D12 target needs D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET\n");
    return -1;
  }
  Format f;
  const DXGI_FORMAT format = t.format ? DXGI_FORMAT(t.format) : desc.Format;
  if (!d3d12Format(format, &f)) {
    SkDebugf("jsui: D3D12 target format %u is not RGBA8 / BGRA8 UNORM (or RGBA8 sRGB)\n", unsigned(format));
    return -1;
  }
  const D3D12_RESOURCE_STATES layout = D3D12_RESOURCE_STATES(t.layout);
  const D3D12_RESOURCE_STATES finalLayout = D3D12_RESOURCE_STATES(t.final_layout);
  GrBackendTexture bt = wrapResource(res, t.width, t.height, f.draw, layout, desc.MipLevels);
  SkSurfaceProps props = jsui::surfaceProps();
  sk_sp<SkSurface> surface =
      SkSurfaces::WrapBackendTexture(g->ctx.get(), bt, kTopLeft_GrSurfaceOrigin, 1, f.ct, f.cs, &props);
  if (!surface) {
    SkDebugf("jsui: Skia could not wrap the D3D12 target\n");
    return -1;
  }
  g->frame++;

  // JSUI_GPU_INCREMENTAL: the target is the retained frame, keyed by the
  // resource (the host drops the flag after recreating it). Otherwise as
  // in gpu.cpp:
  //   * Small damage: only the damage, into a retained frame owned by the
  //     context, then one draw copies it into the target.
  //   * Most of the frame changed: a full repaint straight into the
  //     target; the retained frame misses that frame and is repainted
  //     fully once the damage stayed small for kRetainAfter frames.
  const bool incremental = (t.flags & JSUI_GPU_INCREMENTAL) != 0;
  const bool clear = (t.flags & JSUI_GPU_NO_CLEAR) == 0;
  jsui::TextureScope textureScope(&g->textures);
  const SkImageInfo info = surface->imageInfo();
  D3DGpu::Retained* r = incremental ? nullptr : g->retainedEntry(&rt);
  if (r && !D3DGpu::matches(r->surface.get(), info)) r->surface.reset();
  const void* key = incremental ? t.texture : r && r->surface ? reinterpret_cast<const void*>(r->key) : nullptr;
  bool viaRetained = false, failed = false;
  const int64_t frameArea = int64_t(t.width) * t.height;
  SkIRect painted = rt.renderCanvasTracked(key, t.width, t.height, [&](const SkIRect&, bool keyValid) -> jsui::Surface::CanvasChoice {
    // A target that holds just our frame afterwards may go incremental next.
    jsui::Surface::CanvasChoice direct{surface->getCanvas(), clear ? t.texture : nullptr, clear};
    direct.splitDamage = true;  // Ganesh: one scissor per pass, no stencil or mask
    if (incremental) return direct;
    int64_t area = 0;
    for (SkRegion::Iterator it(rt.trackedDamage()); !it.done(); it.next())
      area += int64_t(it.rect().width()) * it.rect().height();
    const bool large = area * 2 >= frameArea;
    r->smallFrames = large ? 0 : r->smallFrames + 1;
    const bool retain = keyValid ? !large : r->smallFrames >= kRetainAfter;
    if (!retain) {
      if (++r->directFrames > kFreeRetainedAfter) r->surface.reset();  // a frame's worth of memory
      return direct;
    }
    r->directFrames = 0;
    if (!r->surface && !g->makeRetainedSurface(r, info)) {
      failed = true;
      return {};
    }
    viaRetained = true;
    jsui::Surface::CanvasChoice c{r->surface->getCanvas(), reinterpret_cast<const void*>(r->key), true};
    c.splitDamage = true;
    return c;
  });
  if (failed) return -1;
  if (viaRetained) {
    SkPaint paint;
    paint.setBlendMode(clear ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    r->surface->draw(surface->getCanvas(), 0, 0, SkSamplingOptions(), &paint);
    painted = SkIRect::MakeWH(t.width, t.height);  // the whole target was written
  }
  // Nothing changed and the target is retained: no GPU work at all (unless
  // the resource must still move to another state).
  if (painted.isEmpty() && incremental && layout == finalLayout) {
    g->textures.endFrame();
    return 0;
  }
  {
    jsui::Perf::Scope gpuScope(rt.doc.perf, jsui::Phase::Gpu);
    g->ctx->flushAndSubmit(GrSyncCpu::kNo);
  }
  // Skia left the target in whatever state its last use needed (tracked in
  // `bt`'s shared state): move it to `final_layout`, and sampled host
  // textures home, in one command list after Skia's.
  g->transitions.add(res, stateOf(bt), finalLayout);
  g->restoreHostTextures();
  const bool ok = g->transitions.execute();
  surface.reset();
  g->textures.endFrame();
  if (!ok || !g->alive()) return -1;
  if (dirty) *dirty = JsuiRect{painted.x(), painted.y(), painted.width(), painted.height()};
  return 0;
}

int render11(jsui::Surface& rt, D3DGpu* g, const JsuiGpuTarget& t, JsuiRect* dirty) {
  auto* host = static_cast<ID3D11Texture2D*>(t.texture);
  D3D11_TEXTURE2D_DESC desc{};
  host->GetDesc(&desc);
  if (desc.SampleDesc.Count != 1 || desc.Width < UINT(t.width) || desc.Height < UINT(t.height) ||
      desc.Usage != D3D11_USAGE_DEFAULT) {
    SkDebugf("jsui: D3D11 target must be a single-sampled DEFAULT-usage texture of at least %dx%d\n", t.width,
             t.height);
    return -1;
  }
  bool bgra = false;
  const DXGI_FORMAT format = t.format ? DXGI_FORMAT(t.format) : desc.Format;
  if (!d3d11Family(format, &bgra)) {
    SkDebugf("jsui: D3D11 target format %u is not RGBA8 / BGRA8 (UNORM, sRGB or TYPELESS)\n", unsigned(format));
    return -1;
  }
  g->frame++;
  D3DGpu::Retained* r = g->retainedEntry(&rt);
  if (!r->surface || r->surface->width() != t.width || r->surface->height() != t.height || r->bgra != bgra) {
    if (!g->makeShared(r, t.width, t.height, bgra)) {
      r->surface.reset();
      return -1;
    }
  }
  ID3D11DeviceContext4* d11 = g->context11.get();
  const bool composite = (t.flags & JSUI_GPU_NO_CLEAR) != 0;
  // NO_CLEAR: the frame is drawn over the host's contents, so they go into
  // the shared texture first (which then no longer holds just our frame).
  const D3D11_BOX full{0, 0, 0, UINT(t.width), UINT(t.height), 1};
  if (composite) {
    d11->CopySubresourceRegion(r->shared11.get(), 0, 0, 0, 0, host, 0, &full);
    r->used11 = true;
  }
  jsui::TextureScope textureScope(&g->textures);
  const void* key = composite ? nullptr : reinterpret_cast<const void*>(r->key);
  SkIRect painted = rt.renderCanvasTracked(key, t.width, t.height, [&](const SkIRect&, bool) -> jsui::Surface::CanvasChoice {
    jsui::Surface::CanvasChoice c{r->surface->getCanvas(), key, !composite};
    c.splitDamage = true;
    return c;
  });
  // The host's texture holds our last frame: only the damage is copied.
  const SkIRect copy = (t.flags & JSUI_GPU_INCREMENTAL) && !composite ? painted : SkIRect::MakeWH(t.width, t.height);
  if (copy.isEmpty()) {
    g->textures.endFrame();
    return 0;
  }
  if (!painted.isEmpty()) {
    // D3D12 waits for D3D11's earlier copies into / out of the shared texture.
    if (r->used11) {
      ++g->fenceValue;
      if (FAILED(d11->Signal(g->fence11.get(), g->fenceValue))) return -1;
      d11->Flush();  // so the D3D12 queue's wait can be met without the host's next flush
      if (FAILED(g->queue->Wait(g->fence12.get(), g->fenceValue))) return -1;
      r->used11 = false;
    }
    {
      jsui::Perf::Scope gpuScope(rt.doc.perf, jsui::Phase::Gpu);
      g->ctx->flushAndSubmit(GrSyncCpu::kNo);
    }
    // A simultaneous-access texture decays to COMMON (where the other API may
    // use it) when the command lists that used it complete: no barrier, but
    // Skia must start from COMMON next frame.
    GrBackendTextures::SetD3DResourceState(&r->bt, D3D12_RESOURCE_STATE_COMMON);
    ++g->fenceValue;
    if (FAILED(g->queue->Signal(g->fence12.get(), g->fenceValue))) return -1;
    if (FAILED(d11->Wait(g->fence11.get(), g->fenceValue))) return -1;
  }
  // Without new pixels the previous frame's wait already ordered the copy.
  const D3D11_BOX box{UINT(copy.left()), UINT(copy.top()), 0, UINT(copy.right()), UINT(copy.bottom()), 1};
  d11->CopySubresourceRegion(host, 0, box.left, box.top, 0, r->shared11.get(), 0, &box);
  r->used11 = true;
  g->textures.endFrame();
  if (!g->alive()) return -1;
  if (dirty) *dirty = JsuiRect{copy.x(), copy.y(), copy.width(), copy.height()};
  return 0;
}

}  // namespace

namespace jsui {
namespace d3d {

bool available() { return true; }

JsuiGpuContext* create12(void* device, void* queue) {
  if (!device || !queue) return nullptr;
  auto queue12 = static_cast<ID3D12CommandQueue*>(queue);
  if (queue12->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
    SkDebugf("jsui_gpu_create_d3d12: the queue must be a DIRECT queue\n");
    return nullptr;
  }
  auto g = std::make_unique<D3DGpu>(false);
  g->device.retain(static_cast<ID3D12Device*>(device));
  g->queue.retain(queue12);
  if (!g->init("jsui_gpu_create_d3d12")) return nullptr;
  return g.release();
}

JsuiGpuContext* create11(void* device) {
  if (!device) return nullptr;
  auto g = std::make_unique<D3DGpu>(true);
  auto* d11 = static_cast<ID3D11Device*>(device);
  if (FAILED(d11->QueryInterface(IID_PPV_ARGS(&g->device11)))) {
    SkDebugf("jsui_gpu_create_d3d11: the device is not an ID3D11Device5 (Windows 10 1703+)\n");
    return nullptr;
  }
  gr_cp<ID3D11DeviceContext> immediate;
  d11->GetImmediateContext(&immediate);
  if (!immediate || FAILED(immediate->QueryInterface(IID_PPV_ARGS(&g->context11)))) return nullptr;
  // A D3D12 device on the same adapter.
  gr_cp<IDXGIDevice> dxgiDevice;
  gr_cp<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adesc{};
  if (FAILED(d11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
      FAILED(adapter->GetDesc(&adesc)))
    return nullptr;
  gr_cp<IDXGIFactory4> factory;
  gr_cp<IDXGIAdapter1> adapter1;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapterByLuid(adesc.AdapterLuid, IID_PPV_ARGS(&adapter1))))
    return nullptr;
  if (FAILED(D3D12CreateDevice(adapter1.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g->device)))) {
    SkDebugf("jsui_gpu_create_d3d11: no Direct3D 12 device on the D3D11 device's adapter\n");
    return nullptr;
  }
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g->device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g->queue)))) return nullptr;
  // The fence both APIs signal and wait on.
  if (FAILED(g->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g->fence12)))) return nullptr;
  HANDLE handle = nullptr;
  if (FAILED(g->device->CreateSharedHandle(g->fence12.get(), nullptr, GENERIC_ALL, nullptr, &handle))) return nullptr;
  HRESULT hr = g->device11->OpenSharedFence(handle, IID_PPV_ARGS(&g->fence11));
  CloseHandle(handle);
  if (FAILED(hr)) {
    SkDebugf("jsui_gpu_create_d3d11: D3D11 could not open the shared fence (0x%08lx)\n", (unsigned long)hr);
    return nullptr;
  }
  if (!g->init("jsui_gpu_create_d3d11")) return nullptr;
  return g.release();
}

bool owns(JsuiGpuContext* gpu) { return asD3D(gpu) != nullptr; }

void destroy(JsuiGpuContext* gpu) { delete asD3D(gpu); }

void purge(JsuiGpuContext* gpu, size_t cacheBytes) {
  D3DGpu* g = asD3D(gpu);
  if (!g) return;
  if (cacheBytes) g->cacheBytes = cacheBytes;
  if (!g->ctx || g->lost) return;
  if (cacheBytes) g->ctx->setResourceCacheLimit(cacheBytes);
  g->clearCaches();  // the next frames repaint fully
  g->ctx->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
}

int render(Surface& rt, JsuiGpuContext* gpu, const JsuiGpuTarget& t, JsuiRect* dirty) {
  if (dirty) *dirty = JsuiRect{0, 0, 0, 0};
  D3DGpu* g = asD3D(gpu);
  if (!g || !t.texture || t.width <= 0 || t.height <= 0) return -1;
  if (!g->alive()) return -1;
  return g->d3d11 ? render11(rt, g, t, dirty) : render12(rt, g, t, dirty);
}

}  // namespace d3d
}  // namespace jsui

#else  // !JSUI_D3D

namespace jsui {
namespace d3d {
bool available() { return false; }
JsuiGpuContext* create12(void*, void*) { return nullptr; }
JsuiGpuContext* create11(void*) { return nullptr; }
bool owns(JsuiGpuContext* gpu) { return gpu && gpu->kind == JsuiGpuContext::Kind::D3D; }
void destroy(JsuiGpuContext*) {}
void purge(JsuiGpuContext*, size_t) {}
int render(Surface&, JsuiGpuContext*, const JsuiGpuTarget&, JsuiRect* dirty) {
  if (dirty) *dirty = JsuiRect{0, 0, 0, 0};
  return -1;
}
}  // namespace d3d
}  // namespace jsui

#endif
