#include "dev_overlay.h"

#include <algorithm>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "text/font_manager.h"

namespace jsui {

DevOverlay::DevOverlay(FontManager& fonts) : fonts_(fonts) {}
DevOverlay::~DevOverlay() = default;

void DevOverlay::resolveFonts() {
  resolved_ = true;
  sk_sp<SkFontMgr> mgr = fonts_.fontMgr();
  if (mgr) {
    for (const char* fam : {"monospace", "DejaVu Sans Mono", "Menlo", "Consolas", "Liberation Mono", "Courier New"}) {
      mono_ = mgr->matchFamilyStyle(fam, SkFontStyle());
      if (mono_) break;
    }
  }
  auto collection = fonts_.collection();
  if (collection) {
    auto found = collection->findTypefaces({SkString(fonts_.defaultFamily.c_str())}, SkFontStyle::Bold());
    if (!found.empty()) sans_ = found.front();
    if (!sans_) sans_ = collection->defaultFallback();
  }
  if (!sans_ && mgr) sans_ = mgr->legacyMakeTypeface(nullptr, SkFontStyle::Bold());
  if (!mono_) mono_ = sans_;
}

namespace {

size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

// Greedy wrap by measured width, splitting at code point boundaries.
void wrapLines(const std::string& text, const SkFont& font, float maxWidth, std::vector<std::string>* out) {
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    std::string expanded;
    for (char c : line) {
      if (c == '\t') expanded += "  ";
      else if (c != '\r') expanded += c;
    }
    if (expanded.empty()) out->push_back("");
    size_t i = 0;
    while (i < expanded.size()) {
      float w = 0;
      size_t j = i, lastSpace = std::string::npos;
      while (j < expanded.size()) {
        size_t n = std::min(utf8Len((unsigned char)expanded[j]), expanded.size() - j);
        float cw = font.measureText(expanded.data() + j, n, SkTextEncoding::kUTF8);
        if (w + cw > maxWidth && j > i) break;
        if (expanded[j] == ' ') lastSpace = j;
        w += cw;
        j += n;
      }
      if (j < expanded.size() && lastSpace != std::string::npos && lastSpace > i) j = lastSpace + 1;
      out->push_back(expanded.substr(i, j - i));
      i = j;
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
}

}  // namespace

void DevOverlay::paint(SkCanvas* canvas, const std::string& message, float width, float height, float pixelRatio) {
  if (!resolved_) resolveFonts();
  canvas->save();
  canvas->scale(pixelRatio, pixelRatio);

  SkPaint backdrop;
  backdrop.setColor(SkColorSetARGB(150, 30, 0, 0));
  canvas->drawRect(SkRect::MakeWH(width, height), backdrop);

  float margin = std::min(24.f, std::max(8.f, width * 0.03f));
  SkRect panel = SkRect::MakeXYWH(margin, margin, std::max(0.f, width - 2 * margin), std::max(0.f, height - 2 * margin));
  SkPaint fill;
  fill.setAntiAlias(true);
  fill.setColor(SkColorSetARGB(240, 60, 8, 12));
  canvas->drawRRect(SkRRect::MakeRectXY(panel, 8, 8), fill);
  SkPaint border;
  border.setAntiAlias(true);
  border.setStyle(SkPaint::kStroke_Style);
  border.setStrokeWidth(2);
  border.setColor(SkColorSetARGB(255, 230, 60, 60));
  canvas->drawRRect(SkRRect::MakeRectXY(panel.makeInset(1, 1), 7, 7), border);

  float pad = 16;
  float x = panel.left() + pad, y = panel.top() + pad;
  float textWidth = panel.width() - 2 * pad;
  canvas->clipRect(panel.makeInset(pad / 2, pad / 2));

  SkFont title(sans_, 18);
  title.setEdging(SkFont::Edging::kAntiAlias);
  SkFontMetrics tm;
  title.getMetrics(&tm);
  SkPaint white;
  white.setAntiAlias(true);
  white.setColor(SK_ColorWHITE);
  y += -tm.fAscent;
  canvas->drawString("JavaScript error", x, y, title, white);
  y += tm.fDescent + 10;

  SkFont mono(mono_, 13);
  mono.setEdging(SkFont::Edging::kAntiAlias);
  SkFontMetrics mm;
  mono.getMetrics(&mm);
  float lineH = std::max(14.f, mm.fDescent - mm.fAscent + mm.fLeading + 2);
  std::vector<std::string> lines;
  wrapLines(message, mono, std::max(20.f, textWidth), &lines);

  SkFont hint(sans_, 12);
  float bottom = panel.bottom() - pad - 18;
  SkPaint msgPaint, stackPaint;
  msgPaint.setAntiAlias(true);
  msgPaint.setColor(SkColorSetARGB(255, 255, 220, 220));
  stackPaint.setAntiAlias(true);
  stackPaint.setColor(SkColorSetARGB(255, 230, 170, 170));
  bool inStack = false;
  for (size_t i = 0; i < lines.size(); i++) {
    if (y + lineH > bottom) {
      canvas->drawString("...", x, y - mm.fAscent, mono, msgPaint);
      break;
    }
    const std::string& l = lines[i];
    if (l.rfind("    at ", 0) == 0) inStack = true;
    canvas->drawSimpleText(l.data(), l.size(), SkTextEncoding::kUTF8, x, y - mm.fAscent, mono,
                           inStack ? stackPaint : msgPaint);
    y += lineH;
  }

  SkPaint hintPaint;
  hintPaint.setAntiAlias(true);
  hintPaint.setColor(SkColorSetARGB(200, 255, 200, 200));
  canvas->drawString("Esc: dismiss    The bundle reloads automatically when it changes.", x, panel.bottom() - pad,
                     hint, hintPaint);
  canvas->restore();
}

}  // namespace jsui
