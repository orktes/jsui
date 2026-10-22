#include <cstdio>
#include "text/text_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/document.h"
#include "core/node.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkTextBlob.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "text/font_manager.h"
#include "text/text_edit.h"

namespace jsui::text {

namespace tl = skia::textlayout;

namespace {

constexpr float kDefaultFontSize = 14;  // React Native default
constexpr float kInf = std::numeric_limits<float>::infinity();

// Fully resolved (inherited) text style of one span.
struct Resolved {
  Color color{0, 0, 0, 1};
  std::string family;  // CSS list as given; empty = default family
  float fontSize = kDefaultFontSize;
  int weight = 400;
  bool italic = false;
  std::optional<float> lineHeight;  // px or (relative ? multiple of fontSize)
  bool lineHeightRelative = false;
  float letterSpacing = 0;
  TextTransform transform = TextTransform::None;
  uint8_t decoration = DecorNone;
  std::optional<Color> decorationColor;
  uint8_t decorationStyle = 0;
  std::vector<Shadow> shadows;
  std::vector<std::string> fontVariant;
  TextAlign align = TextAlign::Auto;
  int8_t direction = 0;
};

void merge(Resolved* r, const TextStyle& t) {
  if (t.color) r->color = *t.color;
  if (t.fontFamily) r->family = *t.fontFamily;
  if (t.fontSize) r->fontSize = std::max(0.f, *t.fontSize);
  if (t.fontWeight) r->weight = *t.fontWeight;
  if (t.italic) r->italic = *t.italic;
  if (t.lineHeight) {
    r->lineHeight = *t.lineHeight;
    r->lineHeightRelative = t.lineHeightRelative;
  }
  if (t.letterSpacing) r->letterSpacing = *t.letterSpacing;
  if (t.textTransform) r->transform = *t.textTransform;
  if (t.decoration) r->decoration = *t.decoration;
  if (t.decorationColor) r->decorationColor = *t.decorationColor;
  if (t.decorationStyle) r->decorationStyle = *t.decorationStyle;
  if (t.hasTextShadow) r->shadows = t.textShadows;
  if (t.fontVariant) r->fontVariant = *t.fontVariant;
  if (t.textAlign) r->align = *t.textAlign;
  if (t.writingDirection) r->direction = *t.writingDirection;
}

float linePx(const Resolved& r) {
  if (!r.lineHeight) return 0;
  return r.lineHeightRelative ? *r.lineHeight * r.fontSize : *r.lineHeight;
}

SkColor toSk(const Color& c) {
  auto b = [](float v) { return U8CPU(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
  return SkColorSetARGB(b(c.a), b(c.r), b(c.g), b(c.b));
}

// CSS font-family list -> skparagraph families, always ending in the default
// family. Generic / platform UI names map to the default (react-native-web's
// "System" stack resolves to the bundled font here).
std::vector<SkString> families(const std::string& list, const std::string& def) {
  std::vector<SkString> out;
  size_t i = 0;
  while (i <= list.size()) {
    size_t comma = list.find(',', i);
    if (comma == std::string::npos) comma = list.size();
    std::string f = list.substr(i, comma - i);
    i = comma + 1;
    size_t a = f.find_first_not_of(" \t\"'"), b = f.find_last_not_of(" \t\"'");
    if (a == std::string::npos) continue;
    f = f.substr(a, b - a + 1);
    std::string lf = f;
    for (char& ch : lf) ch = char(std::tolower((unsigned char)ch));
    if (lf == "system" || lf == "system-ui" || lf == "sans-serif" || lf == "-apple-system" ||
        lf == "blinkmacsystemfont" || lf == "ui-sans-serif" || lf == "inherit")
      f = def;
    if (std::find(out.begin(), out.end(), SkString(f.c_str())) == out.end()) out.emplace_back(f.c_str());
  }
  if (std::find(out.begin(), out.end(), SkString(def.c_str())) == out.end()) out.emplace_back(def.c_str());
  return out;
}

// Vertical metrics the way Chrome computes them: ascent and descent of the
// primary font are rounded to whole pixels, "normal" line height is their sum
// (+ line gap), and with an explicit line-height the half-leading above the
// glyphs is floored. skparagraph uses unrounded metrics and splits leading
// evenly, so we give every run the Chrome line height and shift the
// baseline by the difference.
struct LineMetrics {
  float height = 0;         // line box height (px)
  float baselineShift = 0;  // added to skparagraph's half-leading baseline
  float paintDy = 0;        // root only: offset that puts skparagraph's (rounded) baseline at Chrome's
};

LineMetrics chromeLineMetrics(FontManager* fm, const tl::TextStyle& ts, float explicitLineHeight) {
  LineMetrics out;
  float size = ts.getFontSize();
  if (size <= 0 || !fm) return out;
  auto faces = fm->collection()->findTypefaces(ts.getFontFamilies(), ts.getFontStyle());
  if (faces.empty() || !faces[0]) return out;
  // Chrome rounds the outline (unhinted) metrics.
  SkFont font(faces[0], size);
  font.setHinting(SkFontHinting::kNone);
  font.setSubpixel(true);
  SkFontMetrics m;
  font.getMetrics(&m);
  float a = -m.fAscent, d = m.fDescent;
  float ra = std::floor(a + 0.5f), rd = std::floor(d + 0.5f), rg = std::floor(std::max(0.f, m.fLeading) + 0.5f);
  // Chrome (Linux): when the descent rounds down it borrows a pixel from the
  // ascent so descenders are not cut off by overflow clips.
  if (rd < d && ra >= 1) {
    rd += 1;
    ra -= 1;
  }
  float h = explicitLineHeight > 0 ? explicitLineHeight : ra + rd + rg;
  float chromeBaseline = ra + std::floor((h - ra - rd) / 2);
  // skparagraph places the baseline using its shaping font's metrics, which
  // has slight hinting (FreeType then reports rounded ascent / descent).
  SkFont shaping(faces[0], size);
  shaping.setHinting(SkFontHinting::kSlight);
  shaping.setSubpixel(true);
  shaping.setEdging(SkFont::Edging::kAntiAlias);
  SkFontMetrics sm;
  shaping.getMetrics(&sm);
  float sa = -sm.fAscent, sd = sm.fDescent;
  float skBaseline = sa + (h - sa - sd) / 2;
  out.height = h;
  out.baselineShift = chromeBaseline - skBaseline;
  // skparagraph paints at floor(baseline + 0.5) within the line.
  out.paintDy = chromeBaseline - std::floor(skBaseline + 0.5f);
  return out;
}

// `rootShift`: the paragraph root's baseline shift, applied as a paint offset
// instead (shifting runs relative to the strut would grow every line box).
tl::TextStyle toSkStyle(const Resolved& r, FontManager* fm, float rootShift, LineMetrics* metricsOut = nullptr) {
  const std::string& defFamily = fm->defaultFamily;
  tl::TextStyle ts;
  ts.setColor(toSk(r.color));
  ts.setFontFamilies(families(r.family, defFamily));
  ts.setFontSize(r.fontSize);
  ts.setFontStyle(SkFontStyle(r.weight, SkFontStyle::kNormal_Width,
                              r.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant));
  LineMetrics lm = chromeLineMetrics(fm, ts, linePx(r));
  if (lm.height > 0) {
    ts.setHeight(lm.height / r.fontSize);
    ts.setHeightOverride(true);
    ts.setHalfLeading(true);
    if (!metricsOut && lm.baselineShift != rootShift) ts.setBaselineShift(lm.baselineShift - rootShift);
  }
  if (metricsOut) *metricsOut = lm;
  if (r.letterSpacing != 0) ts.setLetterSpacing(r.letterSpacing);
  if (r.decoration) {
    int d = 0;
    if (r.decoration & DecorUnderline) d |= tl::kUnderline;
    if (r.decoration & DecorLineThrough) d |= tl::kLineThrough;
    ts.setDecoration(tl::TextDecoration(d));
    ts.setDecorationColor(toSk(r.decorationColor ? *r.decorationColor : r.color));
    ts.setDecorationStyle(tl::TextDecorationStyle(std::min<int>(r.decorationStyle, 4)));
    ts.setDecorationMode(tl::kGaps);  // Chrome's text-decoration-skip-ink: auto
  }
  for (auto& s : r.shadows)
    ts.addShadow(tl::TextShadow(toSk(s.color), {s.offsetX, s.offsetY}, s.blur / 2));  // CSS: sigma = blur / 2
  for (auto& v : r.fontVariant) {
    const char* tag = nullptr;
    if (v == "tabular-nums") tag = "tnum";
    else if (v == "proportional-nums") tag = "pnum";
    else if (v == "lining-nums") tag = "lnum";
    else if (v == "oldstyle-nums") tag = "onum";
    else if (v == "small-caps") tag = "smcp";
    else if (v == "slashed-zero") tag = "zero";
    if (tag) ts.addFontFeature(SkString(tag), 1);
  }
  return ts;
}

// ---------------------------------------------------------------- unicode

// Decodes one UTF-8 code point; invalid bytes decode as U+FFFD.
uint32_t nextUtf8(const std::string& s, size_t* i) {
  unsigned char c = (unsigned char)s[(*i)++];
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> extra);
  for (int k = 0; k < extra; k++) {
    if (*i >= s.size() || ((unsigned char)s[*i] & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | ((unsigned char)s[(*i)++] & 0x3F);
  }
  return cp;
}

void appendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) out->push_back(char(cp));
  else if (cp < 0x800) {
    out->push_back(char(0xC0 | (cp >> 6)));
    out->push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(char(0xE0 | (cp >> 12)));
    out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(char(0xF0 | (cp >> 18)));
    out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(char(0x80 | (cp & 0x3F)));
  }
}

// Lower-casing for the scripts games commonly ship (Latin, Greek, Cyrillic,
// Armenian). Upper-casing goes through ICU.
uint32_t toLower(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x130) return 'i';
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    return (c & 1) ? c : c + 1;
  }
  if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x460 && c <= 0x4FF && !(c & 1)) return c + 1;
  if (c >= 0x531 && c <= 0x556) return c + 48;
  if (c >= 0x1E00 && c <= 0x1EFF && !(c & 1)) return c + 1;
  return c;
}

bool isRtl(uint32_t c) {
  return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
         (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
}
bool isStrongLtr(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
  if (c < 0xC0) return false;
  if (c == 0xD7 || c == 0xF7) return false;
  if (c >= 0x2000 && c <= 0x2BFF) return false;  // punctuation, symbols, arrows
  if (c >= 0x3000 && c <= 0x303F) return false;  // CJK punctuation
  if (c >= 0xFE00 && c <= 0xFE6F) return false;
  if (c >= 0x1F000) return false;               // emoji
  return !isRtl(c);
}

std::string applyTransform(const std::string& s, TextTransform t, FontManager* fm, bool* wordStart) {
  if (t == TextTransform::None) return s;
  if (t == TextTransform::Uppercase) {
    if (SkUnicode* u = fm->unicode()) {
      SkString up = u->toUpper(SkString(s.c_str(), s.size()));
      return std::string(up.c_str(), up.size());
    }
  }
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    size_t start = i;
    uint32_t cp = nextUtf8(s, &i);
    if (t == TextTransform::Lowercase) {
      appendUtf8(&out, toLower(cp));
    } else if (t == TextTransform::Capitalize) {
      bool letter = isStrongLtr(cp) || isRtl(cp) || (cp >= '0' && cp <= '9');
      if (letter && *wordStart) {
        std::string one = s.substr(start, i - start);
        if (SkUnicode* u = fm->unicode()) {
          SkString up = u->toUpper(SkString(one.c_str(), one.size()));
          out.append(up.c_str(), up.size());
        } else {
          out += one;
        }
      } else {
        out.append(s, start, i - start);
      }
      *wordStart = !letter && cp != '\'' && cp != 0x2019;
    } else {
      out.append(s, start, i - start);
    }
  }
  return out;
}

// ------------------------------------------------------------- the cache

struct TextCache : Node::Cache {
  std::unique_ptr<tl::Paragraph> paragraph;
  bool empty = true;
  uint32_t fontGeneration = 0;
  float laidOutWidth = -1;  // width passed to the last layout()
  float maxIntrinsic = 0;
  bool rtl = false;
  float paintDy = 0;  // baseline correction of the root style (see chromeLineMetrics)
  // Unhinted copies of the paragraph's text blobs, keyed by blob id (see
  // ChromeLikePainter). Cleared whenever the paragraph is laid out again.
  std::unordered_map<uint32_t, sk_sp<SkTextBlob>> blobs;
  // TextInput: the displayed text (value after textTransform / bullets, or
  // the placeholder) and the value -> display index map (UTF-16 units,
  // size value.size() + 1).
  std::u16string display;
  std::vector<int> v2d;
  bool placeholder = false;
};

// skparagraph shapes with slight hinting; Chrome (Linux, headless) draws
// glyph outlines unhinted, which changes every glyph's vertical edges. This
// painter redraws each blob with hinting off (same glyphs and positions);
// the rebuilt blobs are cached so steady-state frames do not allocate.
class ChromeLikePainter : public tl::ParagraphPainter {
 public:
  ChromeLikePainter(SkCanvas* c, TextCache* cache) : c_(c), cache_(cache) {}

  void drawTextBlob(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y, const SkPaintOrID& paint) override {
    if (const SkPaint* p = std::get_if<SkPaint>(&paint)) c_->drawTextBlob(unhinted(blob), x, y, *p);
  }
  void drawTextShadow(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y, SkColor color, SkScalar sigma) override {
    SkPaint p;
    p.setColor(color);
    p.setAntiAlias(true);
    if (sigma > 0) p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma, false));
    c_->drawTextBlob(unhinted(blob), x, y, p);
  }
  void drawRect(const SkRect& r, const SkPaintOrID& paint) override {
    if (const SkPaint* p = std::get_if<SkPaint>(&paint)) c_->drawRect(r, *p);
  }
  void drawFilledRect(const SkRect& r, const DecorationStyle& d) override {
    SkPaint p(d.skPaint());
    p.setStroke(false);
    c_->drawRect(r, p);
  }
  void drawPath(const SkPath& path, const DecorationStyle& d) override { c_->drawPath(path, d.skPaint()); }
  void drawLine(SkScalar x0, SkScalar y0, SkScalar x1, SkScalar y1, const DecorationStyle& d) override {
    c_->drawLine(x0, y0, x1, y1, d.skPaint());
  }
  void clipRect(const SkRect& r) override { c_->clipRect(r); }
  void translate(SkScalar dx, SkScalar dy) override { c_->translate(dx, dy); }
  void save() override { c_->save(); }
  void restore() override { c_->restore(); }

 private:
  sk_sp<SkTextBlob> unhinted(const sk_sp<SkTextBlob>& blob) {
    if (!blob) return blob;
    auto it = cache_->blobs.find(blob->uniqueID());
    if (it != cache_->blobs.end()) return it->second;
    SkTextBlobBuilder b;
    SkTextBlob::Iter iter(*blob);
    SkTextBlob::Iter::ExperimentalRun run;
    while (iter.experimentalNext(&run)) {
      SkFont f = run.font;
      f.setHinting(SkFontHinting::kNone);
      // Whole-pixel glyph origins, like Chrome with subpixel positioning off
      // (the reference harness and most Linux/Windows configs).
      f.setSubpixel(true);
      const auto& buf = b.allocRunPos(f, run.count);
      std::memcpy(buf.glyphs, run.glyphs, size_t(run.count) * sizeof(SkGlyphID));
      std::memcpy(buf.points(), run.positions, size_t(run.count) * sizeof(SkPoint));
    }
    sk_sp<SkTextBlob> out = b.make();
    if (!out) out = blob;
    cache_->blobs.emplace(blob->uniqueID(), out);
    return out;
  }

  SkCanvas* c_;
  TextCache* cache_;
};

struct BuildState {
  FontManager* fm;
  tl::ParagraphBuilder* builder;
  std::string content;  // for direction detection
  bool wordStart = true;
  float rootShift = 0;
};

void addChildren(Node* n, const Resolved& style, BuildState& st) {
  for (Node* c : n->children()) {
    if (c->type() == NodeType::RawText) {
      if (c->text().empty()) continue;
      std::string t = applyTransform(c->text(), style.transform, st.fm, &st.wordStart);
      st.builder->addText(t.data(), t.size());
      st.content += t;
    } else if (c->type() == NodeType::Text) {
      if (!c->style().display) continue;
      Resolved r = style;
      merge(&r, c->style().text);
      st.builder->pushStyle(toSkStyle(r, st.fm, st.rootShift));
      addChildren(c, r, st);
      st.builder->pop();
    }
    // Views / Images inside Text (inline blocks) are not supported yet.
  }
}

bool detectRtl(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = nextUtf8(s, &i);
    if (isRtl(cp)) return true;
    if (isStrongLtr(cp)) return false;
  }
  return false;
}

tl::TextAlign toSkAlign(TextAlign a) {
  switch (a) {
    case TextAlign::Left: return tl::TextAlign::kLeft;
    case TextAlign::Right: return tl::TextAlign::kRight;
    case TextAlign::Center: return tl::TextAlign::kCenter;
    case TextAlign::Justify: return tl::TextAlign::kJustify;
    case TextAlign::End: return tl::TextAlign::kEnd;
    case TextAlign::Auto: break;
  }
  return tl::TextAlign::kStart;
}

// What a TextInput shows: its (edit state) value with textTransform applied
// per code point (so indices map back) or bullets for secureTextEntry, else
// the placeholder.
void inputDisplay(Node* n, TextTransform transform, FontManager* fm, std::string* out, TextCache* c) {
  const Value& p = n->props();
  const std::u16string& v = edit::state(n)->value;
  c->v2d.assign(v.size() + 1, 0);
  out->clear();
  if (v.empty()) {
    c->placeholder = true;
    bool ws = true;
    *out = applyTransform(p["placeholder"].asString(), transform, fm, &ws);
    return;
  }
  c->placeholder = false;
  bool secure = p["secureTextEntry"].asBool();
  bool multiline = p["multiline"].asBool();
  bool plain = !secure && transform == TextTransform::None &&
               (multiline || v.find(u'\n') == std::u16string::npos);
  if (plain) {
    for (size_t i = 0; i <= v.size(); i++) c->v2d[i] = int(i);
    *out = edit::toUtf8(v);
    return;
  }
  bool wordStart = true;
  size_t i = 0;
  int d = 0;
  while (i < v.size()) {
    size_t len = (v[i] >= 0xD800 && v[i] <= 0xDBFF && i + 1 < v.size()) ? 2 : 1;
    std::string piece;
    if (secure) {
      piece = "\xE2\x80\xA2";  // U+2022 bullet
    } else if (!multiline && (v[i] == u'\n' || v[i] == u'\r')) {
      piece = " ";
    } else {
      piece = applyTransform(edit::toUtf8(v.substr(i, len)), transform, fm, &wordStart);
    }
    for (size_t k = 0; k < len; k++) c->v2d[i + k] = d;
    d += int(edit::toUtf16(piece).size());
    *out += piece;
    i += len;
  }
  c->v2d[v.size()] = d;
}

TextCache* ensureCache(Node* n) {
  Document* doc = n->document();
  FontManager* fm = doc ? doc->fonts : nullptr;
  if (!fm) return nullptr;
  auto* cache = static_cast<TextCache*>(n->textCache.get());
  if (cache && cache->fontGeneration == fm->generation) return cache;

  auto fresh = std::make_unique<TextCache>();
  fresh->fontGeneration = fm->generation;
  Resolved base;
  merge(&base, n->style().text);

  tl::ParagraphStyle ps;
  LineMetrics baseMetrics;
  tl::TextStyle baseSk = toSkStyle(base, fm, 0, &baseMetrics);
  fresh->paintDy = baseMetrics.paintDy;
  ps.setTextStyle(baseSk);
  ps.setApplyRoundingHack(false);  // Chrome keeps fractional line widths
  ps.setReplaceTabCharacters(true);
  const Value& props = n->props();
  bool isInput = n->type() == NodeType::TextInput;
  bool multiline = isInput && props["multiline"].asBool();
  int maxLines = int(props["numberOfLines"].asNumber(0));
  if (isInput && !multiline) maxLines = 1;
  if (maxLines > 0) {
    ps.setMaxLines(size_t(maxLines));
    if (!isInput && props["ellipsizeMode"].asString() != "clip") ps.setEllipsis(std::u16string(u"…"));
  }
  // The root's font + line-height acts as the CSS strut: every line is at
  // least that tall even if all its spans use smaller fonts.
  tl::StrutStyle strut;
  strut.setStrutEnabled(true);
  strut.setFontFamilies(baseSk.getFontFamilies());
  strut.setFontStyle(baseSk.getFontStyle());
  strut.setFontSize(base.fontSize);
  if (baseMetrics.height > 0) {
    strut.setHeight(baseMetrics.height / base.fontSize);
    strut.setHeightOverride(true);
    strut.setHalfLeading(true);
  }
  ps.setStrutStyle(strut);

  BuildState st{fm, nullptr, {}};
  st.rootShift = baseMetrics.baselineShift;
  std::string inputText;
  if (isInput) {
    inputDisplay(n, base.transform, fm, &inputText, fresh.get());
    fresh->display = edit::toUtf16(inputText);
    if (fresh->placeholder) {
      Color pc = Color::rgba(117, 117, 117);  // Chrome's ::placeholder color
      parseColor(props["placeholderTextColor"], &pc);
      base.color = pc;
      baseSk.setColor(toSk(pc));
      ps.setTextStyle(baseSk);
    }
    st.content = inputText;
  } else {
    // Content first, for dir="auto" detection (react-native-web sets it on Text).
    std::vector<Node*> stack{n};
    while (!stack.empty()) {
      Node* c = stack.back();
      stack.pop_back();
      if (c->type() == NodeType::RawText) st.content += c->text();
      for (auto it = c->children().rbegin(); it != c->children().rend(); ++it) stack.push_back(*it);
    }
  }
  bool rtl = base.direction == 2 || (base.direction == 0 && detectRtl(st.content));
  ps.setTextDirection(rtl ? tl::TextDirection::kRtl : tl::TextDirection::kLtr);
  // Single-line inputs are laid out unconstrained and aligned by the painter.
  ps.setTextAlign(isInput && !multiline ? tl::TextAlign::kLeft : toSkAlign(base.align));
  fresh->rtl = rtl;

  auto builder = tl::ParagraphBuilder::make(ps, fm->collection());
  if (!builder) {
    // No SkUnicode (e.g. ICU data missing): lay the text out as empty rather
    // than crash.
    static bool warned = false;
    if (!warned) std::fprintf(stderr, "jsui: text shaping unavailable (ICU data missing?)\n");
    warned = true;
    fresh->empty = true;
    cache = fresh.get();
    n->textCache = std::move(fresh);
    return cache;
  }
  st.builder = builder.get();
  st.content.clear();
  builder->pushStyle(baseSk);
  if (isInput) {
    builder->addText(inputText.data(), inputText.size());
    st.content = inputText;
  } else {
    addChildren(n, base, st);
  }
  builder->pop();
  fresh->empty = st.content.empty();
  if (!fresh->empty) fresh->paragraph = builder->Build();
  cache = fresh.get();
  n->textCache = std::move(fresh);
  return cache;
}

void layoutAt(TextCache* c, float width) {
  if (!c->paragraph) return;
  if (c->laidOutWidth < 0) {
    // First layout: unconstrained, to learn the intrinsic widths.
    c->paragraph->layout(kInf);
    c->laidOutWidth = kInf;
    c->blobs.clear();
    c->maxIntrinsic = c->paragraph->getMaxIntrinsicWidth();
  }
  // A width equal to the measured max-content width must not wrap because of
  // float error; lay out a hair wider instead.
  float w = width;
  if (w >= c->maxIntrinsic - 0.05f) w = std::max(w, c->maxIntrinsic) + 0.01f;
  if (std::isnan(w) || w <= 0) w = 0.01f;
  if (w != c->laidOutWidth) {
    c->paragraph->layout(w);
    c->laidOutWidth = w;
    c->blobs.clear();
  }
}

YGSize measureText(YGNodeConstRef y, float width, YGMeasureMode wm, float height, YGMeasureMode hm) {
  (void)height;
  (void)hm;
  Node* n = static_cast<Node*>(YGNodeGetContext(y));
  TextCache* c = n ? ensureCache(n) : nullptr;
  bool isInput = n && n->type() == NodeType::TextInput;
  if (!c || (c->empty && !isInput)) return {0, 0};
  if (isInput && c->empty) {
    // Empty input: one line of the input's font.
    float fs = n->style().text.fontSize.value_or(kDefaultFontSize);
    float lh = n->style().text.lineHeight.value_or(std::ceil(fs * 1.21f));
    return {wm == YGMeasureModeExactly ? width : fs * 11, lh};
  }
  layoutAt(c, kInf);
  float maxW = c->maxIntrinsic;
  float target = maxW;
  if (wm == YGMeasureModeExactly) target = width;
  else if (wm == YGMeasureModeAtMost) target = std::min(width, maxW);
  if (isInput && !(n->props()["multiline"].asBool())) {
    // Single-line inputs never wrap; Chrome sizes them ~20 characters wide.
    float fs = n->style().text.fontSize.value_or(kDefaultFontSize);
    float w = wm == YGMeasureModeExactly ? width : std::min(fs * 11, wm == YGMeasureModeAtMost ? width : kInf);
    layoutAt(c, kInf);
    return {w, c->paragraph->getHeight()};
  }
  layoutAt(c, target);
  float h = c->paragraph->getHeight();
  float w = wm == YGMeasureModeExactly ? width : target;
  return {w, h};
}

// First-line baseline (as painted) from the border-box top, for
// alignItems: 'baseline'. Without it Yoga uses the box bottom.
float textBaseline(YGNodeConstRef y, float width, float height) {
  Node* n = static_cast<Node*>(YGNodeGetContext(y));
  TextCache* c = n ? ensureCache(n) : nullptr;
  YGNodeRef yn = const_cast<YGNodeRef>(y);
  float top = YGNodeLayoutGetPadding(yn, YGEdgeTop) + YGNodeLayoutGetBorder(yn, YGEdgeTop);
  if (!c || !c->paragraph || c->empty) return height;
  float inner = width - YGNodeLayoutGetPadding(yn, YGEdgeLeft) - YGNodeLayoutGetPadding(yn, YGEdgeRight) -
                YGNodeLayoutGetBorder(yn, YGEdgeLeft) - YGNodeLayoutGetBorder(yn, YGEdgeRight);
  layoutAt(c, std::max(0.f, inner));
  float b = c->paragraph->getAlphabeticBaseline();
  return top + std::floor(b + 0.5f) + c->paintDy;
}

// ------------------------------------------------------ TextInput geometry
//
// Content-box coordinates (origin at the content box's top-left) unless
// noted. Indices are value indices (UTF-16 units of the edit state).

constexpr float kCaretWidth = 1;

struct InputGeom {
  TextCache* c = nullptr;
  tl::Paragraph* p = nullptr;  // null: nothing to show (empty, no placeholder)
  edit::EditState* s = nullptr;
  bool multiline = false;
  bool alignRight = false, alignCenter = false;
  float cw = 0, ch = 0;                // content box size
  float ox = 0, oy = 0;                // paragraph origin, scroll applied
  float minScroll = 0, maxScroll = 0;  // scroll range (x single line, y multiline)
  float lineHeight = 0;                // empty input line height
};

float emptyLineHeight(Node* n) {
  float fs = n->style().text.fontSize.value_or(kDefaultFontSize);
  if (n->style().text.lineHeight) {
    float lh = *n->style().text.lineHeight;
    return n->style().text.lineHeightRelative ? lh * fs : lh;
  }
  return std::ceil(fs * 1.21f);
}

bool inputGeom(Node* n, float cw, float ch, InputGeom* g) {
  TextCache* c = ensureCache(n);
  if (!c) return false;
  g->c = c;
  g->s = edit::state(n);
  g->multiline = n->props()["multiline"].asBool();
  g->cw = cw;
  g->ch = ch;
  g->lineHeight = emptyLineHeight(n);
  TextAlign a = n->style().text.textAlign.value_or(TextAlign::Auto);
  g->alignCenter = a == TextAlign::Center;
  g->alignRight = a == TextAlign::Right || (a == TextAlign::Auto && c->rtl) || (a == TextAlign::End && !c->rtl);
  float ph = g->lineHeight, lw = 0;
  if (c->paragraph && !c->empty) {
    layoutAt(c, g->multiline ? cw : kInf);
    g->p = c->paragraph.get();
    ph = g->p->getHeight();
    lw = g->p->getLongestLine();
  }
  if (g->multiline) {
    g->ox = 0;
    if (!g->p) g->ox = g->alignCenter ? cw / 2 : g->alignRight ? cw - kCaretWidth : 0;
    g->maxScroll = std::max(0.f, ph - ch);
    g->oy = -std::clamp(g->s->scrollY, 0.f, g->maxScroll);
    return true;
  }
  g->oy = (ch - ph) / 2;  // inputs center one line vertically
  float tx;
  if (!g->p) tx = g->alignCenter ? cw / 2 : g->alignRight ? cw - kCaretWidth : 0;
  else if (lw + kCaretWidth > cw) tx = c->rtl ? cw - lw - kCaretWidth : 0;  // overflow: start edge visible
  else if (g->alignCenter) tx = (cw - lw) / 2;
  else if (g->alignRight) tx = cw - lw;
  else tx = 0;
  g->minScroll = std::min(0.f, tx);
  g->maxScroll = std::max(0.f, tx + lw + kCaretWidth - cw);
  g->ox = tx - std::clamp(g->s->scrollX, g->minScroll, g->maxScroll);
  return true;
}

int displayIndex(const InputGeom& g, int v) {
  if (g.c->placeholder || g.c->v2d.empty()) return 0;
  v = std::clamp(v, 0, int(g.c->v2d.size()) - 1);
  return g.c->v2d[size_t(v)];
}

int valueIndex(const InputGeom& g, int d) {
  if (g.c->placeholder || g.c->v2d.empty()) return 0;
  auto it = std::lower_bound(g.c->v2d.begin(), g.c->v2d.end(), d);
  int v = it == g.c->v2d.end() ? int(g.c->v2d.size()) - 1 : int(it - g.c->v2d.begin());
  // Never inside a cluster (surrogate pair, combining sequence).
  const std::u16string& val = g.s->value;
  int b = edit::prevBoundary(val, v);
  if (v > 0 && edit::nextBoundary(val, b) != v) v = b;
  return v;
}

// Caret at display index d, paragraph coordinates.
SkRect caretInParagraph(const InputGeom& g, int d) {
  tl::Paragraph* p = g.p;
  const std::u16string& t = g.c->display;
  int len = int(t.size());
  d = std::clamp(d, 0, len);
  float x = 0;
  int line = -1;
  tl::Paragraph::GlyphInfo gi;
  // At a soft line break the caret belongs to the start of the next line
  // (downstream, like a click there in Chrome).
  bool wrapped = d > 0 && d < len && t[size_t(d - 1)] != u'\n' && t[size_t(d)] != u'\n' &&
                 p->getLineNumberAtUTF16Offset(size_t(d - 1)) != p->getLineNumberAtUTF16Offset(size_t(d));
  if (d > 0 && !wrapped && t[size_t(d - 1)] != u'\n' && p->getGlyphInfoAtUTF16Offset(size_t(d - 1), &gi)) {
    // After the previous cluster: its trailing edge.
    x = gi.fDirection == tl::TextDirection::kRtl ? gi.fGraphemeLayoutBounds.fLeft : gi.fGraphemeLayoutBounds.fRight;
    line = p->getLineNumberAtUTF16Offset(size_t(d - 1));
  } else if (d < len && t[size_t(d)] != u'\n' && p->getGlyphInfoAtUTF16Offset(size_t(d), &gi)) {
    // Line start: the next cluster's leading edge.
    x = gi.fDirection == tl::TextDirection::kRtl ? gi.fGraphemeLayoutBounds.fRight : gi.fGraphemeLayoutBounds.fLeft;
    line = p->getLineNumberAtUTF16Offset(size_t(d));
  } else {
    // Empty line (between / after hard breaks).
    line = d < len ? p->getLineNumberAtUTF16Offset(size_t(d)) : int(p->lineNumber()) - 1;
    tl::LineMetrics lm;
    if (line >= 0 && p->getLineMetricsAt(line, &lm)) {
      x = float(lm.fLeft);
      if (g.c->rtl && x > 0) x -= kCaretWidth;
    }
  }
  float top = 0, bottom = g.lineHeight;
  tl::LineMetrics lm;
  if (line >= 0 && p->getLineMetricsAt(line, &lm)) {
    top = float(lm.fBaseline - lm.fAscent);
    bottom = float(lm.fBaseline + lm.fDescent);
  }
  return SkRect::MakeLTRB(x, top, x + kCaretWidth, bottom);
}

SkRect caretContent(const InputGeom& g, int v) {
  if (!g.p) return SkRect::MakeXYWH(g.ox, g.oy, kCaretWidth, g.lineHeight);
  return caretInParagraph(g, displayIndex(g, v)).makeOffset(g.ox, g.oy);
}

int indexAtContent(const InputGeom& g, float x, float y) {
  if (!g.p || g.c->placeholder) return 0;
  auto pos = g.p->getGlyphPositionAtCoordinate(x - g.ox, y - g.oy);
  return valueIndex(g, int(pos.position));
}

void contentOrigin(Node* n, float* x, float* y, float* w, float* h) {
  YGNodeRef yn = n->yoga();
  float l = YGNodeLayoutGetPadding(yn, YGEdgeLeft) + YGNodeLayoutGetBorder(yn, YGEdgeLeft);
  float t = YGNodeLayoutGetPadding(yn, YGEdgeTop) + YGNodeLayoutGetBorder(yn, YGEdgeTop);
  float r = YGNodeLayoutGetPadding(yn, YGEdgeRight) + YGNodeLayoutGetBorder(yn, YGEdgeRight);
  float b = YGNodeLayoutGetPadding(yn, YGEdgeBottom) + YGNodeLayoutGetBorder(yn, YGEdgeBottom);
  *x = l;
  *y = t;
  *w = std::max(0.f, YGNodeLayoutGetWidth(yn) - l - r);
  *h = std::max(0.f, YGNodeLayoutGetHeight(yn) - t - b);
}

// Geometry of a laid-out TextInput from its Yoga box.
bool nodeGeom(Node* n, InputGeom* g, float* x0, float* y0) {
  if (!n || n->type() != NodeType::TextInput || !n->yoga()) return false;
  float w, h;
  contentOrigin(n, x0, y0, &w, &h);
  return inputGeom(n, w, h, g);
}

SkColor selectionColorOf(Node* n) {
  Color c;
  if (parseColor(n->props()["selectionColor"], &c)) {
    if (c.a >= 1) c.a = 0.3f;  // an opaque tint highlights translucently (iOS)
    return toSk(c);
  }
  return SkColorSetARGB(77, 0, 122, 255);  // rgba(0, 122, 255, 0.3): #B3D7FF on white
}

SkColor caretColorOf(Node* n) {
  Color c{0, 0, 0, 1};
  if (parseColor(n->props()["cursorColor"], &c) || parseColor(n->props()["selectionColor"], &c)) return toSk(c);
  if (n->style().text.color) return toSk(*n->style().text.color);
  return SK_ColorBLACK;
}

}  // namespace

void installMeasure(Node* node) {
  if (!node->yoga()) return;
  if (node->type() == NodeType::Text || node->type() == NodeType::TextInput) {
    YGNodeSetBaselineFunc(node->yoga(), textBaseline);
    if (YGNodeGetChildCount(node->yoga()) == 0) YGNodeSetMeasureFunc(node->yoga(), measureText);
    YGNodeSetNodeType(node->yoga(), YGNodeTypeText);
  }
}

tl::Paragraph* layoutParagraph(Node* node, float width) {
  TextCache* c = ensureCache(node);
  if (!c || !c->paragraph) return nullptr;
  layoutAt(c, width);
  return c->paragraph.get();
}

void paintText(Node* node, SkCanvas* canvas, float x, float y, float width, float height) {
  (void)height;
  if (tl::Paragraph* p = layoutParagraph(node, width)) {
    auto* c = static_cast<TextCache*>(node->textCache.get());
    ChromeLikePainter painter(canvas, c);
    p->paint(&painter, x, y + c->paintDy);
  }
}

void paintTextInput(Node* node, SkCanvas* canvas, float x, float y, float width, float height) {
  InputGeom g;
  if (!inputGeom(node, width, height, &g)) return;
  edit::EditState* s = g.s;
  canvas->save();
  canvas->clipRect(SkRect::MakeXYWH(x, y, width, height));
  if (g.p && s->focused && !s->collapsed() && !g.c->placeholder) {
    SkPaint sp;
    sp.setColor(selectionColorOf(node));
    for (const tl::TextBox& b :
         g.p->getRectsForRange(unsigned(displayIndex(g, s->start())), unsigned(displayIndex(g, s->end())),
                               tl::RectHeightStyle::kMax, tl::RectWidthStyle::kTight))
      canvas->drawRect(b.rect.makeOffset(x + g.ox, y + g.oy), sp);
  }
  if (g.p) {
    ChromeLikePainter painter(canvas, g.c);
    g.p->paint(&painter, x + g.ox, y + g.oy + g.c->paintDy);
  }
  canvas->restore();
  if (inputCaretVisible(node)) {
    // The caret may sit on the content edge (end of right-aligned text).
    canvas->save();
    canvas->clipRect(SkRect::MakeXYWH(x - kCaretWidth, y, width + 2 * kCaretWidth, height));
    SkPaint cp;
    cp.setColor(caretColorOf(node));
    canvas->drawRect(caretContent(g, s->focus).makeOffset(x, y), cp);
    canvas->restore();
  }
}

bool inputCaretVisible(Node* n) {
  edit::EditState* s = edit::peek(n);
  if (!s || !s->focused || !s->caretOn || !s->collapsed()) return false;
  const Value& p = n->props();
  if (p["caretHidden"].asBool()) return false;
  if (p["editable"].isBool() && !p["editable"].asBool()) return false;
  if (p["readOnly"].asBool()) return false;
  return true;
}

SkRect inputCaretRect(Node* n, int index) {
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0)) return SkRect::MakeEmpty();
  return caretContent(g, index).makeOffset(x0, y0);
}

int inputIndexAt(Node* n, float x, float y) {
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0)) return 0;
  return indexAtContent(g, x - x0, y - y0);
}

void inputSelectionRects(Node* n, int start, int end, std::vector<SkRect>* out) {
  out->clear();
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0) || !g.p || g.c->placeholder || start >= end) return;
  for (const tl::TextBox& b : g.p->getRectsForRange(unsigned(displayIndex(g, start)), unsigned(displayIndex(g, end)),
                                                     tl::RectHeightStyle::kMax, tl::RectWidthStyle::kTight))
    out->push_back(b.rect.makeOffset(x0 + g.ox, y0 + g.oy));
}

int inputVerticalMove(Node* n, int index, int dir, float* goalX, bool hasGoal) {
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0)) return index;
  int len = int(g.s->value.size());
  SkRect r = caretContent(g, index);
  float gx = hasGoal ? *goalX : r.fLeft;
  *goalX = gx;
  if (!g.p || g.c->placeholder || !g.multiline) return dir < 0 ? 0 : len;
  float lh = std::max(1.f, r.height());
  float ty = dir < 0 ? r.fTop - lh / 2 : r.fBottom + lh / 2;
  if (ty < g.oy) return 0;
  if (ty >= g.oy + g.p->getHeight()) return len;
  return indexAtContent(g, gx, ty);
}

int inputLineEdge(Node* n, int index, bool end) {
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0)) return index;
  int len = int(g.s->value.size());
  if (!g.multiline || !g.p || g.c->placeholder) return end ? len : 0;
  SkRect r = caretContent(g, index);
  bool right = end != g.c->rtl;
  int i = indexAtContent(g, right ? 1e7f : -1e7f, r.centerY());
  if (end && i > 0 && i < len) {
    // A soft-wrapped line ends before its trailing space (the index after it
    // is the next line's start).
    int d = displayIndex(g, i);
    const std::u16string& t = g.c->display;
    if (d > 0 && d < int(t.size()) && t[size_t(d)] != u'\n' && t[size_t(d - 1)] != u'\n' &&
        g.p->getLineNumberAtUTF16Offset(size_t(d - 1)) != g.p->getLineNumberAtUTF16Offset(size_t(d)))
      i = edit::prevBoundary(g.s->value, i);
  }
  return i;
}

bool inputRtl(Node* n) {
  TextCache* c = n ? ensureCache(n) : nullptr;
  return c && c->rtl;
}

bool inputScrollToCaret(Node* n) {
  InputGeom g;
  float x0, y0;
  if (!nodeGeom(n, &g, &x0, &y0)) return false;
  edit::EditState* s = g.s;
  SkRect r = caretContent(g, s->focus);
  if (g.multiline) {
    float sy = std::clamp(s->scrollY, g.minScroll, g.maxScroll);
    if (r.fTop < 0) sy += r.fTop;
    else if (r.fBottom > g.ch) sy += r.fBottom - g.ch;
    sy = std::clamp(sy, g.minScroll, g.maxScroll);
    bool changed = sy != s->scrollY;
    s->scrollY = sy;
    return changed;
  }
  float sx = std::clamp(s->scrollX, g.minScroll, g.maxScroll);
  if (r.fLeft < 0) sx += r.fLeft;
  else if (r.fRight > g.cw) sx += r.fRight - g.cw;
  sx = std::clamp(sx, g.minScroll, g.maxScroll);
  bool changed = sx != s->scrollX;
  s->scrollX = sx;
  return changed;
}

bool inputCaretDamage(Node* n, SkRect* before, SkRect* after) {
  edit::EditState* s = edit::peek(n);
  if (!s) return false;
  bool vis = inputCaretVisible(n);
  SkRect r = vis ? inputCaretRect(n, s->focus) : SkRect::MakeEmpty();
  if (vis == s->recordedVisible && (!vis || r == s->recordedCaret)) return false;
  *before = s->recordedVisible ? s->recordedCaret : SkRect::MakeEmpty();
  *after = r;
  s->recordedVisible = vis;
  s->recordedCaret = r;
  return true;
}

void invalidateAll(Node* root) {
  if (!root) return;
  if (root->type() == NodeType::Text || root->type() == NodeType::TextInput) {
    root->textCache.reset();
    if (root->yoga() && YGNodeHasMeasureFunc(root->yoga())) YGNodeMarkDirty(root->yoga());
  }
  for (Node* c : root->children()) invalidateAll(c);
}

}  // namespace jsui::text
