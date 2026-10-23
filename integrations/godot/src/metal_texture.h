#pragma once

namespace jsui_godot {
// id<MTLTexture> (+1) RGBA8Unorm (BGRA8Unorm with bgra), render target + shader read, private storage,
// MTLHazardTrackingModeTracked; NULL on failure. Apple platforms only.
void* newTrackedMetalTexture(void* device, int width, int height, bool bgra = false);
void releaseMetalTexture(void* texture);
}  // namespace jsui_godot
