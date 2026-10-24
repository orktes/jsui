#include "host_state.h"

namespace jsui {

HostState::Result HostState::set(const std::string& key, std::string_view json, std::string* error, bool fromUi) {
  auto it = entries_.find(key);
  if (it != entries_.end() && it->second.jsonValid && it->second.json == json) return Result::Unchanged;
  Json value;
  if (!Json::parse(json, &value, error)) return Result::Invalid;
  if (it == entries_.end()) {
    it = entries_.emplace(key, Entry{}).first;
  } else if (it->second.value == value) {
    return Result::Unchanged;  // same value spelled differently (whitespace)
  }
  Entry& e = it->second;
  e.value = std::move(value);
  e.patches.clear();
  e.patchBytes = 0;
  e.json.assign(json);
  e.jsonValid = true;
  e.version = nextVersion_++;
  if (!fromUi) markDirty(key);
  if (onChange) onChange(key);
  return Result::Changed;
}

HostState::Result HostState::patch(const std::string& key, std::string_view json, std::string* error, bool fromUi) {
  Json p;
  if (!Json::parse(json, &p, error)) return Result::Invalid;
  // A whole-document null patch yields null: treated as removing the key.
  if (p.isNull()) return remove(key, fromUi) ? Result::Changed : Result::Unchanged;
  auto it = entries_.find(key);
  bool fresh = it == entries_.end();
  if (fresh) it = entries_.emplace(key, Entry{}).first;
  Entry& e = it->second;
  if (!e.value.mergePatch(p) && !fresh) return Result::Unchanged;
  e.jsonValid = false;
  e.json.clear();
  uint64_t from = e.version;
  e.version = nextVersion_++;
  if (!fresh && json.size() <= kMaxPatchBytes) {
    e.patches.push_back({from, e.version, std::string(json)});
    e.patchBytes += json.size();
    size_t drop = 0;
    while (e.patches.size() - drop > kMaxPatches || e.patchBytes > kMaxPatchBytes)
      e.patchBytes -= e.patches[drop++].json.size();
    e.patches.erase(e.patches.begin(), e.patches.begin() + long(drop));
  } else {
    e.patches.clear();
    e.patchBytes = 0;
  }
  if (!fromUi) markDirty(key);
  if (onChange) onChange(key);
  return Result::Changed;
}

const Json* HostState::value(const std::string& key) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second.value;
}

bool HostState::remove(const std::string& key, bool fromUi) {
  if (!entries_.erase(key)) return false;
  if (!fromUi) markDirty(key);
  if (onChange) onChange(key);
  return true;
}

bool HostState::patchesSince(const std::string& key, uint64_t since, std::vector<const std::string*>* out) const {
  out->clear();
  auto it = entries_.find(key);
  if (it == entries_.end() || since == 0) return false;
  const Entry& e = it->second;
  if (since == e.version) return true;
  // The log is a chain (each patch starts where the previous ended).
  for (size_t i = 0; i < e.patches.size(); i++) {
    if (e.patches[i].from != since) continue;
    for (size_t j = i; j < e.patches.size(); j++) out->push_back(&e.patches[j].json);
    return true;
  }
  return false;
}

uint64_t HostState::version(const std::string& key) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? 0 : it->second.version;
}

const std::string* HostState::json(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return nullptr;
  Entry& e = it->second;
  if (!e.jsonValid) {
    e.json.clear();
    e.value.dump(e.json);
    e.jsonValid = true;
  }
  return &e.json;
}

std::vector<std::string> HostState::keys() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (auto& [k, e] : entries_) out.push_back(k);
  return out;
}

void HostState::markDirty(const std::string& key) {
  if (dirtySet_.insert(key).second) dirty_.push_back(key);
}

std::vector<std::string> HostState::takeDirty() {
  std::vector<std::string> out;
  out.swap(dirty_);
  dirtySet_.clear();
  return out;
}

}  // namespace jsui
