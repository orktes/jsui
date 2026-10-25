// Host buffers: named typed arrays of numbers the game pushes every frame
// (enemy positions for nameplates and markers, minimap blips, ...) without
// JSON. Owned by the Runtime like HostState, so they outlive JS contexts.
// See docs/ARCHITECTURE.md "Frame data".
//
// JS reads a buffer through an ArrayBuffer over the runtime's storage (no
// copy, see JsEngine::bufferView); native Animated nodes read single
// elements of it (Animated.hostBuffer), so a view can follow the data
// without any JS running per frame.
//
// Storage only moves when a buffer grows past its capacity or is removed;
// `onRelease` then runs first, so JS views over the old memory are detached
// before it is freed.
//
// JS on its own thread cannot view the storage the owner thread rewrites.
// It reads published copies instead (copy-on-publish): a key JS asked for
// (subscribe) is copied by publish() on the owner thread, once per tick and
// only when its version changed; the copies and the version table are
// shared under a lock. Native Animated hostBuffer nodes keep reading the
// live storage on the owner thread (no lag).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jsui/jsui.h"

namespace jsui {

class HostBuffers {
 public:
  struct Entry {
    JsuiBufferType type = JSUI_BUFFER_F32;
    size_t count = 0;     // elements
    size_t capacity = 0;  // bytes allocated
    std::unique_ptr<double[]> storage;  // 8-byte aligned for every element type
    uint64_t version = 0;
    uint8_t* data() const { return reinterpret_cast<uint8_t*>(storage.get()); }
    size_t bytes() const { return count * elementSize(type); }
    // Element `i` as a double (0 when out of range).
    double at(size_t i) const;
  };

  static size_t elementSize(JsuiBufferType type);
  static bool validType(int type) { return type >= JSUI_BUFFER_F32 && type <= JSUI_BUFFER_U8; }

  // Copies `count` elements into `key`. Returns 1 when the contents (or the
  // type or length) changed, 0 when they are the same as before, -1 for an
  // invalid type.
  int set(const std::string& key, JsuiBufferType type, const void* data, size_t count);
  // Storage of `count` elements of `type` for the caller to fill, then
  // commit(). Earlier contents are kept (up to the new length; new elements
  // are 0). nullptr for an invalid type.
  void* map(const std::string& key, JsuiBufferType type, size_t count);
  // Publishes the contents written through map() (bumps the version).
  // Returns false for an unknown key.
  bool commit(const std::string& key);
  bool remove(const std::string& key);

  const Entry* find(const std::string& key) const;
  // 0 when the key does not exist.
  uint64_t version(const std::string& key) const;
  std::vector<std::string> keys() const;

  // Keys changed since the last call (deduplicated, in order of first change).
  std::vector<std::string> takeDirty();
  bool hasDirty() const { return !dirty_.empty(); }

  // Bumped whenever an entry is removed (cached Entry pointers are stale).
  uint64_t generation() const { return generation_; }

  // ---- published copies (JS on its own thread; any thread unless noted)
  struct Copy {
    JsuiBufferType type = JSUI_BUFFER_F32;
    size_t count = 0;
    uint64_t version = 0;
    std::vector<uint8_t> bytes;  // count elements
  };
  // JS reads `key`: publish() copies it from now on.
  void subscribe(const std::string& key);
  // Owner thread: copies the subscribed buffers whose contents changed since
  // their last copy (a copy no reader has taken is rewritten in place; one
  // handed out never changes) and records every key's version for
  // publishedVersion / publishedKeys.
  void publish();
  // The last copy of `key` (nullptr: not subscribed / published, or unset).
  // The copy stays valid while the pointer is held.
  std::shared_ptr<const Copy> published(const std::string& key) const;
  // The version of `key` as of the last publish() (0: unset).
  uint64_t publishedVersion(const std::string& key) const;
  std::vector<std::string> publishedKeys() const;

  // The storage of `key` is about to be freed (grown or removed).
  std::function<void(const std::string& key)> onRelease;
  // `key` has new contents (set / commit / remove).
  std::function<void(const std::string& key)> onChange;

 private:
  Entry& prepare(const std::string& key, JsuiBufferType type, size_t count);
  void changed(const std::string& key);
  std::unordered_map<std::string, Entry> entries_;
  std::vector<std::string> dirty_;
  std::unordered_set<std::string> dirtySet_;
  uint64_t nextVersion_ = 1;
  uint64_t generation_ = 1;
  // Published copies and versions (copiesMutex_).
  mutable std::mutex copiesMutex_;
  std::unordered_set<std::string> subscribed_;
  std::unordered_map<std::string, std::shared_ptr<Copy>> copies_;
  // Copies published() handed out: never rewritten (a reader dropping its
  // reference does not order its reads before the owner's next write).
  mutable std::unordered_set<const Copy*> handedOut_;
  std::unordered_map<std::string, uint64_t> versions_;
};

}  // namespace jsui
