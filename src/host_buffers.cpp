#include "host_buffers.h"

#include <algorithm>
#include <cstring>

namespace jsui {

size_t HostBuffers::elementSize(JsuiBufferType type) {
  switch (type) {
    case JSUI_BUFFER_F64: return 8;
    case JSUI_BUFFER_F32:
    case JSUI_BUFFER_I32:
    case JSUI_BUFFER_U32: return 4;
    case JSUI_BUFFER_I16:
    case JSUI_BUFFER_U16: return 2;
    case JSUI_BUFFER_I8:
    case JSUI_BUFFER_U8: return 1;
  }
  return 1;
}

double HostBuffers::Entry::at(size_t i) const {
  if (i >= count) return 0;
  const uint8_t* p = data();
  switch (type) {
    case JSUI_BUFFER_F32: return double(reinterpret_cast<const float*>(p)[i]);
    case JSUI_BUFFER_F64: return reinterpret_cast<const double*>(p)[i];
    case JSUI_BUFFER_I32: return double(reinterpret_cast<const int32_t*>(p)[i]);
    case JSUI_BUFFER_U32: return double(reinterpret_cast<const uint32_t*>(p)[i]);
    case JSUI_BUFFER_I16: return double(reinterpret_cast<const int16_t*>(p)[i]);
    case JSUI_BUFFER_U16: return double(reinterpret_cast<const uint16_t*>(p)[i]);
    case JSUI_BUFFER_I8: return double(reinterpret_cast<const int8_t*>(p)[i]);
    case JSUI_BUFFER_U8: return double(p[i]);
  }
  return 0;
}

HostBuffers::Entry& HostBuffers::prepare(const std::string& key, JsuiBufferType type, size_t count) {
  Entry& e = entries_[key];
  size_t bytes = count * elementSize(type);
  if (bytes > e.capacity || !e.storage) {
    // Grow geometrically: a list that gains an element per frame does not
    // reallocate (and detach the JS views) every frame.
    size_t cap = std::max<size_t>({bytes, e.capacity + e.capacity / 2, 64});
    cap = (cap + 7) & ~size_t(7);
    std::unique_ptr<double[]> next(new double[cap / 8]());
    if (e.storage) {
      std::memcpy(next.get(), e.storage.get(), std::min(e.bytes(), bytes));
      if (onRelease) onRelease(key);
    }
    e.storage = std::move(next);
    e.capacity = cap;
  } else if (bytes > e.bytes()) {
    std::memset(e.data() + e.bytes(), 0, bytes - e.bytes());
  }
  e.type = type;
  e.count = count;
  return e;
}

int HostBuffers::set(const std::string& key, JsuiBufferType type, const void* data, size_t count) {
  if (!validType(type)) return -1;
  size_t bytes = count * elementSize(type);
  if (!data) bytes = count = 0;
  auto it = entries_.find(key);
  if (it != entries_.end() && it->second.version && it->second.type == type && it->second.count == count &&
      (bytes == 0 || std::memcmp(it->second.data(), data, bytes) == 0))
    return 0;
  Entry& e = prepare(key, type, count);
  if (bytes) std::memcpy(e.data(), data, bytes);
  e.version = nextVersion_++;
  changed(key);
  return 1;
}

void* HostBuffers::map(const std::string& key, JsuiBufferType type, size_t count) {
  if (!validType(type)) return nullptr;
  return prepare(key, type, count).data();
}

bool HostBuffers::commit(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  it->second.version = nextVersion_++;
  changed(key);
  return true;
}

bool HostBuffers::remove(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  bool published = it->second.version != 0;
  if (onRelease) onRelease(key);
  entries_.erase(it);
  generation_++;
  if (published) changed(key);
  return true;
}

const HostBuffers::Entry* HostBuffers::find(const std::string& key) const {
  auto it = entries_.find(key);
  // Mapped but never committed: not visible yet.
  return it == entries_.end() || !it->second.version ? nullptr : &it->second;
}

uint64_t HostBuffers::version(const std::string& key) const {
  const Entry* e = find(key);
  return e ? e->version : 0;
}

std::vector<std::string> HostBuffers::keys() const {
  std::vector<std::string> out;
  for (auto& [k, e] : entries_)
    if (e.version) out.push_back(k);
  return out;
}

void HostBuffers::changed(const std::string& key) {
  if (dirtySet_.insert(key).second) dirty_.push_back(key);
  if (onChange) onChange(key);
}

std::vector<std::string> HostBuffers::takeDirty() {
  std::vector<std::string> out;
  out.swap(dirty_);
  dirtySet_.clear();
  return out;
}

}  // namespace jsui
