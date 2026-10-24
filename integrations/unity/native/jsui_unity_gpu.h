// Between jsui_unity.cpp and the graphics-API side (jsui_unity_metal.mm),
// built with JSUI_UNITY_GPU=1. All of these run on Unity's render thread,
// except jsuiu_gpu_supported (any thread).
#pragma once

struct JsuiGpuContext;

// Unity renders with an API the plugin can draw into (Metal) and the Skia
// context did not fail on this device.
bool jsuiu_gpu_supported();
// Before a render: the Skia context (created on first use) with Unity's
// work so far committed, or NULL (no usable device: CPU path).
JsuiGpuContext* jsuiu_gpu_begin();
// The texture's native pixel format (MTLPixelFormat).
unsigned jsuiu_gpu_texture_format(void* texture);
// A render failed (device lost, rejected target): the GPU path is off.
void jsuiu_gpu_fail();
// Frees the Skia context (device shutdown).
void jsuiu_gpu_shutdown();
