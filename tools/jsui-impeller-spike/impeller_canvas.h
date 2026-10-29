// SkCanvas -> Impeller display list translator for the Impeller spike.
//
// JSUI's painter draws into an SkCanvas. To put the same frames through
// Flutter's Impeller (its standalone SDK, impeller.h) this canvas takes the
// calls a recorded JSUI frame replays into it (Surface::replayFrame) and
// issues the matching ImpellerDisplayListBuilder calls. Resources that live
// across frames (textures for images, paths, paragraphs for text runs) are
// cached in ImpellerResources, as a real Impeller backend would keep them.
//
// It covers what the painter emits for GPU frames (rects, rounded rects,
// paths, clips, images and nine-patch shadow masks, text blobs, gradients,
// blur mask filters, colour filters, save layers with opacity / blend). What
// it cannot translate is counted in Stats::unsupported (by kind) so a run
// says how much of the frame Impeller did not draw.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "impeller.h"
#include "include/core/SkCanvas.h"
#include "include/utils/SkNoDrawCanvas.h"

class SkTypeface;

namespace spike {

struct Stats {
  int draws = 0, rects = 0, rrects = 0, paths = 0, images = 0, textRuns = 0, layers = 0, clips = 0;
  int textureUploads = 0, paragraphBuilds = 0;
  std::map<std::string, int> unsupported;  // kind -> count
  void add(const Stats& o);
};

class ImpellerResources {
 public:
  explicit ImpellerResources(ImpellerContext ctx);
  ~ImpellerResources();
  ImpellerResources(const ImpellerResources&) = delete;
  ImpellerResources& operator=(const ImpellerResources&) = delete;

  ImpellerContext context() const { return ctx_; }
  ImpellerTypographyContext typography() const { return typo_; }

  // Texture for an image (raster pixels, uploaded once per image id). Alpha
  // masks are drawn tinted with the paint colour in Skia; Impeller samples a
  // texture as colour, so `tint` bakes that colour in (one texture per tint).
  ImpellerTexture texture(const SkImage* image, const SkColor4f* tint, Stats* stats);
  ImpellerPath path(const SkPath& path);
  // A family alias registered with the typography context for `tf`.
  const std::string& family(SkTypeface* tf);
  // Glyph id -> code point for `tf` (built once per typeface by mapping the
  // BMP through the font's cmap).
  const std::unordered_map<uint16_t, int32_t>& glyphMap(SkTypeface* tf);
  ImpellerParagraph paragraph(const std::string& key, const std::function<ImpellerParagraph()>& make);

  // Drops what was not used for `maxAge` frames.
  void endFrame(int maxAge = 120);

 private:
  struct Tex {
    ImpellerTexture tex = nullptr;
    int used = 0;
  };
  ImpellerContext ctx_;
  ImpellerTypographyContext typo_;
  int frame_ = 0;
  std::unordered_map<uint64_t, Tex> textures_;
  std::unordered_map<uint32_t, std::pair<ImpellerPath, int>> paths_;
  std::unordered_map<uint32_t, std::string> families_;
  std::unordered_map<uint32_t, std::unordered_map<uint16_t, int32_t>> glyphMaps_;
  std::unordered_map<std::string, std::pair<ImpellerParagraph, int>> paragraphs_;
};

class ImpellerCanvas final : public SkNoDrawCanvas {
 public:
  ImpellerCanvas(int width, int height, ImpellerDisplayListBuilder builder, ImpellerResources& res);
  ~ImpellerCanvas() override;
  const Stats& stats() const { return stats_; }

 protected:
  void willSave() override;
  SaveLayerStrategy getSaveLayerStrategy(const SaveLayerRec& rec) override;
  bool onDoSaveBehind(const SkRect*) override;
  void willRestore() override;

  void onClipRect(const SkRect& r, SkClipOp op, ClipEdgeStyle edge) override;
  void onClipRRect(const SkRRect& rr, SkClipOp op, ClipEdgeStyle edge) override;
  void onClipPath(const SkPath& path, SkClipOp op, ClipEdgeStyle edge) override;
  void onClipShader(sk_sp<SkShader>, SkClipOp) override;
  void onClipRegion(const SkRegion& region, SkClipOp op) override;
  void onResetClip() override;

  void onDrawPaint(const SkPaint& paint) override;
  void onDrawBehind(const SkPaint& paint) override;
  void onDrawPoints(PointMode mode, size_t count, const SkPoint pts[], const SkPaint& paint) override;
  void onDrawRect(const SkRect& rect, const SkPaint& paint) override;
  void onDrawRegion(const SkRegion& region, const SkPaint& paint) override;
  void onDrawOval(const SkRect& rect, const SkPaint& paint) override;
  void onDrawArc(const SkRect& rect, SkScalar start, SkScalar sweep, bool useCenter, const SkPaint& paint) override;
  void onDrawRRect(const SkRRect& rrect, const SkPaint& paint) override;
  void onDrawDRRect(const SkRRect& outer, const SkRRect& inner, const SkPaint& paint) override;
  void onDrawPath(const SkPath& path, const SkPaint& paint) override;
  void onDrawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y, const SkPaint& paint) override;
  void onDrawImage2(const SkImage* image, SkScalar x, SkScalar y, const SkSamplingOptions& sampling,
                    const SkPaint* paint) override;
  void onDrawImageRect2(const SkImage* image, const SkRect& src, const SkRect& dst, const SkSamplingOptions& sampling,
                        const SkPaint* paint, SrcRectConstraint constraint) override;
  void onDrawImageLattice2(const SkImage* image, const Lattice& lattice, const SkRect& dst, SkFilterMode filter,
                           const SkPaint* paint) override;
  void onDrawAtlas2(const SkImage*, const SkRSXform[], const SkRect[], const SkColor[], int, SkBlendMode,
                    const SkSamplingOptions&, const SkRect*, const SkPaint*) override;
  void onDrawVerticesObject(const SkVertices*, SkBlendMode, const SkPaint&) override;
  void onDrawPatch(const SkPoint[12], const SkColor[4], const SkPoint[4], SkBlendMode, const SkPaint&) override;
  void onDrawShadowRec(const SkPath& path, const SkDrawShadowRec& rec) override;
  void onDrawDrawable(SkDrawable* drawable, const SkMatrix* matrix) override;
  void onDrawPicture(const SkPicture* picture, const SkMatrix* matrix, const SkPaint* paint) override;
  void onDrawEdgeAAQuad(const SkRect& rect, const SkPoint clip[4], QuadAAFlags aa, const SkColor4f& color,
                        SkBlendMode mode) override;
  void onDrawEdgeAAImageSet2(const ImageSetEntry set[], int count, const SkPoint dstClips[],
                             const SkMatrix preViewMatrices[], const SkSamplingOptions& sampling,
                             const SkPaint* paint, SrcRectConstraint constraint) override;

 private:
  void syncMatrix();
  // A paint for `p` (nullptr: the draw is skipped, counted as unsupported).
  ImpellerPaint makePaint(const SkPaint& p);
  void unsupported(const char* kind) { stats_.unsupported[kind]++; }
  void drawTexture(const SkImage* image, const SkRect& src, const SkRect& dst, const SkSamplingOptions& sampling,
                   const SkPaint* paint);

  ImpellerDisplayListBuilder b_;
  ImpellerResources& res_;
  Stats stats_;
  bool matrixDirty_ = true;
  SkM44 lastMatrix_;
};

}  // namespace spike
