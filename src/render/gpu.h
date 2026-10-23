// GPU backends behind one opaque JsuiGpuContext (jsui_gpu_* in jsui.h):
//
//   * GL: Skia Ganesh on OpenGL / OpenGL ES / WebGL (jsui_render_gl*), compiled
//     in when JSUI_GPU is defined (gpu.cpp).
//   * WebGPU: Skia Graphite on a WebGPU device (jsui_render_webgpu*), compiled
//     in when JSUI_WEBGPU is defined (gpu_webgpu.cpp): the browser's WebGPU
//     through emscripten, or Dawn native with an embedder-provided device.
//   * Graphite: Skia Graphite on an embedder's Vulkan / Metal device
//     (jsui_gpu_create_vulkan / _metal, jsui_render_gpu_target), compiled in
//     when JSUI_VULKAN / JSUI_GRAPHITE_METAL is defined (gpu_graphite.cpp).
//
// Without a backend its entry points exist but always fail, so hosts fall
// back to the next backend / the CPU path.
#pragma once

#include "jsui/jsui.h"

struct JsuiGpuContext {
  enum class Kind { GL, WebGPU, Graphite };
  explicit JsuiGpuContext(Kind k) : kind(k) {}
  virtual ~JsuiGpuContext() = default;
  const Kind kind;
};

namespace jsui {

class Runtime;

namespace gpu {

// True when this build has the GL backend.
bool available();
JsuiGpuContext* createGL(JsuiGLGetProcFn getProc, void* user);
void destroy(JsuiGpuContext* gpu);
void reset(JsuiGpuContext* gpu);
void contextLost(JsuiGpuContext* gpu);
void purge(JsuiGpuContext* gpu, size_t cacheBytes);
// Full repaint of `rt` into `target`. 0 on success, -1 on failure.
int render(Runtime& rt, JsuiGpuContext* gpu, const JsuiGLTarget& target);

// Tests only (gpu_retained_frame_matches_direct): variations of the
// retained-frame path, to tell which step misbehaves on a driver.
enum RetainedDebug : unsigned {
  kRetainedOneRect = 1,      // damage-only repaints in one bounding rect, not split passes
  kRetainedFlushFirst = 2,   // flush the retained frame's repaint before copying it into the target
  kRetainedFullRepaint = 4,  // repaint the whole retained frame every frame: only the copy remains
};
void setRetainedDebug(unsigned flags);

}  // namespace gpu

namespace webgpu {

// True when this build has the WebGPU (Graphite) backend.
bool available();
JsuiGpuContext* create(void* instance, void* device, void* queue);
void destroy(JsuiGpuContext* gpu);
void contextLost(JsuiGpuContext* gpu);
void purge(JsuiGpuContext* gpu, size_t cacheBytes);
// 0 on success, -1 on failure (device lost: see jsui.h).
int render(Runtime& rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget& target);

}  // namespace webgpu

namespace graphite_gpu {

// True when this build has the Vulkan, Metal and/or Dawn Graphite backend.
bool available();
JsuiGpuContext* createVulkan(const JsuiVulkanDevice& device);
JsuiGpuContext* createMetal(void* device, void* queue);
JsuiGpuContext* createDawn(void* instance, void* device, void* queue);  // entry points: dlsym
JsuiGpuContext* createDawnProcs(void* getProcAddress, void* instance, void* device, void* queue);
const char* dawnRevision();  // the Dawn the build's headers are from; nullptr without Dawn
bool owns(JsuiGpuContext* gpu);  // a Graphite (Vulkan / Metal / Dawn) context
void destroy(JsuiGpuContext* gpu);
void purge(JsuiGpuContext* gpu, size_t cacheBytes);
// 0 on success, -1 on failure; `dirty` (optional) gets the repainted rect.
int render(Runtime& rt, JsuiGpuContext* gpu, const JsuiGpuTarget& target, JsuiRect* dirty);

}  // namespace graphite_gpu
}  // namespace jsui
