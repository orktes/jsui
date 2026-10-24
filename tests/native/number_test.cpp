// Number parsing and formatting (core/number.h): the same results as strtod,
// and the text JS's Number#toString gives.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "core/number.h"
#include "test.h"

using namespace jsui;

namespace {

// parseNumber against strtod: same value (bit for bit) and same end.
bool sameAsStrtod(const std::string& s) {
  char* e = nullptr;
  double a = std::strtod(s.c_str(), &e);
  double b = 0;
  const char* pe = parseNumber(s.data(), s.data() + s.size(), &b);
  if (e == s.c_str()) return pe == nullptr;
  return pe && pe - s.data() == e - s.c_str() && (std::memcmp(&a, &b, sizeof a) == 0 || (a != a && b != b));
}

}  // namespace

TEST(number_parse_matches_strtod) {
  const char* cases[] = {"",     "-",         "+1",    "0x1A",   "1e",          "1e+",    "1.",       "-.5",
                         ".",    "-0",        "0.0",   "1e400",  "1e-400",      "inf",    "nan",      " 5",
                         "0012", "1.5px",     "12%",   "-3.25e-2deg", "0.1", "9007199254740993", "1e22", "1e23",
                         "4.35", "123456789012345678901234", "2.675e-3"};
  for (const char* c : cases) CHECK(sameAsStrtod(c));
  std::mt19937_64 r(1);
  for (int i = 0; i < 200000; i++) {
    std::string s;
    if (r() % 2) s += '-';
    for (int k = int(r() % 8); k > 0; k--) s += char('0' + r() % 10);
    if (int fp = int(r() % 10)) {
      s += '.';
      while (fp--) s += char('0' + r() % 10);
    }
    if (r() % 5 == 0) {
      s += 'e';
      if (r() % 2) s += '-';
      s += std::to_string(r() % 30);
    }
    if (r() % 4 == 0) s += "px";
    if (!sameAsStrtod(s)) {
      std::fprintf(stderr, "  parse differs: %s\n", s.c_str());
      CHECK(false);
      break;
    }
  }
  // Only [p, end) is read.
  const char* t = "12345";
  double d = 0;
  CHECK(parseNumber(t, t + 2, &d) == t + 2);
  CHECK_EQ(d, 12.0);
}

TEST(number_format_like_js) {
  CHECK_EQ(formatNumber(0.1), std::string("0.1"));
  CHECK_EQ(formatNumber(-0.0), std::string("0"));
  CHECK_EQ(formatNumber(42), std::string("42"));
  CHECK_EQ(formatNumber(-1.5), std::string("-1.5"));
  CHECK_EQ(formatNumber(0.1 + 0.2), std::string("0.30000000000000004"));
  CHECK_EQ(formatNumber(0.00001), std::string("0.00001"));
  CHECK_EQ(formatNumber(0.000001), std::string("0.000001"));
  CHECK_EQ(formatNumber(1e-7), std::string("1e-7"));
  CHECK_EQ(formatNumber(1.5e-10), std::string("1.5e-10"));
  CHECK_EQ(formatNumber(1e21), std::string("1e+21"));
  CHECK_EQ(formatNumber(1.2345e22), std::string("1.2345e+22"));
  CHECK_EQ(formatNumber(1e20), std::string("100000000000000000000"));
  CHECK_EQ(formatNumber(70534575327.644714), std::string("70534575327.64471"));
  CHECK_EQ(formatNumber(5.3570911874756963e-07), std::string("5.357091187475696e-7"));
  CHECK_EQ(formatNumber(NAN), std::string("NaN"));
  CHECK_EQ(formatNumber(-INFINITY), std::string("-Infinity"));
  // Everything round-trips.
  std::mt19937_64 r(2);
  for (int i = 0; i < 200000; i++) {
    double v;
    switch (r() % 3) {
      case 0: v = std::round((double(r() % 100000000) / 1e3 - 5e4) * 1e5) / 1e5; break;
      case 1: {
        uint64_t b = r();
        std::memcpy(&v, &b, sizeof v);
        break;
      }
      default: v = (double(r()) / 1.8e19) * std::pow(10.0, int(r() % 40) - 20);
    }
    if (!std::isfinite(v)) continue;
    std::string s = formatNumber(v);
    if (std::strtod(s.c_str(), nullptr) != v) {
      std::fprintf(stderr, "  %.17g -> %s\n", v, s.c_str());
      CHECK(false);
      break;
    }
  }
}
