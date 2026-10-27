#include "render/host_textures.h"

#include <algorithm>
#include <cstring>

#include "core/document.h"
#include "core/node.h"
#include "include/core/SkData.h"
#include "include/core/SkImageGenerator.h"
#include "include/core/SkImageInfo.h"

namespace jsui {

namespace {

thread_local TextureWrapper* tWrapper = nullptr;
thread_local DeferredTextures* tRecording = nullptr;
thread_local const DeferredTextures* tReplay = nullptr;

// A recorded frame's stand-in for a GPU texture: its size, no pixels.
class PlaceholderGenerator final : public SkImageGenerator {
 public:
  explicit PlaceholderGenerator(const SkImageInfo& info) : SkImageGenerator(info) {}
};

sk_sp<SkImage> placeholderFor(HostTextures::Entry& e) {
  if (!e.placeholder || e.placeholderVersion != e.contentVersion) {
    SkImageInfo info = SkImageInfo::Make(e.desc.width, e.desc.height, kRGBA_8888_SkColorType, textureAlphaType(e.desc));
    e.placeholder = SkImages::DeferredFromGenerator(std::make_unique<PlaceholderGenerator>(info));
    e.placeholderVersion = e.contentVersion;
  }
  return e.placeholder;
}

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
HostTextures::~HostTextures() {
  inherit(nullptr);
  for (HostTextures* c : children_) c->parent_ = nullptr;
}

void HostTextures::markDirty(const std::string& key) {
  if (dirtySet_.insert(key).second) dirty_.push_back(key);
  for (HostTextures* c : children_) c->parentChanged(key);
}

void HostTextures::inherit(HostTextures* parent) {
  if (parent_) {
    auto& kids = parent_->children_;
    kids.erase(std::remove(kids.begin(), kids.end(), this), kids.end());
    // Users registered with the parent for inherited keys.
    for (auto& [key, nodes] : users_) {
      std::string pk = parentKey(key);
      if (!pk.empty())
        for (Node* n : nodes) parent_->removeUser(pk, n);
    }
  }
  parent_ = parent;
  if (parent_) {
    parent_->children_.push_back(this);
    for (auto& [key, nodes] : users_) {
      std::string pk = parentKey(key);
      if (!pk.empty())
        for (Node* n : nodes) parent_->addUser(pk, n);
    }
  }
}

std::string HostTextures::parentKey(const std::string& key) const {
  if (!parent_) return "";
  auto it = aliases_.find(key);
  if (it != aliases_.end()) return it->second;
  return inheritAll ? key : "";
}

void HostTextures::alias(const std::string& key, const std::string& parentKeyName) {
  HostTextures* p = parent_;
  // Views showing `key` move to the new parent key.
  auto users = users_.find(key);
  std::string before = parentKey(key);
  if (parentKeyName.empty()) aliases_.erase(key);
  else aliases_[key] = parentKeyName;
  std::string after = parentKey(key);
  if (p && users != users_.end() && before != after)
    for (Node* n : users->second) {
      if (!before.empty()) p->removeUser(before, n);
      if (!after.empty()) p->addUser(after, n);
    }
  if (before != after && !entries_.count(key)) {
    markDirty(key);
    touch(key, true);
  }
}

void HostTextures::parentChanged(const std::string& pk) {
  auto relay = [&](const std::string& key) {
    if (!entries_.count(key) && parentKey(key) == pk) markDirty(key);
  };
  for (auto& [key, target] : aliases_)
    if (target == pk) relay(key);
  if (inheritAll && !aliases_.count(pk)) relay(pk);
}

int HostTextures::set(const std::string& key, const JsuiTexture* desc) {
  if (!desc) return remove(key) ? 1 : 0;
  if (!desc->handle || desc->width <= 0 || desc->height <= 0 || desc->kind <= JSUI_TEXTURE_PIXELS ||
      desc->kind > JSUI_TEXTURE_D3D12)
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
  setSize(key, &e);
  markDirty(key);
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
    setSize(key, &e);
    markDirty(key);
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
  setSize(key, nullptr);
  markDirty(key);
  touch(key, true);
  return true;
}

const HostTextures::Entry* HostTextures::find(const std::string& key) const {
  auto it = entries_.find(key);
  if (it != entries_.end()) return &it->second;
  std::string pk = parentKey(key);
  return pk.empty() ? nullptr : parent_->find(pk);
}

uint64_t HostTextures::version(const std::string& key) const {
  const Entry* e = find(key);
  return e ? e->descVersion : 0;
}

void HostTextures::setSize(const std::string& key, const Entry* e) {
  std::lock_guard<std::mutex> lock(sizesMutex_);
  if (e) sizes_[key] = Size{e->desc.width, e->desc.height, e->descVersion};
  else sizes_.erase(key);
}

bool HostTextures::size(const std::string& key, int* width, int* height, uint64_t* version) const {
  std::lock_guard<std::mutex> lock(sizesMutex_);
  auto it = sizes_.find(key);
  if (it == sizes_.end()) {
    std::string pk = parentKey(key);
    return !pk.empty() && parent_->size(pk, width, height, version);
  }
  *width = it->second.width;
  *height = it->second.height;
  *version = it->second.version;
  return true;
}

std::vector<std::string> HostTextures::keys() const {
  std::lock_guard<std::mutex> lock(sizesMutex_);
  std::vector<std::string> out;
  out.reserve(sizes_.size());
  for (auto& [k, e] : sizes_) out.push_back(k);
  if (parent_) {
    for (auto& [k, pk] : aliases_)
      if (!sizes_.count(k)) {
        int w, h;
        uint64_t v;
        if (parent_->size(pk, &w, &h, &v)) out.push_back(k);
      }
    if (inheritAll)
      for (const std::string& k : parent_->keys())
        if (!sizes_.count(k) && !aliases_.count(k)) out.push_back(k);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<std::string> HostTextures::takeDirty() {
  std::vector<std::string> out;
  out.swap(dirty_);
  dirtySet_.clear();
  return out;
}

void HostTextures::addUser(const std::string& key, Node* node) {
  users_[key].insert(node);
  std::string pk = parentKey(key);
  if (!pk.empty()) parent_->addUser(pk, node);
}

void HostTextures::removeUser(const std::string& key, Node* node) {
  std::string pk = parentKey(key);
  if (!pk.empty()) parent_->removeUser(pk, node);
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
  if (it == entries_.end()) {
    std::string pk = parentKey(key);
    return pk.empty() ? nullptr : parent_->image(pk);
  }
  Entry& e = it->second;
  if (e.pixels) return e.pixels;
  if (tRecording) {
    sk_sp<SkImage> p = placeholderFor(e);
    if (!p) return nullptr;
    auto& items = tRecording->items;
    if (std::none_of(items.begin(), items.end(), [&](const DeferredTextures::Item& i) { return i.imageId == p->uniqueID(); })) {
      DeferredTextures::Item item;
      item.imageId = p->uniqueID();
      item.entry = e;
      item.entry.placeholder = nullptr;  // the frame holds it already
      items.push_back(std::move(item));
    }
    return p;
  }
  sk_sp<SkImage> img = tWrapper ? tWrapper->wrap(e) : nullptr;
  if (!img && !e.warned) {
    e.warned = true;
    static const char* const kKinds[] = {"pixels", "GL", "WebGPU", "Vulkan", "Metal", "D3D12"};
    static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == JSUI_TEXTURE_D3D12 + 1, "a name per JsuiTextureKind");
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
  // Only wraps the frame just drawn used are kept: the host may free a
  // texture once it is replaced or removed, and a wrap (a Vulkan image view,
  // a retained MTLTexture) must not outlive it by more than the frame that
  // drew it. Rewrapping a texture that is repainted later is cheap.
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.lastUsed + 1 < frame_) it = cache_.erase(it);
    else ++it;
  }
}

DeferredTextures::Recording::Recording(DeferredTextures* into) : prev_(tRecording) { tRecording = into; }
DeferredTextures::Recording::~Recording() { tRecording = prev_; }
DeferredTextures::Replay::Replay(const DeferredTextures* from) : prev_(tReplay) { tReplay = from; }
DeferredTextures::Replay::~Replay() { tReplay = prev_; }

sk_sp<SkImage> resolveDeferredTexture(const SkImage* image) {
  if (!tReplay || !image || tReplay->items.empty()) return nullptr;
  for (const DeferredTextures::Item& i : tReplay->items)
    if (i.imageId == image->uniqueID()) return tWrapper ? tWrapper->wrap(i.entry) : nullptr;
  return nullptr;
}

TextureScope::TextureScope(TextureWrapper* wrapper) : prev_(tWrapper) { tWrapper = wrapper; }
TextureScope::~TextureScope() { tWrapper = prev_; }

}  // namespace jsui
