// jsui_unity's Metal side: the plugin as a Unity native rendering plugin.
//
// Unity calls UnityPluginLoad when it loads the bundle (the first P/Invoke)
// and the device event callback on its render thread. Everything here except
// UnityPluginLoad / Unload runs on the render thread: the Graphite context is
// created lazily by the first render job on Unity's MTLDevice and
// MTLCommandQueue, and goes with the device (kUnityGfxDeviceEventShutdown).
//
// Ordering: a render job first ends Unity's current encoder and commits its
// command buffer (what Unity recorded so far this frame), then Graphite
// commits its own to the same queue; Unity records the rest of the frame,
// which samples the RenderTexture, into a new command buffer. Same queue and
// tracked resources, so the GPU runs them in that order (like the Godot
// integration's Metal path).
#import <Metal/Metal.h>

#include "IUnityGraphics.h"
#include "IUnityGraphicsMetal.h"
#include "IUnityInterface.h"
#include "jsui/jsui.h"
#include "jsui_unity_gpu.h"

namespace {

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
IUnityGraphicsMetalV2* g_metal = nullptr;
JsuiGpuContext* g_gpu = nullptr;
bool g_gpuFailed = false;  // context creation failed on this device: CPU from now on

void UNITY_INTERFACE_API OnDeviceEvent(UnityGfxDeviceEventType type) {
  if (type == kUnityGfxDeviceEventInitialize) {
    g_metal = g_graphics->GetRenderer() == kUnityGfxRendererMetal ? g_interfaces->Get<IUnityGraphicsMetalV2>() : nullptr;
    g_gpuFailed = false;
  } else if (type == kUnityGfxDeviceEventShutdown) {
    jsuiu_gpu_shutdown();
    g_metal = nullptr;
  }
}

}  // namespace

extern "C" {

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces) {
  g_interfaces = interfaces;
  g_graphics = interfaces->Get<IUnityGraphics>();
  g_graphics->RegisterDeviceEventCallback(OnDeviceEvent);
  // The device exists already when the plugin loads after startup.
  OnDeviceEvent(kUnityGfxDeviceEventInitialize);
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload() {
  if (g_graphics) g_graphics->UnregisterDeviceEventCallback(OnDeviceEvent);
  g_graphics = nullptr;
}

}  // extern "C"

bool jsuiu_gpu_supported() { return g_metal != nullptr && !g_gpuFailed; }

JsuiGpuContext* jsuiu_gpu_begin() {
  if (!g_metal || g_gpuFailed) return nullptr;
  if (!g_gpu) {
    id<MTLDevice> device = g_metal->MetalDevice();
    id<MTLCommandQueue> queue = g_metal->CommandQueue();
    g_gpu = jsui_gpu_create_metal((__bridge void*)device, (__bridge void*)queue);
    if (!g_gpu) {
      g_gpuFailed = true;
      return nullptr;
    }
  }
  g_metal->EndCurrentCommandEncoder();
  g_metal->CommitCurrentCommandBuffer();
  return g_gpu;
}

unsigned jsuiu_gpu_texture_format(void* texture) {
  return unsigned([(__bridge id<MTLTexture>)texture pixelFormat]);
}

void jsuiu_gpu_fail() { g_gpuFailed = true; }

void jsuiu_gpu_shutdown() {
  if (g_gpu) jsui_gpu_destroy(g_gpu);
  g_gpu = nullptr;
}
