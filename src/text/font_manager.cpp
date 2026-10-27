#include "text/font_manager.h"

#include <algorithm>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontParameters.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skunicode/include/SkUnicode.h"
#if JSUI_SKUNICODE_REFCNT
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "text/embedded_fonts.h"
#include "text/font_decode.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#else
#include "include/ports/SkFontMgr_empty.h"
#if !defined(__EMSCRIPTEN__)
#include "include/ports/SkFontMgr_fontconfig.h"
// Since ~m130 the FontConfig manager takes the font scanner explicitly.
#if __has_include("include/ports/SkFontScanner_FreeType.h")
#include "include/ports/SkFontScanner_FreeType.h"
#define JSUI_FONTCONFIG_SCANNER 1
#endif
#endif
#endif

namespace jsui {

namespace {

std::string lowerAscii(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

// A variable font's axes. Skia m157 takes an SkSpan, m124 a pointer + count.
template <class T>
std::vector<SkFontParameters::Variation::Axis> variationAxes(const T& tf) {
  using Axis = SkFontParameters::Variation::Axis;
  std::vector<Axis> axes;
  if constexpr (requires { tf.getVariationDesignParameters(SkSpan<Axis>()); }) {
    int n = tf.getVariationDesignParameters(SkSpan<Axis>());
    if (n <= 0) return axes;
    axes.resize(size_t(n));
    if (tf.getVariationDesignParameters(SkSpan<Axis>(axes.data(), axes.size())) != n) axes.clear();
  } else {
    int n = tf.getVariationDesignParameters(nullptr, 0);
    if (n <= 0) return axes;
    axes.resize(size_t(n));
    if (tf.getVariationDesignParameters(axes.data(), n) != n) axes.clear();
  }
  return axes;
}

bool sameStyle(const SkFontStyle& a, const SkFontStyle& b) {
  return a.weight() == b.weight() && a.width() == b.width() && a.slant() == b.slant();
}

// The faces registered under one family name. Immutable once handed out:
// the registry swaps in a new set when a face is added (skparagraph keeps
// the sets it matched in its caches).
class FaceSet final : public SkFontStyleSet {
 public:
  struct Face {
    sk_sp<SkTypeface> typeface;
    SkFontStyle style;  // what matching uses (the @font-face descriptors)
  };
  FaceSet() = default;
  FaceSet(std::string name, std::vector<Face> faces) : name(std::move(name)), key(lowerAscii(this->name)), faces(std::move(faces)) {}

  int count() override { return int(faces.size()); }
  void getStyle(int index, SkFontStyle* style, SkString* styleName) override {
    if (style) *style = faces[size_t(index)].style;
    if (styleName) styleName->reset();
  }
  sk_sp<SkTypeface> createTypeface(int index) override { return faces[size_t(index)].typeface; }
  sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override { return matchStyleCSS3(pattern); }

  const std::string name;
  const std::string key;  // lower case: CSS family names match case-insensitively
  const std::vector<Face> faces;
};

}  // namespace

// The registered + embedded fonts as an SkFontMgr: skparagraph's asset font
// manager (family matching with the registered styles, per-character
// fallback across the fallback list and every registered font) and Skottie's.
class FontRegistry final : public SkFontMgr {
 public:
  explicit FontRegistry(sk_sp<SkFontMgr> data) : data_(std::move(data)) {}

  std::vector<sk_sp<FaceSet>> sets;  // registration order
  std::vector<std::string> fallbacks;
  std::string defaultFamily = "Inter";
  // An inherited registry (a JSUIContainer's parent runtime's): families
  // this one lacks are found there, and glyphs no font here has.
  sk_sp<FontRegistry> parent;

  sk_sp<FaceSet> find(const std::string& family) const {
    std::string key = lowerAscii(family);
    for (auto& s : sets)
      if (s->key == key) return s;
    return parent ? parent->find(family) : nullptr;
  }

  void add(const std::string& family, sk_sp<SkTypeface> tf, const SkFontStyle& style) {
    std::string key = lowerAscii(family);
    for (auto& s : sets) {
      if (s->key != key) continue;
      std::vector<FaceSet::Face> faces;
      for (auto& f : s->faces)
        if (!sameStyle(f.style, style)) faces.push_back(f);
      faces.push_back({std::move(tf), style});
      s = sk_make_sp<FaceSet>(s->name, std::move(faces));
      return;
    }
    sets.push_back(sk_make_sp<FaceSet>(family, std::vector<FaceSet::Face>{{std::move(tf), style}}));
  }

  bool remove(const std::string& family) {
    std::string key = lowerAscii(family);
    auto it = std::find_if(sets.begin(), sets.end(), [&](auto& s) { return s->key == key; });
    if (it == sets.end()) return false;
    sets.erase(it);
    return true;
  }

  sk_sp<SkFontMgr> data() const { return data_; }

 protected:
  int onCountFamilies() const override { return int(sets.size()); }
  void onGetFamilyName(int index, SkString* familyName) const override { familyName->set(sets[size_t(index)]->name.c_str()); }
  sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override { return sets[size_t(index)]; }
  sk_sp<SkFontStyleSet> onMatchFamily(const char familyName[]) const override {
    sk_sp<FaceSet> s = find(familyName ? familyName : defaultFamily);
    if (s) return s;
    return sk_make_sp<FaceSet>();
  }
  sk_sp<SkTypeface> onMatchFamilyStyle(const char familyName[], const SkFontStyle& style) const override {
    sk_sp<FaceSet> s = find(familyName ? familyName : defaultFamily);
    return s ? s->matchStyle(style) : nullptr;
  }
  // Glyph fallback: the named family, the fallback list, the default family,
  // then every other registered font. skparagraph asks this manager before
  // the system one, so registered fonts win over whatever the OS has.
  sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char familyName[], const SkFontStyle& style, const char*[], int,
                                                SkUnichar character) const override {
    std::vector<FaceSet*> tried;
    auto attempt = [&](FaceSet* s) -> sk_sp<SkTypeface> {
      if (!s || std::find(tried.begin(), tried.end(), s) != tried.end()) return nullptr;
      tried.push_back(s);
      sk_sp<SkTypeface> tf = s->matchStyle(style);
      if (tf && tf->unicharToGlyph(character)) return tf;
      // Another face of the family may cover it (e.g. only Bold has symbols).
      for (auto& f : s->faces)
        if (f.typeface != tf && f.typeface->unicharToGlyph(character)) return f.typeface;
      return nullptr;
    };
    if (familyName)
      if (auto tf = attempt(find(familyName).get())) return tf;
    for (auto& f : fallbacks)
      if (auto tf = attempt(find(f).get())) return tf;
    if (auto tf = attempt(find(defaultFamily).get())) return tf;
    for (auto& s : sets)
      if (auto tf = attempt(s.get())) return tf;
    if (parent) return parent->onMatchFamilyStyleCharacter(familyName, style, nullptr, 0, character);
    return nullptr;
  }
  sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int ttcIndex) const override {
    return data_ ? data_->makeFromData(std::move(data), ttcIndex) : nullptr;
  }
  sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream, int ttcIndex) const override {
    return data_ ? data_->makeFromStream(std::move(stream), ttcIndex) : nullptr;
  }
  sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream, const SkFontArguments& args) const override {
    return data_ ? data_->makeFromStream(std::move(stream), args) : nullptr;
  }
  sk_sp<SkTypeface> onMakeFromFile(const char path[], int ttcIndex) const override {
    return data_ ? data_->makeFromFile(path, ttcIndex) : nullptr;
  }
  sk_sp<SkTypeface> onLegacyMakeTypeface(const char familyName[], SkFontStyle style) const override {
    if (auto tf = onMatchFamilyStyle(familyName, style)) return tf;
    return sets.empty() ? nullptr : sets.front()->matchStyle(style);
  }

 private:
  sk_sp<SkFontMgr> data_;  // makes typefaces from memory
};

struct FontManager::Impl {
  sk_sp<SkFontMgr> system;  // fallback for glyphs the registered fonts lack (may be null)
  sk_sp<FontRegistry> registry;
  sk_sp<skia::textlayout::FontCollection> collection;
#if JSUI_SKUNICODE_REFCNT
  sk_sp<SkUnicode> unicode;
#else
  std::unique_ptr<SkUnicode> unicode;
#endif
};

static sk_sp<SkFontMgr> makeSystemFontMgr() {
#if defined(__APPLE__)
  return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
  return SkFontMgr_New_DirectWrite();
#elif defined(__ANDROID__)
  // /system/etc/fonts.xml: Roboto, Noto (CJK, emoji, scripts) as fallbacks.
  return SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif defined(__EMSCRIPTEN__)
  // No system fonts in the browser; FreeType-backed manager so makeFromData works.
  return SkFontMgr_New_Custom_Empty();
#else
#if JSUI_FONTCONFIG_SCANNER
  return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#else
  return SkFontMgr_New_FontConfig(nullptr);
#endif
#endif
}

// FreeType from memory on Linux/WASM (same rasteriser Chrome uses on Linux,
// which keeps reference renders close); the platform manager elsewhere
// (Android's is FreeType too).
static sk_sp<SkFontMgr> makeDataFontMgr(const sk_sp<SkFontMgr>& system) {
#if defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)
  return system;
#else
  (void)system;
  return SkFontMgr_New_Custom_Empty();
#endif
}

FontManager::FontManager() : impl_(std::make_unique<Impl>()) {
  impl_->system = makeSystemFontMgr();
  impl_->registry = sk_make_sp<FontRegistry>(makeDataFontMgr(impl_->system));
  impl_->collection = sk_make_sp<skia::textlayout::FontCollection>();
  impl_->collection->setAssetFontManager(impl_->registry);
  if (impl_->system) impl_->collection->setDefaultFontManager(impl_->system, defaultFamily_.c_str());
  impl_->collection->enableFontFallback();
  // Embedded data is static: wrap it without copying.
  sk_sp<SkFontMgr> data = impl_->registry->data();
  for (size_t i = 0; i < kEmbeddedFontCount; i++) {
    const EmbeddedFont& f = kEmbeddedFonts[i];
    sk_sp<SkTypeface> tf = data ? data->makeFromData(SkData::MakeWithoutCopy(f.data, f.size)) : nullptr;
    if (tf) impl_->registry->add(f.family, tf, tf->fontStyle());
  }
}

FontManager::~FontManager() = default;

void FontManager::changed() {
  impl_->collection->clearCaches();
  generation++;
}

int FontManager::registerFont(const std::string& family, const void* data, size_t size, int weight, int italic,
                              std::string* error) {
  std::string scratch;
  std::string& err = error ? *error : scratch;
  sk_sp<SkFontMgr> mgr = impl_->registry->data();
  if (!data || !size || !mgr) return err = "no font data", 0;
  sk_sp<SkData> bytes;
  FontContainer kind = sniffFont(data, size);
  if (kind == FontContainer::Woff || kind == FontContainer::Woff2) {
    std::vector<uint8_t> sfnt;
    if (!decodeWebFont(data, size, &sfnt, &err)) return 0;
    bytes = SkData::MakeWithCopy(sfnt.data(), sfnt.size());
  } else {
    bytes = SkData::MakeWithCopy(data, size);
  }
  int faces = collectionFaceCount(bytes->data(), bytes->size());
  int registered = 0;
  for (int index = 0; index < faces; index++) {
    sk_sp<SkTypeface> tf = mgr->makeFromData(bytes, index);
    std::vector<uint8_t> face;
    if (!tf && index > 0 && extractCollectionFace(bytes->data(), bytes->size(), index, &face))
      tf = mgr->makeFromData(SkData::MakeWithCopy(face.data(), face.size()), 0);
    if (!tf) continue;
    std::string name = family;
    if (name.empty()) {
      SkString n;
      tf->getFamilyName(&n);
      name = n.c_str();
    }
    if (name.empty()) continue;
    SkFontStyle fileStyle = tf->fontStyle();
    SkFontStyle::Slant slant = italic < 0 ? fileStyle.slant() : italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant;

    // Variable font: one named instance per weight step of its 'wght' axis.
    const SkFourByteTag wghtTag = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFontParameters::Variation::Axis* wght = nullptr;
    std::vector<SkFontParameters::Variation::Axis> axes = variationAxes(*tf);
    for (auto& a : axes)
      if (a.tag == wghtTag) wght = &a;
    auto instance = [&](float w) -> sk_sp<SkTypeface> {
      SkFontArguments::VariationPosition::Coordinate c{wghtTag, std::clamp(w, wght->min, wght->max)};
      SkFontArguments args;
      args.setCollectionIndex(index);
      args.setVariationDesignPosition({&c, 1});
      return tf->makeClone(args);
    };
    if (wght && weight <= 0) {
      int added = 0;
      for (int w = 100; w <= 900; w += 100) {
        if (w < wght->min - 0.5f || w > wght->max + 0.5f) continue;
        if (sk_sp<SkTypeface> clone = instance(float(w))) {
          impl_->registry->add(name, clone, SkFontStyle(w, fileStyle.width(), slant));
          added++;
        }
      }
      if (added) {
        registered++;
        continue;
      }
    }
    int w = weight > 0 ? std::clamp(weight, 1, 1000) : fileStyle.weight();
    if (wght && weight > 0)
      if (sk_sp<SkTypeface> clone = instance(float(w))) tf = clone;
    impl_->registry->add(name, tf, SkFontStyle(w, fileStyle.width(), slant));
    registered++;
  }
  if (!registered) {
    err = kind == FontContainer::Unknown ? "not a supported font file (expected TTF, OTF, TTC, WOFF or WOFF2)"
                                         : "the font data could not be read";
    return 0;
  }
  changed();
  return registered;
}

bool FontManager::unregisterFamily(const std::string& family) {
  if (!impl_->registry->remove(family)) return false;
  changed();
  return true;
}

void FontManager::setDefaultFamily(const std::string& family) {
  if (family.empty() || family == defaultFamily_) return;
  defaultFamily_ = family;
  impl_->registry->defaultFamily = family;
  if (impl_->system) impl_->collection->setDefaultFontManager(impl_->system, family.c_str());
  changed();
}

const std::vector<std::string>& FontManager::fallbackFamilies() const { return impl_->registry->fallbacks; }

void FontManager::setFallbackFamilies(std::vector<std::string> families) {
  families.erase(std::remove(families.begin(), families.end(), std::string()), families.end());
  if (families == impl_->registry->fallbacks) return;
  impl_->registry->fallbacks = std::move(families);
  changed();
}

bool FontManager::hasFamily(const std::string& family) const { return impl_->registry->find(family) != nullptr; }

void FontManager::inherit(FontManager* parent) {
  impl_->registry->parent = parent ? parent->impl_->registry : nullptr;
  changed();
}

std::vector<std::string> FontManager::familyNames() const {
  std::vector<std::string> out, keys;
  for (const FontRegistry* r = impl_->registry.get(); r; r = r->parent.get())
    for (auto& s : r->sets)
      if (std::find(keys.begin(), keys.end(), s->key) == keys.end()) {
        keys.push_back(s->key);
        out.push_back(s->name);
      }
  return out;
}

SkUnicode* FontManager::unicode() {
#if JSUI_SKUNICODE_REFCNT
  if (!impl_->unicode) impl_->unicode = SkUnicodes::ICU::Make();
#else
  if (!impl_->unicode) impl_->unicode = SkUnicode::MakeIcuBasedUnicode();
#endif
  return impl_->unicode.get();
}

#if JSUI_SKUNICODE_REFCNT
sk_sp<SkUnicode> FontManager::unicodeRef() {
  unicode();
  return impl_->unicode;
}
#endif

sk_sp<skia::textlayout::FontCollection> FontManager::collection() { return impl_->collection; }
sk_sp<SkFontMgr> FontManager::fontMgr() { return impl_->system; }

sk_sp<SkFontMgr> FontManager::assetFontMgr() { return impl_->registry; }

}  // namespace jsui
