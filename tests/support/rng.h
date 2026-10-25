// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::test {

// Deterministic xorshift32: the same sequence on every host, so fixtures and the fuzz loop are reproducible.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : s_(seed != 0 ? seed : 0x9E3779B9u) {}
  std::uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  // 0 .. n-1 (n > 0)
  std::uint32_t below(std::uint32_t n) { return next() % n; }
  std::uint8_t byte() { return static_cast<std::uint8_t>(next() >> 11); }

 private:
  std::uint32_t s_;
};

}  // namespace o3sdk::test
