// Paints a laid-out Document with Skia.
#pragma once

#include <cstdint>

#include "include/core/SkMatrix.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurfaceProps.h"

class SkCanvas;

namespace jsui {

class Document;
class Node;

// `canvas` is in device pixels; the painter applies doc.pixelRatio().
// With `damage`, only subtrees touching that device region are painted (the
// caller clips to it); see updateDamage().
void paintDocument(Document& doc, SkCanvas* canvas, const SkRegion* damage = nullptr);

// Incremental repaint support. Compares what each node would paint now
// (paint version, size, device matrix, child list, ...) with what it painted
// at the previous call and returns the device region whose pixels changed.
// Must run once per frame before paintDocument(), also for full repaints,
// so the records stay current.
SkRegion updateDamage(Document& doc, int deviceWidth, int deviceHeight);

// Surface properties render targets should use: grayscale text with
// Chrome's (Linux) text contrast and gamma, so glyph coverage matches.
SkSurfaceProps surfaceProps();

// The node's CSS transform (transform about transformOrigin) in its own
// border-box coordinates, excluding its layout position. Identity when the
// node has no transform. Hit testing uses the same matrix as painting.
SkMatrix localTransform(const Node& node);

// Painter statistics of the last paintDocument() call (for benchmarks).
struct PaintStats {
  int nodesPainted = 0;       // nodes drawn
  int nodesCulled = 0;        // subtrees skipped (outside the damage)
  int64_t damagedPixels = 0;  // area of the damage bounds (0: full repaint)
};
PaintStats lastPaintStats(const Document& doc);

}  // namespace jsui
