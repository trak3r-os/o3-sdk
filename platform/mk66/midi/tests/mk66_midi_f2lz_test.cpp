// SPDX-License-Identifier: GPL-3.0-or-later
//
// midi/f2lz.h: float -> int64 by integer arithmetic, the image's __aeabi_f2lz (platform/mk66/src/aeabi_f2lz.cpp). It must be the compiler's conversion wherever that is defined,
// because the clock-follow loop's gains depend on it.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/midi/f2lz.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;

TEST_CASE("f2lz: the values around zero and the powers of two are the C++ conversion") {
  const float values[] = {0.0f, -0.0f, 0.4f, -0.4f, 0.9999999f, 1.0f, -1.0f, 1.5f, -1.5f, 2.5f, -2.5f, 123456.75f, -123456.75f, 8388607.5f, 8388608.0f, 16777216.0f,
                          -16777216.0f, 4294967296.0f, -4294967296.0f, 1.0e10f, -1.0e10f, 4.5e18f, -4.5e18f, 9.2233715e18f, -9.2233715e18f};
  for (const float v : values) {
    INFO(v);
    CHECK(midi::float_to_int64(v) == static_cast<std::int64_t>(v));
  }
  for (int e = 0; e < 63; ++e) {
    const float p = std::ldexp(1.0f, e);
    CHECK(midi::float_to_int64(p) == static_cast<std::int64_t>(p));
    CHECK(midi::float_to_int64(-p) == static_cast<std::int64_t>(-p));
    const float q = std::nextafter(p, 0.0f);
    CHECK(midi::float_to_int64(q) == static_cast<std::int64_t>(q));
  }
}

TEST_CASE("f2lz: random floats over the range of a Q32 time (|v| up to 2^62) are the C++ conversion") {
  o3sdk::test::Rng rng(11);
  for (unsigned i = 0; i < 200000; ++i) {
    std::uint32_t bits = rng.next();
    const std::uint32_t exponent = 100u + (rng.next() % 63u);  // 2^-27 .. 2^62
    bits = (bits & 0x807FFFFFu) | (exponent << 23);
    float v;
    std::memcpy(&v, &bits, sizeof v);
    CHECK(midi::float_to_int64(v) == static_cast<std::int64_t>(v));
  }
}

TEST_CASE("f2lz: what the C++ conversion leaves undefined saturates, and a NaN is zero") {
  constexpr float inf = std::numeric_limits<float>::infinity();
  CHECK(midi::float_to_int64(inf) == INT64_MAX);
  CHECK(midi::float_to_int64(-inf) == INT64_MIN);
  CHECK(midi::float_to_int64(1.0e30f) == INT64_MAX);
  CHECK(midi::float_to_int64(-1.0e30f) == INT64_MIN);
  CHECK(midi::float_to_int64(std::ldexp(1.0f, 63)) == INT64_MAX);
  CHECK(midi::float_to_int64(-std::ldexp(1.0f, 63)) == INT64_MIN);
  CHECK(midi::float_to_int64(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(midi::float_to_int64(std::numeric_limits<float>::denorm_min()) == 0);
}
