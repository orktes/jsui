// Paints a laid-out Document with Skia.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
//
// `blit` (callers that repaint into a CPU buffer holding the last frame):
// when a ScrollView's content only scrolled, by whole device pixels, over a
// uniform backdrop, the damage leaves out its viewport except what the
// scroll exposes, and `blit` says how to move the viewport's pixels first:
// what was at p goes to p + (dx, dy), within `rect`. The caller must do that
// before painting the damage (or repaint fully). Empty rect: no blit.
//
// `growCurves` false (Graphite): damage that crosses an anti-aliased curved
// outline grows to the whole node only when the backend's pixels for that
// outline may depend on the clip (dashed edges, outlines over 1024 px; see
// CurvedNode). Graphite draws solid rrects analytically and, patched, applies
// a whole-pixel rect clip as a scissor, so the rest come out the same.
struct ScrollBlit {
  SkIRect rect = SkIRect::MakeEmpty();
  int dx = 0, dy = 0;
};
SkRegion updateDamage(Document& doc, int deviceWidth, int deviceHeight, ScrollBlit* blit = nullptr,
                      bool growCurves = true);
// Moves the pixels of `b.rect` by (dx, dy) in an RGBA8888 buffer.
void applyScrollBlit(void* pixels, size_t rowBytes, int width, int height, const ScrollBlit& b);
// Records a blit in the last paint's stats (PaintStats::blitPixels).
void noteScrollBlit(Document& doc, const ScrollBlit& b);

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
  int nodesUpdated = 0;       // nodes the last updateDamage() walked (not in skipped, unchanged subtrees)
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
  // Always counted: draws blitted from cached raster sprites (gradients,
  // scaled images), and of those the ones rendered this paint (misses).
  int spriteDraws = 0;
  int spriteRenders = 0;
  int64_t blitPixels = 0;  // pixels moved by a scroll blit instead of repainted
  // Always counted: opacity layers composited from a LayerCache, and of
  // those the ones rendered this paint (misses).
  int layerDraws = 0;
  int layerRenders = 0;
};
PaintStats lastPaintStats(const Document& doc);

// Composited opacity layers (GPU backends). A node painted through a layer
// (opacity < 1 over children or overlapping draws) normally renders its
// subtree into an offscreen texture every time it is painted: on Graphite a
// render pass per layer and per damage rect, every frame of a fade. With a
// LayerCache active (LayerCacheScope, set by the GPU context around a
// frame), the layer's texture is kept and drawn again, at the frame's
// opacity, while the subtree paints the same relative to the node (no
// change below it other than the node's own opacity / transform, see
// Node::contentVersion) and the node's device matrix differs from the cached
// one only by a whole-pixel translation. Those draws are the pixels the
// layer would have produced. A subtree that keeps changing (stamp changed in
// the last frame) is painted through a plain layer, clipped to the damage.
// Textures belong to the GPU context the canvas draws with: the owner frees
// the cache (clear()) before its context.
class LayerCache {
 public:
  explicit LayerCache(size_t budgetBytes = size_t(96) << 20);
  ~LayerCache();
  LayerCache(const LayerCache&) = delete;
  LayerCache& operator=(const LayerCache&) = delete;
  void clear();
  // What it caches: opacity layers and / or raster-cached subtrees (see
  // updateRasterCache in painter.cpp). Both by default.
  void setModes(bool opacityLayers, bool raster);
  // After a frame: frees layers unused for a while, and the least recently
  // used ones over the budget.
  void endFrame();
  size_t bytes() const;
  int size() const;
  struct Impl;
  std::unique_ptr<Impl> impl;
};
// Makes `cache` the one paintDocument() uses on this thread while in scope
// (null: none).
class LayerCacheScope {
 public:
  explicit LayerCacheScope(LayerCache* cache);
  ~LayerCacheScope();
  LayerCacheScope(const LayerCacheScope&) = delete;
  LayerCacheScope& operator=(const LayerCacheScope&) = delete;

 private:
  LayerCache* prev_;
};

// Benchmarks / perf tests: count the canvas calls of each paintDocument()
// (see PaintStats). Off by default; it wraps the canvas, which costs a
// little per call.
void setPaintCounting(Document& doc, bool on);

// Tests: updateDamage skips subtrees in which nothing changed (on by
// default); off, it walks every node, which must give the same damage.
void setRecordSkipping(bool on);

}  // namespace jsui
