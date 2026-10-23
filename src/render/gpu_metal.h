// Metal backend: Skia Ganesh on Metal (jsui_metal_* and jsui_render_metal in
// jsui.h). Compiled in when JSUI_METAL is defined (iOS builds by default);
// without it the entry points exist but always fail, so hosts fall back to
// the CPU path.
#pragma once

#include "jsui/jsui.h"

namespace jsui {

class Surface;

namespace metal {

bool available();
JsuiMetalContext* create(void* device, void* queue);
void destroy(JsuiMetalContext* mtl);
void purge(JsuiMetalContext* mtl, size_t cacheBytes);
// Full repaint of `rt` into `texture`. 0 on success, -1 on failure.
int render(Surface& rt, JsuiMetalContext* mtl, void* texture, unsigned format, int width, int height, int flags);

}  // namespace metal
}  // namespace jsui
