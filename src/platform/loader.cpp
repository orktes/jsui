#include "platform/loader.h"

#include <fstream>
#include <iterator>

namespace jsui {

struct ResourceLoader::Worker {};

ResourceLoader::ResourceLoader(JsuiRuntime* rt, JsuiLoadFn custom, void* customUser)
    : rt_(rt), custom_(custom), customUser_(customUser) {}

ResourceLoader::~ResourceLoader() = default;

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
  // Strip query/fragment and file name from the base path.
  path = path.substr(0, path.find_first_of("?#"));
  std::string dir = url[0] == '/' ? "" : path.substr(0, path.rfind('/') + 1);
  std::string joined = dir + url;
  // Normalize ./ and ../ segments.
  std::vector<std::string> segs;
  size_t i = 0;
  bool absolute = !joined.empty() && joined[0] == '/';
  while (i <= joined.size()) {
    size_t j = joined.find('/', i);
    if (j == std::string::npos) j = joined.size();
    std::string seg = joined.substr(i, j - i);
    if (seg == "..") {
      if (!segs.empty() && segs.back() != "..") segs.pop_back();
      else if (!absolute) segs.push_back(seg);
    } else if (seg != "." && !(seg.empty() && j != joined.size())) {
      segs.push_back(seg);
    }
    i = j + 1;
  }
  std::string out = absolute ? "/" : "";
  for (size_t k = 0; k < segs.size(); k++) {
    if (k) out += '/';
    out += segs[k];
  }
  return prefix + out;
}

uint64_t ResourceLoader::load(const std::string& url, Callback cb) {
  uint64_t id = nextId_++;
  pending_[id] = std::move(cb);
  if (custom_) {
    custom_(customUser_, rt_, id, url.c_str());
  } else {
    builtinLoad(id, url);
  }
  return id;
}

void ResourceLoader::builtinLoad(uint64_t id, const std::string& url) {
  Resource res;
  res.url = url;
  std::string path = url;
  if (path.rfind("file://", 0) == 0) path = path.substr(7);
  if (path.find("://") != std::string::npos) {
    res.status = 0;  // network backends are added per platform
    complete(id, std::move(res));
    return;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    res.status = 404;
  } else {
    res.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    res.status = 200;
  }
  complete(id, std::move(res));
}

void ResourceLoader::complete(uint64_t id, Resource res) {
  std::lock_guard<std::mutex> lock(mutex_);
  done_.emplace_back(id, std::move(res));
}

int ResourceLoader::pump() {
  std::vector<std::pair<uint64_t, Resource>> done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done.swap(done_);
  }
  int n = 0;
  for (auto& [id, res] : done) {
    auto it = pending_.find(id);
    if (it == pending_.end()) continue;
    Callback cb = std::move(it->second);
    pending_.erase(it);
    cb(std::move(res));
    n++;
  }
  return n;
}

void ResourceLoader::cancelAll() { pending_.clear(); }

bool ResourceLoader::busy() const { return !pending_.empty(); }

}  // namespace jsui
