#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
  // Unbuffered so CI logs show progress even if a test crashes the process.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed = 0;
  for (auto& c : jsui_test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    int before = jsui_test::failures();
    c.fn();
    ran++;
    bool ok = jsui_test::failures() == before;
    if (!ok) failed++;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
  }
  std::printf("%d tests, %d failed\n", ran, failed);
  return failed ? 1 : 0;
}
