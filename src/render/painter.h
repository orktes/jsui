// Paints a laid-out Document with Skia.
#pragma once

#include "include/core/SkMatrix.h"

class SkCanvas;

namespace jsui {

class Document;
class Node;

// `canvas` is in device pixels; the painter applies doc.pixelRatio().
void paintDocument(Document& doc, SkCanvas* canvas);

// The node's CSS transform (transform about transformOrigin) in its own
// border-box coordinates, excluding its layout position. Identity when the
// node has no transform. Hit testing uses the same matrix as painting.
SkMatrix localTransform(const Node& node);

// Painter statistics of the last paintDocument() call (for benchmarks).
struct PaintStats {
  int nodesPainted = 0;     // nodes whose content was drawn directly
  int picturesReused = 0;   // subtrees replayed from a cached picture
  int picturesRecorded = 0; // subtrees (re)recorded this frame
};
PaintStats lastPaintStats(const Document& doc);

}  // namespace jsui
