// Paints a laid-out Document with Skia.
#pragma once

class SkCanvas;

namespace jsui {

class Document;

// `canvas` is in device pixels; the painter applies doc.pixelRatio().
void paintDocument(Document& doc, SkCanvas* canvas);

}  // namespace jsui
