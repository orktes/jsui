// jsui_unity's Metal backend (jsui_unity_gpu.h): Skia Graphite on Unity's
// own MTLDevice and MTLCommandQueue.
//
// Everything here runs on Unity's render thread: the Graphite context is
// created lazily by the first render job and goes with the device
// (kUnityGfxDeviceEventShutdown, jsui_unity_plugin.cpp).
//
// Ordering: a render job first ends Unity's current encoder and commits its
// command buffer (what Unity recorded so far this frame), then Graphite
// commits its own to the same queue; Unity records the rest of the frame,
// which samples the RenderTexture, into a new command buffer. Same queue and
// tracked resources, so the GPU runs them in that order (like the Godot
// integration's Metal path). Nothing to prepare.
#import <Metal/Metal.h>

#include "jsui_unity_api.h"
#include "jsui/jsui.h"
#include "jsui_unity_gpu.h"

namespace {

IUnityGraphicsMetalV2* g_metal = nullptr;
JsuiGpuContext* g_gpu = nullptr;

void prepare(JsuiUnityJob* job) { job->prepared = true; }

void render(JsuiUnityJob* job) {
  if (!g_gpu) {
    id<MTLDevice> device = g_metal->MetalDevice();
    id<MTLCommandQueue> queue = g_metal->CommandQueue();
    g_gpu = jsui_gpu_create_metal((__bridge void*)device, (__bridge void*)queue);
    if (!g_gpu) {
      jsuiu_gpu_disable();
      jsuiu_job_done(job, false);
      return;
    }
  }
  g_metal->EndCurrentCommandEncoder();
  g_metal->CommitCurrentCommandBuffer();
  JsuiGpuTarget t{};
  t.texture = job->texture;
  t.format = unsigned([(__bridge id<MTLTexture>)job->texture pixelFormat]);
  t.width = job->width;
  t.height = job->height;
  t.flags = job->incremental ? JSUI_GPU_INCREMENTAL : 0;
  JsuiRect dirty;
  jsuiu_job_done(job, jsui_render_gpu_target(job->rt, g_gpu, &t, &dirty) == 0);
}

void shutdown() {
  if (g_gpu) jsui_gpu_destroy(g_gpu);
  g_gpu = nullptr;
  g_metal = nullptr;
}

const JsuiUnityBackend kMetal = {"Skia Graphite (Metal)", prepare, render, shutdown};

}  // namespace

const JsuiUnityBackend* jsuiu_metal_backend(IUnityInterfaces* unity) {
  g_metal = unity->Get<IUnityGraphicsMetalV2>();
  return g_metal ? &kMetal : nullptr;
}
