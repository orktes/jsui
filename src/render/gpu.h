// GPU backend: Skia Ganesh on OpenGL / OpenGL ES / WebGL (jsui_gpu_* and
// jsui_render_gl* in jsui.h). Compiled in when JSUI_GPU is defined; without
// it the entry points exist but always fail, so hosts fall back to the CPU
// path.
#pragma once

#include "jsui/jsui.h"

namespace jsui {

class Runtime;

namespace gpu {

// True when this build has the GPU backend.
bool available();
JsuiGpuContext* createGL(JsuiGLGetProcFn getProc, void* user);
void destroy(JsuiGpuContext* gpu);
void reset(JsuiGpuContext* gpu);
void contextLost(JsuiGpuContext* gpu);
void purge(JsuiGpuContext* gpu, size_t cacheBytes);
// Full repaint of `rt` into `target`. 0 on success, -1 on failure.
int render(Runtime& rt, JsuiGpuContext* gpu, const JsuiGLTarget& target);

}  // namespace gpu
}  // namespace jsui
