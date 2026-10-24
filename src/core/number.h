// Number <-> text without the C library's slow paths. strtod and printf go
// through long-double arithmetic that WebAssembly emulates in software (musl's
// __floatscan / __multf3), which showed up as ~9% of the browser build's main
// thread on JSON from the host-JS bridge. The common cases (short decimals,
// integers) are exact in double arithmetic; everything else falls back to the
// C library, so results are identical to it.
#pragma once

#include <cstddef>
#include <string>

namespace jsui {

// strtod over [p, end): parses a leading number the way strtod does (the same
// value and the same end, nullptr when nothing was parsed) without needing a
// NUL terminator.
const char* parseNumber(const char* p, const char* end, double* out);

// The shortest text that parses back to `v` (JS Number#toString for finite
// numbers: positional from 1e-6 to 1e21, exponent outside it; "0" for -0).
// NaN / Infinity are spelled the JS way. Writes into `buf` (at least
// kNumberBufSize bytes) and returns the length.
constexpr size_t kNumberBufSize = 64;
size_t formatNumber(double v, char* buf);
std::string formatNumber(double v);

}  // namespace jsui
