// Skia Ganesh on Direct3D 12 (jsui_gpu_create_d3d12 / _d3d11,
// jsui_render_gpu_target): devices of our own on WARP (Windows' software
// adapter, present without a GPU), frames rendered into a texture, read back
// and compared with the CPU raster of the same frame, including incremental
// frames (the texture kept between them) and the retained-frame path of
// non-incremental targets. The D3D11 context is checked the same way on a
// WARP D3D11 device (its frames go through a D3D12 device of its own and a
// shared texture), plus JSUI_GPU_NO_CLEAR compositing over the host's pixels.
//
// Needs Windows and JSUI_D3D (a Skia built with skia_use_direct3d:
// scripts/build-skia-desktop.sh); otherwise the tests print SKIP and pass.
// JSUI_D3D_TEST_REQUIRE=1 turns a skip into a failure (the Windows CI job).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

#if defined(_WIN32) && defined(JSUI_D3D)
#define JSUI_D3D_TESTS 1
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace {

bool requireD3D() {
  const char* v = std::getenv("JSUI_D3D_TEST_REQUIRE");
  return v && *v && std::strcmp(v, "0") != 0;
}

void skip(const char* test, const char* why) {
  if (requireD3D()) {
    std::fprintf(stderr, "  %s: %s but JSUI_D3D_TEST_REQUIRE is set\n", test, why);
    jsui_test::failures()++;
    return;
  }
  std::printf("SKIP %s: %s\n", test, why);
}

#if JSUI_D3D_TESTS

constexpr int kCopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;

// Mean absolute channel difference and the share of pixels off by > tol.
void compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int tol, double* mean, double* over) {
  uint64_t sum = 0, n = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int m = 0;
    for (int c = 0; c < 4; c++) {
      int x = int((a[i] >> (c * 8)) & 255), y = int((b[i] >> (c * 8)) & 255);
      int v = x > y ? x - y : y - x;
      sum += uint64_t(v);
      if (v > m) m = v;
    }
    if (m > tol) n++;
  }
  *mean = double(sum) / double(a.size() * 4);
  *over = double(n) / double(a.size());
}

// The GPU frame matches the CPU one (antialiasing differs a little).
bool matchesCpu(const char* what, int frame, const std::vector<uint32_t>& gpu, const std::vector<uint32_t>& cpu,
                const JsuiRect& dirty) {
  double mean = 0, over = 0;
  compare(gpu, cpu, 24, &mean, &over);
  std::printf("  %s frame %d: dirty %d,%d %dx%d, mean diff %.3f\n", what, frame, dirty.x, dirty.y, dirty.width,
              dirty.height, mean);
  if (mean > 1.0 || over > 0.01) {
    std::fprintf(stderr, "  %s frame %d: GPU differs from CPU: mean %.3f, %.2f%% of pixels off by > 24\n", what, frame,
                 mean, over * 100);
    jsui_test::failures()++;
    return false;
  }
  return true;
}

// Two boxes in opposite corners that move every 16 ms (scattered damage)
// over a gradient backdrop, a rounded border and text (as vulkan_test.cpp).
const char* kBundle =
    "const n = __jsui_native;"
    "const bg = n.createNode('View');"
    "n.setProps(bg, {style: {position: 'absolute', left: 0, top: 0, width: 240, height: 160,"
    "  backgroundImage: 'linear-gradient(135deg, #203040, #402030)'}});"
    "n.appendChild(n.rootId, bg);"
    "const card = n.createNode('View');"
    "n.setProps(card, {style: {position: 'absolute', left: 70, top: 50, width: 100, height: 60, borderRadius: 12,"
    "  borderWidth: 2, borderColor: '#ffffff', backgroundColor: 'rgba(255,255,255,0.15)',"
    "  boxShadow: '0 4px 10px rgba(0,0,0,0.5)'}});"
    "n.appendChild(n.rootId, card);"
    "const t = n.createNode('Text'); const s = n.createNode('RawText'); n.setText(s, 'Direct3D');"
    "n.appendChild(t, s); n.setProps(t, {style: {color: '#ffcc00', fontSize: 16}}); n.appendChild(card, t);"
    "const a = n.createNode('View'), b = n.createNode('View');"
    "n.appendChild(n.rootId, a); n.appendChild(n.rootId, b);"
    "let f = 0;"
    "function step() {"
    "  n.setProps(a, {style: {position: 'absolute', left: 4 + f * 2, top: 4, width: 16, height: 16,"
    "    borderRadius: 8, backgroundColor: '#00ff80'}});"
    "  n.setProps(b, {style: {position: 'absolute', left: 216 - f * 2, top: 140, width: 16, height: 16,"
    "    backgroundColor: '#ff4060', opacity: 0.8}});"
    "  f++; if (f < 8) setTimeout(step, 16);"
    "}"
    "step();";

JsuiRuntime* makeRuntime(int w, int h, const char* src = kBundle) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = w;
  cfg.height = h;
  JsuiRuntime* rt = jsui_create(&cfg);
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "d3d.js"), 0);
  return rt;
}

// A WARP D3D12 device, a DIRECT queue, an RGBA8 render target JSUI draws
// into and a readback buffer it is copied to for checking.
struct D3D12 {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  uint64_t fenceValue = 0;
  ComPtr<ID3D12Resource> target, readback;
  std::vector<ComPtr<ID3D12Resource>> owned;  // host textures and their upload buffers
  bool debug = false;
  int w = 0, h = 0;
  UINT rowPitch = 0;

  // Empty string on success, else why there is no device.
  std::string init(int width, int height) {
    w = width;
    h = height;
    // The debug layer, where the Graphics Tools are installed, reports
    // wrong resource states (printed after the tests).
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
      dbg->EnableDebugLayer();
      debug = true;
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "CreateDXGIFactory1 failed";
    ComPtr<IDXGIAdapter1> warp;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return "no WARP adapter";
    if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
      return "D3D12CreateDevice on WARP failed";
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return "CreateCommandQueue failed";
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
      return "CreateCommandList failed";
    list->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return "CreateFence failed";
    target = texture(w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    rowPitch = (UINT(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    readback = buffer(UINT64(rowPitch) * h, D3D12_HEAP_TYPE_READBACK);
    if (!target || !readback) return "could not create the target";
    return "";
  }

  ComPtr<ID3D12Resource> texture(int tw, int th, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = UINT64(tw);
    d.Height = UINT(th);
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
  }

  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                    type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                   : D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
  }

  void begin() {
    alloc->Reset();
    list->Reset(alloc.Get(), nullptr);
  }
  // Executes the list on the queue (after JSUI's work) and waits for it.
  void submitAndWait() {
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fenceValue);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(fenceValue, e);
    WaitForSingleObject(e, INFINITE);
    CloseHandle(e);
  }

  // The target (in COPY_SOURCE, as JSUI left it) as premultiplied RGBA.
  std::vector<uint32_t> read() {
    begin();
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(w), UINT(h), 1, rowPitch};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submitAndWait();
    std::vector<uint32_t> px(size_t(w) * h);
    void* p = nullptr;
    D3D12_RANGE range{0, size_t(rowPitch) * h};
    readback->Map(0, &range, &p);
    for (int y = 0; y < h; y++) std::memcpy(&px[size_t(y) * w], static_cast<char*>(p) + size_t(y) * rowPitch, size_t(w) * 4);
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return px;
  }

  // A sampled texture holding `px` (tw x th RGBA8), left in COPY_DEST like
  // Godot's mirror textures (JsuiTexture.usage = COPY_DEST).
  ID3D12Resource* makeTexture(const std::vector<uint32_t>& px, int tw, int th) {
    ComPtr<ID3D12Resource> tex = texture(tw, th, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!tex) return nullptr;
    owned.push_back(tex);
    upload(tex.Get(), px, tw, th);
    return tex.Get();
  }
  // New contents for a makeTexture() texture (in COPY_DEST); waits.
  void upload(ID3D12Resource* tex, const std::vector<uint32_t>& px, int tw, int th) {
    const UINT pitch = (UINT(tw) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    ComPtr<ID3D12Resource> up = buffer(UINT64(pitch) * th, D3D12_HEAP_TYPE_UPLOAD);
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    for (int y = 0; y < th; y++) std::memcpy(static_cast<char*>(p) + size_t(y) * pitch, &px[size_t(y) * tw], size_t(tw) * 4);
    up->Unmap(0, nullptr);
    begin();
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(tw), UINT(th), 1, pitch};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submitAndWait();
  }

  // Debug layer errors (wrong resource states and the like), printed.
  int debugErrors() {
    ComPtr<ID3D12InfoQueue> q;
    if (!debug || FAILED(device.As(&q))) return 0;
    int errors = 0;
    for (UINT64 i = 0; i < q->GetNumStoredMessages(); i++) {
      SIZE_T len = 0;
      q->GetMessage(i, nullptr, &len);
      std::vector<char> buf(len);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
      if (FAILED(q->GetMessage(i, m, &len))) continue;
      if (m->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
      // Skia's GrD3DDescriptorHeap asks every heap for its GPU start, also
      // the CPU-only ones (it only uses the result for shader-visible heaps).
      if (std::string_view(m->pDescription, m->DescriptionByteLength).find("GetGPUDescriptorHandleForHeapStart") !=
          std::string_view::npos)
        continue;
      errors++;
      std::printf("  D3D12 debug layer: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
    }
    q->ClearStoredMessages();
    return errors;
  }
};

JsuiGpuTarget target12(D3D12& d, int layout, int flags) {
  JsuiGpuTarget t{};
  t.texture = d.target.Get();
  t.width = d.w;
  t.height = d.h;
  t.layout = layout;
  t.final_layout = kCopySource;
  t.flags = flags;
  return t;
}

// A WARP D3D11 device and an RGBA8 render texture (no shared or special
// flags) the D3D11 context's frames are copied into, read back through a
// staging texture.
struct D3D11 {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  int w = 0, h = 0;

  std::string init(int width, int height) {
    w = width;
    h = height;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
      return "D3D11CreateDevice (WARP) failed";
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &target))) return "CreateTexture2D failed";
    device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return "CreateTexture2D (staging) failed";
    return "";
  }

  std::vector<uint32_t> read() {
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    std::vector<uint32_t> px(size_t(w) * h);
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return px;  // waits for the GPU
    for (int y = 0; y < h; y++)
      std::memcpy(&px[size_t(y) * w], static_cast<char*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
    context->Unmap(staging.Get(), 0);
    return px;
  }
};

#endif  // JSUI_D3D_TESTS

}  // namespace

// D3D12: incremental frames into a retained target, then the same frames
// into a target the host does not keep (the context's retained frame).
TEST(d3d12_ganesh_matches_cpu) {
#if !JSUI_D3D_TESTS
  skip("d3d12_ganesh_matches_cpu", "needs Windows and JSUI_D3D");
#else
  const int W = 240, H = 160;
  D3D12 d;
  std::string why = d.init(W, H);
  if (!why.empty()) {
    skip("d3d12_ganesh_matches_cpu", why.c_str());
    return;
  }
  JsuiGpuContext* gpu = jsui_gpu_create_d3d12(d.device.Get(), d.queue.Get());
  CHECK(gpu != nullptr);
  if (!gpu) return;
  int layout = D3D12_RESOURCE_STATE_COMMON;  // the target's state, across both passes
  for (int pass = 0; pass < 2; pass++) {
    const bool incremental = pass == 0;
    const char* what = incremental ? "incremental" : "retained";
    // The same bundle in two runtimes on the same clock: one on the GPU, one
    // as the CPU reference.
    JsuiRuntime* g = makeRuntime(W, H);
    JsuiRuntime* c = makeRuntime(W, H);
    std::vector<uint32_t> cpu(size_t(W) * H);
    int smallFrames = 0;
    for (int frame = 0; frame < 8; frame++) {
      const double now = frame * 16.0 + 1;
      jsui_tick(g, now);
      jsui_tick(c, now);
      JsuiGpuTarget t = target12(d, layout, incremental && frame > 0 ? JSUI_GPU_INCREMENTAL : 0);
      JsuiRect dirty{};
      CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
      layout = kCopySource;
      if (frame > 0 && dirty.width * dirty.height < W * H) smallFrames++;
      jsui_render_rgba(c, cpu.data(), W, H, W * 4);
      std::vector<uint32_t> out = d.read();
      matchesCpu(what, frame, out, cpu, dirty);
      // Inside the moved (translucent) box: the same colour within rounding.
      if (frame == 7) {
        const size_t i = size_t(140 + 8) * W + (216 - 14 + 8);
        for (int ch = 0; ch < 4; ch++)
          CHECK(std::abs(int((out[i] >> (ch * 8)) & 255) - int((cpu[i] >> (ch * 8)) & 255)) <= 2);
      }
    }
    // Incremental targets repaint just the damage; the others are written
    // whole every frame.
    if (incremental) CHECK(smallFrames >= 6);
    else CHECK_EQ(smallFrames, 0);
    if (incremental) {
      // Nothing changed: nothing reported dirty.
      jsui_tick(g, 300);
      JsuiGpuTarget t = target12(d, kCopySource, JSUI_GPU_INCREMENTAL);
      JsuiRect dirty{1, 1, 1, 1};
      CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
      CHECK_EQ(dirty.width * dirty.height, 0);
      // A state change only (COPY_SOURCE -> PIXEL_SHADER_RESOURCE -> COPY_SOURCE).
      t.final_layout = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
      t.layout = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      t.final_layout = kCopySource;
      CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
      jsui_render_rgba(c, cpu.data(), W, H, W * 4);
      matchesCpu("unchanged", 8, d.read(), cpu, dirty);
    }
    jsui_destroy(g);
    jsui_destroy(c);
  }
  jsui_gpu_purge(gpu, 0);
  jsui_gpu_destroy(gpu);
  CHECK_EQ(d.debugErrors(), 0);
#endif
}

// Host textures (JSUI_TEXTURE_D3D12): a texture the host keeps in COPY_DEST
// (JsuiTexture.usage) drawn in place, new contents shown after
// jsui_texture_updated, the texture back in COPY_DEST after every render
// (the host's next copy into it needs that state).
TEST(d3d12_host_texture) {
#if !JSUI_D3D_TESTS
  skip("d3d12_host_texture", "needs Windows and JSUI_D3D");
#else
  const int W = 64, H = 48;
  D3D12 d;
  std::string why = d.init(W, H);
  if (!why.empty()) {
    skip("d3d12_host_texture", why.c_str());
    return;
  }
  JsuiGpuContext* gpu = jsui_gpu_create_d3d12(d.device.Get(), d.queue.Get());
  CHECK(gpu != nullptr);
  if (!gpu) return;
  std::vector<uint32_t> px(8);
  for (int x = 0; x < 4; x++) {
    px[x] = 0xff0000ffu;      // top row red
    px[4 + x] = 0xffff0000u;  // bottom row blue
  }
  ID3D12Resource* tex = d.makeTexture(px, 4, 2);
  CHECK(tex != nullptr);
  const char* src =
      "const n = __jsui_native; const v = n.createNode('Image');"
      "n.setProps(v, {texture: 'engine', resizeMode: 'stretch',"
      " style: {position: 'absolute', left: 8, top: 8, width: 40, height: 32}});"
      "n.appendChild(n.rootId, v);";
  JsuiRuntime* rt = makeRuntime(W, H, src);
  JsuiTexture t{};
  t.kind = JSUI_TEXTURE_D3D12;
  t.handle = tex;
  t.width = 4;
  t.height = 2;
  t.usage = D3D12_RESOURCE_STATE_COPY_DEST;
  CHECK_EQ(jsui_set_texture(rt, "engine", &t), 1);
  int layout = D3D12_RESOURCE_STATE_COMMON;
  auto render = [&](int flags) {
    jsui_tick(rt, 16);
    JsuiGpuTarget gt = target12(d, layout, flags);
    CHECK_EQ(jsui_render_gpu_target(rt, gpu, &gt, nullptr), 0);
    layout = kCopySource;
    return d.read();
  };
  std::vector<uint32_t> out = render(0);
  auto at = [&](int x, int y) { return out[size_t(y) * W + x]; };
  CHECK_EQ(at(28, 12), 0xff0000ffu);
  CHECK_EQ(at(28, 36), 0xffff0000u);
  CHECK_EQ(at(2, 2), 0u);
  // The host copies the next "frame" into the same texture (COPY_DEST).
  std::vector<uint32_t> green(8, 0xff00ff00u);
  d.upload(tex, green, 4, 2);
  CHECK_EQ(jsui_texture_updated(rt, "engine"), 1);
  CHECK(jsui_needs_redraw(rt));
  out = render(JSUI_GPU_INCREMENTAL);
  CHECK_EQ(at(28, 12), 0xff00ff00u);
  CHECK_EQ(at(28, 36), 0xff00ff00u);
  // Once more, now from the wrap cached across frames.
  std::vector<uint32_t> white(8, 0xffffffffu);
  d.upload(tex, white, 4, 2);
  CHECK_EQ(jsui_texture_updated(rt, "engine"), 1);
  out = render(JSUI_GPU_INCREMENTAL);
  CHECK_EQ(at(28, 12), 0xffffffffu);
  // A format Ganesh cannot sample here: the view is left empty.
  t.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  CHECK_EQ(jsui_set_texture(rt, "engine", &t), 1);
  out = render(0);
  CHECK_EQ(at(28, 12), 0u);
  jsui_destroy(rt);
  jsui_gpu_destroy(gpu);
  CHECK_EQ(d.debugErrors(), 0);
#endif
}

// D3D11: frames drawn on the context's own D3D12 device and copied into a
// plain D3D11 render texture, incremental (only the damage copied) and full;
// JSUI_GPU_NO_CLEAR composites over what the host drew into it.
TEST(d3d11_ganesh_matches_cpu) {
#if !JSUI_D3D_TESTS
  skip("d3d11_ganesh_matches_cpu", "needs Windows and JSUI_D3D");
#else
  const int W = 240, H = 160;
  D3D11 d;
  std::string why = d.init(W, H);
  if (!why.empty()) {
    skip("d3d11_ganesh_matches_cpu", why.c_str());
    return;
  }
  JsuiGpuContext* gpu = jsui_gpu_create_d3d11(d.device.Get());
  if (!gpu) {
    // E.g. no D3D12 on the adapter, or no cross-API sharing on WARP.
    skip("d3d11_ganesh_matches_cpu", "jsui_gpu_create_d3d11 returned NULL on the WARP device");
    return;
  }
  for (int pass = 0; pass < 2; pass++) {
    const bool incremental = pass == 0;
    const char* what = incremental ? "d3d11 incremental" : "d3d11 full";
    JsuiRuntime* g = makeRuntime(W, H);
    JsuiRuntime* c = makeRuntime(W, H);
    std::vector<uint32_t> cpu(size_t(W) * H);
    int smallFrames = 0;
    for (int frame = 0; frame < 8; frame++) {
      const double now = frame * 16.0 + 1;
      jsui_tick(g, now);
      jsui_tick(c, now);
      JsuiGpuTarget t{};
      t.texture = d.target.Get();
      t.width = W;
      t.height = H;
      t.flags = incremental && frame > 0 ? JSUI_GPU_INCREMENTAL : 0;
      JsuiRect dirty{};
      CHECK_EQ(jsui_render_gpu_target(g, gpu, &t, &dirty), 0);
      if (frame > 0 && dirty.width * dirty.height < W * H) smallFrames++;
      jsui_render_rgba(c, cpu.data(), W, H, W * 4);
      matchesCpu(what, frame, d.read(), cpu, dirty);
    }
    if (incremental) CHECK(smallFrames >= 6);
    else CHECK_EQ(smallFrames, 0);
    jsui_destroy(g);
    jsui_destroy(c);
  }
  // NO_CLEAR: a red box over the host's blue.
  const char* src =
      "const n = __jsui_native; const v = n.createNode('View');"
      "n.setProps(v, {style: {position: 'absolute', left: 8, top: 8, width: 16, height: 16,"
      " backgroundColor: '#ff0000'}});"
      "n.appendChild(n.rootId, v);";
  JsuiRuntime* rt = makeRuntime(W, H, src);
  const float blue[4] = {0, 0, 1, 1};
  d.context->ClearRenderTargetView(d.rtv.Get(), blue);
  jsui_tick(rt, 16);
  JsuiGpuTarget t{};
  t.texture = d.target.Get();
  t.width = W;
  t.height = H;
  t.flags = JSUI_GPU_NO_CLEAR;
  CHECK_EQ(jsui_render_gpu_target(rt, gpu, &t, nullptr), 0);
  std::vector<uint32_t> out = d.read();
  CHECK_EQ(out[size_t(16) * W + 16], 0xff0000ffu);
  CHECK_EQ(out[size_t(100) * W + 100], 0xffff0000u);
  jsui_destroy(rt);
  jsui_gpu_destroy(gpu);
#endif
}

TEST(d3d_capi_rejects_bad_input) {
  CHECK(jsui_gpu_create_d3d12(nullptr, nullptr) == nullptr);
  CHECK(jsui_gpu_create_d3d11(nullptr) == nullptr);
  JsuiRuntime* rt = jsui_create(nullptr);
  JsuiGpuTarget t{};
  CHECK_EQ(jsui_render_gpu_target(rt, nullptr, &t, nullptr), -1);
  jsui_destroy(rt);
}
