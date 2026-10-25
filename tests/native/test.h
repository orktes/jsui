// Minimal test harness (no external dependency).
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); }
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace jsui_test {
struct Case {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
// JSUI_TEST_JS_THREAD=on|lockstep: the React e2e fixtures create their
// runtime with that JsuiConfig.js_thread (packages/cli/test/e2e.test.js runs
// them again in lockstep, which must give what single-threaded gives).
inline int jsThreadMode() {
  const char* e = std::getenv("JSUI_TEST_JS_THREAD");
  if (!e) return 0;
  if (std::strcmp(e, "lockstep") == 0) return 2;  // JSUI_JS_THREAD_LOCKSTEP
  if (std::strcmp(e, "on") == 0) return 1;        // JSUI_JS_THREAD_ON
  return 0;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Registrar {
  Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
template <class A, class B>
void checkEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << a << " != " << b;
    std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %s\n", file, line, ea, eb, os.str().c_str());
    failures()++;
  }
}
}  // namespace jsui_test

#define JSUI_CAT2(a, b) a##b
#define JSUI_CAT(a, b) JSUI_CAT2(a, b)
#define TEST(name)                                                                                   \
  static void name();                                                                                \
  static jsui_test::Registrar JSUI_CAT(reg_, name)(#name, name);                                     \
  static void name()
#define CHECK(c)                                                                                     \
  do {                                                                                               \
    if (!(c)) {                                                                                      \
      std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                   \
      jsui_test::failures()++;                                                                       \
    }                                                                                                \
  } while (0)
#define CHECK_EQ(a, b) jsui_test::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps)                                                                        \
  do {                                                                                               \
    double _a = (a), _b = (b);                                                                       \
    if (std::fabs(_a - _b) > (eps)) {                                                                \
      std::fprintf(stderr, "  %s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, \
                   #b, _a, _b);                                                                      \
      jsui_test::failures()++;                                                                       \
    }                                                                                                \
  } while (0)
