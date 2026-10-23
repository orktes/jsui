// The Dawn entry-point table (tools/dawn-shim, jsui_gpu_create_dawn_procs):
// resolution through a fake wgpuGetProcAddress, all or nothing, without a
// GPU or any Dawn. Also jsui_dawn_revision().
//
// With JSUI_GRAPHITE_DAWN (a Skia from scripts/build-skia-desktop.sh) the
// table is exercised directly; without it only the C API's NULL results are.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

#if defined(JSUI_GRAPHITE_DAWN)
// Generated shim (hidden symbols, reachable from a static link).
extern "C" {
int jsui_dawn_proc_count(void);
const char* jsui_dawn_proc_name(int i);
void* jsui_dawn_proc(int i);
int jsui_dawn_ready(void);
int jsui_dawn_fill_from_get_proc(void* get_proc);
// A trampoline, declared with handle types erased (WGPUInstance is a pointer).
void wgpuInstanceProcessEvents(void* instance);
}
#endif

namespace {

#if defined(JSUI_GRAPHITE_DAWN)
// webgpu.h's WGPUStringView, by value.
struct StringView {
  const char* data;
  size_t length;
};
using Proc = void (*)(void);

std::string viewName(StringView v) {
  if (!v.data) return std::string();
  return std::string(v.data, v.length == SIZE_MAX ? std::strlen(v.data) : v.length);
}

// Every name maps to a distinct fake address (never called), except
// wgpuInstanceProcessEvents, which records its argument.
std::vector<std::string> g_lookups;
std::string g_missing;  // a name the fake does not resolve ("" = none)
void* g_processed = nullptr;
char g_stubs[1024];

void fakeProcessEvents(void* instance) { g_processed = instance; }

Proc fakeAddress(const std::string& name) {
  if (name == "wgpuInstanceProcessEvents") return reinterpret_cast<Proc>(&fakeProcessEvents);
  for (int i = 0; i < jsui_dawn_proc_count(); i++)
    if (name == jsui_dawn_proc_name(i)) return reinterpret_cast<Proc>(&g_stubs[i]);
  return nullptr;  // not a webgpu.h name
}

Proc fakeGetProcAddress(StringView v) {
  std::string name = viewName(v);
  g_lookups.push_back(name);
  if (name == g_missing) return nullptr;
  return fakeAddress(name);
}

int indexOf(const char* name) {
  for (int i = 0; i < jsui_dawn_proc_count(); i++)
    if (std::strcmp(jsui_dawn_proc_name(i), name) == 0) return i;
  return -1;
}
#endif

}  // namespace

TEST(dawn_revision) {
#if defined(JSUI_GRAPHITE_DAWN)
  const char* rev = jsui_dawn_revision();
  CHECK(rev != nullptr);
  if (!rev) return;
  CHECK_EQ(std::string(rev), std::string(JSUI_TEST_DAWN_REVISION));
  CHECK_EQ(std::strlen(rev), size_t(40));
  CHECK(std::strspn(rev, "0123456789abcdef") == std::strlen(rev));
#else
  CHECK(jsui_dawn_revision() == nullptr);
#endif
}

TEST(dawn_procs_resolve_all) {
#if defined(JSUI_GRAPHITE_DAWN)
  const int n = jsui_dawn_proc_count();
  CHECK(n > 200);  // webgpu.h of m157's Dawn: ~276
  CHECK(n <= int(sizeof(g_stubs)));
  g_lookups.clear();
  g_missing.clear();
  CHECK_EQ(jsui_dawn_fill_from_get_proc(reinterpret_cast<void*>(&fakeGetProcAddress)), n);
  CHECK(jsui_dawn_ready());
  // Every entry point but the getter itself is looked up, once, by name.
  CHECK_EQ(int(g_lookups.size()), n - 1);
  for (int i = 0; i < n; i++) {
    const std::string name = jsui_dawn_proc_name(i);
    if (name == "wgpuGetProcAddress") {
      CHECK(jsui_dawn_proc(i) == reinterpret_cast<void*>(&fakeGetProcAddress));
    } else {
      CHECK(jsui_dawn_proc(i) == reinterpret_cast<void*>(fakeAddress(name)));
    }
  }
  for (const char* name : {"wgpuDeviceCreateTexture", "wgpuQueueSubmit", "wgpuInstanceProcessEvents",
                           "wgpuDeviceGetQueue", "wgpuTextureCreateView", "wgpuGetProcAddress"})
    CHECK(indexOf(name) >= 0);
  // The trampolines call through the table.
  g_processed = nullptr;
  int instance = 0;
  wgpuInstanceProcessEvents(&instance);
  CHECK(g_processed == &instance);
#endif
}

TEST(dawn_procs_missing_entry_point) {
  // NULL arguments: no context, nothing resolved.
  CHECK(jsui_gpu_create_dawn_procs(nullptr, reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), nullptr) == nullptr);
#if defined(JSUI_GRAPHITE_DAWN)
  const int n = jsui_dawn_proc_count();
  g_missing.clear();
  CHECK_EQ(jsui_dawn_fill_from_get_proc(reinterpret_cast<void*>(&fakeGetProcAddress)), n);
  const int created = indexOf("wgpuDeviceCreateTexture");
  void* before = jsui_dawn_proc(created);
  // A host without one entry point: NULL, cleanly (no wgpu* call is made with
  // the fake handles), and the table is left as it was.
  g_missing = "wgpuDeviceCreateTexture";
  g_lookups.clear();
  CHECK(jsui_gpu_create_dawn_procs(reinterpret_cast<void*>(&fakeGetProcAddress), reinterpret_cast<void*>(0x10),
                                   reinterpret_cast<void*>(0x20), reinterpret_cast<void*>(0x30)) == nullptr);
  CHECK_EQ(int(g_lookups.size()), n - 1);  // it still counted them all
  CHECK_EQ(jsui_dawn_fill_from_get_proc(reinterpret_cast<void*>(&fakeGetProcAddress)), n - 1);
  CHECK(jsui_dawn_proc(created) == before);
  CHECK(jsui_dawn_ready());
  g_missing.clear();
#else
  CHECK(jsui_gpu_create_dawn_procs(reinterpret_cast<void*>(1), reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
                                   nullptr) == nullptr);
#endif
}
