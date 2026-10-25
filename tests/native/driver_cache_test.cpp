// The Vulkan driver pipeline cache's file format (src/render/driver_cache.h):
// round trip, the device checks (vendor, device, driver version, pipeline
// cache UUID), damage, foreign files and atomic replacement. No GPU: the
// payload is a made-up vkGetPipelineCacheData blob (the Vulkan side is in
// vulkan_test.cpp).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "render/driver_cache.h"
#include "test.h"

using namespace jsui::driver_cache;

namespace {

DeviceId device() {
  DeviceId d;
  d.vendorID = 0x10de;
  d.deviceID = 0x2684;
  d.driverVersion = 0x8a3c4000;
  for (int i = 0; i < 16; i++) d.uuid[i] = uint8_t(i * 7 + 1);
  return d;
}

// A blob as a driver writes it: the version-one Vulkan header, then data.
std::string blob(const DeviceId& d, size_t extra) {
  auto put = [](std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
  };
  std::string s;
  put(s, 32);
  put(s, 1);
  put(s, d.vendorID);
  put(s, d.deviceID);
  s.append(reinterpret_cast<const char*>(d.uuid), 16);
  for (size_t i = 0; i < extra; i++) s.push_back(char(i * 31));
  return s;
}

Decoded dec(const std::string& s, const DeviceId& d) { return decode(s.data(), s.size(), d); }

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

}  // namespace

TEST(driver_cache_round_trip) {
  const DeviceId d = device();
  const std::string payload = blob(d, 1000);
  const std::string file = encode(d, payload.data(), payload.size());
  CHECK_EQ(file.size(), kHeaderBytes + payload.size());
  Decoded r = dec(file, d);
  CHECK_EQ(int(r.status), int(Decoded::Ok));
  CHECK_EQ(r.size, payload.size());
  CHECK(r.payload && std::string(reinterpret_cast<const char*>(r.payload), r.size) == payload);
  CHECK_EQ(int(dec("", d).status), int(Decoded::Empty));
}

TEST(driver_cache_rejects_other_devices_and_drivers) {
  const DeviceId d = device();
  const std::string payload = blob(d, 200);
  const std::string file = encode(d, payload.data(), payload.size());
  DeviceId other = d;
  other.vendorID = 0x1002;
  CHECK_EQ(int(dec(file, other).status), int(Decoded::Stale));
  other = d;
  other.deviceID++;
  CHECK_EQ(int(dec(file, other).status), int(Decoded::Stale));
  // A driver update keeps vendor / device (and sometimes the UUID).
  other = d;
  other.driverVersion++;
  Decoded r = dec(file, other);
  CHECK_EQ(int(r.status), int(Decoded::Stale));
  CHECK(r.reason.find("driver version") != std::string::npos);
  CHECK(r.payload == nullptr);
  other = d;
  other.uuid[15] ^= 1;
  CHECK_EQ(int(dec(file, other).status), int(Decoded::Stale));
  // Another format version: stale too.
  std::string v2 = file;
  v2[8] = 2;
  CHECK_EQ(int(dec(v2, d).status), int(Decoded::Stale));
}

TEST(driver_cache_rejects_damage) {
  const DeviceId d = device();
  const std::string payload = blob(d, 300);
  const std::string file = encode(d, payload.data(), payload.size());
  // Cut short anywhere: in the header, in the payload.
  for (size_t n : {size_t(4), size_t(20), kHeaderBytes - 1, kHeaderBytes, kHeaderBytes + 40, file.size() - 1})
    CHECK_EQ(int(dec(file.substr(0, n), d).status), int(Decoded::Damaged));
  // A flipped bit in the payload or in the header.
  std::string bad = file;
  bad[kHeaderBytes + 100] ^= 0x10;
  CHECK_EQ(int(dec(bad, d).status), int(Decoded::Damaged));
  bad = file;
  bad[17] ^= 0x01;  // vendor: the header checksum catches it first
  CHECK_EQ(int(dec(bad, d).status), int(Decoded::Damaged));
  // Bytes appended (a size that no longer matches).
  CHECK_EQ(int(dec(file + "x", d).status), int(Decoded::Damaged));
  // A payload whose own Vulkan header names another device, or is too short
  // to have one: never handed to the driver.
  DeviceId other = d;
  other.deviceID = 1;
  const std::string foreignBlob = blob(other, 50);
  CHECK_EQ(int(dec(encode(d, foreignBlob.data(), foreignBlob.size()), d).status), int(Decoded::Damaged));
  CHECK_EQ(int(dec(encode(d, "tiny", 4), d).status), int(Decoded::Damaged));
  // Not a driver cache at all.
  CHECK_EQ(int(dec("JSUIPIPE and more bytes", d).status), int(Decoded::Foreign));
  CHECK_EQ(int(dec("hello", d).status), int(Decoded::Foreign));
}

TEST(driver_cache_write_atomic) {
  const std::string dir = (std::filesystem::temp_directory_path() / "jsui-test-driver-cache").string();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = dir + "/vkdriver.bin";
  CHECK(writeAtomic(path, "first"));
  CHECK(readAll(path) == "first");
  CHECK(writeAtomic(path, std::string(100000, 'z')));  // replaces it
  CHECK(readAll(path) == std::string(100000, 'z'));
  // No temporary files left beside it.
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) files += e.is_regular_file();
  CHECK_EQ(files, 1);
  // A directory that does not exist: fails, nothing created.
  CHECK(!writeAtomic(dir + "/missing/x.bin", "x"));
  std::filesystem::remove_all(dir);
}

TEST(driver_cache_file_names) {
  // Tests run without default files (main.cpp); the hook names one.
  const DeviceId d = device();
  const bool env = std::getenv("JSUI_PIPELINE_CACHE") != nullptr;
  if (!env) CHECK(defaultFile(d).empty());
  setFileForTest("/tmp/x.bin");
  CHECK(defaultFile(d) == "/tmp/x.bin");
  resetFileForTest();
  if (!env) CHECK(defaultFile(d).empty());
}
