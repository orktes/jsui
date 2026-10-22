#include "text/font_manager.h"

#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__EMSCRIPTEN__)
#include "include/ports/SkFontMgr_empty.h"
#else
#include "include/ports/SkFontMgr_fontconfig.h"
#endif

namespace jsui {

struct FontManager::Impl {
  sk_sp<SkFontMgr> system;
  sk_sp<skia::textlayout::TypefaceFontProvider> provider;
  sk_sp<skia::textlayout::FontCollection> collection;
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

FontManager::FontManager() : impl_(std::make_unique<Impl>()) {
  impl_->system = makeSystemFontMgr();
  impl_->provider = sk_make_sp<skia::textlayout::TypefaceFontProvider>();
  impl_->collection = sk_make_sp<skia::textlayout::FontCollection>();
  impl_->collection->setAssetFontManager(impl_->provider);
  if (impl_->system) impl_->collection->setDefaultFontManager(impl_->system);
  impl_->collection->enableFontFallback();
}

FontManager::~FontManager() = default;

bool FontManager::registerFont(const std::string& family, const void* data, size_t size, int, bool) {
  auto mgr = impl_->system ? impl_->system : SkFontMgr::RefEmpty();
  sk_sp<SkTypeface> tf = mgr->makeFromData(SkData::MakeWithCopy(data, size));
  if (!tf) return false;
  impl_->provider->registerTypeface(tf, SkString(family.c_str()));
  impl_->collection->clearCaches();
  return true;
}

sk_sp<skia::textlayout::FontCollection> FontManager::collection() { return impl_->collection; }
sk_sp<SkFontMgr> FontManager::fontMgr() { return impl_->system; }

}  // namespace jsui
