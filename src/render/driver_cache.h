// The Vulkan driver's pipeline cache (VkPipelineCache data) kept in a file
// across launches, for the Graphite Vulkan contexts (jsui_gpu_create_vulkan).
//
// The pipeline cache (pipeline_cache.h) records which pipelines a program
// uses and has the next launch create them again off the frame; on Vulkan
// each of those creations is still a full driver compile (SPIR-V to GPU
// code) unless the driver finds the pipeline in a VkPipelineCache. Graphite
// creates one VkPipelineCache per context and seeds it from
// ContextOptions::fPersistentPipelineStorage when the context is created;
// Storage is that object. From the second launch on the driver then finds
// the pipelines the precompiles (and frames) ask for instead of compiling
// them: the background precompile finishes in a fraction of the time and a
// frame that needs a pipeline before its precompile ran waits for a lookup,
// not a compile. Metal and Dawn have no such hook in Graphite: macOS / iOS
// keep compiled Metal pipelines in the system's per-app shader cache, Dawn
// devices are the host's (Dawn's blob cache is configured on the device).
//
// The file is named at context creation (Graphite reads it then):
// `<user cache dir>/jsui/vkdriver-<program>-<vendor>-<device>.bin`
// (pipeline_cache::cacheFile), `<JSUI_PIPELINE_CACHE>.vkdriver` with that
// development override, none with JSUI_PIPELINE_CACHE=off, on Android or in
// the browser. A thread of the context saves it while new pipelines appear
// (at most every few seconds) and the context saves it once more when it is
// destroyed; jsui_gpu_purge saves it too (hosts call it when the
// app goes to the background, where it may be killed without a shutdown).
// Every save replaces the file atomically (a temporary file renamed over
// it), so a crash leaves the previous version.
//
// File, little-endian:
//
//   magic "JSUIVKPC", u32 version (kVersion), u32 header bytes (kHeaderBytes),
//   u32 vendorID, u32 deviceID, u32 driverVersion, u8[16] pipelineCacheUUID
//   (VkPhysicalDeviceProperties), u64 payload bytes, u64 payload FNV-1a 64,
//   u32 FNV-1a 32 of the header bytes before it; then the payload: the
//   driver's vkGetPipelineCacheData bytes, as Graphite hands them over.
//
// A file of another GPU, driver version or pipeline cache UUID is stale: it
// is ignored (Graphite starts an empty cache) and replaced by the next save.
// A damaged file (bad checksum, payload cut short or whose own Vulkan header
// does not match the device) is ignored the same way; no byte of it reaches
// the driver. A file that is not one of these is left alone (no saves).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(JSUI_VULKAN)
#include "include/core/SkData.h"
#include "include/core/SkRefCnt.h"
#include "include/gpu/graphite/PersistentPipelineStorage.h"
namespace skgpu::graphite {
class Context;
}
#endif

namespace jsui {
namespace driver_cache {

constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 8 + 4 + 4 + 3 * 4 + 16 + 8 + 8 + 4;
// Graphite keeps at most 512 pipelines; drivers store from a few KB (a
// pipeline's key and binary) to a few hundred KB each.
constexpr size_t kMaxPayload = 64u << 20;

// The device a cache belongs to (VkPhysicalDeviceProperties).
struct DeviceId {
  uint32_t vendorID = 0;
  uint32_t deviceID = 0;
  uint32_t driverVersion = 0;
  uint8_t uuid[16] = {};
};

// The file's bytes for `payload` (vkGetPipelineCacheData) of `device`.
std::string encode(const DeviceId& device, const void* payload, size_t size);

struct Decoded {
  enum Status {
    Ok,       // `payload` / `size`: the driver's data for this device
    Empty,    // no bytes: no cache yet
    Foreign,  // not a driver cache (no magic): left alone
    Damaged,  // a driver cache cut short, with a bad checksum or payload
    Stale,    // a valid one of another device / driver / format version
  } status = Empty;
  const uint8_t* payload = nullptr;  // into the decoded bytes
  size_t size = 0;
  std::string reason;  // why it was rejected (log text)
};
Decoded decode(const void* bytes, size_t size, const DeviceId& device);

// The file a context on `device` uses (above); empty: none.
std::string defaultFile(const DeviceId& device);
// Tests only: every new context uses `path` ("" none) instead of
// defaultFile; reset() goes back to it.
void setFileForTest(const std::string& path);
void resetFileForTest();

// Replaces `path` with `bytes` (written to a temporary file of this process
// beside it, flushed to disk, renamed over it). False on any error (the old
// file is then untouched).
bool writeAtomic(const std::string& path, const std::string& bytes);

#if defined(JSUI_VULKAN)
// Graphite's PersistentPipelineStorage for one Vulkan context.
class Storage final : public skgpu::graphite::PersistentPipelineStorage {
 public:
  Storage(std::string path, const DeviceId& device);
  ~Storage() override;

  // Graphite, while it creates the context: the payload of a valid file.
  sk_sp<SkData> load() override;
  // Graphite, from sync(): writes the file.
  void store(const SkData& data) override;

  // Once the context exists: a thread saves the cache every `interval` ms
  // while `created` (pipelines created so far) grows.
  void start(skgpu::graphite::Context* ctx, std::function<uint64_t()> created, int intervalMs = 5000);
  // Stops that thread (before the context goes; sync() still works).
  void stop();
  // Saves now, on the calling thread, when Graphite has new pipeline data.
  void sync();
  // Before the context is destroyed: stop() and a last sync().
  void finish();

  struct Stats {
    size_t loadedBytes = 0;  // payload handed to Graphite at creation (0: none)
    uint64_t saves = 0;      // files written
    size_t savedBytes = 0;   // payload of the last one
    uint64_t syncs = 0;      // sync() calls (also those that wrote nothing)
  };
  Stats stats();
  const std::string& path() const { return path_; }

 private:
  void run(int intervalMs);

  const std::string path_;
  const DeviceId device_;
  int log_ = 0;  // JSUI_LOG_PIPELINES
  std::mutex syncMutex_;  // one sync at a time (thread, finish, purge)
  skgpu::graphite::Context* ctx_ = nullptr;  // guarded by syncMutex_
  bool foreign_ = false;  // never overwrite
  std::function<uint64_t()> created_;
  std::mutex mutex_;  // stopping_, stats_
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread thread_;
  Stats stats_;
};
#endif

}  // namespace driver_cache
}  // namespace jsui
