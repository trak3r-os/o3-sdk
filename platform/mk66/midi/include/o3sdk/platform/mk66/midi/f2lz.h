// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// float -> int64 by integer arithmetic only. The clock-follow loop (clock_dll.cpp, ported from track3r-os) turns a float of Q32 samples into a 64-bit integer, which the compiler
// emits as a call to the run-time helper __aeabi_f2lz; the toolchain's libgcc implements that helper (and the unsigned one it calls) with the software double helpers
// (__aeabi_dmul, _dadd, _d2uiz), and no image of o3-sdk may carry them (the FPU is single precision; check_template.py and `nm` look for them). The image defines
// __aeabi_f2lz itself with this function (platform/mk66/src/aeabi_f2lz.cpp), so libgcc's member is never linked. The result is that of the C++ conversion: truncation toward zero;
// out of the range of int64_t saturates, and a NaN is 0 (the C++ conversion is undefined there).

#include <cstdint>
#include <cstring>

namespace o3sdk::platform::mk66::midi {

inline std::int64_t float_to_int64(float f) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  const bool negative = (bits >> 31) != 0u;
  const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127;
  const std::uint32_t fraction = bits & 0x7FFFFFu;
  if (exponent < 0) {
    return 0;  // below one in magnitude, zero, a denormal
  }
  if (exponent == 128 && fraction != 0u) {
    return 0;  // NaN
  }
  if (exponent >= 63) {
    return negative ? INT64_MIN : INT64_MAX;  // |f| >= 2^63 (and infinity); -2^63 itself is exactly INT64_MIN
  }
  const std::uint64_t mantissa = std::uint64_t{fraction} | 0x800000u;  // 1.fraction, 23 fraction bits
  const std::uint64_t magnitude = exponent >= 23 ? mantissa << (exponent - 23) : mantissa >> (23 - exponent);
  return negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
}

}  // namespace o3sdk::platform::mk66::midi
