// The Vulkan driver's pipeline cache in a file (see driver_cache.h).
#include "render/driver_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render/pipeline_cache.h"

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <process.h>
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

#if defined(JSUI_VULKAN)
#include "include/core/SkTypes.h"
#include "include/gpu/graphite/Context.h"
#include "src/gpu/graphite/ContextPriv.h"
#include "src/gpu/graphite/SharedContext.h"
#endif

namespace jsui {
namespace driver_cache {

namespace {

constexpr char kMagic[8] = {'J', 'S', 'U', 'I', 'V', 'K', 'P', 'C'};
// The Vulkan pipeline cache header, version one (Vulkan spec, "Pipeline
// Cache"): u32 length, u32 version, u32 vendorID, u32 deviceID, u8[16] UUID.
constexpr size_t kVkHeaderBytes = 4 * 4 + 16;
constexpr uint32_t kVkHeaderVersionOne = 1;

void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
}
void put64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(char((v >> (8 * i)) & 0xff));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

uint32_t fnv32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

std::string hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04x", v);
  return buf;
}

std::mutex gTestMutex;
bool gTestFileSet = false;
std::string gTestFile;

}  // namespace

std::string encode(const DeviceId& device, const void* payload, size_t size) {
  std::string s(kMagic, sizeof(kMagic));
  put32(s, kVersion);
  put32(s, uint32_t(kHeaderBytes));
  put32(s, device.vendorID);
  put32(s, device.deviceID);
  put32(s, device.driverVersion);
  s.append(reinterpret_cast<const char*>(device.uuid), sizeof(device.uuid));
  put64(s, uint64_t(size));
  put64(s, pipeline_cache::hash(payload, size));
  put32(s, fnv32(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
  s.append(static_cast<const char*>(payload), size);
  return s;
}

Decoded decode(const void* bytes, size_t size, const DeviceId& device) {
  Decoded d;
  const uint8_t* b = static_cast<const uint8_t*>(bytes);
  if (size == 0) return d;
  if (std::memcmp(b, kMagic, std::min(size, sizeof(kMagic))) != 0) {
    d.status = Decoded::Foreign;
    d.reason = "not a JSUI driver pipeline cache";
    return d;
  }
  d.status = Decoded::Damaged;
  if (size < 16) {
    d.reason = "header cut short";
    return d;
  }
  const uint32_t version = get32(b + 8);
  if (version != kVersion) {
    d.status = Decoded::Stale;
    d.reason = "format version " + std::to_string(version) + ", this build reads " + std::to_string(kVersion);
    return d;
  }
  if (get32(b + 12) != kHeaderBytes || size < kHeaderBytes) {
    d.reason = "header cut short";
    return d;
  }
  if (get32(b + kHeaderBytes - 4) != fnv32(b, kHeaderBytes - 4)) {
    d.reason = "header checksum mismatch";
    return d;
  }
  const uint32_t vendor = get32(b + 16), dev = get32(b + 20), driver = get32(b + 24);
  if (vendor != device.vendorID || dev != device.deviceID) {
    d.status = Decoded::Stale;
    d.reason = "written for GPU " + hex(vendor) + ":" + hex(dev) + ", this is " + hex(device.vendorID) + ":" +
               hex(device.deviceID);
    return d;
  }
  if (driver != device.driverVersion) {
    d.status = Decoded::Stale;
    d.reason = "written by driver version " + std::to_string(driver) + ", this is " +
               std::to_string(device.driverVersion);
    return d;
  }
  if (std::memcmp(b + 28, device.uuid, sizeof(device.uuid)) != 0) {
    d.status = Decoded::Stale;
    d.reason = "the driver's pipeline cache UUID changed";
    return d;
  }
  const uint64_t n = get64(b + 44);
  if (n > kMaxPayload || n != size - kHeaderBytes) {
    d.reason = n > kMaxPayload ? "payload too large" : "payload cut short";
    return d;
  }
  const uint8_t* p = b + kHeaderBytes;
  if (pipeline_cache::hash(p, size_t(n)) != get64(b + 52)) {
    d.reason = "payload checksum mismatch";
    return d;
  }
  // The driver's own header (what Graphite and the driver check too).
  if (n < kVkHeaderBytes || get32(p) < kVkHeaderBytes || get32(p) > n || get32(p + 4) != kVkHeaderVersionOne ||
      get32(p + 8) != device.vendorID || get32(p + 12) != device.deviceID ||
      std::memcmp(p + 16, device.uuid, sizeof(device.uuid)) != 0) {
    d.reason = "the payload's Vulkan header does not match the device";
    return d;
  }
  d.status = Decoded::Ok;
  d.payload = p;
  d.size = size_t(n);
  return d;
}

void setFileForTest(const std::string& path) {
  std::lock_guard<std::mutex> lock(gTestMutex);
  gTestFileSet = true;
  gTestFile = path;
}

void resetFileForTest() {
  std::lock_guard<std::mutex> lock(gTestMutex);
  gTestFileSet = false;
  gTestFile.clear();
}

std::string defaultFile(const DeviceId& device) {
  {
    std::lock_guard<std::mutex> lock(gTestMutex);
    if (gTestFileSet) return gTestFile;
  }
  const char* env = std::getenv("JSUI_PIPELINE_CACHE");
  if (env && (!std::strcmp(env, "off") || !std::strcmp(env, "0"))) return {};
  if (env && *env) return std::string(env) + ".vkdriver";
  if (pipeline_cache::noDefaultFile()) return {};
  return pipeline_cache::cacheFile("vkdriver", hex(device.vendorID) + "-" + hex(device.deviceID));
}

bool writeAtomic(const std::string& path, const std::string& bytes) {
#if defined(__EMSCRIPTEN__)
  (void)path;
  (void)bytes;
  return false;
#else
  static std::atomic<unsigned> counter{0};
#if defined(_WIN32)
  const int pid = _getpid();
#else
  const int pid = int(getpid());
#endif
  // Per process and save: two processes (or contexts) saving at once each
  // rename a complete file; the last one wins.
  const std::string tmp = path + ".tmp." + std::to_string(pid) + "." + std::to_string(counter++);
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = std::fflush(f) == 0 && ok;
#if defined(_WIN32)
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;  // the rename must not land before the data
#endif
  ok = std::fclose(f) == 0 && ok;
#if defined(_WIN32)
  // MoveFileEx replaces the file in one step (rename() does not replace).
  ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
  if (!ok) std::remove(tmp.c_str());
  return ok;
#endif
}

#if defined(JSUI_VULKAN)

namespace {

bool readAll(const std::string& path, std::string* out, bool* exists) {
  *exists = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  *exists = true;
  out->clear();
  char buf[64 << 10];
  size_t n;
  while (out->size() <= kMaxPayload + kHeaderBytes && (n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

int logLevel() {
  const char* v = std::getenv("JSUI_LOG_PIPELINES");
  return v && *v && std::strcmp(v, "0") != 0 ? 1 : 0;
}

}  // namespace

Storage::Storage(std::string path, const DeviceId& device) : path_(std::move(path)), device_(device), log_(logLevel()) {}

Storage::~Storage() { stop(); }

sk_sp<SkData> Storage::load() {
  std::string bytes;
  bool exists = false;
  if (!readAll(path_, &bytes, &exists)) {
    if (exists) SkDebugf("jsui: Vulkan driver pipeline cache %s cannot be read\n", path_.c_str());
    return nullptr;
  }
  const Decoded d = decode(bytes.data(), bytes.size(), device_);
  switch (d.status) {
    case Decoded::Empty: return nullptr;
    case Decoded::Foreign:
      SkDebugf("jsui: %s is not a JSUI driver pipeline cache; leaving it alone\n", path_.c_str());
      foreign_ = true;
      return nullptr;
    case Decoded::Damaged:
    case Decoded::Stale:
      SkDebugf("jsui: Vulkan driver pipeline cache %s ignored (%s); starting a new one\n", path_.c_str(),
               d.reason.c_str());
      return nullptr;
    case Decoded::Ok: break;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.loadedBytes = d.size;
  }
  if (log_) SkDebugf("jsui: Vulkan driver pipeline cache: %zu bytes from %s\n", d.size, path_.c_str());
  return SkData::MakeWithCopy(d.payload, d.size);
}

void Storage::store(const SkData& data) {
  if (foreign_ || !data.size()) return;
  if (!writeAtomic(path_, encode(device_, data.data(), data.size()))) {
    SkDebugf("jsui: Vulkan driver pipeline cache %s cannot be written\n", path_.c_str());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.saves++;
    stats_.savedBytes = data.size();
  }
  if (log_) SkDebugf("jsui: Vulkan driver pipeline cache: saved %zu bytes to %s\n", data.size(), path_.c_str());
}

void Storage::start(skgpu::graphite::Context* ctx, std::function<uint64_t()> created, int intervalMs) {
  {
    std::lock_guard<std::mutex> lock(syncMutex_);
    ctx_ = ctx;
  }
  created_ = std::move(created);
  if (!ctx || foreign_ || thread_.joinable()) return;
  thread_ = std::thread([this, intervalMs] { run(intervalMs); });
}

void Storage::run(int intervalMs) {
  uint64_t seen = created_ ? created_() : 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    cv_.wait_for(lock, std::chrono::milliseconds(intervalMs), [this] { return stopping_; });
    if (stopping_) return;
    lock.unlock();
    // Only while pipelines appear: a session past its first minutes asks the
    // driver for nothing.
    const uint64_t now = created_ ? created_() : seen + 1;
    if (now != seen) {
      seen = now;
      sync();
    }
    lock.lock();
  }
}

void Storage::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Storage::sync() {
  std::lock_guard<std::mutex> lock(syncMutex_);
  if (!ctx_ || foreign_) return;
  {
    std::lock_guard<std::mutex> l(mutex_);
    stats_.syncs++;
  }
  // The SharedContext's, not Context::syncPipelineData: that one belongs to
  // the render thread (Graphite's single-owner check). vkGetPipelineCacheData
  // needs no external synchronisation (Graphite's VkPipelineCache is not
  // created EXTERNALLY_SYNCHRONIZED), so this runs beside pipeline creation
  // on the compile threads and the host's use of the device. Graphite calls
  // store() only when the driver compiled something since the last sync.
  ctx_->priv().sharedContext()->syncPipelineData(this, kMaxPayload);
}

void Storage::finish() {
  stop();
  sync();
  std::lock_guard<std::mutex> lock(syncMutex_);
  ctx_ = nullptr;
}

Storage::Stats Storage::stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

#endif  // JSUI_VULKAN

}  // namespace driver_cache
}  // namespace jsui
