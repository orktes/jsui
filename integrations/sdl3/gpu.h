// The SDL3 example's GPU paths: Skia draws the HUD straight into the
// SDL_Texture that SDL_Renderer composites over the arena, on the renderer's
// own GPU API, so nothing is rasterised on the CPU or uploaded.
//
//   metal   Skia Graphite on SDL's MTLDevice (gpu_metal.mm)
//   vulkan  Skia Graphite on SDL's VkDevice and queue (gpu.cpp)
//   opengl  Skia Ganesh in SDL's GL context (gpu.cpp)
//
// SDL_Renderer is not built for this: it exposes some native handles but no
// "draw into this texture yourself" API. Each path uses what SDL does offer
// (SDL_FlushRenderer, which submits SDL's queued work and drops its state
// cache, plus textures created around the path's own GPU objects) and leaves
// SDL's view of the texture correct, see each implementation.
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include "jsui/jsui.h"

namespace sdl3_gpu {

class UiTarget {
 public:
  virtual ~UiTarget() = default;
  // What draws the UI (for the HUD) and the GPU it runs on (the "GPU: " line).
  virtual std::string describe() const = 0;
  virtual std::string device() const = 0;
  // (Re)creates the UI texture at width x height device px.
  virtual bool resize(int width, int height) = 0;
  // The premultiplied-alpha texture to composite (top row first).
  virtual SDL_Texture* texture() const = 0;
  // Paints the runtime into the texture if it needs a redraw (or `force`).
  // False when the GPU work failed: the caller falls back to the CPU path.
  virtual bool paint(JsuiRuntime* rt, bool force) = 0;
  // Before the frame that samples the texture is submitted (SDL_RenderPresent,
  // SDL_RenderReadPixels): this frame's paint must be visible to SDL.
  virtual void beforePresent() {}
};

// The preferred SDL render driver for the GPU path of this build ("metal",
// "vulkan", "opengl"), or nullptr when the library has no GPU backend.
const char* preferredDriver();

// SDL_CreateRenderer for `driver`. For "vulkan" the renderer gets a VkInstance
// created here at API 1.1 (Graphite's minimum; SDL's own is 1.0) and SDL
// creates the device on it; the instance is destroyed with destroyRenderer.
SDL_Renderer* createRenderer(SDL_Window* window, const char* driver);
void destroyRenderer(SDL_Renderer* renderer);

// The GPU path for the renderer's backend (SDL_GetRendererName), or nullptr
// (no GPU path for it in this build, or Skia rejected the device).
std::unique_ptr<UiTarget> createTarget(SDL_Renderer* renderer);

#if defined(__APPLE__) && defined(JSUI_GRAPHITE_METAL)
std::unique_ptr<UiTarget> createMetalTarget(SDL_Renderer* renderer);  // gpu_metal.mm
#endif

}  // namespace sdl3_gpu
