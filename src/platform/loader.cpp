#include "platform/loader.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <thread>

#include "net/http_client.h"

namespace jsui {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int a = hexVal(s[i + 1]), b = hexVal(s[i + 2]);
      if (a >= 0 && b >= 0) {
        out += char(a * 16 + b);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(in.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(uint8_t((acc >> bits) & 0xff));
    }
  }
  return true;
}

std::string guessContentType(const std::string& path) {
  std::string p = lower(path.substr(0, path.find_first_of("?#")));
  auto dot = p.rfind('.');
  std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
  static const struct {
    const char* ext;
    const char* type;
  } kTypes[] = {{"js", "text/javascript"},  {"mjs", "text/javascript"}, {"json", "application/json"},
                {"html", "text/html"},      {"css", "text/css"},        {"txt", "text/plain"},
                {"png", "image/png"},       {"jpg", "image/jpeg"},      {"jpeg", "image/jpeg"},
                {"webp", "image/webp"},     {"gif", "image/gif"},       {"svg", "image/svg+xml"},
                {"ttf", "font/ttf"},        {"otf", "font/otf"},        {"woff", "font/woff"},
                {"woff2", "font/woff2"},    {"wasm", "application/wasm"}, {"map", "application/json"}};
  for (auto& t : kTypes)
    if (ext == t.ext) return t.type;
  return "application/octet-stream";
}

// file:// URL or plain path -> filesystem path.
std::string filePath(const std::string& url) {
  if (url.rfind("file://", 0) != 0) return url;
  std::string p = url.substr(7);
  if (p.rfind("localhost/", 0) == 0) p = p.substr(9);
  p = percentDecode(p.substr(0, p.find_first_of("?#")));
#if defined(_WIN32)
  if (p.size() >= 3 && p[0] == '/' && p[2] == ':') p = p.substr(1);  // /C:/x -> C:/x
#endif
  return p;
}

void readFile(const std::string& url, Resource* res) {
  std::string path = filePath(url);
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    res->status = 404;
    res->statusText = "Not Found";
    res->error = "cannot open file '" + path + "'";
    return;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {  // not seekable (e.g. a directory or pipe)
    std::fclose(f);
    res->status = 404;
    res->error = "cannot read '" + path + "'";
    return;
  }
  res->data.resize(size_t(size));
  size_t got = size ? std::fread(res->data.data(), 1, size_t(size), f) : 0;
  bool err = std::ferror(f) != 0;
  std::fclose(f);
  if (err || got != size_t(size)) {
    res->data.clear();
    res->status = 404;
    res->error = "cannot read '" + path + "' (is it a directory?)";
    return;
  }
  res->status = 200;
  res->statusText = "OK";
  res->contentType = guessContentType(path);
}

}  // namespace

// Small on-demand thread pool. Threads are spawned when a job is queued and
// all existing threads are busy (up to kMaxThreads), so a slow HTTP request
// never blocks file reads behind it.
struct ResourceLoader::Worker {
  static constexpr int kMaxThreads = 4;
  struct Job {
    uint64_t id;
    LoadRequest req;
    std::shared_ptr<std::atomic<bool>> cancel;
  };
  ResourceLoader* owner;
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job> jobs;
  std::vector<std::thread> threads;
  std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> inFlight;
  int idle = 0;
  bool stop = false;
  std::atomic<bool> stopFlag{false};

  explicit Worker(ResourceLoader* o) : owner(o) {}
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(m);
      stop = true;
      stopFlag = true;
      for (auto& [id, c] : inFlight) *c = true;
    }
    cv.notify_all();
    for (auto& t : threads) t.join();
  }

  void push(uint64_t id, LoadRequest req) {
    std::lock_guard<std::mutex> lock(m);
    auto c = std::make_shared<std::atomic<bool>>(false);
    inFlight[id] = c;
    jobs.push_back({id, std::move(req), c});
    if (idle == 0 && int(threads.size()) < kMaxThreads) threads.emplace_back([this] { run(); });
    else cv.notify_one();
  }

  void cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(m);
    auto it = inFlight.find(id);
    if (it != inFlight.end()) *it->second = true;
  }

  void cancelAll() {
    std::lock_guard<std::mutex> lock(m);
    for (auto& [id, c] : inFlight) *c = true;
  }

  void run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(m);
        idle++;
        cv.wait(lock, [&] { return stop || !jobs.empty(); });
        idle--;
        if (stop) return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      Resource res;
      res.url = job.req.url;
      if (!*job.cancel) execute(job, &res);
      {
        std::lock_guard<std::mutex> lock(m);
        inFlight.erase(job.id);
      }
      if (!*job.cancel) owner->complete(job.id, std::move(res));
    }
  }

  void execute(const Job& job, Resource* res) {
    const std::string& url = job.req.url;
    if (lower(url.substr(0, 7)) == "http://" || lower(url.substr(0, 8)) == "https://") {
      net::HttpRequest hr;
      hr.method = job.req.method;
      hr.url = url;
      hr.headers = job.req.headers;
      hr.body = job.req.body;
      if (job.req.timeoutMs > 0) hr.timeoutMs = job.req.timeoutMs;
      net::HttpResponse r = net::httpRequest(hr, job.cancel.get());
      res->status = r.status;
      res->statusText = std::move(r.statusText);
      res->url = std::move(r.url);
      res->headers = std::move(r.headers);
      res->data = std::move(r.body);
      res->error = std::move(r.error);
      for (auto& [k, v] : res->headers)
        if (lower(k) == "content-type") res->contentType = v;
      return;
    }
    if (url.find("://") != std::string::npos && url.rfind("file://", 0) != 0) {
      res->status = 0;
      res->error = "unsupported URL scheme in " + url;
      return;
    }
    readFile(url, res);
  }
};

ResourceLoader::ResourceLoader(JsuiRuntime* rt, JsuiLoadFn custom, void* customUser)
    : rt_(rt), custom_(custom), customUser_(customUser) {}

ResourceLoader::~ResourceLoader() {
  // Join workers before the queues they complete into go away.
  worker_.reset();
}

std::string ResourceLoader::resolve(const std::string& base, const std::string& url) {
  if (url.empty()) return base;
  if (url.find("://") != std::string::npos || url.rfind("data:", 0) == 0) return url;
  if (base.empty()) return url;
  if (url.rfind("//", 0) == 0) {
    auto p = base.find("://");
    return (p == std::string::npos ? std::string("http:") : base.substr(0, p + 1)) + url;
  }
  std::string prefix, path;
  auto scheme = base.find("://");
  if (scheme != std::string::npos) {
    auto slash = base.find('/', scheme + 3);
    prefix = slash == std::string::npos ? base : base.substr(0, slash);
    path = slash == std::string::npos ? "/" : base.substr(slash);
  } else {
    path = base;
  }
  // A query-only or fragment-only reference keeps the base path.
  path = path.substr(0, path.find_first_of("?#"));
  if (url[0] == '?' || url[0] == '#') return prefix + path + url;
  std::string query;
  std::string rel = url;
  if (auto q = rel.find_first_of("?#"); q != std::string::npos) {
    query = rel.substr(q);
    rel = rel.substr(0, q);
  }
  std::string dir = rel[0] == '/' ? "" : path.substr(0, path.rfind('/') + 1);
  std::string joined = dir + rel;
  // Normalize ./ and ../ segments.
  std::vector<std::string> segs;
  size_t i = 0;
  bool absolute = !joined.empty() && joined[0] == '/';
  while (i <= joined.size()) {
    size_t j = joined.find('/', i);
    if (j == std::string::npos) j = joined.size();
    std::string seg = joined.substr(i, j - i);
    bool last = j == joined.size();
    if (seg == "..") {
      if (!segs.empty() && segs.back() != ".." && !segs.back().empty()) segs.pop_back();
      else if (!absolute) segs.push_back(seg);
      if (last) segs.push_back("");
    } else if (seg == ".") {
      if (last) segs.push_back("");
    } else if (!(seg.empty() && !last)) {
      segs.push_back(seg);
    }
    i = j + 1;
  }
  std::string out = absolute ? "/" : "";
  for (size_t k = 0; k < segs.size(); k++) {
    if (k) out += '/';
    out += segs[k];
  }
  return prefix + out + query;
}

bool ResourceLoader::decodeDataUri(const std::string& uri, Resource* out) {
  out->url = uri;
  if (uri.rfind("data:", 0) != 0) return false;
  auto comma = uri.find(',');
  if (comma == std::string::npos) {
    out->status = 0;
    out->error = "malformed data: URI (no comma)";
    return false;
  }
  std::string meta = uri.substr(5, comma - 5);
  std::string payload = uri.substr(comma + 1);
  bool b64 = false;
  std::string type;
  size_t p = 0;
  while (p <= meta.size()) {
    size_t e = meta.find(';', p);
    if (e == std::string::npos) e = meta.size();
    std::string part = meta.substr(p, e - p);
    if (p == 0) type = part;
    else if (lower(part) == "base64") b64 = true;
    else if (!part.empty()) type += ";" + part;
    p = e + 1;
  }
  if (type.empty() || type[0] == ';') type = "text/plain" + std::string(type.empty() ? ";charset=US-ASCII" : type);
  out->contentType = type;
  if (b64) {
    if (!base64Decode(percentDecode(payload), &out->data)) {
      out->status = 0;
      out->error = "malformed base64 in data: URI";
      out->data.clear();
      return false;
    }
  } else {
    std::string d = percentDecode(payload);
    out->data.assign(d.begin(), d.end());
  }
  out->status = 200;
  out->statusText = "OK";
  return true;
}

uint64_t ResourceLoader::load(const std::string& url, Callback cb) {
  LoadRequest req;
  req.url = url;
  return load(std::move(req), std::move(cb));
}

uint64_t ResourceLoader::load(LoadRequest req, Callback cb) {
  uint64_t id = nextId_++;
  pending_[id] = {std::move(cb), req.url};
  if (req.url.rfind("data:", 0) == 0) {
    // Decoded inline on every platform, but still delivered from pump() so
    // callbacks never run re-entrantly inside load().
    Resource res;
    decodeDataUri(req.url, &res);
    complete(id, std::move(res));
  } else if (custom_) {
    custom_(customUser_, rt_, id, req.url.c_str());
  } else {
    builtinLoad(id, std::move(req));
  }
  return id;
}

#if defined(__EMSCRIPTEN__)
// Emscripten backend hook: the wasm build implements browser fetch here.
void ResourceLoader::builtinLoad(uint64_t id, LoadRequest req) {
  Resource res;
  res.url = req.url;
  res.error = "no browser loader backend compiled in";
  complete(id, std::move(res));
}
#else
void ResourceLoader::builtinLoad(uint64_t id, LoadRequest req) {
  if (!worker_) worker_ = std::make_unique<Worker>(this);
  worker_->push(id, std::move(req));
}
#endif

void ResourceLoader::complete(uint64_t id, Resource res) {
  std::lock_guard<std::mutex> lock(mutex_);
  done_.emplace_back(id, std::move(res));
}

int ResourceLoader::pump() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty()) return 0;
    doneScratch_.swap(done_);
  }
  int n = 0;
  for (auto& [id, res] : doneScratch_) {
    auto it = pending_.find(id);
    if (it == pending_.end()) continue;
    Callback cb = std::move(it->second.cb);
    if (res.url.empty()) res.url = it->second.url;  // embedder completions carry no URL
    if (res.status == 0 && res.error.empty()) res.error = "failed to load " + res.url;
    pending_.erase(it);
    cb(std::move(res));
    n++;
  }
  doneScratch_.clear();
  return n;
}

void ResourceLoader::cancel(uint64_t id) {
  pending_.erase(id);
  if (worker_) worker_->cancel(id);
}

void ResourceLoader::cancelAll() {
  pending_.clear();
  if (worker_) worker_->cancelAll();
}

bool ResourceLoader::busy() const { return !pending_.empty(); }

}  // namespace jsui
