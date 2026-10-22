// Paints a laid-out Document with Skia.
#pragma once

#include <cstdint>
#include <vector>

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

// Repaints the damage given as rects: each rect is cleared and painted in
// its own pass, clipped to it (a scissor rect on the GPU). Rects may overlap.
// The stats of the passes are summed.
void paintDocumentRects(Document& doc, SkCanvas* canvas, const std::vector<SkIRect>& rects);

// Painter statistics of the last paintDocument() call (for benchmarks).
struct PaintStats {
  int nodesPainted = 0;       // nodes drawn
  int nodesCulled = 0;        // subtrees skipped (outside the damage)
  int64_t damagedPixels = 0;  // area of the damage bounds (0: full repaint)
  int64_t repaintedPixels = 0;  // area of the damage region (full repaint: the canvas)
  int damageRects = 0;        // rects in the damage region (0: full repaint)
  // Only counted while setPaintCounting() is on (0 otherwise): canvas calls
  // the painter issued that survived Skia's quick reject against the clip.
  int draws = 0;              // draw calls (rects, rrects, paths, images, text blobs, ...)
  int textDraws = 0;          // of which text blobs
  int imageDraws = 0;         // of which images
  int saveLayers = 0;         // offscreen layers
  int clips = 0;              // clip operations
};
PaintStats lastPaintStats(const Document& doc);

// Benchmarks / perf tests: count the canvas calls of each paintDocument()
// (see PaintStats). Off by default; it wraps the canvas, which costs a
// little per call.
void setPaintCounting(Document& doc, bool on);

}  // namespace jsui
