// Skia Ganesh on Direct3D 12 (jsui_gpu_create_d3d12 / _d3d11 in jsui.h).
#include "render/gpu.h"

#if defined(JSUI_D3D)
#error "JSUI_D3D: the Direct3D backend is not implemented yet"
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
