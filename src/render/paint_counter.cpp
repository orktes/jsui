// Counting canvas for setPaintCounting() (see paint_counter.h).
//
// Compiled without RTTI: subclassing a Skia canvas needs the base class's
// typeinfo, which the prebuilt Skia (built with -fno-rtti) does not have.
#include "render/paint_counter.h"

#if !defined(__EMSCRIPTEN__)
#include "include/utils/SkNWayCanvas.h"

namespace jsui {
namespace {

// Forwards everything to one canvas and counts the calls (setPaintCounting).
// The base SkCanvas quick-rejects draws against its own clip first, so with
// the target's clip replicated the counts are the draws that reach it.
class CountingCanvas : public SkNWayCanvas {
 public:
  CountingCanvas(SkCanvas* target, PaintStats* stats)
      : SkNWayCanvas(target->imageInfo().width(), target->imageInfo().height()), s_(stats) {
    this->addCanvas(target);
  }

 protected:
  SaveLayerStrategy getSaveLayerStrategy(const SaveLayerRec& r) override {
    s_->saveLayers++;
    return SkNWayCanvas::getSaveLayerStrategy(r);
  }
  void onDrawDRRect(const SkRRect& a, const SkRRect& b, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawDRRect(a, b, p);
  }
  void onDrawGlyphRunList(const sktext::GlyphRunList& l, const SkPaint& p) override {
    s_->draws++;
    s_->textDraws++;
    SkNWayCanvas::onDrawGlyphRunList(l, p);
  }
  void onDrawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y, const SkPaint& p) override {
    s_->draws++;
    s_->textDraws++;
    SkNWayCanvas::onDrawTextBlob(blob, x, y, p);
  }
  void onDrawPaint(const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawPaint(p);
  }
  void onDrawPoints(PointMode m, size_t n, const SkPoint pts[], const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawPoints(m, n, pts, p);
  }
  void onDrawRect(const SkRect& r, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawRect(r, p);
  }
  void onDrawRegion(const SkRegion& r, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawRegion(r, p);
  }
  void onDrawOval(const SkRect& r, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawOval(r, p);
  }
  void onDrawArc(const SkRect& r, SkScalar a, SkScalar b, bool c, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawArc(r, a, b, c, p);
  }
  void onDrawRRect(const SkRRect& r, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawRRect(r, p);
  }
  void onDrawPath(const SkPath& path, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawPath(path, p);
  }
  void onDrawImage2(const SkImage* i, SkScalar x, SkScalar y, const SkSamplingOptions& o, const SkPaint* p) override {
    s_->draws++;
    s_->imageDraws++;
    SkNWayCanvas::onDrawImage2(i, x, y, o, p);
  }
  void onDrawImageRect2(const SkImage* i, const SkRect& a, const SkRect& b, const SkSamplingOptions& o,
                        const SkPaint* p, SrcRectConstraint c) override {
    s_->draws++;
    s_->imageDraws++;
    SkNWayCanvas::onDrawImageRect2(i, a, b, o, p, c);
  }
  void onDrawImageLattice2(const SkImage* i, const Lattice& l, const SkRect& r, SkFilterMode f,
                           const SkPaint* p) override {
    s_->draws++;
    s_->imageDraws++;
    SkNWayCanvas::onDrawImageLattice2(i, l, r, f, p);
  }
  void onDrawVerticesObject(const SkVertices* v, SkBlendMode m, const SkPaint& p) override {
    s_->draws++;
    SkNWayCanvas::onDrawVerticesObject(v, m, p);
  }
  void onDrawPicture(const SkPicture* pic, const SkMatrix* m, const SkPaint* p) override {
    // Counted per drawn op through the replay.
    SkNWayCanvas::onDrawPicture(pic, m, p);
  }
  void onDrawEdgeAAQuad(const SkRect& r, const SkPoint q[4], QuadAAFlags f, const SkColor4f& c,
                        SkBlendMode m) override {
    s_->draws++;
    SkNWayCanvas::onDrawEdgeAAQuad(r, q, f, c, m);
  }
  void onClipRect(const SkRect& r, SkClipOp op, ClipEdgeStyle e) override {
    s_->clips++;
    SkNWayCanvas::onClipRect(r, op, e);
  }
  void onClipRRect(const SkRRect& r, SkClipOp op, ClipEdgeStyle e) override {
    s_->clips++;
    SkNWayCanvas::onClipRRect(r, op, e);
  }
  void onClipPath(const SkPath& path, SkClipOp op, ClipEdgeStyle e) override {
    s_->clips++;
    SkNWayCanvas::onClipPath(path, op, e);
  }
  void onClipRegion(const SkRegion& r, SkClipOp op) override {
    s_->clips++;
    SkNWayCanvas::onClipRegion(r, op);
  }

 private:
  PaintStats* s_;
};

}  // namespace

std::unique_ptr<SkCanvas> makeCountingCanvas(SkCanvas* target, PaintStats* stats) {
  return std::make_unique<CountingCanvas>(target, stats);
}

}  // namespace jsui

#else

namespace jsui {
std::unique_ptr<SkCanvas> makeCountingCanvas(SkCanvas*, PaintStats*) { return nullptr; }
}  // namespace jsui

#endif
