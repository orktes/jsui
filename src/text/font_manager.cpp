#include "text/font_manager.h"

#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "text/embedded_fonts.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_empty.h"
#if !defined(__EMSCRIPTEN__)
#include "include/ports/SkFontMgr_fontconfig.h"
#endif
#endif

namespace jsui {

struct FontManager::Impl {
  sk_sp<SkFontMgr> system;  // fallback for glyphs the registered fonts lack (may be null)
  sk_sp<SkFontMgr> data;    // makes typefaces from memory
  sk_sp<skia::textlayout::TypefaceFontProvider> provider;
  sk_sp<skia::textlayout::FontCollection> collection;
  std::unique_ptr<SkUnicode> unicode;
};

static sk_sp<SkFontMgr> makeSystemFontMgr() {
#if defined(__APPLE__)
  return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
  return SkFontMgr_New_DirectWrite();
#elif defined(__EMSCRIPTEN__)
  // No system fonts in the browser; FreeType-backed manager so makeFromData works.
  return SkFontMgr_New_Custom_Empty();
#else
  return SkFontMgr_New_FontConfig(nullptr);
#endif
}

// FreeType from memory on Linux/WASM (same rasteriser Chrome uses on Linux,
// which keeps reference renders close); the platform manager elsewhere.
static sk_sp<SkFontMgr> makeDataFontMgr(const sk_sp<SkFontMgr>& system) {
#if defined(__APPLE__) || defined(_WIN32)
  return system;
#else
  (void)system;
  return SkFontMgr_New_Custom_Empty();
#endif
}

FontManager::FontManager() : impl_(std::make_unique<Impl>()) {
  impl_->system = makeSystemFontMgr();
  impl_->data = makeDataFontMgr(impl_->system);
  impl_->provider = sk_make_sp<skia::textlayout::TypefaceFontProvider>();
  impl_->collection = sk_make_sp<skia::textlayout::FontCollection>();
  impl_->collection->setAssetFontManager(impl_->provider);
  if (impl_->system) impl_->collection->setDefaultFontManager(impl_->system, defaultFamily.c_str());
  impl_->collection->enableFontFallback();
  // Embedded data is static: wrap it without copying.
  for (size_t i = 0; i < kEmbeddedFontCount; i++) {
    const EmbeddedFont& f = kEmbeddedFonts[i];
    sk_sp<SkTypeface> tf = impl_->data ? impl_->data->makeFromData(SkData::MakeWithoutCopy(f.data, f.size)) : nullptr;
    if (tf) impl_->provider->registerTypeface(tf, SkString(f.family));
  }
}

FontManager::~FontManager() = default;

bool FontManager::registerFont(const std::string& family, const void* data, size_t size, int, bool) {
  if (!data || !size || !impl_->data) return false;
  sk_sp<SkTypeface> tf = impl_->data->makeFromData(SkData::MakeWithCopy(data, size));
  if (!tf) return false;
  impl_->provider->registerTypeface(tf, SkString(family.c_str()));
  impl_->collection->clearCaches();
  generation++;
  return true;
}

bool FontManager::hasFamily(const std::string& family) const {
  sk_sp<SkFontStyleSet> set = impl_->provider->matchFamily(family.c_str());
  return set && set->count() > 0;
}

SkUnicode* FontManager::unicode() {
  if (!impl_->unicode) impl_->unicode = SkUnicode::MakeIcuBasedUnicode();
  return impl_->unicode.get();
}

sk_sp<skia::textlayout::FontCollection> FontManager::collection() { return impl_->collection; }
sk_sp<SkFontMgr> FontManager::fontMgr() { return impl_->system; }

}  // namespace jsui
