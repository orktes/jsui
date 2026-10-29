// SkCanvas -> Impeller display list translator (see impeller_canvas.h).
//
// Skia's public API does not say what a shader or mask filter is made of,
// so gradients, blur mask filters and nine-patch lattices are read through
// Skia's private headers (src/...). They move between Skia milestones; this
// is a spike, built against the Skia JSUI pins.
#include "impeller_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkDrawable.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkM44.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPicture.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/utils/SkShadowUtils.h"
#include "src/core/SkDrawShadowInfo.h"
#include "src/core/SkGeometry.h"
#include "src/core/SkLatticeIter.h"
#include "src/core/SkMaskFilterBase.h"
#include "src/shaders/SkShaderBase.h"

namespace spike {

namespace {

ImpellerColor ic(const SkColor4f& c) { return ImpellerColor{c.fR, c.fG, c.fB, c.fA, kImpellerColorSpaceSRGB}; }
ImpellerRect ir(const SkRect& r) { return ImpellerRect{r.fLeft, r.fTop, r.width(), r.height()}; }
ImpellerPoint ip(const SkPoint& p) { return ImpellerPoint{p.fX, p.fY}; }

ImpellerMatrix im(const SkMatrix& m) {
  ImpellerMatrix out;
  SkM44(m).getColMajor(out.m);
  return out;
}

ImpellerRoundingRadii radii(const SkRRect& rr) {
  auto r = [&](SkRRect::Corner c) {
    SkVector v = rr.radii(c);
    return ImpellerPoint{v.fX, v.fY};
  };
  return ImpellerRoundingRadii{r(SkRRect::kUpperLeft_Corner), r(SkRRect::kLowerLeft_Corner),
                               r(SkRRect::kUpperRight_Corner), r(SkRRect::kLowerRight_Corner)};
}

ImpellerClipOperation clipOp(SkClipOp op) {
  return op == SkClipOp::kDifference ? kImpellerClipOperationDifference : kImpellerClipOperationIntersect;
}

ImpellerTileMode tile(SkTileMode m) {
  switch (m) {
    case SkTileMode::kRepeat: return kImpellerTileModeRepeat;
    case SkTileMode::kMirror: return kImpellerTileModeMirror;
    case SkTileMode::kDecal: return kImpellerTileModeDecal;
    default: return kImpellerTileModeClamp;
  }
}

ImpellerTextureSampling sampling(const SkSamplingOptions& s) {
  return s.filter == SkFilterMode::kNearest && s.mipmap == SkMipmapMode::kNone && !s.useCubic
             ? kImpellerTextureSamplingNearestNeighbor
             : kImpellerTextureSamplingLinear;
}

// Skia's and Impeller's blend mode enums are in the same order.
static_assert(int(SkBlendMode::kLuminosity) == int(kImpellerBlendModeLuminosity));

void appendUtf8(std::string& s, int32_t c) {
  if (c < 0x80) {
    s += char(c);
  } else if (c < 0x800) {
    s += char(0xC0 | (c >> 6));
    s += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    s += char(0xE0 | (c >> 12));
    s += char(0x80 | ((c >> 6) & 0x3F));
    s += char(0x80 | (c & 0x3F));
  } else {
    s += char(0xF0 | (c >> 18));
    s += char(0x80 | ((c >> 12) & 0x3F));
    s += char(0x80 | ((c >> 6) & 0x3F));
    s += char(0x80 | (c & 0x3F));
  }
}

void freeBytes(void* p) { delete static_cast<std::vector<uint8_t>*>(p); }
void freeData(void* p) { SkSafeUnref(static_cast<SkData*>(p)); }

}  // namespace

void Stats::add(const Stats& o) {
  draws += o.draws;
  rects += o.rects;
  rrects += o.rrects;
  paths += o.paths;
  images += o.images;
  textRuns += o.textRuns;
  layers += o.layers;
  clips += o.clips;
  textureUploads += o.textureUploads;
  paragraphBuilds += o.paragraphBuilds;
  for (auto& [k, v] : o.unsupported) unsupported[k] += v;
}

// ------------------------------------------------------------- resources

ImpellerResources::ImpellerResources(ImpellerContext ctx) : ctx_(ctx), typo_(ImpellerTypographyContextNew()) {}

ImpellerResources::~ImpellerResources() {
  for (auto& [k, t] : textures_) ImpellerTextureRelease(t.tex);
  for (auto& [k, p] : paths_) ImpellerPathRelease(p.first);
  for (auto& [k, p] : paragraphs_) ImpellerParagraphRelease(p.first);
  ImpellerTypographyContextRelease(typo_);
}

ImpellerTexture ImpellerResources::texture(const SkImage* image, const SkColor4f* tint, Stats* stats) {
  uint64_t key = image->uniqueID();
  if (tint) key = key * 1000003u ^ uint64_t(tint->toSkColor());
  auto it = textures_.find(key);
  if (it != textures_.end()) {
    it->second.used = frame_;
    return it->second.tex;
  }
  const int w = image->width(), h = image->height();
  auto* px = new std::vector<uint8_t>(size_t(w) * h * 4);
  SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  if (!image->readPixels(nullptr, info, px->data(), size_t(w) * 4, 0, 0)) {
    delete px;
    return nullptr;  // e.g. a GPU-backed image
  }
  if (tint) {
    // Alpha mask: Skia draws it in the paint colour. Bake that in (opaque;
    // the paint's alpha still applies).
    for (size_t i = 0; i < px->size(); i += 4) {
      float a = (*px)[i + 3] / 255.f;
      (*px)[i + 0] = uint8_t(std::lround(tint->fR * a * 255));
      (*px)[i + 1] = uint8_t(std::lround(tint->fG * a * 255));
      (*px)[i + 2] = uint8_t(std::lround(tint->fB * a * 255));
    }
  }
  ImpellerTextureDescriptor desc{kImpellerPixelFormatRGBA8888, {w, h}, 1};
  ImpellerMapping mapping{px->data(), px->size(), freeBytes};
  ImpellerTexture tex = ImpellerTextureCreateWithContentsNew(ctx_, &desc, &mapping, px);
  if (!tex) return nullptr;
  if (stats) stats->textureUploads++;
  textures_[key] = Tex{tex, frame_};
  return tex;
}

ImpellerPath ImpellerResources::path(const SkPath& path) {
  const uint32_t key = path.getGenerationID();
  auto it = paths_.find(key);
  if (it != paths_.end()) {
    it->second.second = frame_;
    return it->second.first;
  }
  ImpellerPathBuilder pb = ImpellerPathBuilderNew();
  SkPathIter iter = path.iter();
  while (auto rec = iter.next()) {
    const SkPoint* pts = rec->fPoints.data();
    switch (rec->fVerb) {
      case SkPathVerb::kMove: {
        ImpellerPoint p = ip(pts[0]);
        ImpellerPathBuilderMoveTo(pb, &p);
        break;
      }
      case SkPathVerb::kLine: {
        ImpellerPoint p = ip(pts[1]);
        ImpellerPathBuilderLineTo(pb, &p);
        break;
      }
      case SkPathVerb::kQuad: {
        ImpellerPoint c = ip(pts[1]), e = ip(pts[2]);
        ImpellerPathBuilderQuadraticCurveTo(pb, &c, &e);
        break;
      }
      case SkPathVerb::kConic: {
        SkAutoConicToQuads quads;
        const SkPoint* q = quads.computeQuads(pts, rec->conicWeight(), 0.25f);
        for (int i = 0; i < quads.countQuads(); i++) {
          ImpellerPoint c = ip(q[2 * i + 1]), e = ip(q[2 * i + 2]);
          ImpellerPathBuilderQuadraticCurveTo(pb, &c, &e);
        }
        break;
      }
      case SkPathVerb::kCubic: {
        ImpellerPoint c1 = ip(pts[1]), c2 = ip(pts[2]), e = ip(pts[3]);
        ImpellerPathBuilderCubicCurveTo(pb, &c1, &c2, &e);
        break;
      }
      case SkPathVerb::kClose: ImpellerPathBuilderClose(pb); break;
    }
  }
  const bool evenOdd =
      path.getFillType() == SkPathFillType::kEvenOdd || path.getFillType() == SkPathFillType::kInverseEvenOdd;
  ImpellerPath out = ImpellerPathBuilderTakePathNew(pb, evenOdd ? kImpellerFillTypeOdd : kImpellerFillTypeNonZero);
  ImpellerPathBuilderRelease(pb);
  paths_[key] = {out, frame_};
  return out;
}

const std::string& ImpellerResources::family(SkTypeface* tf) {
  const uint32_t id = tf->uniqueID();
  auto it = families_.find(id);
  if (it != families_.end()) return it->second;
  std::string alias = "jsui-" + std::to_string(id);
  int ttc = 0;
  std::unique_ptr<SkStreamAsset> stream = tf->openStream(&ttc);
  bool ok = false;
  if (stream) {
    sk_sp<SkData> data = SkData::MakeFromStream(stream.get(), stream->getLength());
    if (data) {
      ImpellerMapping m{data->bytes(), data->size(), freeData};
      SkData* raw = data.release();
      ok = ImpellerTypographyContextRegisterFont(typo_, &m, raw, alias.c_str());
    }
  }
  if (!ok) {
    // Not loadable as data: fall back to the system font of that family.
    SkString name;
    tf->getFamilyName(&name);
    alias = name.c_str();
  }
  return families_[id] = alias;
}

const std::unordered_map<uint16_t, int32_t>& ImpellerResources::glyphMap(SkTypeface* tf) {
  const uint32_t id = tf->uniqueID();
  auto it = glyphMaps_.find(id);
  if (it != glyphMaps_.end()) return it->second;
  auto& map = glyphMaps_[id];
  std::vector<SkUnichar> uni;
  for (SkUnichar c = 0x20; c < 0x3000; c++) uni.push_back(c);
  for (SkUnichar c = 0xFE00; c < 0x10000; c++) uni.push_back(c);
  std::vector<SkGlyphID> glyphs(uni.size());
  SkFont font(sk_ref_sp(tf));
  font.unicharsToGlyphs(uni, glyphs);
  for (size_t i = 0; i < uni.size(); i++) {
    if (glyphs[i] && !map.count(glyphs[i])) map[glyphs[i]] = uni[i];
  }
  return map;
}

ImpellerParagraph ImpellerResources::paragraph(const std::string& key, const std::function<ImpellerParagraph()>& make) {
  auto it = paragraphs_.find(key);
  if (it != paragraphs_.end()) {
    it->second.second = frame_;
    return it->second.first;
  }
  ImpellerParagraph p = make();
  if (p) paragraphs_[key] = {p, frame_};
  return p;
}

void ImpellerResources::endFrame(int maxAge) {
  frame_++;
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (frame_ - it->second.used > maxAge) {
      ImpellerTextureRelease(it->second.tex);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = paths_.begin(); it != paths_.end();) {
    if (frame_ - it->second.second > maxAge) {
      ImpellerPathRelease(it->second.first);
      it = paths_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = paragraphs_.begin(); it != paragraphs_.end();) {
    if (frame_ - it->second.second > maxAge) {
      ImpellerParagraphRelease(it->second.first);
      it = paragraphs_.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------- canvas

ImpellerCanvas::ImpellerCanvas(int width, int height, ImpellerDisplayListBuilder builder, ImpellerResources& res)
    : SkNoDrawCanvas(width, height), b_(builder), res_(res) {}

ImpellerCanvas::~ImpellerCanvas() = default;

void ImpellerCanvas::syncMatrix() {
  SkM44 m = this->getLocalToDevice();
  if (!matrixDirty_ && m == lastMatrix_) return;
  ImpellerMatrix out;
  m.getColMajor(out.m);
  ImpellerDisplayListBuilderSetTransform(b_, &out);
  lastMatrix_ = m;
  matrixDirty_ = false;
}

ImpellerPaint ImpellerCanvas::makePaint(const SkPaint& p) {
  ImpellerPaint out = ImpellerPaintNew();
  ImpellerColor c = ic(p.getColor4f());
  ImpellerPaintSetColor(out, &c);
  switch (p.getStyle()) {
    case SkPaint::kStroke_Style: ImpellerPaintSetDrawStyle(out, kImpellerDrawStyleStroke); break;
    case SkPaint::kStrokeAndFill_Style: ImpellerPaintSetDrawStyle(out, kImpellerDrawStyleStrokeAndFill); break;
    default: ImpellerPaintSetDrawStyle(out, kImpellerDrawStyleFill); break;
  }
  if (p.getStyle() != SkPaint::kFill_Style) {
    ImpellerPaintSetStrokeWidth(out, p.getStrokeWidth());
    ImpellerPaintSetStrokeMiter(out, p.getStrokeMiter());
    ImpellerPaintSetStrokeCap(out, p.getStrokeCap() == SkPaint::kRound_Cap    ? kImpellerStrokeCapRound
                                   : p.getStrokeCap() == SkPaint::kSquare_Cap ? kImpellerStrokeCapSquare
                                                                              : kImpellerStrokeCapButt);
    ImpellerPaintSetStrokeJoin(out, p.getStrokeJoin() == SkPaint::kRound_Join    ? kImpellerStrokeJoinRound
                                    : p.getStrokeJoin() == SkPaint::kBevel_Join ? kImpellerStrokeJoinBevel
                                                                                : kImpellerStrokeJoinMiter);
  }
  if (auto bm = p.asBlendMode()) {
    ImpellerPaintSetBlendMode(out, ImpellerBlendMode(int(*bm)));
  } else {
    unsupported("custom-blender");
  }

  if (SkShader* sh = p.getShader()) {
    SkMatrix local;
    SkTileMode tm[2];
    ImpellerColorSource src = nullptr;
    if (SkImage* img = sh->isAImage(&local, tm)) {
      if (ImpellerTexture tex = res_.texture(img, nullptr, &stats_)) {
        ImpellerMatrix m = im(local);
        src = ImpellerColorSourceCreateImageNew(tex, tile(tm[0]), tile(tm[1]), kImpellerTextureSamplingLinear, &m);
      }
    } else {
      SkShaderBase::GradientInfo info{};
      SkShaderBase::GradientType type = as_SB(sh)->asGradient(&info, &local);
      if (type != SkShaderBase::GradientType::kNone && info.fColorCount > 0) {
        std::vector<SkColor4f> colors(info.fColorCount);
        std::vector<float> stops(info.fColorCount);
        info.fColors = colors.data();
        info.fColorOffsets = stops.data();
        as_SB(sh)->asGradient(&info, &local);
        std::vector<ImpellerColor> icolors;
        for (const SkColor4f& col : colors) icolors.push_back(ic(col));
        ImpellerMatrix m = im(local);
        const uint32_t n = uint32_t(icolors.size());
        ImpellerPoint p0 = ip(info.fPoint[0]), p1 = ip(info.fPoint[1]);
        switch (type) {
          case SkShaderBase::GradientType::kLinear:
            // Impeller 1.4 (Flutter 3.47.9) draws nothing for some axis-aligned
            // linear gradients whose end points sit exactly on the rect's edges
            // (main-menu's 4-stop scrim, 0,360 -> 1280,360), probably its fast
            // per-vertex gradient path. A 1/64 px nudge takes the general path.
            if (std::fabs(p0.x - p1.x) < 1e-3f || std::fabs(p0.y - p1.y) < 1e-3f) {
              float& end = std::fabs(p0.x - p1.x) < 1e-3f ? p1.y : p1.x;
              end += 1.f / 64;
            }
            src = ImpellerColorSourceCreateLinearGradientNew(&p0, &p1, n, icolors.data(), stops.data(),
                                                             tile(info.fTileMode), &m);
            break;
          case SkShaderBase::GradientType::kRadial:
            src = ImpellerColorSourceCreateRadialGradientNew(&p0, info.fRadius[0], n, icolors.data(), stops.data(),
                                                             tile(info.fTileMode), &m);
            break;
          case SkShaderBase::GradientType::kConical:
            src = ImpellerColorSourceCreateConicalGradientNew(&p0, info.fRadius[0], &p1, info.fRadius[1], n,
                                                              icolors.data(), stops.data(), tile(info.fTileMode), &m);
            break;
          default: unsupported("sweep-gradient"); break;
        }
      } else {
        unsupported("shader");  // runtime effects (ShaderView), colour shaders, ...
      }
    }
    if (src) {
      ImpellerPaintSetColorSource(out, src);
      ImpellerColorSourceRelease(src);
    }
  }

  if (SkMaskFilter* mf = p.getMaskFilter()) {
    SkMaskFilterBase::BlurRec rec;
    if (as_MFB(mf)->asABlur(&rec)) {
      ImpellerBlurStyle st = rec.fStyle == kSolid_SkBlurStyle   ? kImpellerBlurStyleSolid
                             : rec.fStyle == kOuter_SkBlurStyle ? kImpellerBlurStyleOuter
                             : rec.fStyle == kInner_SkBlurStyle ? kImpellerBlurStyleInner
                                                                : kImpellerBlurStyleNormal;
      ImpellerMaskFilter f = ImpellerMaskFilterCreateBlurNew(st, rec.fSigma);
      ImpellerPaintSetMaskFilter(out, f);
      ImpellerMaskFilterRelease(f);
    } else {
      unsupported("mask-filter");
    }
  }

  if (SkColorFilter* cf = p.getColorFilter()) {
    SkColor color;
    SkBlendMode mode;
    float matrix[20];
    ImpellerColorFilter f = nullptr;
    if (cf->asAColorMode(&color, &mode)) {
      ImpellerColor c2 = ic(SkColor4f::FromColor(color));
      f = ImpellerColorFilterCreateBlendNew(&c2, ImpellerBlendMode(int(mode)));
    } else if (cf->asAColorMatrix(matrix)) {
      ImpellerColorMatrix m;
      std::memcpy(m.m, matrix, sizeof(matrix));
      f = ImpellerColorFilterCreateColorMatrixNew(&m);
    } else {
      unsupported("color-filter");
    }
    if (f) {
      ImpellerPaintSetColorFilter(out, f);
      ImpellerColorFilterRelease(f);
    }
  }
  if (p.getImageFilter()) unsupported("paint-image-filter");
  if (p.getPathEffect()) unsupported("path-effect");
  return out;
}

// save / restore

void ImpellerCanvas::willSave() { ImpellerDisplayListBuilderSave(b_); }

SkCanvas::SaveLayerStrategy ImpellerCanvas::getSaveLayerStrategy(const SaveLayerRec& rec) {
  syncMatrix();
  stats_.layers++;
  SkRect bounds = rec.fBounds ? *rec.fBounds : SkRect::MakeLTRB(-1e6f, -1e6f, 1e6f, 1e6f);
  ImpellerRect r = ir(bounds);
  ImpellerPaint lp = rec.fPaint ? makePaint(*rec.fPaint) : nullptr;
  ImpellerImageFilter backdrop = nullptr;
  if (rec.fBackdrop) {
    // Skia does not say what an image filter is; the painter's backdrop
    // filters are CSS backdrop blurs. Approximated with a 10 px blur.
    unsupported("backdrop-filter(approximated)");
    backdrop = ImpellerImageFilterCreateBlurNew(10, 10, kImpellerTileModeClamp);
  }
  ImpellerDisplayListBuilderSaveLayer(b_, &r, lp, backdrop);
  ImpellerPaintRelease(lp);
  ImpellerImageFilterRelease(backdrop);
  // The base class keeps its matrix / clip stack with a plain save.
  return kNoLayer_SaveLayerStrategy;
}

bool ImpellerCanvas::onDoSaveBehind(const SkRect*) {
  // SkCanvas::saveBehind saves without willSave() but restores with
  // willRestore(): keep Impeller's stack balanced.
  ImpellerDisplayListBuilderSave(b_);
  unsupported("save-behind");
  return false;
}

void ImpellerCanvas::willRestore() {
  ImpellerDisplayListBuilderRestore(b_);
  matrixDirty_ = true;
}

// clips (the base class tracks them too, for quickReject and clip bounds)

void ImpellerCanvas::onClipRect(const SkRect& r, SkClipOp op, ClipEdgeStyle edge) {
  syncMatrix();
  stats_.clips++;
  ImpellerRect rr = ir(r);
  ImpellerDisplayListBuilderClipRect(b_, &rr, clipOp(op));
  SkNoDrawCanvas::onClipRect(r, op, edge);
}

void ImpellerCanvas::onClipRRect(const SkRRect& rr, SkClipOp op, ClipEdgeStyle edge) {
  syncMatrix();
  stats_.clips++;
  ImpellerRect r = ir(rr.rect());
  ImpellerRoundingRadii rad = radii(rr);
  ImpellerDisplayListBuilderClipRoundedRect(b_, &r, &rad, clipOp(op));
  SkNoDrawCanvas::onClipRRect(rr, op, edge);
}

void ImpellerCanvas::onClipPath(const SkPath& path, SkClipOp op, ClipEdgeStyle edge) {
  syncMatrix();
  stats_.clips++;
  if (path.isInverseFillType()) unsupported("inverse-clip");
  ImpellerDisplayListBuilderClipPath(b_, res_.path(path), clipOp(op));
  SkNoDrawCanvas::onClipPath(path, op, edge);
}

void ImpellerCanvas::onClipShader(sk_sp<SkShader> sh, SkClipOp op) {
  unsupported("clip-shader");
  SkNoDrawCanvas::onClipShader(std::move(sh), op);
}

void ImpellerCanvas::onClipRegion(const SkRegion& region, SkClipOp op) {
  // Device space: clip to the region's bounds with an identity transform.
  // (the next draw sets the transform again).
  unsupported("clip-region(bounds)");
  ImpellerDisplayListBuilderResetTransform(b_);
  ImpellerRect r = ir(SkRect::Make(region.getBounds()));
  ImpellerDisplayListBuilderClipRect(b_, &r, clipOp(op));
  matrixDirty_ = true;
  SkNoDrawCanvas::onClipRegion(region, op);
}

void ImpellerCanvas::onResetClip() {
  unsupported("reset-clip");
  SkNoDrawCanvas::onResetClip();
}

// draws

void ImpellerCanvas::onDrawPaint(const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  ImpellerPaint p = makePaint(paint);
  ImpellerDisplayListBuilderDrawPaint(b_, p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawBehind(const SkPaint& paint) {
  unsupported("draw-behind(as paint)");
  onDrawPaint(paint);
}

void ImpellerCanvas::onDrawPoints(PointMode mode, size_t count, const SkPoint pts[], const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  ImpellerPaint p = makePaint(paint);
  if (mode == kPoints_PointMode) {
    const float r = std::max(paint.getStrokeWidth(), 1.f) / 2;
    for (size_t i = 0; i < count; i++) {
      ImpellerRect rr{pts[i].fX - r, pts[i].fY - r, 2 * r, 2 * r};
      if (paint.getStrokeCap() == SkPaint::kRound_Cap) ImpellerDisplayListBuilderDrawOval(b_, &rr, p);
      else ImpellerDisplayListBuilderDrawRect(b_, &rr, p);
    }
  } else {
    const size_t step = mode == kLines_PointMode ? 2 : 1;
    for (size_t i = 0; i + 1 < count; i += step) {
      ImpellerPoint a = ip(pts[i]), b = ip(pts[i + 1]);
      ImpellerDisplayListBuilderDrawLine(b_, &a, &b, p);
    }
  }
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawRect(const SkRect& rect, const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  stats_.rects++;
  ImpellerPaint p = makePaint(paint);
  ImpellerRect r = ir(rect);
  ImpellerDisplayListBuilderDrawRect(b_, &r, p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawRegion(const SkRegion& region, const SkPaint& paint) {
  for (SkRegion::Iterator it(region); !it.done(); it.next()) onDrawRect(SkRect::Make(it.rect()), paint);
}

void ImpellerCanvas::onDrawOval(const SkRect& rect, const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  stats_.rrects++;
  ImpellerPaint p = makePaint(paint);
  ImpellerRect r = ir(rect);
  ImpellerDisplayListBuilderDrawOval(b_, &r, p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawArc(const SkRect& rect, SkScalar start, SkScalar sweep, bool useCenter,
                               const SkPaint& paint) {
  SkPathBuilder pb;
  if (useCenter) pb.moveTo(rect.center());
  pb.arcTo(rect, start, sweep, !useCenter);
  if (useCenter) pb.close();
  onDrawPath(pb.detach(), paint);
}

void ImpellerCanvas::onDrawRRect(const SkRRect& rrect, const SkPaint& paint) {
  if (rrect.isRect()) return onDrawRect(rrect.rect(), paint);
  if (rrect.isOval()) return onDrawOval(rrect.rect(), paint);
  syncMatrix();
  stats_.draws++;
  stats_.rrects++;
  ImpellerPaint p = makePaint(paint);
  ImpellerRect r = ir(rrect.rect());
  ImpellerRoundingRadii rad = radii(rrect);
  ImpellerDisplayListBuilderDrawRoundedRect(b_, &r, &rad, p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawDRRect(const SkRRect& outer, const SkRRect& inner, const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  stats_.rrects++;
  ImpellerPaint p = makePaint(paint);
  ImpellerRect ro = ir(outer.rect()), ri = ir(inner.rect());
  ImpellerRoundingRadii ao = radii(outer), ai = radii(inner);
  ImpellerDisplayListBuilderDrawRoundedRectDifference(b_, &ro, &ao, &ri, &ai, p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawPath(const SkPath& path, const SkPaint& paint) {
  syncMatrix();
  stats_.draws++;
  stats_.paths++;
  if (path.isInverseFillType()) unsupported("inverse-fill");
  ImpellerPaint p = makePaint(paint);
  ImpellerDisplayListBuilderDrawPath(b_, res_.path(path), p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y, const SkPaint& paint) {
  syncMatrix();
  SkTextBlob::Iter it(*blob);
  SkTextBlob::Iter::ExperimentalRun run;
  while (it.experimentalNext(&run)) {
    if (run.count <= 0) continue;
    SkTypeface* tf = run.font.getTypeface();
    if (!tf) {
      unsupported("text-no-typeface");
      continue;
    }
    stats_.draws++;
    stats_.textRuns++;
    // Impeller's text API takes strings (it shapes them itself), JSUI's
    // painter draws shaped glyph runs: map the glyphs back to text.
    const auto& map = res_.glyphMap(tf);
    std::string text;
    int missing = 0;
    for (int i = 0; i < run.count; i++) {
      auto g = map.find(run.glyphs[i]);
      if (g == map.end()) missing++;
      else appendUtf8(text, g->second);
    }
    if (missing) stats_.unsupported["text-unmapped-glyphs"] += missing;
    if (text.empty()) continue;
    const std::string& family = res_.family(tf);
    const SkColor4f color = paint.getColor4f();
    const int weight = std::clamp(tf->fontStyle().weight() / 100 - 1, 0, 8);
    const bool italic = tf->fontStyle().slant() != SkFontStyle::kUpright_Slant;
    std::string key = family + "|" + std::to_string(run.font.getSize()) + "|" + std::to_string(color.toSkColor()) +
                      "|" + std::to_string(weight) + (italic ? "i|" : "|") + text;
    ImpellerParagraph para = res_.paragraph(key, [&]() -> ImpellerParagraph {
      stats_.paragraphBuilds++;
      ImpellerParagraphStyle style = ImpellerParagraphStyleNew();
      ImpellerPaint fg = makePaint(paint);
      ImpellerParagraphStyleSetForeground(style, fg);
      ImpellerParagraphStyleSetFontFamily(style, family.c_str());
      ImpellerParagraphStyleSetFontSize(style, run.font.getSize());
      ImpellerParagraphStyleSetFontWeight(style, ImpellerFontWeight(weight));
      ImpellerParagraphStyleSetFontStyle(style, italic ? kImpellerFontStyleItalic : kImpellerFontStyleNormal);
      ImpellerParagraphBuilder pb = ImpellerParagraphBuilderNew(res_.typography());
      ImpellerParagraphBuilderPushStyle(pb, style);
      ImpellerParagraphBuilderAddText(pb, reinterpret_cast<const uint8_t*>(text.data()), uint32_t(text.size()));
      ImpellerParagraph out = ImpellerParagraphBuilderBuildParagraphNew(pb, 1e6f);
      ImpellerParagraphBuilderRelease(pb);
      ImpellerPaintRelease(fg);
      ImpellerParagraphStyleRelease(style);
      return out;
    });
    if (!para) continue;
    const SkPoint origin = run.positions[0] + SkPoint::Make(x, y);
    ImpellerPoint at{origin.fX, origin.fY - ImpellerParagraphGetAlphabeticBaseline(para)};
    ImpellerDisplayListBuilderDrawParagraph(b_, para, &at);
  }
}

void ImpellerCanvas::drawTexture(const SkImage* image, const SkRect& src, const SkRect& dst,
                                 const SkSamplingOptions& samp, const SkPaint* paint) {
  syncMatrix();
  stats_.draws++;
  stats_.images++;
  SkColor4f tint;
  const bool mask = image->isAlphaOnly();
  if (mask) tint = paint ? paint->getColor4f() : SkColors::kBlack;
  ImpellerTexture tex = res_.texture(image, mask ? &tint : nullptr, &stats_);
  if (!tex) {
    unsupported("image-unreadable");
    return;
  }
  ImpellerPaint p = nullptr;
  if (paint) {
    SkPaint q(*paint);
    // Images ignore the paint's colour except its alpha (a mask's colour is
    // baked into its texture).
    q.setColor4f(SkColor4f{1, 1, 1, paint->getAlphaf()});
    p = makePaint(q);
  }
  ImpellerRect s = ir(src), d = ir(dst);
  ImpellerDisplayListBuilderDrawTextureRect(b_, tex, &s, &d, sampling(samp), p);
  ImpellerPaintRelease(p);
}

void ImpellerCanvas::onDrawImage2(const SkImage* image, SkScalar x, SkScalar y, const SkSamplingOptions& samp,
                                  const SkPaint* paint) {
  drawTexture(image, SkRect::Make(image->bounds()), SkRect::MakeXYWH(x, y, image->width(), image->height()), samp,
              paint);
}

void ImpellerCanvas::onDrawImageRect2(const SkImage* image, const SkRect& src, const SkRect& dst,
                                      const SkSamplingOptions& samp, const SkPaint* paint, SrcRectConstraint) {
  drawTexture(image, src, dst, samp, paint);
}

void ImpellerCanvas::onDrawImageLattice2(const SkImage* image, const Lattice& lattice, const SkRect& dst,
                                         SkFilterMode filter, const SkPaint* paint) {
  if (!SkLatticeIter::Valid(image->width(), image->height(), lattice)) {
    unsupported("lattice-invalid");
    return;
  }
  SkLatticeIter iter(lattice, dst);
  SkRect s, d;
  while (iter.next(&s, &d)) drawTexture(image, s, d, SkSamplingOptions(filter), paint);
}

void ImpellerCanvas::onDrawAtlas2(const SkImage*, const SkRSXform[], const SkRect[], const SkColor[], int,
                                  SkBlendMode, const SkSamplingOptions&, const SkRect*, const SkPaint*) {
  unsupported("atlas");
}

void ImpellerCanvas::onDrawVerticesObject(const SkVertices*, SkBlendMode, const SkPaint&) { unsupported("vertices"); }

void ImpellerCanvas::onDrawPatch(const SkPoint[12], const SkColor[4], const SkPoint[4], SkBlendMode, const SkPaint&) {
  unsupported("patch");
}

void ImpellerCanvas::onDrawShadowRec(const SkPath& path, const SkDrawShadowRec& rec) {
  syncMatrix();
  stats_.draws++;
  ImpellerColor c = ic(SkColor4f::FromColor(rec.fSpotColor));
  ImpellerDisplayListBuilderDrawShadow(b_, res_.path(path), &c, rec.fZPlaneParams.fZ,
                                       (rec.fFlags & kTransparentOccluder_ShadowFlag) != 0, 1.f);
}

void ImpellerCanvas::onDrawDrawable(SkDrawable* drawable, const SkMatrix* matrix) { drawable->draw(this, matrix); }

void ImpellerCanvas::onDrawPicture(const SkPicture* picture, const SkMatrix* matrix, const SkPaint* paint) {
  const int count = this->save();
  if (matrix) this->concat(*matrix);
  if (paint) {
    const SkRect cull = picture->cullRect();
    this->saveLayer(&cull, paint);
  }
  picture->playback(this);
  this->restoreToCount(count);
}

void ImpellerCanvas::onDrawEdgeAAQuad(const SkRect& rect, const SkPoint clip[4], QuadAAFlags, const SkColor4f& color,
                                      SkBlendMode mode) {
  SkPaint p(color);
  p.setBlendMode(mode);
  if (!clip) return onDrawRect(rect, p);
  SkPathBuilder pb;
  pb.addPolygon({clip, 4}, true);
  onDrawPath(pb.detach(), p);
}

void ImpellerCanvas::onDrawEdgeAAImageSet2(const ImageSetEntry set[], int count, const SkPoint[],
                                           const SkMatrix preViewMatrices[], const SkSamplingOptions& samp,
                                           const SkPaint* paint, SrcRectConstraint) {
  for (int i = 0; i < count; i++) {
    const ImageSetEntry& e = set[i];
    const int saved = this->save();
    if (e.fMatrixIndex >= 0) this->concat(preViewMatrices[e.fMatrixIndex]);
    SkPaint p = paint ? *paint : SkPaint();
    p.setAlphaf(p.getAlphaf() * e.fAlpha);
    drawTexture(e.fImage.get(), e.fSrcRect, e.fDstRect, samp, &p);
    this->restoreToCount(saved);
  }
}

}  // namespace spike
