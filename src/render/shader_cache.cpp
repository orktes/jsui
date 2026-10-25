#include "render/shader_cache.h"

#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

#include "include/core/SkString.h"
#include "render/pipeline_cache.h"

namespace jsui {
namespace shader_cache {

namespace {

constexpr char kMagic[8] = {'J', 'S', 'U', 'I', 'S', 'H', 'D', 'R'};
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxFileBytes = 64u << 20;  // stop appending past it
constexpr size_t kMaxPart = 16u << 20;       // a key or a shader

void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 255));
}
void put64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(char((v >> (8 * i)) & 255));
}
uint64_t get(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= uint64_t(p[i]) << (8 * i);
  return v;
}

std::string header(const std::string& backend) {
  std::string h(kMagic, sizeof(kMagic));
  put32(h, kVersion);
  const std::string id = pipeline_cache::buildId() + " " + backend;
  put32(h, uint32_t(id.size()));
  h += id;
  return h;
}

uint64_t recordHash(const SkData& key, const SkData& data) {
  uint64_t a = pipeline_cache::hash(key.data(), key.size());
  return a ^ (pipeline_cache::hash(data.data(), data.size()) * 31);
}

// Files in use by a Cache of this process: a second context reads the file
// but leaves the appending to the first.
std::mutex gFilesMutex;
std::set<std::string> gFiles;

}  // namespace

Cache::Cache(std::string backend) : backend_(std::move(backend)) {}

Cache::~Cache() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_) {
    std::fclose(file_);
    std::lock_guard<std::mutex> files(gFilesMutex);
    gFiles.erase(path_);
  }
}

void Cache::openDefaultLocked() {
  if (opened_) return;
  opened_ = true;
  const char* env = std::getenv("JSUI_PIPELINE_CACHE");
  if (env && (!std::strcmp(env, "off") || !std::strcmp(env, "0"))) return;
  if (pipeline_cache::noDefaultFile()) return;
  std::string path = pipeline_cache::cacheFile("shaders", backend_);
  if (!path.empty()) openLocked(path);
}

int Cache::setFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  opened_ = true;
  if (path == path_ && file_) return int(entries_.size());
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
    std::lock_guard<std::mutex> files(gFilesMutex);
    gFiles.erase(path_);
  }
  path_.clear();
  if (path.empty()) return 0;
  return openLocked(path);
}

int Cache::openLocked(const std::string& path) {
  const std::string head = header(backend_);
  std::string bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[64 << 10];
    size_t n;
    while (bytes.size() < kMaxFileBytes && (n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
    std::fclose(f);
  }
  // The records that check out; the file is cut back to them.
  size_t good = 0;
  int loaded = 0;
  if (bytes.size() >= head.size() && bytes.compare(0, head.size(), head) == 0) {
    size_t at = head.size();
    good = at;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
    while (at + 16 <= bytes.size()) {
      const size_t k = size_t(get(p + at, 4)), d = size_t(get(p + at + 4, 4));
      const uint64_t h = get(p + at + 8, 8);
      if (!k || k > kMaxPart || d > kMaxPart || at + 16 + k + d > bytes.size()) break;
      sk_sp<SkData> key = SkData::MakeWithCopy(p + at + 16, k);
      sk_sp<SkData> data = SkData::MakeWithCopy(p + at + 16 + k, d);
      if (recordHash(*key, *data) != h) break;
      const uint64_t kh = pipeline_cache::hash(key->data(), key->size());
      bool known = false;
      auto range = entries_.equal_range(kh);
      for (auto it = range.first; it != range.second; ++it)
        if (it->second.key->equals(key.get())) {
          it->second.data = data;  // the later record wins
          known = true;
        }
      if (!known) entries_.emplace(kh, Entry{std::move(key), std::move(data)});
      loaded++;
      at += 16 + k + d;
      good = at;
    }
  } else if (!bytes.empty() && bytes.compare(0, sizeof kMagic, std::string(kMagic, sizeof kMagic)) != 0) {
    SkDebugf("jsui: %s is not a JSUI shader cache; leaving it alone\n", path.c_str());
    return -1;
  }
  stats_.loaded += uint64_t(loaded);
  {
    std::lock_guard<std::mutex> files(gFilesMutex);
    if (!gFiles.insert(path).second) return loaded;  // another context appends to it
  }
  // Rewrite when it is new, from another build, or has a damaged tail;
  // otherwise append.
  if (good == 0 || good != bytes.size()) {
    std::string keep = good ? bytes.substr(0, good) : head;
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f && std::fwrite(keep.data(), 1, keep.size(), f) == keep.size();
    ok = f && std::fclose(f) == 0 && ok;
    std::remove(path.c_str());
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
      std::remove(tmp.c_str());
      SkDebugf("jsui: shader cache %s cannot be written\n", path.c_str());
      std::lock_guard<std::mutex> files(gFilesMutex);
      gFiles.erase(path);
      return -1;
    }
    good = keep.size();
  }
  file_ = std::fopen(path.c_str(), "ab");
  if (!file_) {
    std::lock_guard<std::mutex> files(gFilesMutex);
    gFiles.erase(path);
    return -1;
  }
  path_ = path;
  fileBytes_ = good;
  return loaded;
}

sk_sp<SkData> Cache::load(const SkData& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  openDefaultLocked();
  auto range = entries_.equal_range(pipeline_cache::hash(key.data(), key.size()));
  for (auto it = range.first; it != range.second; ++it)
    if (it->second.key->equals(&key)) {
      stats_.hits++;
      return it->second.data;
    }
  stats_.misses++;
  return nullptr;
}

void Cache::store(const SkData& key, const SkData& data, const SkString&) {
  if (!key.size() || key.size() > kMaxPart || data.size() > kMaxPart) return;
  std::lock_guard<std::mutex> lock(mutex_);
  openDefaultLocked();
  const uint64_t kh = pipeline_cache::hash(key.data(), key.size());
  Entry e{SkData::MakeWithCopy(key.data(), key.size()), SkData::MakeWithCopy(data.data(), data.size())};
  bool replaced = false;
  auto range = entries_.equal_range(kh);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second.key->equals(&key)) {
      if (it->second.data->equals(&data)) return;
      it->second.data = e.data;
      replaced = true;
    }
  if (!replaced) entries_.emplace(kh, e);
  stats_.stored++;
  appendLocked(e);
}

void Cache::appendLocked(const Entry& e) {
  if (!file_ || fileBytes_ + 16 + e.key->size() + e.data->size() > kMaxFileBytes) return;
  std::string r;
  put32(r, uint32_t(e.key->size()));
  put32(r, uint32_t(e.data->size()));
  put64(r, recordHash(*e.key, *e.data));
  r.append(static_cast<const char*>(e.key->data()), e.key->size());
  r.append(static_cast<const char*>(e.data->data()), e.data->size());
  // One write: a crash leaves at most a damaged tail, dropped on load.
  if (std::fwrite(r.data(), 1, r.size(), file_) != r.size() || std::fflush(file_) != 0) {
    SkDebugf("jsui: shader cache %s cannot be appended to; not recording more shaders\n", path_.c_str());
    std::fclose(file_);
    file_ = nullptr;
    std::lock_guard<std::mutex> files(gFilesMutex);
    gFiles.erase(path_);
    return;
  }
  fileBytes_ += r.size();
}

Cache::Stats Cache::stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace shader_cache
}  // namespace jsui
