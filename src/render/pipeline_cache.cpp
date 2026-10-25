// Persistent Graphite pipeline cache: the file format, the cache files and
// the precompiler (see pipeline_cache.h).
#include "render/pipeline_cache.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

#include "include/core/SkExecutor.h"
#include "include/core/SkMilestone.h"
#include "include/core/SkTypes.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <direct.h>
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(JSUI_VULKAN) || defined(JSUI_GRAPHITE_METAL) || defined(JSUI_GRAPHITE_DAWN) || defined(JSUI_WEBGPU)
#define JSUI_PRECOMPILE 1
#include "include/core/SkData.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/PrecompileContext.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/ContextPriv.h"
#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/gpu/graphite/RenderPassDesc.h"
#include "src/gpu/graphite/SerializationUtils.h"
#include "src/gpu/graphite/TextureFormat.h"
#if SK_MILESTONE >= 140
#define JSUI_PRECOMPILE_COMMON 1
#include "include/core/SkBlendMode.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/graphite/precompile/PaintOptions.h"
#include "include/gpu/graphite/precompile/Precompile.h"
#include "include/gpu/graphite/precompile/PrecompileMaskFilter.h"
#include "include/gpu/graphite/precompile/PrecompileShader.h"
#endif
#endif

namespace jsui {
namespace pipeline_cache {

namespace {

constexpr char kMagic[8] = {'J', 'S', 'U', 'I', 'P', 'I', 'P', 'E'};
// magic, version, header bytes, milestone, backend, build id length, checksum
constexpr size_t kFixedHeader = 8 + 4 * 5 + 4;
constexpr size_t kRecordHeader = 4 + 8;
// Sliced mode: the precompile time after a render (at least one key), at
// most once per interval: once per frame however many surfaces render.
constexpr double kFrameSliceMs = 2.0;
constexpr auto kSliceInterval = std::chrono::milliseconds(12);

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

uint32_t fnv32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
}
void put64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(char((v >> (8 * i)) & 0xff));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

const char* backendName(uint32_t b) {
  switch (Backend(b)) {
    case Backend::Vulkan: return "Vulkan";
    case Backend::Metal: return "Metal";
    case Backend::Dawn: return "Dawn";
    case Backend::BrowserWebGPU: return "browser WebGPU";
  }
  return "an unknown backend";
}

std::atomic<bool> gForceSliced{false};
std::atomic<bool> gNoCommon{false};
std::atomic<bool> gNoDefaultFile{false};

}  // namespace

uint64_t hash(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

std::string buildId() {
  std::string id = "skia m" + std::to_string(SK_MILESTONE);
#if defined(JSUI_SKIA_REVISION)
  id += " " JSUI_SKIA_REVISION;
#endif
  return id;
}

std::string header(Backend backend) {
  const std::string id = buildId();
  std::string h(kMagic, sizeof(kMagic));
  put32(h, kFormatVersion);
  put32(h, uint32_t(kFixedHeader + id.size()));
  put32(h, SK_MILESTONE);
  put32(h, uint32_t(backend));
  put32(h, uint32_t(id.size()));
  h += id;
  put32(h, fnv32(reinterpret_cast<const uint8_t*>(h.data()), h.size()));
  return h;
}

std::string record(const void* data, size_t size) {
  std::string r;
  r.reserve(kRecordHeader + size);
  put32(r, uint32_t(size));
  put64(r, hash(data, size));
  r.append(static_cast<const char*>(data), size);
  return r;
}

Parsed parse(const void* data, size_t size, Backend backend) {
  Parsed p;
  const uint8_t* b = static_cast<const uint8_t*>(data);
  if (size == 0) return p;
  if (std::memcmp(b, kMagic, std::min(size, sizeof(kMagic))) != 0) {
    p.status = Parsed::Foreign;
    p.reason = "not a JSUI pipeline cache";
    return p;
  }
  p.status = Parsed::BadHeader;
  if (size < kFixedHeader) {
    p.reason = "header cut short";
    return p;
  }
  const uint32_t version = get32(b + 8);
  const uint32_t headerBytes = get32(b + 12);
  if (version != kFormatVersion) {
    p.status = Parsed::Stale;
    p.reason = "format version " + std::to_string(version) + ", this build reads " + std::to_string(kFormatVersion);
    return p;
  }
  if (headerBytes < kFixedHeader || headerBytes > kFixedHeader + 256) {
    p.reason = "bad header size";
    return p;
  }
  if (headerBytes > size) {
    p.reason = "header cut short";
    return p;
  }
  if (get32(b + headerBytes - 4) != fnv32(b, headerBytes - 4)) {
    p.reason = "header checksum mismatch";
    return p;
  }
  const uint32_t milestone = get32(b + 16);
  const uint32_t fileBackend = get32(b + 20);
  const uint32_t idLen = get32(b + 24);
  if (kFixedHeader + idLen != headerBytes) {
    p.reason = "bad header size";
    return p;
  }
  const std::string id(reinterpret_cast<const char*>(b + 28), idLen);
  if (milestone != SK_MILESTONE || id != buildId()) {
    p.status = Parsed::Stale;
    p.reason = "written by " + id + ", this is " + buildId();
    return p;
  }
  if (fileBackend != uint32_t(backend)) {
    p.status = Parsed::Stale;
    p.reason = std::string("written for ") + backendName(fileBackend) + ", this context is " +
               backendName(uint32_t(backend));
    return p;
  }
  p.status = Parsed::Ok;
  size_t pos = headerBytes;
  p.goodBytes = pos;
  std::unordered_set<uint64_t> seen;
  while (pos < size) {
    if (p.entries.size() >= kMaxEntries) {
      p.reason = "more than " + std::to_string(kMaxEntries) + " pipelines";
      break;
    }
    if (size - pos < kRecordHeader) {
      p.reason = "last record cut short";
      break;
    }
    const uint32_t n = get32(b + pos);
    const uint64_t h = get64(b + pos + 4);
    if (n == 0 || n > kMaxRecord) {
      p.reason = "bad record size";
      break;
    }
    if (size - pos - kRecordHeader < n) {
      p.reason = "last record cut short";
      break;
    }
    const uint8_t* payload = b + pos + kRecordHeader;
    if (hash(payload, n) != h) {
      p.reason = "record checksum mismatch";
      break;
    }
    pos += kRecordHeader + n;
    p.goodBytes = pos;
    if (seen.insert(h).second) {
      p.entries.push_back(Entry{h, std::string(reinterpret_cast<const char*>(payload), n)});
    } else {
      p.duplicates++;
    }
  }
  p.badTail = p.goodBytes < size;
  return p;
}

void setForceSliced(bool on) { gForceSliced = on; }
bool forceSliced() { return gForceSliced; }
void setNoDefaultFile(bool on) { gNoDefaultFile = on; }
void setNoCommonPrecompile(bool on) { gNoCommon = on; }

#if SK_MILESTONE >= 140  // SkExecutor work lists, ContextOptions::fExecutor
namespace {

// Work list 0 (Graphite's pipelines a frame waits for) before work list 1
// (precompiles).
class CompileExecutor final : public SkExecutor {
 public:
  explicit CompileExecutor(int threads) {
    for (int i = 0; i < threads; i++) threads_.emplace_back([this] { run(); });
  }
  ~CompileExecutor() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
  }
  void add(std::function<void(void)> fn, int workList) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lists_[workList > 0 ? 1 : 0].push_back(std::move(fn));
    }
    cv_.notify_one();
  }
  void add(std::function<void(void)> fn) override { add(std::move(fn), 0); }
  int pending(int workList) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return int(lists_[workList].size()) + running_[workList];
  }

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stopping_ || !lists_[0].empty() || !lists_[1].empty(); });
      // Graphite waits for its tasks before the Context goes: none are left.
      if (stopping_) return;
      const int i = lists_[0].empty() ? 1 : 0;
      std::function<void(void)> fn = std::move(lists_[i].front());
      lists_[i].pop_front();
      running_[i]++;
      lock.unlock();
      fn();
      lock.lock();
      running_[i]--;
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void(void)>> lists_[2];
  int running_[2] = {0, 0};
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

}  // namespace

std::unique_ptr<SkExecutor> makeCompileExecutor() {
  // Compiling is CPU work on the driver's side too: leave cores to the game.
  const int cores = int(std::thread::hardware_concurrency());
  return std::make_unique<CompileExecutor>(std::clamp(cores / 2, 1, 4));
}

int pendingPrecompiles(const SkExecutor* executor) {
  return executor ? static_cast<const CompileExecutor*>(executor)->pending(1) : 0;
}

int pendingCompiles(const SkExecutor* executor) {
  if (!executor) return 0;
  auto* e = static_cast<const CompileExecutor*>(executor);
  return e->pending(0) + e->pending(1);
}
#else
std::unique_ptr<SkExecutor> makeCompileExecutor() { return nullptr; }
int pendingPrecompiles(const SkExecutor*) { return 0; }
int pendingCompiles(const SkExecutor*) { return 0; }
#endif

// ---------------------------------------------------------------- Files

namespace {

#if !defined(__EMSCRIPTEN__)
// The whole file; false when it does not exist or cannot be read. A file
// over kMaxFileBytes reads as its first bytes only (its tail is then bad).
bool readFile(const std::string& path, std::string* out, bool* exists) {
  *exists = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  *exists = true;
  out->clear();
  char buf[64 << 10];
  size_t n;
  while (out->size() < kMaxFileBytes && (n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

// Replaces the file (a temporary file renamed over it).
bool writeFile(const std::string& path, const std::string& bytes) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = std::fclose(f) == 0 && ok;
#if defined(_WIN32)
  if (ok) std::remove(path.c_str());  // rename does not replace on Windows
#endif
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// Appends `rec` with one write (O_APPEND), so a crash leaves at most a torn
// last record. A file that disappeared meanwhile starts again with `header`.
bool appendFile(const std::string& path, const std::string& header, const std::string& rec) {
#if defined(_WIN32)
  FILE* f = std::fopen(path.c_str(), "ab");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  std::string bytes = std::ftell(f) == 0 ? header + rec : rec;
  bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
#else
  const std::string* bytes = &rec;
  std::string fresh;
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) {
    fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    fresh = header + rec;
    bytes = &fresh;
  }
  if (fd < 0) return false;
  ssize_t n;
  do {
    n = ::write(fd, bytes->data(), bytes->size());
  } while (n < 0 && errno == EINTR);
  bool ok = n == ssize_t(bytes->size());
  return ::close(fd) == 0 && ok;
#endif
}
#endif

}  // namespace

// A cache file, shared by the contexts that use the same path (each new
// pipeline is appended once).
class File {
 public:
  // Loads `path` (creating it when missing), repairs it when stale or
  // damaged, and returns its entries. Null when it cannot be used.
  static std::shared_ptr<File> open(const std::string& path, Backend backend, std::vector<Entry>* entries) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)backend;
    (void)entries;
    return nullptr;  // no file system: jsui_gpu_precompile_pipelines / jsui_gpu_save_pipelines
#else
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<File>> registry;
    std::shared_ptr<File> file;
    {
      std::lock_guard<std::mutex> lock(registryMutex);
      file = registry[path].lock();
      if (file && file->backend_ != backend) {
        SkDebugf("jsui: pipeline cache %s is in use by a %s context; use one file per backend\n", path.c_str(),
                 backendName(uint32_t(file->backend_)));
        return nullptr;
      }
      if (!file) {
        file = std::shared_ptr<File>(new File(path, backend));
        registry[path] = file;
      }
    }
    std::lock_guard<std::mutex> lock(file->mutex_);
    std::string bytes;
    bool exists = false;
    if (!readFile(path, &bytes, &exists) && exists) {
      SkDebugf("jsui: pipeline cache %s cannot be read\n", path.c_str());
      return nullptr;
    }
    Parsed p = parse(bytes.data(), bytes.size(), backend);
    bool rewrite = false;
    switch (p.status) {
      case Parsed::Empty: rewrite = true; break;
      case Parsed::Foreign:
        SkDebugf("jsui: %s is not a JSUI pipeline cache; leaving it alone\n", path.c_str());
        return nullptr;
      case Parsed::BadHeader:
      case Parsed::Stale:
        SkDebugf("jsui: pipeline cache %s ignored (%s); starting a new one\n", path.c_str(), p.reason.c_str());
        rewrite = true;
        break;
      case Parsed::Ok:
        if (p.badTail)
          SkDebugf("jsui: pipeline cache %s: dropped a damaged tail (%s) after %zu pipelines\n", path.c_str(),
                   p.reason.c_str(), p.entries.size());
        rewrite = p.badTail || p.duplicates > 0;
        break;
    }
    if (rewrite) {
      std::string out = header(backend);
      for (const Entry& e : p.entries) out += record(e.data.data(), e.data.size());
      if (!writeFile(path, out)) {
        SkDebugf("jsui: pipeline cache %s cannot be written\n", path.c_str());
        return nullptr;
      }
    }
    file->hashes_.clear();
    for (const Entry& e : p.entries) file->hashes_.insert(e.hash);
    file->failed_ = false;
    *entries = std::move(p.entries);
    return file;
#endif
  }

  void append(const Entry& e) {
#if !defined(__EMSCRIPTEN__)
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || hashes_.size() >= kMaxEntries || !hashes_.insert(e.hash).second) return;
    if (!appendFile(path_, header(backend_), record(e.data.data(), e.data.size()))) {
      failed_ = true;  // one line, then no more attempts
      SkDebugf("jsui: pipeline cache %s cannot be appended to; not recording more pipelines\n", path_.c_str());
    }
#else
    (void)e;
#endif
  }

 private:
  File(std::string path, Backend backend) : path_(std::move(path)), backend_(backend) {}

  std::mutex mutex_;
  const std::string path_;
  const Backend backend_;
  std::unordered_set<uint64_t> hashes_;
  bool failed_ = false;
};

// ----------------------------------------------------------- Precompiler

struct Precompiler::Compiler {
#if defined(JSUI_PRECOMPILE)
  std::unique_ptr<skgpu::graphite::PrecompileContext> pc;
  // The context's (fitKey()); pc's SharedContext keeps them alive.
  const skgpu::graphite::Caps* caps = nullptr;
  skgpu::graphite::ShaderCodeDictionary* dict = nullptr;
#endif
};

namespace {

#if defined(JSUI_PRECOMPILE)
void pipelineCallback(void* self, skgpu::graphite::ContextOptions::PipelineCacheOp op, const std::string& label,
                      uint32_t, bool fromPrecompile, sk_sp<SkData> data) {
  // kPipelineFound comes for every cache hit: nothing to do.
  if (op != skgpu::graphite::ContextOptions::PipelineCacheOp::kAddingPipeline) return;
  static_cast<Precompiler*>(self)->onPipelineAdded(label, fromPrecompile, data ? data->data() : nullptr,
                                                   data ? data->size() : 0);
}

// One attachment of a key's render pass, as this device would have it: a
// depth/stencil format becomes the one this device's Graphite picks for the
// same aspects. False when the device cannot render the format at the
// attachment's sample count.
bool fitAttachment(const skgpu::graphite::Caps* caps, skgpu::graphite::AttachmentDesc& a, bool depthStencil,
                   bool* changed) {
  using namespace skgpu::graphite;
  if (a.fFormat == TextureFormat::kUnsupported) return true;  // the pass has no such attachment
  if (depthStencil) {
    SkEnumBitMask<DepthStencilFlags> flags = DepthStencilFlags::kNone;
    if (TextureFormatHasDepth(a.fFormat)) flags |= DepthStencilFlags::kDepth;
    if (TextureFormatHasStencil(a.fFormat)) flags |= DepthStencilFlags::kStencil;
    const TextureFormat f = caps->getDepthStencilFormat(flags);
    if (f == TextureFormat::kUnsupported) return false;
    *changed |= f != a.fFormat;
    a.fFormat = f;
  }
  return caps->isSampleCountSupported(a.fFormat, a.fSampleCount);
}

// A key names its render pass's attachment formats, sample counts and dst
// read strategy, and Graphite precompiles it as given: it checks only that
// the values are in range, and on Vulkan a render pass with a format the
// device lacks is undefined behaviour, not an error. Keys this device
// recorded always fit; a list recorded on another (the shipped defaults) may
// have D24_S8 where this device has only D32F_S8 (AMD's Vulkan drivers), or a
// dst read this device does not do. Returns the key as this device would have
// recorded it, or null when the device cannot build it.
sk_sp<SkData> fitKey(const skgpu::graphite::Caps* caps, skgpu::graphite::ShaderCodeDictionary* dict,
                     sk_sp<SkData> key) {
  using namespace skgpu::graphite;
  GraphicsPipelineDesc pipeline;
  RenderPassDesc pass;
  if (!DataToPipelineDesc(caps, dict, key.get(), &pipeline, &pass)) return nullptr;
  bool changed = false;
  if (!fitAttachment(caps, pass.fColorAttachment, false, &changed) ||
      !fitAttachment(caps, pass.fColorResolveAttachment, false, &changed) ||
      !fitAttachment(caps, pass.fDepthStencilAttachment, true, &changed))
    return nullptr;
  // Multisampling into a single-sampled attachment (no resolve attachment).
  if (pass.fSampleCount != pass.fColorAttachment.fSampleCount && !caps->msaaRenderToSingleSampledSupport())
    return nullptr;
  if (pass.fDstReadStrategy != DstReadStrategy::kNoneRequired &&
      pass.fDstReadStrategy != DstReadStrategy::kTextureCopy &&
      pass.fDstReadStrategy != caps->getDstReadStrategy()) {
    pass.fDstReadStrategy = caps->getDstReadStrategy();
    changed = true;
  }
  return changed ? PipelineDescToData(caps, dict, pipeline, pass) : key;
}
#endif

// JSUI_LOG_PIPELINES: unset / 0 nothing, "compiled" the pipelines frames
// compiled, anything else every pipeline created.
int logLevel() {
  const char* v = std::getenv("JSUI_LOG_PIPELINES");
  if (!v || !*v || std::strcmp(v, "0") == 0) return 0;
  return std::strcmp(v, "compiled") == 0 ? 2 : 1;
}

}  // namespace

Precompiler::Precompiler(Backend backend) : backend_(backend) {}

Precompiler::~Precompiler() { shutdown(); }

void Precompiler::configure(skgpu::graphite::ContextOptions& opts) {
#if defined(JSUI_PRECOMPILE)
  log_ = logLevel();
  opts.fPipelineCallbackContext = this;
  opts.fPipelineCachingCallback = pipelineCallback;
#else
  (void)opts;
#endif
}

namespace {

// The program's file name, for the default cache file's name.
std::string programName() {
  std::string name;
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) name.assign(buf, n);
#elif defined(__APPLE__)
  if (const char* p = getprogname()) name = p;
#elif defined(__linux__)
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) name.assign(buf, size_t(n));
#endif
  size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".exe") == 0 || name.compare(name.size() - 4, 4, ".EXE") == 0))
    name.resize(name.size() - 4);
  for (char& ch : name)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'))
      ch = '_';
  return name.empty() ? std::string("app") : name;
}

bool makeDir(const std::string& path) {
#if defined(_WIN32)
  return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#elif defined(__EMSCRIPTEN__)
  (void)path;
  return false;
#else
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

}  // namespace

std::string defaultFile(Backend backend) {
  if (backend == Backend::BrowserWebGPU) return {};
  const char* env = std::getenv("JSUI_PIPELINE_CACHE");
  if (env && (!std::strcmp(env, "off") || !std::strcmp(env, "0"))) return {};
  std::string dir;
#if defined(_WIN32)
  if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) dir = l;
  if (dir.empty()) return {};
  dir += "\\jsui";
  if (!makeDir(dir)) return {};
  dir += "\\";
#elif defined(__APPLE__)
  // macOS and iOS (HOME is the app's container there).
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  dir = std::string(home) + "/Library/Caches";
  if (!makeDir(dir) || !makeDir(dir += "/jsui")) return {};
  dir += "/";
#elif defined(__linux__) && !defined(__ANDROID__)
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x == '/') {
    dir = x;
  } else if (const char* home = std::getenv("HOME"); home && *home) {
    dir = std::string(home) + "/.cache";
  } else {
    return {};
  }
  if (!makeDir(dir) || !makeDir(dir += "/jsui")) return {};
  dir += "/";
#else
  return {};
#endif
  std::string b = backendName(uint32_t(backend));
  for (char& ch : b) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return dir + "pipelines-" + programName() + "-" + b + ".bin";
}

void Precompiler::attach(skgpu::graphite::Context* ctx, Mode mode, const SkExecutor* executor) {
  executor_ = executor;
#if defined(JSUI_PRECOMPILE)
  if (mode != Mode::None && gForceSliced) mode = Mode::Sliced;
  auto c = std::make_unique<Compiler>();
  if (ctx && mode != Mode::None) {
    c->pc = ctx->makePrecompileContext();
    c->caps = ctx->priv().caps();
    c->dict = ctx->priv().shaderCodeDictionary();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (c->pc) {
      compiler_ = std::move(c);
      mode_ = mode;
    }
    kickLocked();
  }
  const char* env = std::getenv("JSUI_PIPELINE_CACHE");
  if (env && *env && std::strcmp(env, "off") && std::strcmp(env, "0")) {
    openFile(env);
    envFile_ = env;
  } else if (mode != Mode::None && !gNoDefaultFile) {
    // Until the host names its own (jsui_gpu_set_pipeline_cache).
    std::string path = defaultFile(backend_);
    if (!path.empty()) openFile(path);
  }
#else
  (void)ctx;
  (void)mode;
#endif
}

void Precompiler::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  if (worker_.joinable()) worker_.join();
  compiler_.reset();
}

int Precompiler::setFile(const std::string& path) {
  if (!envFile_.empty()) {
    SkDebugf("jsui: JSUI_PIPELINE_CACHE=%s overrides the pipeline cache %s\n", envFile_.c_str(), path.c_str());
    return 0;
  }
  return openFile(path);
}

int Precompiler::openFile(const std::string& path) {
  std::vector<Entry> entries;
  std::shared_ptr<File> file = path.empty() ? nullptr : File::open(path, backend_, &entries);
  std::vector<Entry> created;
  int n;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    file_ = file;
    if (!file) return path.empty() ? 0 : -1;
    created = created_;  // pipelines created before the file was set belong in it too
    n = queueLocked(std::move(entries));
    kickLocked();
  }
  for (const Entry& e : created) file->append(e);
  return n;
}

int Precompiler::addList(const void* data, size_t size) {
  if (!data || !size) return -1;
  Parsed p = parse(data, size, backend_);
  if (p.status != Parsed::Ok) {
    SkDebugf("jsui: pipeline list ignored (%s)\n", p.reason.empty() ? "empty" : p.reason.c_str());
    return -1;
  }
  if (p.badTail) SkDebugf("jsui: pipeline list: dropped a damaged tail (%s)\n", p.reason.c_str());
  std::lock_guard<std::mutex> lock(mutex_);
  int n = queueLocked(std::move(p.entries));
  kickLocked();
  return n;
}

std::string Precompiler::save() {
  std::string out = header(backend_);
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Entry& e : entries_) out += record(e.data.data(), e.data.size());
  return out;
}

int Precompiler::queueLocked(std::vector<Entry>&& entries) {
  int n = 0;
  for (Entry& e : entries) {
    if (entries_.size() >= kMaxEntries) break;
    if (!known_.insert(e.hash).second) continue;
    entries_.push_back(e);
    queue_.push_back(std::move(e));
    n++;
  }
  if (n) reported_ = false;
  return n;
}

void Precompiler::kickLocked() {
  if (mode_ != Mode::Thread || !compiler_ || stopping_ || running_ || queue_.empty()) return;
  // A previous worker that drained the queue cleared running_ as its last
  // step under the lock: joining it does not wait for the lock.
  if (worker_.joinable()) worker_.join();
  running_ = true;
  worker_ = std::thread([this] { run(); });
}

// The queue drained: one summary line when keys failed (or JSUI_LOG_PIPELINES).
void Precompiler::doneLocked() {
  if (reported_ || inFlight_ || !queue_.empty()) return;
  reported_ = true;
  if (stats_.failed || log_ == 1)
    SkDebugf("jsui: precompiled %llu pipelines in %.0f ms (%s), %llu keys rejected by this Skia / device\n",
             (unsigned long long)stats_.precompiled, precompileMs_, mode_ == Mode::Thread ? "worker thread" : "sliced",
             (unsigned long long)stats_.failed);
}

void Precompiler::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_ && !queue_.empty()) {
    // Keys go to the executor a few at a time: Graphite compiles everything
    // handed over before the Context goes, so a context destroyed early
    // waits for a few compiles, not the whole list.
    while (!stopping_ && pendingPrecompiles(executor_) >= 4) {
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
    }
    if (stopping_) break;
    Entry e = std::move(queue_.front());
    queue_.pop_front();
    inFlight_ = 1;
    lock.unlock();
    const Clock::time_point t0 = Clock::now();
    const bool ok = compile(e);
    const double ms = msSince(t0);
    lock.lock();
    inFlight_ = 0;
    precompileMs_ += ms;
    ok ? stats_.precompiled++ : stats_.failed++;
  }
  doneLocked();
  running_ = false;
}

bool Precompiler::compile(const Entry& e) {
#if defined(JSUI_PRECOMPILE)
  // A key that does not decode or that this device cannot build fails.
  sk_sp<SkData> key = fitKey(compiler_->caps, compiler_->dict, SkData::MakeWithCopy(e.data.data(), e.data.size()));
  return key && compiler_->pc->precompile(std::move(key));
#else
  (void)e;
  return false;
#endif
}

int Precompiler::slice(double budgetMs) {
  const Clock::time_point t0 = Clock::now();
  std::unique_lock<std::mutex> lock(mutex_);
  if (mode_ != Mode::Sliced || !compiler_) return int(queue_.size()) + inFlight_;
  while (!stopping_ && !queue_.empty()) {
    Entry e = std::move(queue_.front());
    queue_.pop_front();
    inFlight_ = 1;
    lock.unlock();
    const Clock::time_point t1 = Clock::now();
    const bool ok = compile(e);
    const double ms = msSince(t1);
    lock.lock();
    inFlight_ = 0;
    precompileMs_ += ms;
    ok ? stats_.precompiled++ : stats_.failed++;
    if (msSince(t0) >= budgetMs) break;
  }
  doneLocked();
  return int(queue_.size());
}

int Precompiler::step(double budgetMs) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hostSteps_ = true;
    if (mode_ != Mode::Sliced) return int(queue_.size()) + inFlight_;
  }
  return slice(budgetMs);
}

void Precompiler::afterFrame() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != Mode::Sliced || hostSteps_ || queue_.empty()) return;
    const Clock::time_point now = Clock::now();
    if (now - lastSlice_ < kSliceInterval) return;
    lastSlice_ = now;
  }
  slice(kFrameSliceMs);
}

Stats Precompiler::stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats s = stats_;
  s.mode = int(mode_);
  s.queued = int(queue_.size()) + inFlight_ + pendingPrecompiles(executor_);
  s.known = entries_.size();
  return s;
}

void Precompiler::precompileCommon(SkColorType ct, const sk_sp<SkColorSpace>& cs) {
#if defined(JSUI_PRECOMPILE_COMMON)
  using namespace skgpu::graphite;
  if (!executor_ || ct == kUnknown_SkColorType || gNoCommon) return;
  static const bool off = [] {
    const char* v = std::getenv("JSUI_PRECOMPILE_COMMON");
    return v && (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0);
  }();
  if (off) return;
  // The render thread's (shutdown runs there too): pc outlives this call.
  PrecompileContext* pc = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != Mode::Thread || !compiler_ || stopping_) return;
    const std::pair<SkColorType, uint32_t> target{ct, cs ? cs->hash() : 0};
    for (const auto& t : common_)
      if (t == target) return;
    common_.push_back(target);
    pc = compiler_->pc.get();
  }

  // What the painter draws on the frame's target: one sample, a depth buffer
  // (Graphite orders draws with it), no stencil.
  RenderPassProperties rpp;
  rpp.fDSFlags = DepthStencilFlags::kDepth;
  rpp.fDstCT = ct;
  rpp.fDstCS = cs;
  const RenderPassProperties passes[] = {rpp};

  // Images as the image cache uploads them (premultiplied RGBA in the
  // target's color space, clamped, no mipmaps); gradients of up to 4 stops,
  // interpolated premultiplied like the painter's.
  const SkColorInfo imageInfo(kRGBA_8888_SkColorType, kPremul_SkAlphaType, cs);
  SkGradient::Interpolation interp;
  interp.fInPremul = SkGradient::Interpolation::InPremul::kYes;
  const auto small = PrecompileShaders::GradientShaderFlags::kSmall;
  // Hardware sampling only (no tile modes): the painter's images are
  // clamped textures, never shader-tiled.
  sk_sp<PrecompileShader> image =
      PrecompileShaders::Image(PrecompileShaders::ImageShaderFlags::kNone, {&imageInfo, 1}, {});
  sk_sp<PrecompileShader> linear = PrecompileShaders::LinearGradient(small, interp);
  sk_sp<PrecompileShader> radial = PrecompileShaders::RadialGradient(small, interp);

  const auto rect = DrawTypeFlags::kNonAAFillRect, rrect = DrawTypeFlags::kAnalyticRRect,
             quad = DrawTypeFlags::kPerEdgeAAQuad;
  // The pool takes them in this order (a first launch's first frames race
  // it): everything unclipped first, then the same inside an analytic clip
  // (rounded overflow, scroll views).
  for (DrawTypeFlags clip : {DrawTypeFlags::kNone, DrawTypeFlags::kAnalyticClip}) {
    auto precompile = [&](std::initializer_list<sk_sp<PrecompileShader>> shaders, SkBlendMode blend, bool opaque,
                          DrawTypeFlags draws) {
      PaintOptions o;
      std::vector<sk_sp<PrecompileShader>> list(shaders);
      o.setShaders(list);
      o.setBlendModes({&blend, 1});
      o.setPaintColorIsOpaque(opaque);
      Precompile(pc, o, DrawTypeFlags(draws | clip), passes);
    };
    // Clears (every frame's first draw; also of damage rects): never clipped.
    if (clip == DrawTypeFlags::kNone) precompile({}, SkBlendMode::kSrc, true, rect);
    // Solid fills (no shader: the paint's color) and text.
    precompile({}, SkBlendMode::kSrcOver, true, DrawTypeFlags(rect | rrect | DrawTypeFlags::kBitmapText_Mask));
    // Images (also as AA quads: drawImageRect) and gradients.
    precompile({image}, SkBlendMode::kSrcOver, true, DrawTypeFlags(rect | rrect | quad));
    precompile({linear, radial}, SkBlendMode::kSrcOver, true, DrawTypeFlags(rect | rrect));
    // The same under a translucent paint (opacity): the shader times the
    // paint's alpha.
    precompile({image, radial}, SkBlendMode::kSrcOver, false, DrawTypeFlags(rect | rrect | quad));
    // Blurred rrects (box shadows the GPU draws analytically).
    Precompile(pc, PaintOptions(), DrawTypeFlags(DrawTypeFlags::kDropShadows | clip), passes);
    // Copies.
    if (clip == DrawTypeFlags::kNone) precompile({image, linear}, SkBlendMode::kSrc, true, rect);
  }
  if (log_ == 1) SkDebugf("jsui: common pipelines handed to the compile pool (color type %d)\n", int(ct));
#else
  (void)ct;
  (void)cs;
#endif
}

uint64_t Precompiler::takeCompiles() {
  const uint64_t now = compiles_.load();
  return now - compilesTaken_.exchange(now);
}

void Precompiler::onPipelineAdded(const std::string& label, bool fromPrecompile, const void* data, size_t size) {
  if (fromPrecompile) {  // its key came from a list or file
    if (log_ == 1) SkDebugf("jsui: pipeline precompiled: %s\n", label.c_str());
    return;
  }
  compiles_++;
  const char* what = "not serializable";
  std::shared_ptr<File> file;
  Entry e;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.compiled++;
    if (!data || !size || size > kMaxRecord) {
      // Keys with runtime effects Graphite does not know by a stable id.
      stats_.unserializable++;
    } else if (entries_.size() >= kMaxEntries) {
      what = "not recorded, the cache is full";
    } else if (e.hash = hash(data, size); !known_.insert(e.hash).second) {
      // Queued but not precompiled yet, or evicted and compiled again.
      what = "known";
    } else {
      what = "new";
      e.data.assign(static_cast<const char*>(data), size);
      entries_.push_back(e);
      created_.push_back(e);
      stats_.added++;
      file = file_;
    }
  }
  if (log_) SkDebugf("jsui: pipeline compiled on demand by a frame or a shader warm-up (%s): %s\n", what, label.c_str());
  if (file) file->append(e);
}

std::string Precompiler::swapDepthStencilForTest(const std::string& key) {
#if defined(JSUI_PRECOMPILE)
  using namespace skgpu::graphite;
  if (!compiler_) return {};
  GraphicsPipelineDesc pipeline;
  RenderPassDesc pass;
  sk_sp<SkData> data = SkData::MakeWithCopy(key.data(), key.size());
  if (!DataToPipelineDesc(compiler_->caps, compiler_->dict, data.get(), &pipeline, &pass)) return {};
  TextureFormat& f = pass.fDepthStencilAttachment.fFormat;
  switch (f) {
    case TextureFormat::kD16: f = TextureFormat::kD32F; break;
    case TextureFormat::kD32F: f = TextureFormat::kD16; break;
    case TextureFormat::kD24_S8: f = TextureFormat::kD32F_S8; break;
    case TextureFormat::kD32F_S8: f = TextureFormat::kD24_S8; break;
    default: return {};
  }
  data = PipelineDescToData(compiler_->caps, compiler_->dict, pipeline, pass);
  return data ? std::string(static_cast<const char*>(data->data()), data->size()) : std::string();
#else
  (void)key;
  return {};
#endif
}

}  // namespace pipeline_cache
}  // namespace jsui
