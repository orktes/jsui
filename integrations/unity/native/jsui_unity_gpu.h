// Between jsui_unity.cpp, the native-rendering-plugin side
// (jsui_unity_plugin.cpp: UnityPluginLoad, device events, event ids) and the
// per-graphics-API backends, built with JSUI_UNITY_GPU=1:
//
//   jsui_unity_metal.mm    Metal (macOS)              Skia Graphite
//   jsui_unity_vulkan.cpp  Vulkan (Linux, Windows)    Skia Graphite
//   jsui_unity_d3d.cpp     Direct3D 11 / 12 (Windows) Skia Ganesh on D3D12
//
// The backend is picked when Unity's graphics device initialises, from
// IUnityGraphics::GetRenderer().
//
// A render job is two plugin events that the view issues back to back
// (GL.IssuePluginEvent with the job's two event ids): `prepare`, then
// `render`. Both run on Unity's render thread, in the order Unity reaches
// them, except that a backend may move the actual render elsewhere (Vulkan:
// IUnityGraphicsVulkan::AccessQueue; D3D12: its render event is configured
// for queue access and runs on Unity's submission thread). Whatever runs it,
// the backend ends every job with exactly one jsuiu_job_done call.
#pragma once

#include "jsui/jsui.h"

struct JsuiUnityJob {
  JsuiRuntime* rt = nullptr;
  void* texture = nullptr;  // Texture.GetNativeTexturePtr()
  int width = 0, height = 0;
  bool incremental = false;  // the texture still holds this runtime's last frame
  // Backend scratch, set by prepare for render (reset before each job).
  void* image = nullptr;  // e.g. the VkImage behind `texture`
  unsigned format = 0, usage = 0;
  int layout = 0;
  bool prepared = false;
  const char* error = nullptr;  // why the job failed (static string), for the view's log
  int eventId = 0;              // the render event's id
  void* owner = nullptr;        // jsui_unity.cpp's
};

struct JsuiUnityBackend {
  const char* name;
  // Render thread, the job's first event. E.g. moves the texture into the
  // state the render will leave it in, recorded into Unity's command buffer.
  // A failure ends the job (jsuiu_job_done(job, false)); render then sees
  // job->prepared == false and does nothing.
  void (*prepare)(JsuiUnityJob* job);
  // The job's second event; renders (or did already: Vulkan) and ends it.
  void (*render)(JsuiUnityJob* job);
  // Device shutdown: frees the Skia context.
  void (*shutdown)();
};

// jsui_unity_plugin.cpp: the backend of the current device (NULL: none, CPU
// path) and whether the GPU path is usable (any thread).
const JsuiUnityBackend* jsuiu_backend();
bool jsuiu_gpu_supported();
// The GPU path is off for good on this device (no Skia context, a failed
// render).
void jsuiu_gpu_disable();
// The event ids of job slot `slot` (0 .. kJsuiUnitySlots-1): prepare and
// render. Configured per backend at device init (D3D12's render ids for
// queue access).
constexpr int kJsuiUnitySlots = 32;
int jsuiu_event_id(int slot, int phase);
// The (slot, phase) of a plugin event id; false when it is not one of ours.
// The ids are reserved from Unity (IUnityGraphics::ReserveEventIDRange) when
// the plugin loads.
bool jsuiu_event_slot(int eventId, int* slot, int* phase);

// jsui_unity.cpp: the job is over. ok: the texture holds the frame.
void jsuiu_job_done(JsuiUnityJob* job, bool ok);

// Backends, NULL when that API is unusable on this device (missing
// interface). Called on device init from the render thread.
struct IUnityInterfaces;
const JsuiUnityBackend* jsuiu_metal_backend(IUnityInterfaces* unity);
const JsuiUnityBackend* jsuiu_vulkan_backend(IUnityInterfaces* unity);
const JsuiUnityBackend* jsuiu_d3d11_backend(IUnityInterfaces* unity);
const JsuiUnityBackend* jsuiu_d3d12_backend(IUnityInterfaces* unity);
