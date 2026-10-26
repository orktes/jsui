// The pipeline cache's file format and files (src/render/pipeline_cache.h):
// round trips, de-duplication, torn tails, damaged / stale / foreign headers
// and the repairs a load makes. No GPU: entries are opaque bytes here (the
// Graphite side is in vulkan_test.cpp).
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/pipeline_cache.h"
#include "test.h"

using namespace jsui::pipeline_cache;

namespace {

std::string key(int i) { return "skiapipe-key-" + std::to_string(i) + std::string(size_t(40 + i), char('a' + i % 26)); }

std::string list(Backend b, const std::vector<std::string>& keys) {
  std::string s = header(b);
  for (const std::string& k : keys) s += record(k.data(), k.size());
  return s;
}

Parsed parseStr(const std::string& s, Backend b = Backend::Vulkan) { return parse(s.data(), s.size(), b); }

// A header with fields of our choosing and a valid checksum (FNV-1a 32).
std::string craftHeader(uint32_t version, uint32_t milestone, uint32_t backend, const std::string& id) {
  auto put = [](std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
  };
  std::string h = "JSUIPIPE";
  put(h, version);
  put(h, uint32_t(32 + id.size()));
  put(h, milestone);
  put(h, backend);
  put(h, uint32_t(id.size()));
  h += id;
  uint32_t c = 2166136261u;
  for (unsigned char ch : h) c = (c ^ ch) * 16777619u;
  put(h, c);
  return h;
}

std::string tempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / ("jsui-test-" + std::string(name) + ".bin")).string();
}

std::string readAll(const std::string& path) {
  std::string s;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}

void writeAll(const std::string& path, const std::string& s) {
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
}

}  // namespace

TEST(pipeline_cache_format_round_trip) {
  std::vector<std::string> keys = {key(0), key(1), key(2)};
  std::string s = list(Backend::Vulkan, keys);
  Parsed p = parseStr(s);
  CHECK_EQ(int(p.status), int(Parsed::Ok));
  CHECK_EQ(p.entries.size(), size_t(3));
  for (size_t i = 0; i < keys.size() && i < p.entries.size(); i++) {
    CHECK(p.entries[i].data == keys[i]);
    CHECK_EQ(p.entries[i].hash, hash(keys[i].data(), keys[i].size()));
  }
  CHECK_EQ(p.goodBytes, s.size());
  CHECK(!p.badTail);
  // A header alone: a valid, empty cache.
  Parsed e = parseStr(header(Backend::Vulkan));
  CHECK_EQ(int(e.status), int(Parsed::Ok));
  CHECK(e.entries.empty());
  CHECK_EQ(int(parseStr("").status), int(Parsed::Empty));
  // The build id names the Skia milestone.
  CHECK(buildId().rfind("skia m", 0) == 0);
}

TEST(pipeline_cache_format_dedupes) {
  std::string s = list(Backend::Metal, {key(0), key(1), key(0), key(2), key(1)});
  Parsed p = parseStr(s, Backend::Metal);
  CHECK_EQ(int(p.status), int(Parsed::Ok));
  CHECK_EQ(p.entries.size(), size_t(3));
  CHECK_EQ(p.duplicates, size_t(2));
  CHECK(p.entries.size() == 3 && p.entries[2].data == key(2));
  CHECK(!p.badTail);
}

TEST(pipeline_cache_format_drops_a_torn_tail) {
  std::string good = list(Backend::Vulkan, {key(0), key(1)});
  std::string last = record(key(2).data(), key(2).size());
  // Cut the last record anywhere: in its size, its hash or its payload.
  for (size_t cut : {size_t(1), size_t(4), size_t(7), size_t(12), last.size() - 1}) {
    std::string s = good + last.substr(0, cut);
    Parsed p = parseStr(s);
    CHECK_EQ(int(p.status), int(Parsed::Ok));
    CHECK_EQ(p.entries.size(), size_t(2));
    CHECK(p.badTail);
    CHECK_EQ(p.goodBytes, good.size());
  }
  // A record whose payload does not match its hash ends the file there.
  std::string bad = last;
  bad[bad.size() - 3] ^= 0x40;
  Parsed p = parseStr(good + bad + record(key(3).data(), key(3).size()));
  CHECK_EQ(p.entries.size(), size_t(2));
  CHECK(p.badTail);
  CHECK_EQ(p.goodBytes, good.size());
  // A size field that is zero or absurd.
  std::string zero = good + std::string(12, '\0');
  CHECK_EQ(parseStr(zero).entries.size(), size_t(2));
  CHECK(parseStr(zero).badTail);
  std::string huge = good + std::string("\xff\xff\xff\x7f", 4) + std::string(8, 'x') + std::string(64, 'y');
  CHECK_EQ(parseStr(huge).entries.size(), size_t(2));
  CHECK(parseStr(huge).badTail);
}

TEST(pipeline_cache_format_rejects_bad_headers) {
  std::string s = list(Backend::Vulkan, {key(0)});
  // Not ours at all.
  CHECK_EQ(int(parseStr("PK\x03\x04 a zip file, say").status), int(Parsed::Foreign));
  CHECK_EQ(int(parseStr("{").status), int(Parsed::Foreign));
  // Ours, cut inside the header.
  CHECK_EQ(int(parseStr(s.substr(0, 5)).status), int(Parsed::BadHeader));
  CHECK_EQ(int(parseStr(s.substr(0, 20)).status), int(Parsed::BadHeader));
  CHECK_EQ(int(parseStr(header(Backend::Vulkan).substr(0, header(Backend::Vulkan).size() - 1)).status),
           int(Parsed::BadHeader));
  // A flipped bit in the build id: the checksum catches it.
  std::string flipped = s;
  flipped[30] ^= 0x01;
  Parsed p = parseStr(flipped);
  CHECK_EQ(int(p.status), int(Parsed::BadHeader));
  CHECK(p.entries.empty());
  // A header size field pointing past the header.
  std::string sized = s;
  sized[12] = char(sized[12] + 4);
  CHECK_EQ(int(parseStr(sized).status), int(Parsed::BadHeader));
}

TEST(pipeline_cache_format_detects_stale_files) {
  const std::string rec = record(key(0).data(), key(0).size());
  const std::string own = header(Backend::Vulkan);
  const uint32_t milestone = uint32_t(uint8_t(own[16])) | uint32_t(uint8_t(own[17])) << 8 |
                             uint32_t(uint8_t(own[18])) << 16 | uint32_t(uint8_t(own[19])) << 24;
  // The crafted header of this build is ours byte for byte.
  CHECK(craftHeader(kFormatVersion, milestone, 1, buildId()) == own);
  // Another format version.
  Parsed v = parseStr(craftHeader(kFormatVersion + 1, milestone, 1, buildId()) + rec);
  CHECK_EQ(int(v.status), int(Parsed::Stale));
  CHECK(v.entries.empty());
  // Another Skia milestone, or another build of the same milestone.
  Parsed b = parseStr(craftHeader(kFormatVersion, milestone - 1, 1, "skia m" + std::to_string(milestone - 1)) + rec);
  CHECK_EQ(int(b.status), int(Parsed::Stale));
  CHECK(b.entries.empty());
  Parsed id = parseStr(craftHeader(kFormatVersion, milestone, 1, buildId() + " 0123abcd") + rec);
  CHECK_EQ(int(id.status), int(Parsed::Stale));
  CHECK(id.reason.find("0123abcd") != std::string::npos);
  // Another backend's file.
  Parsed m = parseStr(list(Backend::Metal, {key(0)}), Backend::Vulkan);
  CHECK_EQ(int(m.status), int(Parsed::Stale));
  CHECK(m.reason.find("Metal") != std::string::npos);
}

// Files: a load repairs what it can and never overwrites a stranger's file.
TEST(pipeline_cache_files_repair_and_append) {
  const std::string path = tempPath("pipelines");
  std::remove(path.c_str());
  {
    Precompiler pc(Backend::Vulkan);  // no Graphite context: records, never compiles
    CHECK_EQ(pc.setFile(path), 0);    // created with a header
    CHECK(readAll(path) == header(Backend::Vulkan));
    std::string k0 = key(0), k1 = key(1);
    pc.onPipelineAdded("a", false, k0.data(), k0.size());
    pc.onPipelineAdded("b", false, k1.data(), k1.size());
    pc.onPipelineAdded("a again", false, k0.data(), k0.size());  // evicted and compiled again
    pc.onPipelineAdded("precompiled", true, k1.data(), k1.size());
    pc.onPipelineAdded("runtime effect", false, nullptr, 0);
    Stats s = pc.stats();
    CHECK_EQ(s.compiled, uint64_t(4));
    CHECK_EQ(s.added, uint64_t(2));
    CHECK_EQ(s.unserializable, uint64_t(1));
    CHECK_EQ(s.known, uint64_t(2));
    CHECK_EQ(pc.takeCompiles(), uint64_t(4));
    CHECK_EQ(pc.takeCompiles(), uint64_t(0));
    CHECK(readAll(path) == list(Backend::Vulkan, {k0, k1}));
  }
  {
    // The next session queues them; a pipeline created before setFile is
    // appended when the file is set.
    Precompiler pc(Backend::Vulkan);
    std::string k2 = key(2);
    pc.onPipelineAdded("early", false, k2.data(), k2.size());
    CHECK_EQ(pc.setFile(path), 2);
    CHECK_EQ(pc.stats().queued, 2);
    CHECK_EQ(pc.stats().known, uint64_t(3));
    CHECK(readAll(path) == list(Backend::Vulkan, {key(0), key(1), k2}));
    // save() lists everything known, in the file format.
    std::string saved = pc.save();
    CHECK_EQ(parseStr(saved).entries.size(), size_t(3));
  }
  {
    // A crash in the middle of an append: the torn record goes, the file is
    // rewritten without it and appends continue after the good records.
    std::string torn = list(Backend::Vulkan, {key(0), key(1)});
    std::string rec = record(key(2).data(), key(2).size());
    writeAll(path, torn + rec.substr(0, rec.size() / 2));
    Precompiler pc(Backend::Vulkan);
    CHECK_EQ(pc.setFile(path), 2);
    CHECK(readAll(path) == torn);
    std::string k3 = key(3);
    pc.onPipelineAdded("new", false, k3.data(), k3.size());
    CHECK(readAll(path) == torn + record(k3.data(), k3.size()));
  }
  {
    // Duplicates (two processes appended the same pipeline) are compacted.
    writeAll(path, list(Backend::Vulkan, {key(0), key(1), key(0)}));
    Precompiler pc(Backend::Vulkan);
    CHECK_EQ(pc.setFile(path), 2);
    CHECK(readAll(path) == list(Backend::Vulkan, {key(0), key(1)}));
  }
  {
    // A stale file (another backend / Skia) or a damaged header: a new cache.
    writeAll(path, list(Backend::Metal, {key(0)}));
    Precompiler pc(Backend::Vulkan);
    CHECK_EQ(pc.setFile(path), 0);
    CHECK(readAll(path) == header(Backend::Vulkan));
    writeAll(path, std::string("JSUIPIPE\x01", 9));
    CHECK_EQ(Precompiler(Backend::Vulkan).setFile(path), 0);
    CHECK(readAll(path) == header(Backend::Vulkan));
  }
  {
    // Not a pipeline cache: refused and left alone.
    const std::string other = "{\"save\": \"game\"}";
    writeAll(path, other);
    Precompiler pc(Backend::Vulkan);
    CHECK_EQ(pc.setFile(path), -1);
    std::string k0 = key(0);
    pc.onPipelineAdded("a", false, k0.data(), k0.size());
    CHECK(readAll(path) == other);
    // Nor can a directory that does not exist be used.
    CHECK_EQ(pc.setFile(tempPath("no-such-dir") + "/x/cache.bin"), -1);
  }
  {
    // The file disappears while in use (a user clears caches): the next
    // append starts it again with a header.
    std::remove(path.c_str());
    Precompiler pc(Backend::Vulkan);
    CHECK_EQ(pc.setFile(path), 0);
    std::remove(path.c_str());
    std::string k5 = key(5);
    pc.onPipelineAdded("x", false, k5.data(), k5.size());
    CHECK(readAll(path) == list(Backend::Vulkan, {k5}));
  }
  std::remove(path.c_str());
}

TEST(pipeline_cache_lists) {
  Precompiler pc(Backend::Vulkan);
  std::string l = list(Backend::Vulkan, {key(0), key(1)});
  CHECK_EQ(pc.addList(l.data(), l.size()), 2);
  CHECK_EQ(pc.addList(l.data(), l.size()), 0);  // known already
  std::string more = list(Backend::Vulkan, {key(1), key(2)});
  CHECK_EQ(pc.addList(more.data(), more.size()), 1);
  CHECK_EQ(pc.stats().queued, 3);
  // Without a Graphite context nothing compiles; step reports what waits.
  CHECK_EQ(pc.step(1.0), 3);
  // Lists of another backend or build, garbage and empty input are refused.
  std::string metal = list(Backend::Metal, {key(3)});
  CHECK_EQ(pc.addList(metal.data(), metal.size()), -1);
  CHECK_EQ(pc.addList("garbage", 7), -1);
  CHECK_EQ(pc.addList(nullptr, 0), -1);
  CHECK_EQ(pc.stats().known, uint64_t(3));
  // Pipelines that came from a list are not "added" when compiled on demand.
  std::string k1 = key(1);
  pc.onPipelineAdded("listed", false, k1.data(), k1.size());
  CHECK_EQ(pc.stats().added, uint64_t(0));
  CHECK_EQ(pc.stats().compiled, uint64_t(1));
}

// jsui_set_cache_dir: the default files go into the host's directory (made
// when missing), back to the platform's with "".
TEST(pipeline_cache_host_cache_dir) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "jsui-cache-dir-test";
  fs::remove_all(dir);
  jsui_set_cache_dir(dir.string().c_str());
  std::string file = cacheFile("pipelines", "vulkan");
  CHECK(!file.empty());
  CHECK(fs::path(file).parent_path() == dir);
  CHECK(fs::is_directory(dir));
  CHECK(fs::path(file).filename().string().rfind("pipelines-", 0) == 0);
  jsui_set_cache_dir((dir.string() + "/").c_str());  // a trailing separator is fine
  CHECK(fs::path(cacheFile("pipelines", "vulkan")).parent_path() == dir);
  jsui_set_cache_dir(nullptr);
  CHECK(cacheFile("pipelines", "vulkan").find("jsui-cache-dir-test") == std::string::npos);
  fs::remove_all(dir);
}
