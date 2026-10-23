#include "render/host_textures.h"

#include <algorithm>
#include <cstring>

#include "core/document.h"
#include "core/node.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

namespace jsui {

namespace {

thread_local TextureWrapper* tWrapper = nullptr;

bool sameDesc(const JsuiTexture& a, const JsuiTexture& b) {
  return a.kind == b.kind && a.handle == b.handle && a.format == b.format && a.width == b.width &&
         a.height == b.height && a.flags == b.flags && a.usage == b.usage;
}

}  // namespace

SkAlphaType textureAlphaType(const JsuiTexture& t) {
  if (t.flags & JSUI_TEXTURE_OPAQUE) return kOpaque_SkAlphaType;
  return (t.flags & JSUI_TEXTURE_UNPREMULTIPLIED) ? kUnpremul_SkAlphaType : kPremul_SkAlphaType;
}

HostTextures::HostTextures() = default;
HostTextures::~HostTextures() = default;

int HostTextures::set(const std::string& key, const JsuiTexture* desc) {
  if (!desc) return remove(key) ? 1 : 0;
  if (!desc->handle || desc->width <= 0 || desc->height <= 0 || desc->kind <= JSUI_TEXTURE_PIXELS ||
      desc->kind > JSUI_TEXTURE_METAL)
    return -1;
  auto it = entries_.find(key);
  if (it != entries_.end() && !it->second.pixels && sameDesc(it->second.desc, *desc)) return 0;
  bool resized = it == entries_.end() || it->second.desc.width != desc->width || it->second.desc.height != desc->height;
  Entry& e = entries_[key];
  if (!e.id) e.id = nextId_++;
  e.desc = *desc;
  e.pixels.reset();
  e.descVersion++;
  e.contentVersion++;
  e.warned = false;
  if (dirtySet_.insert(key).second) dirty_.push_back(key);
  touch(key, resized);
  return 1;
}

int HostTextures::setPixels(const std::string& key, const void* rgba, int width, int height, size_t rowBytes,
                            int flags) {
  if (!rgba || width <= 0 || height <= 0) return -1;
  if (!rowBytes) rowBytes = size_t(width) * 4;
  if (rowBytes < size_t(width) * 4) return -1;
  JsuiTexture d{};
  d.kind = JSUI_TEXTURE_PIXELS;
  d.width = width;
  d.height = height;
  d.flags = flags & (JSUI_TEXTURE_UNPREMULTIPLIED | JSUI_TEXTURE_OPAQUE | JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN);
  SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, textureAlphaType(d));
  // Copied tightly (and flipped for a bottom-left origin): the image owns
  // its pixels, so the host may reuse its buffer right away.
  const size_t tight = size_t(width) * 4;
  sk_sp<SkData> data = SkData::MakeUninitialized(tight * size_t(height));
  auto* dst = static_cast<uint8_t*>(data->writable_data());
  const auto* src = static_cast<const uint8_t*>(rgba);
  const bool flip = d.flags & JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN;
  for (int y = 0; y < height; y++)
    std::memcpy(dst + size_t(flip ? height - 1 - y : y) * tight, src + size_t(y) * rowBytes, tight);
  sk_sp<SkImage> image = SkImages::RasterFromData(info, std::move(data), tight);
  if (!image) return -1;

  auto it = entries_.find(key);
  bool descChanged = it == entries_.end() || !it->second.pixels || it->second.desc.width != width ||
                     it->second.desc.height != height || it->second.desc.flags != d.flags;
  bool resized = it == entries_.end() || it->second.desc.width != width || it->second.desc.height != height;
  Entry& e = entries_[key];
  if (!e.id) e.id = nextId_++;
  e.desc = d;
  e.pixels = std::move(image);
  e.contentVersion++;
  if (descChanged) {
    e.descVersion++;
    e.warned = false;
    if (dirtySet_.insert(key).second) dirty_.push_back(key);
  }
  touch(key, resized);
  return 1;
}

bool HostTextures::updated(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  it->second.contentVersion++;
  touch(key, false);
  return true;
}

bool HostTextures::remove(const std::string& key) {
  if (!entries_.erase(key)) return false;
  if (dirtySet_.insert(key).second) dirty_.push_back(key);
  touch(key, true);
  return true;
}

const HostTextures::Entry* HostTextures::find(const std::string& key) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second;
}

uint64_t HostTextures::version(const std::string& key) const {
  const Entry* e = find(key);
  return e ? e->descVersion : 0;
}

std::vector<std::string> HostTextures::keys() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (auto& [k, e] : entries_) out.push_back(k);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> HostTextures::takeDirty() {
  std::vector<std::string> out;
  out.swap(dirty_);
  dirtySet_.clear();
  return out;
}

void HostTextures::addUser(const std::string& key, Node* node) { users_[key].insert(node); }

void HostTextures::removeUser(const std::string& key, Node* node) {
  auto it = users_.find(key);
  if (it == users_.end()) return;
  it->second.erase(node);
  if (it->second.empty()) users_.erase(it);
}

void HostTextures::touch(const std::string& key, bool layout) {
  auto it = users_.find(key);
  if (it == users_.end()) return;
  for (Node* n : it->second) {
    n->markPaintDirty();
    // Views sized by the texture re-measure.
    if (layout && n->yoga() && YGNodeHasMeasureFunc(n->yoga())) {
      YGNodeMarkDirty(n->yoga());
      if (n->document()) n->document()->markLayoutDirty();
    }
  }
}

sk_sp<SkImage> HostTextures::image(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return nullptr;
  Entry& e = it->second;
  if (e.pixels) return e.pixels;
  sk_sp<SkImage> img = tWrapper ? tWrapper->wrap(e) : nullptr;
  if (!img && !e.warned) {
    e.warned = true;
    static const char* const kKinds[] = {"pixels", "GL", "WebGPU", "Vulkan", "Metal"};
    if (log)
      log(JSUI_LOG_WARN, "host texture \"" + key + "\" (" + kKinds[e.desc.kind] +
                             (tWrapper ? ", format " + std::to_string(e.desc.format) + ") cannot be drawn by this GPU context"
                                       : ") cannot be drawn by the CPU renderer: use jsui_set_texture_pixels"));
  }
  return img;
}

sk_sp<SkImage> TextureWrapper::wrap(const HostTextures::Entry& e) {
  Cached& c = cache_[e.id];
  c.lastUsed = frame_;
  if (c.image && c.contentVersion == e.contentVersion) return c.image;
  c.contentVersion = e.contentVersion;
  c.image = make_(e.desc);
  return c.image;
}

void TextureWrapper::endFrame() {
  frame_++;
  constexpr uint64_t kMaxAge = 120;
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (frame_ - it->second.lastUsed > kMaxAge) it = cache_.erase(it);
    else ++it;
  }
}

TextureScope::TextureScope(TextureWrapper* wrapper) : prev_(tWrapper) { tWrapper = wrapper; }
TextureScope::~TextureScope() { tWrapper = prev_; }

}  // namespace jsui
