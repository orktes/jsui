// jsui_unity as a Unity native rendering plugin (built with JSUI_UNITY_GPU).
//
// Unity calls UnityPluginLoad when it loads the library (the first P/Invoke)
// and the device event callback on its render thread. At device init the
// backend of Unity's graphics API is picked (jsui_unity_gpu.h); it creates
// its Skia context lazily, on the first render job, and frees it with the
// device (kUnityGfxDeviceEventShutdown). Graphics APIs without a backend
// (OpenGL Core, OpenGL ES, consoles) leave the GPU path off: the views
// rasterise on the CPU.
//
// Event ids: a render job is two plugin events (prepare, render) and a view
// owns one of kJsuiUnitySlots slots of ids for them, reserved from Unity in
// UnityPluginLoad so they cannot clash with other plugins' ids (Vulkan and
// D3D12 event configuration is keyed by id).
#include <atomic>

#include "jsui_unity_api.h"
#include "jsui_unity_gpu.h"

namespace {

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
std::atomic<const JsuiUnityBackend*> g_backend{nullptr};
std::atomic<bool> g_disabled{false};  // the GPU path failed on this device: CPU from now on
int g_eventBase = 0x4A530000;         // replaced by Unity's reservation

const JsuiUnityBackend* pick(UnityGfxRenderer renderer) {
  switch (renderer) {
#if JSUI_UNITY_METAL
    case kUnityGfxRendererMetal:
      return jsuiu_metal_backend(g_interfaces);
#endif
#if JSUI_UNITY_VULKAN
    case kUnityGfxRendererVulkan:
      return jsuiu_vulkan_backend(g_interfaces);
#endif
#if JSUI_UNITY_D3D
    case kUnityGfxRendererD3D11:
      return jsuiu_d3d11_backend(g_interfaces);
    case kUnityGfxRendererD3D12:
      return jsuiu_d3d12_backend(g_interfaces);
#endif
    default:
      return nullptr;
  }
}

void UNITY_INTERFACE_API OnDeviceEvent(UnityGfxDeviceEventType type) {
  if (type == kUnityGfxDeviceEventInitialize) {
    // UnityPluginLoad calls this too, so it may come twice for one device.
    if (g_backend.load()) return;
    g_disabled = false;
    g_backend = g_graphics ? pick(g_graphics->GetRenderer()) : nullptr;
  } else if (type == kUnityGfxDeviceEventShutdown) {
    if (const JsuiUnityBackend* b = g_backend.exchange(nullptr)) b->shutdown();
  }
}

}  // namespace

extern "C" {

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces) {
  g_interfaces = interfaces;
  g_graphics = interfaces->Get<IUnityGraphics>();
  if (!g_graphics) return;
  g_eventBase = g_graphics->ReserveEventIDRange(2 * kJsuiUnitySlots);
  g_graphics->RegisterDeviceEventCallback(OnDeviceEvent);
  // The device exists already when the plugin loads after startup.
  OnDeviceEvent(kUnityGfxDeviceEventInitialize);
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload() {
  if (g_graphics) g_graphics->UnregisterDeviceEventCallback(OnDeviceEvent);
  g_graphics = nullptr;
}

}  // extern "C"

const JsuiUnityBackend* jsuiu_backend() { return g_backend.load(); }

bool jsuiu_gpu_supported() { return g_backend.load() != nullptr && !g_disabled.load(); }

void jsuiu_gpu_disable() { g_disabled = true; }

int jsuiu_event_id(int slot, int phase) { return g_eventBase + 2 * slot + phase; }

bool jsuiu_event_slot(int eventId, int* slot, int* phase) {
  int i = eventId - g_eventBase;
  if (i < 0 || i >= 2 * kJsuiUnitySlots) return false;
  *slot = i / 2;
  *phase = i % 2;
  return true;
}
