// Shared host state: the latest JSON value per key, owned by the Runtime so
// it outlives JS contexts (dev reloads, jsui_reload) and exists before a
// bundle is loaded. See docs/ARCHITECTURE.md "Game data".
//
// Every change bumps a runtime-wide version counter, so a key's version is
// unique for its lifetime and JS can cache its parsed value by version (a
// key that was never set or was removed has version 0).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/json.h"

namespace jsui {

class HostState {
 public:
  enum class Result { Unchanged, Changed, Invalid };

  // Replaces the value of `key` with the JSON text `json`. Identical text, or
  // text that parses to the same value, changes nothing.
  // `fromUi`: a change made by JS, which notifies its own subscribers, so the
  // key is not queued for delivery to JS (see takeDirty).
  Result set(const std::string& key, std::string_view json, std::string* error = nullptr, bool fromUi = false);
  // RFC 7386 merge patch of the value of `key` (a missing key starts as null,
  // so an object patch creates an object).
  Result patch(const std::string& key, std::string_view json, std::string* error = nullptr, bool fromUi = false);
  // Removes the key. Returns true if it existed.
  bool remove(const std::string& key, bool fromUi = false);

  // 0 when the key does not exist.
  uint64_t version(const std::string& key) const;
  // Current JSON text of `key` (serialized lazily after patches), or nullptr.
  const std::string* json(const std::string& key);
  std::vector<std::string> keys() const;

  // The merge patches that took `key` from version `since` to its current
  // version, oldest first, or false when they are not all known (the key
  // was set since, or the log was trimmed): a reader holding the value at
  // `since` applies them instead of re-reading the whole document.
  bool patchesSince(const std::string& key, uint64_t since, std::vector<const std::string*>* out) const;

  // Keys changed by the embedder since the last call (deduplicated, in
  // order of first change). The runtime hands them to JS once per tick.
  std::vector<std::string> takeDirty();
  bool hasDirty() const { return !dirty_.empty(); }

 private:
  struct Patch {
    uint64_t from = 0, to = 0;  // versions before / after
    std::string json;
  };
  // Recent patches of a key (bounded: kMaxPatches, kMaxPatchBytes).
  static constexpr size_t kMaxPatches = 64, kMaxPatchBytes = 64 * 1024;
  struct Entry {
    Json value;
    std::string json;
    bool jsonValid = false;
    uint64_t version = 0;
    std::vector<Patch> patches;
    size_t patchBytes = 0;
  };
  void markDirty(const std::string& key);
  std::unordered_map<std::string, Entry> entries_;
  std::vector<std::string> dirty_;
  std::unordered_set<std::string> dirtySet_;
  uint64_t nextVersion_ = 1;
};

}  // namespace jsui
