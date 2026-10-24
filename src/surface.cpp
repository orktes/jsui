#include "surface.h"

#include <limits>

#include "dev_overlay.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"
#include "input/input.h"
#include "js/engine.h"
#include "render/painter.h"
#include "render/shader_view.h"
#include "runtime.h"

namespace jsui {

namespace {

}  // namespace

Surface::Surface(Runtime& r, uint32_t* nextNodeId, Perf* perf) : rt(r), doc(nextNodeId, perf) {}

bool Surface::needsRedraw() const {
  return redraw_ || doc.paintDirty() || (doc.exactRepaintPending() && !rt.animatingFrames(*this)) ||
         rt.shaders->warmPending();
}

Surface::~Surface() {
  doc.clear();
  doc.onNodeDestroyed = nullptr;
  doc.onLayout = nullptr;
}

void Surface::setViewport(float width, float height, float pixelRatio) {
  float scale = pixelRatio > 0 ? pixelRatio : 1.f;
  bool autoW = !(width > 0), autoH = !(height > 0);
  bool changed = doc.autoWidth() != autoW || doc.autoHeight() != autoH || (!autoW && doc.width() != width) ||
                 (!autoH && doc.height() != height) || doc.pixelRatio() != scale;
  doc.setViewport(width, height, scale);
  if (!changed) return;
  // Hosts may resize many surfaces per frame: those resizes reach JS on the
  // next tick, together (Runtime::flushSurfaces).
  if (this != &rt.main()) resizePending = true;
  else if (rt.js) dispatchResize();
}

void Surface::dispatchResize() {
  resizePending = false;
  // Dimensions / useWindowDimensions in @orktes/jsui-react listen for this.
  Value::Object ev{{"type", Value("resize")},
                   {"width", Value(double(doc.autoWidth() ? 0 : doc.width()))},
                   {"height", Value(double(doc.autoHeight() ? 0 : doc.height()))},
                   {"scale", Value(double(doc.pixelRatio()))},
                   {"rootTag", Value(double(tag()))}};
  rt.js->dispatchEvent(Value(std::move(ev)));
}

bool Surface::frozen() const {
  return rt.config.dev_mode && lastGoodFrame_ && (!rt.lastError.empty() || doc.root()->children().empty());
}

bool Surface::overlay() const { return rt.config.dev_mode && !rt.lastError.empty(); }

void Surface::snapshotFrame() {
  if (!rt.lastError.empty() || doc.root()->children().empty()) return;
  doc.layout();
  float pr = doc.pixelRatio();
  SkPictureRecorder rec;
  paintDocument(doc, rec.beginRecording(SkRect::MakeWH(doc.width() * pr, doc.height() * pr)));
  lastGoodFrame_ = rec.finishRecordingAsPicture();
}

void Surface::render(void* pixels, int width, int height, size_t rowBytes) {
  renderImpl(pixels, width, height, rowBytes, false);
}

SkIRect Surface::renderIncremental(void* pixels, int width, int height, size_t rowBytes) {
  return renderImpl(pixels, width, height, rowBytes, true);
}

SkIRect Surface::renderImpl(void* pixels, int width, int height, size_t rowBytes, bool incremental) {
  SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  SkSurfaceProps props = surfaceProps();
  auto surface = SkSurfaces::WrapPixels(info, pixels, rowBytes, &props);
  if (!surface) return SkIRect::MakeEmpty();
  return paintTracked(surface->getCanvas(), pixels, width, height, rowBytes, incremental);
}

SkIRect Surface::beginCanvasTracked(const void* key, int width, int height, bool* keyValid, bool growCurves) {
  doc.layout();
  bool sameTarget = key && key == lastTarget_ && width == lastTargetW_ && height == lastTargetH_ &&
                    lastTargetRowBytes_ == 0;
  trackedFrozen_ = trackedOverlay_ = false;
  trackedKey_ = key;
  trackedDamage_.setEmpty();
  trackedRects_.clear();
  // Nothing marked dirty and the retained surface holds our last frame:
  // nothing to repaint, without walking the tree.
  if (sameTarget && !doc.paintDirty() && !doc.exactRepaintPending() && rt.lastError.empty() &&
      !rt.shaders->warmPending()) {
    *keyValid = true;
    return SkIRect::MakeEmpty();
  }
  // Always refresh the paint records so the next frame compares against
  // what was painted.
  trackedDamage_ = updateDamage(doc, width, height, nullptr, growCurves);
  const SkRegion& damage = trackedDamage_;
  trackedFrozen_ = frozen();
  trackedOverlay_ = overlay();
  *keyValid = sameTarget && !trackedFrozen_ && !trackedOverlay_;
  return damage.getBounds();
}

SkIRect Surface::finishCanvasTracked(const CanvasChoice& c, int width, int height, bool keyValid,
                                     const SkIRect& damage) {
  doc.clearPaintDirty();
  redraw_ = false;
  lastTargetW_ = width;
  lastTargetH_ = height;
  lastTargetRowBytes_ = 0;
  // Frames with the error overlay or a frozen picture are not tracked by the
  // records: the next frame repaints fully.
  bool tracked = !trackedFrozen_ && !trackedOverlay_;
  lastTarget_ = c.canvas && c.key && tracked ? c.key : nullptr;
  if (!c.canvas) return SkIRect::MakeEmpty();
  SkCanvas* canvas = c.canvas;
  // Preloaded shaders (ShaderEffects::preload): drawn off-screen, so this
  // frame builds their pipelines. A frame with nothing else to paint still
  // reports a pixel, so the backend flushes it.
  bool warmed = rt.shaders->warm(canvas);
  if (c.key && keyValid && c.key == trackedKey_) {
    if (damage.isEmpty()) return warmed ? SkIRect::MakeWH(1, 1) : SkIRect::MakeEmpty();
    if (c.splitDamage) {
      trackedRects_ = coverRects(trackedDamage_);
      paintDocumentRects(doc, canvas, trackedRects_);
      return damage;
    }
    trackedRects_.assign(1, damage);
    // One rect: GPU backends clip to it with a scissor, where a region
    // (several rects) is a complex clip (a mask, or MSAA) on Graphite.
    SkRegion rect(damage);
    canvas->save();
    canvas->clipIRect(damage);
    canvas->clear(SK_ColorTRANSPARENT);
    paintDocument(doc, canvas, &rect);
    canvas->restore();
    return damage;
  }
  if (c.clear || c.key) canvas->clear(SK_ColorTRANSPARENT);
  paintFrame(canvas, trackedFrozen_, trackedOverlay_);
  return SkIRect::MakeWH(width, height);
}

SkIRect Surface::paintTracked(SkCanvas* canvas, const void* pixels, int width, int height, size_t rowBytes,
                              bool incremental) {
  rt.shaders->warm(canvas);  // a raster canvas: drops the queue (nothing to warm)
  doc.layout();
  // Nothing marked dirty and the buffer still holds our last frame: done,
  // without walking the tree.
  if (incremental && !doc.paintDirty() && !doc.exactRepaintPending() && pixels == lastTarget_ &&
      width == lastTargetW_ && height == lastTargetH_ && rowBytes == lastTargetRowBytes_ && rt.lastError.empty()) {
    redraw_ = false;
    return SkIRect::MakeEmpty();
  }
  bool isFrozen = frozen();
  bool isOverlay = overlay();
  bool partial = incremental && !isFrozen && !isOverlay && pixels == lastTarget_ && width == lastTargetW_ &&
                 height == lastTargetH_ && rowBytes == lastTargetRowBytes_;
  // Always refresh the paint records so the next incremental frame compares
  // against what is in the buffer. A partial repaint of a pixel buffer can
  // move scrolled content instead of repainting it.
  ScrollBlit blit;
  SkRegion damage = updateDamage(doc, width, height, partial && rowBytes ? &blit : nullptr);
  // Frames with the error overlay or a frozen picture are not tracked by the
  // records: the next frame repaints fully.
  bool tracked = !isFrozen && !isOverlay;
  lastTarget_ = tracked ? pixels : nullptr;
  lastTargetW_ = width;
  lastTargetH_ = height;
  lastTargetRowBytes_ = rowBytes;
  doc.clearPaintDirty();
  redraw_ = false;
  if (partial) {
    if (damage.isEmpty() && blit.rect.isEmpty()) return SkIRect::MakeEmpty();
    if (!blit.rect.isEmpty()) applyScrollBlit(const_cast<void*>(pixels), rowBytes, width, height, blit);
    canvas->save();
    canvas->clipRegion(damage);
    canvas->clear(SK_ColorTRANSPARENT);
    paintDocument(doc, canvas, &damage);
    canvas->restore();
    SkIRect changed = damage.getBounds();
    if (!blit.rect.isEmpty()) {
      noteScrollBlit(doc, blit);
      changed.join(blit.rect);
    }
    return changed;
  }
  canvas->clear(SK_ColorTRANSPARENT);
  paintFrame(canvas, isFrozen, isOverlay);
  return SkIRect::MakeWH(width, height);
}

void Surface::paintFrame(SkCanvas* canvas, bool isFrozen, bool isOverlay) {
  if (isFrozen) {
    canvas->drawPicture(lastGoodFrame_);
  } else {
    paintDocument(doc, canvas);
    if (lastGoodFrame_ && rt.lastError.empty()) lastGoodFrame_.reset();
  }
  if (isOverlay) {
    if (!overlay_) overlay_ = std::make_unique<DevOverlay>(rt.fonts);
    overlay_->paint(canvas, rt.lastError, doc.width(), doc.height(), doc.pixelRatio());
  }
}

void Surface::renderCanvas(SkCanvas* canvas, int width, int height, bool clear) {
  doc.layout();
  // Keep the paint records current (they are what the CPU incremental path
  // compares against), but nothing here is tracked: a later incremental CPU
  // render repaints fully.
  updateDamage(doc, width, height);
  bool isFrozen = frozen();
  bool isOverlay = overlay();
  lastTarget_ = nullptr;
  doc.clearPaintDirty();
  redraw_ = false;
  if (clear) canvas->clear(SK_ColorTRANSPARENT);
  rt.shaders->warm(canvas);
  paintFrame(canvas, isFrozen, isOverlay);
}

}  // namespace jsui
