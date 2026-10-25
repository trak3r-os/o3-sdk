// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the usb_*_test.cpp files.
#pragma once

#include <doctest/doctest.h>

#include <cstdint>

// The tree builds with -fno-exceptions, so doctest's REQUIRE is unavailable: CHECK, then leave the (void)
// test body or lambda when the condition failed.
#define USB_REQUIRE(cond)    \
  do {                       \
    const bool usb_ok_ = (cond); \
    CHECK(usb_ok_);          \
    if (!usb_ok_) {          \
      return;                \
    }                        \
  } while (0)

namespace usb_test {

// Deterministic xorshift32 so a failing run can be reproduced from its seed.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : s_(seed != 0 ? seed : 0x9E3779B9u) {}
  std::uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  // Uniform in [0, n).
  std::uint32_t below(std::uint32_t n) { return next() % n; }
  // Uniform in [-m, +m].
  std::int32_t sym(std::int32_t m) { return static_cast<std::int32_t>(below(static_cast<std::uint32_t>(2 * m + 1))) - m; }

 private:
  std::uint32_t s_;
};

}  // namespace usb_test
