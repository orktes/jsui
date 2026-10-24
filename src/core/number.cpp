#include "core/number.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jsui {

namespace {

// Powers of ten that are exact doubles.
constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                             1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
constexpr double kTwo53 = 9007199254740992.0;

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

const char* parseSlow(const char* p, const char* end, double* out) {
  char small[64];
  std::string big;
  size_t n = size_t(end - p);
  const char* s;
  if (n < sizeof small) {
    std::memcpy(small, p, n);
    small[n] = 0;
    s = small;
  } else {
    big.assign(p, n);
    s = big.c_str();
  }
  char* e = nullptr;
  double d = std::strtod(s, &e);
  if (e == s) return nullptr;
  *out = d;
  return p + (e - s);
}

// Writes the digits of `m` (< 2^63) and returns the count.
size_t writeUint(uint64_t m, char* buf) {
  char tmp[24];
  size_t n = 0;
  do {
    tmp[n++] = char('0' + m % 10);
    m /= 10;
  } while (m);
  for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
  return n;
}

}  // namespace

const char* parseNumber(const char* p, const char* end, double* out) {
  const char* s = p;
  bool neg = false;
  if (s < end && *s == '-') {
    neg = true;
    s++;
  }
  // 0x... (strtod reads hex), leading whitespace or '+', inf / nan: the C library.
  if (s + 1 < end && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return parseSlow(p, end, out);
  uint64_t m = 0;
  int digits = 0, exp10 = 0;
  bool any = false, overflow = false;
  for (; s < end && isDigit(*s); s++) {
    any = true;
    if (m == 0 && *s == '0') continue;
    if (digits < 19) {
      m = m * 10 + uint64_t(*s - '0');
      digits++;
    } else {
      overflow = true;
    }
  }
  if (s < end && *s == '.') {
    const char* dot = s++;
    for (; s < end && isDigit(*s); s++) {
      any = true;
      if (m == 0 && *s == '0') {
        exp10--;
        continue;
      }
      if (digits < 19) {
        m = m * 10 + uint64_t(*s - '0');
        digits++;
        exp10--;
      } else {
        overflow = true;
      }
    }
    if (!any) s = dot;
  }
  if (!any) return parseSlow(p, end, out);
  if (s < end && (*s == 'e' || *s == 'E')) {
    const char* e = s + 1;
    bool eneg = false;
    if (e < end && (*e == '+' || *e == '-')) eneg = *e++ == '-';
    if (e < end && isDigit(*e)) {
      int ev = 0;
      for (; e < end && isDigit(*e); e++)
        if (ev < 100000) ev = ev * 10 + (*e - '0');
      exp10 += eneg ? -ev : ev;
      s = e;
    }
  }
  if (overflow) return parseSlow(p, end, out);
  if (m == 0) {
    *out = neg ? -0.0 : 0.0;
    return s;
  }
  // Clinger's fast path: both operands exact, so the one rounding is the
  // correct one.
  if (m <= uint64_t(kTwo53) && exp10 >= -22 && exp10 <= 22) {
    double v = double(m);
    v = exp10 < 0 ? v / kPow10[-exp10] : v * kPow10[exp10];
    *out = neg ? -v : v;
    return s;
  }
  return parseSlow(p, end, out);
}

size_t formatNumber(double v, char* buf) {
  auto put = [&](const char* s) {
    size_t n = std::strlen(s);
    std::memcpy(buf, s, n + 1);
    return n;
  };
  if (v != v) return put("NaN");
  if (std::isinf(v)) return put(v > 0 ? "Infinity" : "-Infinity");
  if (v == 0) return put("0");  // also -0, like String(-0)
  double a = std::fabs(v);
  size_t n = 0;
  if (v < 0) buf[n++] = '-';
  if (a < kTwo53 && a == std::floor(a)) {
    n += writeUint(uint64_t(a), buf + n);
    buf[n] = 0;
    return n;
  }
  // A short decimal: the fewest fraction digits k whose nearest integer m to
  // v * 10^k parses back to v (m and 10^k exact, so m / 10^k is the parse).
  // The nearest k-digit decimal is the one JS prints; v * 10^k is kept small
  // enough (< 2^44) that its rounding error cannot pick the wrong m, and a
  // near tie goes the slow way.
  if (a >= 1e-6 && a < 1e15) {
    for (int k = 1; k <= 9; k++) {
      double x = a * kPow10[k];
      if (x >= 17592186044416.0) break;
      double f = x - std::floor(x);
      if (std::fabs(f - 0.5) < 1e-3) break;
      double c = std::floor(x + 0.5);
      if (c / kPow10[k] != a) continue;
      char d[24];
      size_t len = writeUint(uint64_t(c), d);
      if (len <= size_t(k)) {  // 0.000ddd
        buf[n++] = '0';
        buf[n++] = '.';
        for (size_t z = len; z < size_t(k); z++) buf[n++] = '0';
        std::memcpy(buf + n, d, len);
        n += len;
      } else {
        std::memcpy(buf + n, d, len - k);
        n += len - k;
        buf[n++] = '.';
        std::memcpy(buf + n, d + len - k, k);
        n += k;
      }
      while (buf[n - 1] == '0') n--;
      if (buf[n - 1] == '.') n--;
      buf[n] = 0;
      return n;
    }
  }
  // Shortest representation that round-trips, the slow way. A decimal of at
  // most 15 significant digits survives the trip through a double
  // (DBL_DIG), so if the 15-digit rounding round-trips, it is the shortest
  // one padded with zeros; else the nearest 16- or 17-digit one. Subnormals
  // carry fewer digits: those try every length.
  char g[kNumberBufSize];
  for (int p = a < 2.2250738585072014e-308 ? 1 : 15; p <= 17; p++) {
    std::snprintf(g, sizeof g, "%.*e", p - 1, a);
    double back = 0;
    if (p == 17 || (parseNumber(g, g + std::strlen(g), &back) && back == a)) break;
  }
  // g is d[.ddd]e[+-]x: the digits and the decimal exponent.
  char digits[24];
  size_t nd = 0;
  const char* q = g;
  for (; *q && *q != 'e'; q++)
    if (isDigit(*q)) digits[nd++] = *q;
  int e = std::atoi(q + 1);
  while (nd > 1 && digits[nd - 1] == '0') nd--;
  if (a >= 1e-6 && a < 1e21) {
    // positional, like JS
    if (e < 0) {
      buf[n++] = '0';
      buf[n++] = '.';
      for (int z = 0; z < -e - 1; z++) buf[n++] = '0';
      std::memcpy(buf + n, digits, nd);
      n += nd;
    } else {
      for (int i = 0; i <= e || i < int(nd); i++) {
        if (i == e + 1) buf[n++] = '.';
        buf[n++] = i < int(nd) ? digits[i] : '0';
      }
    }
  } else {
    // d.ddde+x, like JS
    buf[n++] = digits[0];
    if (nd > 1) {
      buf[n++] = '.';
      std::memcpy(buf + n, digits + 1, nd - 1);
      n += nd - 1;
    }
    n += size_t(std::snprintf(buf + n, kNumberBufSize - n, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e));
  }
  buf[n] = 0;
  return n;
}

std::string formatNumber(double v) {
  char buf[kNumberBufSize];
  size_t n = formatNumber(v, buf);
  return std::string(buf, n);
}

}  // namespace jsui
