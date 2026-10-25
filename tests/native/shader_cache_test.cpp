// Ganesh's shader cache file (render/shader_cache.h): shaders stored by one
// context are found by the next, a damaged tail or another backend's file
// is dealt with, and a second cache on the same file only reads it.
#include <cstdio>
#include <filesystem>
#include <string>

#include "render/shader_cache.h"
#include "test.h"

using jsui::shader_cache::Cache;

namespace {

sk_sp<SkData> bytes(const std::string& s) { return SkData::MakeWithCopy(s.data(), s.size()); }

std::string tempPath(const char* name) {
  std::string p = (std::filesystem::temp_directory_path() / name).string();
  std::remove(p.c_str());
  return p;
}

bool found(Cache& c, const std::string& key, const std::string& data) {
  sk_sp<SkData> d = c.load(*bytes(key));
  return d && d->equals(bytes(data).get());
}

}  // namespace

TEST(shader_cache_round_trip) {
  const std::string path = tempPath("jsui-test-shaders.bin");
  {
    Cache c("gl");
    CHECK_EQ(c.setFile(path), 0);
    CHECK(!c.load(*bytes("key-a")));
    c.store(*bytes("key-a"), *bytes("program a"), SkString());
    c.store(*bytes("key-b"), *bytes("program b"), SkString());
    c.store(*bytes("key-a"), *bytes("program a"), SkString());  // known: not appended again
    CHECK_EQ(c.stats().stored, 2ull);
    CHECK(found(c, "key-a", "program a"));
  }
  const auto size = std::filesystem::file_size(path);
  {
    Cache c("gl");
    CHECK_EQ(c.setFile(path), 2);
    CHECK(found(c, "key-a", "program a"));
    CHECK(found(c, "key-b", "program b"));
    CHECK(!c.load(*bytes("key-c")));
    CHECK_EQ(c.stats().hits, 2ull);
    CHECK_EQ(c.stats().misses, 1ull);
    // A second cache on the file reads it but does not append.
    Cache other("gl");
    CHECK_EQ(other.setFile(path), 2);
    other.store(*bytes("key-c"), *bytes("program c"), SkString());
  }
  CHECK_EQ(std::filesystem::file_size(path), size);

  // A damaged tail (a crash mid-write) is dropped; the rest stays.
  {
    FILE* f = std::fopen(path.c_str(), "ab");
    std::fwrite("\x05\0\0\0\x09\0\0\0garbage", 1, 15, f);
    std::fclose(f);
    Cache c("gl");
    CHECK_EQ(c.setFile(path), 2);
    CHECK(found(c, "key-b", "program b"));
  }
  CHECK_EQ(std::filesystem::file_size(path), size);

  // Another backend's file is replaced; a file that is not a shader cache
  // is left alone.
  {
    Cache c("d3d");
    CHECK_EQ(c.setFile(path), 0);
    CHECK(!c.load(*bytes("key-a")));
  }
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs("not a cache", f);
    std::fclose(f);
    Cache c("gl");
    CHECK_EQ(c.setFile(path), -1);
    c.store(*bytes("key-a"), *bytes("program a"), SkString());
    CHECK(found(c, "key-a", "program a"));  // still cached in memory
  }
  CHECK_EQ(std::filesystem::file_size(path), std::uintmax_t(11));
  std::remove(path.c_str());
}
