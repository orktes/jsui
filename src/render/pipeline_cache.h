// Persistent pipeline cache of the Graphite contexts (jsui_gpu_set_pipeline_cache,
// jsui_gpu_precompile_pipelines in jsui.h).
//
// Graphite creates a GPU pipeline (compiled shaders + fixed-function state)
// the first time a draw needs it, inside the frame's submit: a kind of draw
// the session has not done before (images under a rounded clip on a list's
// first scroll, a blend in a panel's entrance) stalls that frame, by tens of
// milliseconds on a cold driver. The cache records the serialized key of
// every pipeline a context creates (ContextOptions::fPipelineCachingCallback)
// and, in the next session, has Graphite create them again before the frames
// that need them (PrecompileContext::precompile):
//
//   * Precompiler (one per Graphite context) queues keys from files and
//     in-memory lists, de-duplicated, and precompiles them off the frame: on
//     a worker thread where the device is thread safe, otherwise in slices
//     between frames on the context's thread (Mode, chosen per backend by
//     gpu_graphite.cpp / gpu_webgpu.cpp).
//   * A cache file gets each new key appended as one record as soon as it is
//     created, so a crash loses at most the record being written, which the
//     next load drops as a torn tail.
//
// File format (also the format of the in-memory lists), little-endian:
//
//   header  "JSUIPIPE"         magic
//           u32 version        kFormatVersion
//           u32 header bytes   the whole header, checksum included
//           u32 milestone      SK_MILESTONE of the Skia that wrote it
//           u32 backend        Backend
//           u32 n, n bytes     build id (buildId(): Skia milestone + revision)
//           u32 checksum       FNV-1a 32 of the header bytes before it
//   record  u32 size           payload bytes (1 .. kMaxRecord)
//           u64 hash           FNV-1a 64 of the payload (the de-duplication key)
//           payload            Skia's serialized pipeline key
//
// A header of another format version, Skia build or backend (keys another
// Skia may decode differently, render passes of another backend's formats)
// makes the file stale: it is ignored and started afresh. A record that is
// cut short, too large or whose hash does not match ends the file: it and
// everything after it are dropped. Either way one line is logged, nothing
// crashes and no bytes of a bad record reach Skia. Skia decodes a key with
// range checks only, so before precompiling, a key is also fitted to the
// device (fitKey in the .cpp): a list from another GPU gets this device's
// depth/stencil formats, and a key the device cannot build fails.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class SkExecutor;

namespace skgpu::graphite {
class Context;
struct ContextOptions;
}  // namespace skgpu::graphite

namespace jsui {
namespace pipeline_cache {

constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxRecord = 64 << 10;  // a serialized key is a few hundred bytes
// Per file / list: Graphite keeps at most 512 graphics pipelines (an LRU),
// so precompiling more would only evict the first ones again.
constexpr size_t kMaxEntries = 512;
constexpr size_t kMaxFileBytes = 16 << 20;

// Which Graphite backend's keys a file holds.
enum class Backend : uint32_t { Vulkan = 1, Metal = 2, Dawn = 3, BrowserWebGPU = 4 };

uint64_t hash(const void* data, size_t size);
// "skia m<milestone>", plus " <revision>" when the build knows the Skia
// revision (JSUI_SKIA_REVISION).
std::string buildId();
// The header for this build and `backend`.
std::string header(Backend backend);
// One record (appended with a single write).
std::string record(const void* data, size_t size);

struct Entry {
  uint64_t hash = 0;
  std::string data;  // the serialized key
};

struct Parsed {
  enum Status {
    Ok,         // a valid header (entries may still be empty)
    Empty,      // no bytes at all: a new file
    Foreign,    // not a pipeline cache (no magic): never overwritten
    BadHeader,  // a pipeline cache whose header is damaged or cut short
    Stale,      // a valid header of another format version / Skia build / backend
  } status = Empty;
  std::vector<Entry> entries;  // valid records in order, de-duplicated by hash
  size_t goodBytes = 0;        // header + valid records: a bad tail starts here
  size_t duplicates = 0;       // records dropped as duplicates
  bool badTail = false;        // bytes after goodBytes were dropped
  std::string reason;          // why the header / tail was rejected (log text)
};
Parsed parse(const void* data, size_t size, Backend backend);

// Graphite's ContextOptions::fExecutor for contexts whose device is thread
// safe: a few threads that compile the pipelines a frame needs in parallel
// (and, at a lower priority, the precompiled ones) instead of one after the
// other on the render thread. A frame with several new kinds of draws then
// waits for the slowest compile rather than for their sum. Outlives the
// Context (which waits for its tasks when destroyed).
std::unique_ptr<SkExecutor> makeCompileExecutor();
// Precompiles that executor has queued or is running (Graphite hands them
// over and returns: they finish there).
int pendingPrecompiles(const SkExecutor* executor);
// Every compile that executor has queued or is running (frames' and
// shader warm-ups' too).
int pendingCompiles(const SkExecutor* executor);

// Tests only: precompile in slices (Mode::Sliced) whatever the backend.
void setForceSliced(bool on);
bool forceSliced();
// Tests only: contexts start without the default cache file.
void setNoDefaultFile(bool on);
// The cache file a context uses until the host names one (jsui.h): in the
// user's cache directory, per program and backend; empty where there is
// none (Android, the browser) or JSUI_PIPELINE_CACHE=off.
std::string defaultFile(Backend backend);

struct Stats {
  int mode = 0;            // Mode
  int queued = 0;          // waiting to be precompiled, including the one compiling
  uint64_t precompiled = 0;
  uint64_t failed = 0;     // keys this Skia cannot decode or this device cannot build
  uint64_t compiled = 0;   // pipelines compiled on demand in a frame (cache misses)
  uint64_t added = 0;      // ... of them new: recorded in the cache (file / save())
  uint64_t unserializable = 0;  // ... that Skia cannot serialize (cannot be cached)
  uint64_t known = 0;      // distinct pipelines recorded or queued
};

class File;

class Precompiler {
 public:
  // jsui.h's JSUI_PRECOMPILE_*.
  enum class Mode { None = 0, Thread = 1, Sliced = 2 };

  explicit Precompiler(Backend backend);
  ~Precompiler();
  Precompiler(const Precompiler&) = delete;
  Precompiler& operator=(const Precompiler&) = delete;

  // Before the Context is made: installs the pipeline callback.
  void configure(skgpu::graphite::ContextOptions& opts);
  // Once the Context exists: precompiles in `mode` from now on. Loads the
  // JSUI_PIPELINE_CACHE file (a development override of setFile) if set.
  // `executor`: the Context's makeCompileExecutor(), which then runs the
  // precompiles (they count as queued until it has).
  void attach(skgpu::graphite::Context* ctx, Mode mode, const SkExecutor* executor = nullptr);
  // Before the Context is destroyed: stops the worker (after the pipeline it
  // is compiling) and drops the PrecompileContext.
  void shutdown();

  // jsui_gpu_set_pipeline_cache: loads `path` (queues its keys) and appends
  // every new pipeline to it from now on, including those created before.
  // Returns the number of keys queued, -1 when the file cannot be read or
  // created (or is not a pipeline cache).
  int setFile(const std::string& path);
  // jsui_gpu_precompile_pipelines: queues a list (the file format). Returns
  // the number of keys queued (keys already known are skipped), -1 when the
  // list is not one for this build and backend.
  int addList(const void* data, size_t size);
  // jsui_gpu_save_pipelines: every key known to this context, as a list.
  std::string save();
  // jsui_gpu_precompile_step: in Sliced mode precompiles for up to
  // `budgetMs` (at least one key) and stops the automatic slices of
  // afterFrame (the host schedules them). Returns the keys still queued.
  int step(double budgetMs);
  // After each render: in Sliced mode, one slice unless the host steps (at
  // most one per frame's worth of time, whatever the number of surfaces).
  void afterFrame();
  Stats stats();
  // Pipelines compiled on demand since the last call (Perf counters).
  uint64_t takeCompiles();

  // From Graphite's pipeline callback (any thread): a pipeline was created.
  void onPipelineAdded(const std::string& label, bool fromPrecompile, const void* data, size_t size);

  // Tests only: `key` with its depth/stencil attachment in the other format
  // of the same aspects (D16 / D32F, D24_S8 / D32F_S8): a key from a device
  // that picks the other one. Empty when the key has no depth/stencil
  // attachment or the precompiler is not attached.
  std::string swapDepthStencilForTest(const std::string& key);
  // Tests only: compiles still on the context's compile pool.
  int poolPendingForTest() const { return pendingCompiles(executor_); }

 private:
  struct Compiler;  // the PrecompileContext (Graphite builds only)

  int openFile(const std::string& path);
  int queueLocked(std::vector<Entry>&& entries);
  void kickLocked();
  void doneLocked();
  void run();
  bool compile(const Entry& e);
  int slice(double budgetMs);

  const Backend backend_;
  Mode mode_ = Mode::None;
  int log_ = 0;           // JSUI_LOG_PIPELINES: 1 every pipeline created, 2 those frames compiled
  std::string envFile_;   // JSUI_PIPELINE_CACHE: overrides setFile
  std::unique_ptr<Compiler> compiler_;
  const SkExecutor* executor_ = nullptr;

  std::mutex mutex_;  // everything below (the callback runs on any thread)
  std::shared_ptr<File> file_;
  std::unordered_set<uint64_t> known_;
  std::vector<Entry> entries_;  // every known key, in order (save())
  std::vector<Entry> created_;  // ... of them those compiled on demand
  std::deque<Entry> queue_;
  int inFlight_ = 0;
  bool hostSteps_ = false;
  std::chrono::steady_clock::time_point lastSlice_{};  // afterFrame's last slice
  bool stopping_ = false;
  bool running_ = false;  // the worker thread is draining the queue
  std::thread worker_;
  Stats stats_;
  double precompileMs_ = 0;
  bool reported_ = false;
  std::atomic<uint64_t> compiles_{0}, compilesTaken_{0};
};

}  // namespace pipeline_cache
}  // namespace jsui
