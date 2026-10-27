// Host textures (jsui_set_texture / jsui_set_texture_pixels, HostTexture in
// JS): pictures the host makes itself (decoded video, a render target) shown
// inside the UI. See jsui.h "Host textures" and docs/ARCHITECTURE.md.
//
// The registry belongs to the Runtime and outlives JS contexts and surfaces.
// CPU pixels become a raster SkImage that every canvas can draw (GPU
// backends upload it like any image). A GPU texture is only a description:
// the backend rendering a frame wraps it as an SkImage of its own device
// through a TextureWrapper it makes current for the frame (TextureScope),
// so the painter stays backend independent. A canvas without a wrapper (the
// CPU raster, another API) draws nothing for it.
//
// Image nodes showing a texture register here (image::NodeCache), so new
// contents repaint just those nodes and a new size re-lays them out.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "jsui/jsui.h"

namespace jsui {

class Node;

class HostTextures {
 public:
  struct Entry {
    JsuiTexture desc{};       // kind JSUI_TEXTURE_PIXELS for CPU pixels
    sk_sp<SkImage> pixels;    // CPU pixels as a raster image
    uint64_t id = 0;          // unique per entry (wrapper cache key)
    uint64_t descVersion = 0;     // bumped when handle / size / format / flags change
    uint64_t contentVersion = 0;  // bumped on every new contents (and desc change)
    bool warned = false;          // "cannot draw this texture here" was logged
    // Recorded frames (DeferredTextures): the placeholder drawn for these
    // contents (placeholderVersion: their contentVersion).
    sk_sp<SkImage> placeholder;
    uint64_t placeholderVersion = 0;
  };

  HostTextures();
  ~HostTextures();
  HostTextures(const HostTextures&) = delete;
  HostTextures& operator=(const HostTextures&) = delete;

  // jsui_set_texture (desc nullptr removes). 1 changed, 0 same, -1 invalid.
  int set(const std::string& key, const JsuiTexture* desc);
  // jsui_set_texture_pixels. 1, or -1 for a bad size.
  int setPixels(const std::string& key, const void* rgba, int width, int height, size_t rowBytes, int flags);
  // jsui_texture_updated. False for an unknown key.
  bool updated(const std::string& key);
  bool remove(const std::string& key);
  // Owner thread.
  const Entry* find(const std::string& key) const;
  // 0: unset; changes whenever the texture's size or handle does.
  uint64_t version(const std::string& key) const;
  // Any thread (JS reads them on its own thread): the size table.
  bool size(const std::string& key, int* width, int* height, uint64_t* version) const;
  std::vector<std::string> keys() const;

  // Keys whose description changed since the last take (JS: useHostTexture).
  bool hasDirty() const { return !dirty_.empty(); }
  std::vector<std::string> takeDirty();

  // Image nodes showing `key` (image::NodeCache registers / unregisters).
  void addUser(const std::string& key, Node* node);
  void removeUser(const std::string& key, Node* node);

  // ---- inheritance (a JSUIContainer's child runtime, render/container_view.h)
  // Keys this registry does not have are looked up in `parent` (nullptr:
  // none): under their alias (alias()), else, with `inheritAll`, under the
  // same key. Views showing an inherited texture repaint when the parent's
  // changes, and its size changes reach this registry's JS (takeDirty).
  void inherit(HostTextures* parent);
  // `key` here shows the parent's `parentKey` (empty: drops the alias).
  void alias(const std::string& key, const std::string& parentKey);
  // Unaliased keys are looked up in the parent too (false: only aliases).
  bool inheritAll = true;
  HostTextures* parent() const { return parent_; }

  // Logs (once per texture) through the runtime's logger.
  std::function<void(JsuiLogLevel, const std::string&)> log;

  // The image to draw for `key` on the current frame's canvas: the raster
  // image of CPU pixels, or the current TextureWrapper's wrap of a GPU
  // texture. nullptr when unset or not drawable here.
  sk_sp<SkImage> image(const std::string& key);

 private:
  // New contents (and, with `layout`, a new size): repaint / relayout users.
  void touch(const std::string& key, bool layout);

  // Records the size of `key` (nullptr: removed) in the size table.
  void setSize(const std::string& key, const Entry* e);

  std::unordered_map<std::string, Entry> entries_;
  // What JS may read of entries_ (sizesMutex_).
  struct Size {
    int width = 0, height = 0;
    uint64_t version = 0;
  };
  mutable std::mutex sizesMutex_;
  std::unordered_map<std::string, Size> sizes_;
  std::unordered_map<std::string, std::unordered_set<Node*>> users_;
  std::unordered_set<std::string> dirtySet_;
  std::vector<std::string> dirty_;
  uint64_t nextId_ = 1;

  // Inheritance: the parent key `key` stands for here ("" when none), and
  // the parent's key `parentKey` changed (size / handle / removed).
  std::string parentKey(const std::string& key) const;
  void parentChanged(const std::string& parentKey);
  void markDirty(const std::string& key);
  HostTextures* parent_ = nullptr;
  std::vector<HostTextures*> children_;
  std::unordered_map<std::string, std::string> aliases_;  // key -> the parent's key
};

// Host textures in a recorded frame (Surface::recordFrame, a frame painted
// on one thread and replayed into the GPU on another). While a
// DeferredTextures is being recorded into (Recording scope), HostTextures::
// image() returns, for a GPU texture, a placeholder: a lazy image of the
// texture's size with no pixels, noted with a copy of the texture's entry.
// While it is replayed (Replay scope, under the backend's TextureScope), the
// backend's image provider draws the TextureWrapper's wrap of that entry in
// the placeholder's place (resolveDeferredTexture).
struct DeferredTextures {
  struct Item {
    uint32_t imageId = 0;  // the placeholder's uniqueID
    HostTextures::Entry entry;
  };
  std::vector<Item> items;
  class Recording {
   public:
    explicit Recording(DeferredTextures* into);
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

   private:
    DeferredTextures* prev_;
  };
  class Replay {
   public:
    explicit Replay(const DeferredTextures* from);
    ~Replay();
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;

   private:
    const DeferredTextures* prev_;
  };
};
// While a frame with DeferredTextures is replayed: the wrap to draw for
// `image` when it is one of its placeholders, else nullptr.
sk_sp<SkImage> resolveDeferredTexture(const SkImage* image);

// A GPU backend's way to sample host textures in its frames. `make` wraps a
// description as an SkImage of the backend's device (nullptr: this backend
// cannot sample that kind / format). Wraps are kept while the texture's
// contents stay the same and only until the end of the next frame that
// does not use them (endFrame).
class TextureWrapper {
 public:
  using Make = std::function<sk_sp<SkImage>(const JsuiTexture&)>;
  explicit TextureWrapper(Make make) : make_(std::move(make)) {}
  sk_sp<SkImage> wrap(const HostTextures::Entry& e);
  // After every frame.
  void endFrame();
  void clear() { cache_.clear(); }

 private:
  struct Cached {
    uint64_t contentVersion = 0;
    sk_sp<SkImage> image;
    uint64_t lastUsed = 0;
  };
  Make make_;
  std::unordered_map<uint64_t, Cached> cache_;
  uint64_t frame_ = 0;
};

// Makes `wrapper` the one HostTextures::image() uses on this thread while in
// scope (nullptr: none, e.g. the CPU raster).
class TextureScope {
 public:
  explicit TextureScope(TextureWrapper* wrapper);
  ~TextureScope();
  TextureScope(const TextureScope&) = delete;
  TextureScope& operator=(const TextureScope&) = delete;

 private:
  TextureWrapper* prev_;
};

// Alpha type and colour type for a host texture (8-bit RGBA / BGRA).
SkAlphaType textureAlphaType(const JsuiTexture& t);

}  // namespace jsui
