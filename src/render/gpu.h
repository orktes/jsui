// GPU backends behind one opaque JsuiGpuContext (jsui_gpu_* in jsui.h):
//
//   * GL: Skia Ganesh on OpenGL / OpenGL ES / WebGL (jsui_render_gl*), compiled
//     in when JSUI_GPU is defined (gpu.cpp).
//   * WebGPU: Skia Graphite on a WebGPU device (jsui_render_webgpu*), compiled
//     in when JSUI_WEBGPU is defined (gpu_webgpu.cpp): the browser's WebGPU
//     through emscripten, or Dawn native with an embedder-provided device.
//
// Without a backend its entry points exist but always fail, so hosts fall
// back to the next backend / the CPU path.
#pragma once

#include "jsui/jsui.h"

struct JsuiGpuContext {
  enum class Kind { GL, WebGPU };
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
}  // namespace jsui
