// Ganesh's persistent shader cache (GrContextOptions::fPersistentCache) in
// a file: GL program binaries, Direct3D HLSL, Metal MSL. The first launch
// compiles each shader in the frame that first draws it (tens of ms on
// a desktop GL or Direct3D driver); from the second launch on, Ganesh finds
// it here instead.
//
// The default file is `<user cache dir>/jsui/shaders-<program>-<backend>.bin`
// (pipeline_cache::cacheFile); jsui_gpu_set_pipeline_cache names another,
// JSUI_PIPELINE_CACHE=off turns both off. None in the browser or on Android.
//
// File: the header (magic "JSUISHDR", version, Skia build id, backend), then
// records of u32 key size, u32 data size, u64 FNV-1a 64 of key and data,
// key, data. A damaged tail is dropped; a file from another Skia build or
// backend is replaced. Each new shader is appended with one write.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "include/core/SkData.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"
#if __has_include("include/gpu/ganesh/GrContextOptions.h")
#include "include/gpu/ganesh/GrContextOptions.h"
#else
#include "include/gpu/GrContextOptions.h"
#endif

namespace jsui {
namespace shader_cache {

class Cache final : public GrContextOptions::PersistentCache {
 public:
  // `backend`: "gl", "d3d" or "metal-ganesh" (the file name and header).
  explicit Cache(std::string backend);
  ~Cache() override;

  // The default file opens with the first shader Ganesh looks up, unless the
  // host named one before.
  // jsui_gpu_set_pipeline_cache: loads `path` (keeping what is in memory)
  // and appends to it from now on. Returns the number of shaders in the
  // file, -1 when it cannot be read or created.
  int setFile(const std::string& path);

  sk_sp<SkData> load(const SkData& key) override;
  void store(const SkData& key, const SkData& data, const SkString& description) override;

  struct Stats {
    uint64_t loaded = 0;  // shaders read from the file
    uint64_t hits = 0;    // load() found the shader
    uint64_t misses = 0;
    uint64_t stored = 0;  // new shaders (appended when there is a file)
  };
  Stats stats();

 private:
  struct Entry {
    sk_sp<SkData> key, data;
  };
  void openDefaultLocked();
  int openLocked(const std::string& path);
  void appendLocked(const Entry& e);

  const std::string backend_;
  std::mutex mutex_;
  std::unordered_multimap<uint64_t, Entry> entries_;  // by FNV-1a 64 of the key
  std::string path_;
  FILE* file_ = nullptr;  // open for appending
  size_t fileBytes_ = 0;
  bool opened_ = false;   // a file was named (or the default tried)
  Stats stats_;
};

}  // namespace shader_cache
}  // namespace jsui
