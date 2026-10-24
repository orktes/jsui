// jsui_unity's Direct3D backends (jsui_unity_gpu.h), Windows. Skia Graphite
// has no Direct3D backend: JSUI draws with Skia Ganesh on Direct3D 12
// (jsui_gpu_create_d3d12 / _d3d11, see jsui.h).
//
// Direct3D 11 (Unity's default on Windows): the context draws on a D3D12
// device of its own on the adapter of Unity's ID3D11Device
// (IUnityGraphicsD3D11::GetDevice) and copies the frame into the
// RenderTexture (Texture.GetNativeTexturePtr: its ID3D11Texture2D) with the
// device's immediate context, behind a shared fence. So the render event
// runs it on Unity's render thread, which owns the immediate context: the
// copy is queued there like any of Unity's own commands, after what Unity
// recorded before the event and before what it records after (the canvas
// sampling the texture). Nothing to prepare.
//
// Direct3D 12: Ganesh on Unity's ID3D12Device and its DIRECT command queue
// (IUnityGraphicsD3D12v8, Unity 6.3+). jsui_render_gpu_target executes its
// command lists on that queue before it returns, so, as on Vulkan, it must
// run where Unity has submitted what it recorded so far and is not using the
// queue itself:
//
//  1. prepare (the view's first plugin event, render thread):
//     RequestResourceState(texture, PIXEL_SHADER_RESOURCE) adds, to Unity's
//     active command list, the barrier from however Unity last used the
//     texture, and Unity's state tracking then has it in that state.
//  2. render (the second event): its id is configured (ConfigureEvent at
//     device init) for queue access with FlushCommandBuffers |
//     SyncWorkerThreads, so Unity first executes every command list it
//     recorded so far (the barrier included) and calls it on its
//     submission thread with the queue to itself (GetCommandQueue is only
//     valid there). The render's command lists go onto the queue after
//     Unity's, move the texture from PIXEL_SHADER_RESOURCE to
//     RENDER_TARGET and back to PIXEL_SHADER_RESOURCE, which is the state
//     Unity's tracking has, so the command lists Unity executes afterwards
//     (the canvas sampling it) need no fix-up and run after it in queue
//     order.
//
// Older Unity versions (no IUnityGraphicsD3D12v8, hence no way to learn or
// set the texture's state) leave the GPU path off on D3D12: the views
// rasterise on the CPU. The Skia context is created by the first render
// and goes with the device.
#include <d3d11.h>
#include <d3d12.h>

#define JSUI_UNITY_API_D3D 1
#include "jsui/jsui.h"
#include "jsui_unity_api.h"
#include "jsui_unity_gpu.h"

namespace {

// ------------------------------------------------------------------ D3D11

IUnityGraphicsD3D11* g_d3d11 = nullptr;
JsuiGpuContext* g_gpu11 = nullptr;

void prepare11(JsuiUnityJob* job) { job->prepared = true; }

void render11(JsuiUnityJob* job) {
  if (!g_gpu11) {
    g_gpu11 = jsui_gpu_create_d3d11(g_d3d11->GetDevice());
    if (!g_gpu11) {
      job->error = "no Skia context for Unity's Direct3D 11 device (it needs Windows 10 1703+ and a D3D12-capable adapter)";
      jsuiu_gpu_disable();
      jsuiu_job_done(job, false);
      return;
    }
  }
  JsuiGpuTarget t{};
  t.texture = job->texture;  // ID3D11Texture2D*
  t.width = job->width;
  t.height = job->height;
  t.flags = job->incremental ? JSUI_GPU_INCREMENTAL : 0;
  JsuiRect dirty;
  const bool ok = jsui_render_gpu_target(job->rt, g_gpu11, &t, &dirty) == 0;
  if (!ok) job->error = "Skia failed to render into the RenderTexture (Direct3D 11)";
  jsuiu_job_done(job, ok);
}

void shutdown11() {
  if (g_gpu11) jsui_gpu_destroy(g_gpu11);
  g_gpu11 = nullptr;
  g_d3d11 = nullptr;
}

const JsuiUnityBackend kD3D11 = {"Skia Ganesh (Direct3D 11 via 12)", prepare11, render11, shutdown11};

// ------------------------------------------------------------------ D3D12

IUnityGraphicsD3D12v8* g_d3d12 = nullptr;
JsuiGpuContext* g_gpu12 = nullptr;
bool g_create12Failed = false;

constexpr D3D12_RESOURCE_STATES kState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void prepare12(JsuiUnityJob* job) {
  g_d3d12->RequestResourceState(static_cast<ID3D12Resource*>(job->texture), kState);
  job->prepared = true;
}

void render12(JsuiUnityJob* job) {
  if (!g_gpu12 && !g_create12Failed) {
    g_gpu12 = jsui_gpu_create_d3d12(g_d3d12->GetDevice(), g_d3d12->GetCommandQueue());
    g_create12Failed = !g_gpu12;
  }
  if (!g_gpu12) {
    job->error = "no Skia Ganesh context on Unity's Direct3D 12 device";
    jsuiu_gpu_disable();
    jsuiu_job_done(job, false);
    return;
  }
  JsuiGpuTarget t{};
  t.texture = job->texture;  // ID3D12Resource*
  t.width = job->width;
  t.height = job->height;
  t.layout = int(kState);
  t.final_layout = int(kState);
  t.flags = job->incremental ? JSUI_GPU_INCREMENTAL : 0;
  JsuiRect dirty;
  const bool ok = jsui_render_gpu_target(job->rt, g_gpu12, &t, &dirty) == 0;
  if (!ok) job->error = "Skia failed to render into the RenderTexture (Direct3D 12)";
  jsuiu_job_done(job, ok);
}

void shutdown12() {
  if (g_gpu12) jsui_gpu_destroy(g_gpu12);
  g_gpu12 = nullptr;
  g_create12Failed = false;
  g_d3d12 = nullptr;
}

const JsuiUnityBackend kD3D12 = {"Skia Ganesh (Direct3D 12)", prepare12, render12, shutdown12};

}  // namespace

const JsuiUnityBackend* jsuiu_d3d11_backend(IUnityInterfaces* unity) {
  g_d3d11 = unity->Get<IUnityGraphicsD3D11>();
  return g_d3d11 && g_d3d11->GetDevice() ? &kD3D11 : nullptr;
}

const JsuiUnityBackend* jsuiu_d3d12_backend(IUnityInterfaces* unity) {
  g_d3d12 = unity->Get<IUnityGraphicsD3D12v8>();
  if (!g_d3d12) return nullptr;
  UnityD3D12PluginEventConfig render{};
  render.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
  render.flags = kUnityD3D12EventConfigFlag_FlushCommandBuffers | kUnityD3D12EventConfigFlag_SyncWorkerThreads;
  render.ensureActiveRenderTextureIsBound = false;
  UnityD3D12PluginEventConfig prepare{};
  prepare.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
  prepare.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
  prepare.ensureActiveRenderTextureIsBound = false;
  for (int slot = 0; slot < kJsuiUnitySlots; slot++) {
    g_d3d12->ConfigureEvent(jsuiu_event_id(slot, 0), &prepare);
    g_d3d12->ConfigureEvent(jsuiu_event_id(slot, 1), &render);
  }
  return &kD3D12;
}
