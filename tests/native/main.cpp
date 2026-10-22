#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
  // Unbuffered so CI logs show progress even if a test crashes the process.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  // Arguments are name filters (substring match; a test runs if any matches).
  int ran = 0, failed = 0;
  for (auto& c : jsui_test::registry()) {
    bool selected = argc < 2;
    for (int i = 1; i < argc && !selected; i++) selected = std::strstr(c.name, argv[i]) != nullptr;
    if (!selected) continue;
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
