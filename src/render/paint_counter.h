// A canvas that forwards every call to `target` and counts draws, layers and
// clips into `stats` (painter benchmarks / perf tests, setPaintCounting()).
// Null where unsupported (WASM builds).
#pragma once

#include <memory>

#include "include/core/SkCanvas.h"
#include "render/painter.h"

namespace jsui {

std::unique_ptr<SkCanvas> makeCountingCanvas(SkCanvas* target, PaintStats* stats);

}  // namespace jsui
