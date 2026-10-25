// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Power-on memory tests for the SDRAM, written against a tiny accessor so that the same code runs on the target (over
// volatile pointers) and on the host and QEMU (over a buffer that tests can break on purpose): a test that cannot fail is
// not a test, and "SDRAM pattern test passed" on hardware must mean the tests below can see a stuck data line, a dead
// byte lane, an address line that is stuck or shorted, and a single bad cell.
//
// An accessor `Mem` provides, with word indices 0 .. words-1 (a word is 32 bits, the width the CPU uses; the 16-bit SDRAM
// port moves it as two beats, so a 32-bit walk covers all 16 data lines of both beats):
//   std::uint32_t read(std::uint32_t index);
//   void write(std::uint32_t index, std::uint32_t value);
//   void write8(std::uint32_t byte_index, std::uint8_t value);   // a byte access: exercises the DQM byte lanes
//
// Every test stops at the first failure and says where. All loops are bounded by the arguments (no timing, no waiting),
// the destructive ones overwrite the memory they test: run them before anything lives there.

#include <cstdint>

namespace o3sdk::platform::mk66::memtest {

struct Result {
  bool ok;
  std::uint32_t index;     // word index of the failure (byte index for the byte-lane test)
  std::uint32_t expected;
  std::uint32_t actual;
};

constexpr Result pass() noexcept { return Result{true, 0, 0, 0}; }
constexpr Result fail(std::uint32_t index, std::uint32_t expected, std::uint32_t actual) noexcept {
  return Result{false, index, expected, actual};
}

// Walking ones then walking zeros through one word: finds a data line stuck at 0 or 1 and two lines shorted together.
template <class Mem>
Result data_bus_test(Mem& m, std::uint32_t index = 0) {
  for (std::uint32_t bit = 0; bit < 32; ++bit) {
    const std::uint32_t ones = 1u << bit;
    m.write(index, ones);
    const std::uint32_t got_ones = m.read(index);
    if (got_ones != ones) {
      return fail(index, ones, got_ones);
    }
    const std::uint32_t zeros = ~ones;
    m.write(index, zeros);
    const std::uint32_t got_zeros = m.read(index);
    if (got_zeros != zeros) {
      return fail(index, zeros, got_zeros);
    }
  }
  return pass();
}

// Each of the four byte lanes of a word on its own (a byte write drives only that lane's DQM): a dead or swapped byte
// enable shows as the wrong lane changing.
template <class Mem>
Result byte_lane_test(Mem& m, std::uint32_t word = 0) {
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    m.write(word, 0u);
    const std::uint8_t value = static_cast<std::uint8_t>(0xA5u + lane);
    m.write8(word * 4u + lane, value);
    const std::uint32_t expected = static_cast<std::uint32_t>(value) << (8u * lane);
    const std::uint32_t actual = m.read(word);
    if (actual != expected) {
      return fail(word * 4u + lane, expected, actual);
    }
  }
  return pass();
}

// Address lines, after Michael Barr's classic test: put a pattern at every power-of-two word offset and the anti-pattern
// at offset 0, then check that nothing moved; then flip each power-of-two offset in turn and check that neither offset 0
// nor any other power-of-two offset changed. A stuck or shorted address line makes two offsets the same cell.
// `words` is the size of the memory in words, a power of two.
template <class Mem>
Result address_bus_test(Mem& m, std::uint32_t words) {
  constexpr std::uint32_t kPattern = 0xAAAAAAAAu;
  constexpr std::uint32_t kAnti = 0x55555555u;
  for (std::uint32_t offset = 1; offset < words; offset <<= 1) {
    m.write(offset, kPattern);
  }
  m.write(0, kAnti);
  for (std::uint32_t offset = 1; offset < words; offset <<= 1) {
    const std::uint32_t got = m.read(offset);
    if (got != kPattern) {
      return fail(offset, kPattern, got);
    }
  }
  m.write(0, kPattern);
  for (std::uint32_t probe = 1; probe < words; probe <<= 1) {
    m.write(probe, kAnti);
    const std::uint32_t at_zero = m.read(0);
    if (at_zero != kPattern) {
      return fail(0, kPattern, at_zero);
    }
    for (std::uint32_t offset = 1; offset < words; offset <<= 1) {
      if (offset != probe) {
        const std::uint32_t got = m.read(offset);
        if (got != kPattern) {
          return fail(offset, kPattern, got);
        }
      }
    }
    m.write(probe, kPattern);
  }
  return pass();
}

// Every cell: write a value derived from its index and a seed, read all back; then the same with the complement. A cell
// that cannot hold one of the two values, and any index-dependent pattern error, shows up. The value is the index
// scrambled (an odd multiplier and a shift), so neighbouring words differ in many bits and no value repeats within 2^32
// words.
constexpr std::uint32_t fill_value(std::uint32_t index, std::uint32_t seed) noexcept {
  std::uint32_t x = (index ^ seed) * 2654435761u;  // 0x9E3779B1, odd: a bijection on 32 bits
  return x ^ (x >> 15);
}

template <class Mem>
Result fill_test(Mem& m, std::uint32_t words, std::uint32_t seed) {
  for (std::uint32_t pass_no = 0; pass_no < 2; ++pass_no) {
    const std::uint32_t invert = pass_no == 0 ? 0u : 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < words; ++i) {
      m.write(i, fill_value(i, seed) ^ invert);
    }
    for (std::uint32_t i = 0; i < words; ++i) {
      const std::uint32_t expected = fill_value(i, seed) ^ invert;
      const std::uint32_t actual = m.read(i);
      if (actual != expected) {
        return fail(i, expected, actual);
      }
    }
  }
  return pass();
}

// The quick tests that catch wiring faults in milliseconds: the data lines at a few places, the byte lanes, the address
// lines. The slow one, fill_test, covers every cell.
template <class Mem>
Result wiring_test(Mem& m, std::uint32_t words) {
  Result r = data_bus_test(m, 0);
  if (!r.ok) {
    return r;
  }
  r = data_bus_test(m, words - 1u);
  if (!r.ok) {
    return r;
  }
  r = byte_lane_test(m, 0);
  if (!r.ok) {
    return r;
  }
  r = byte_lane_test(m, words - 1u);
  if (!r.ok) {
    return r;
  }
  return address_bus_test(m, words);
}

}  // namespace o3sdk::platform::mk66::memtest
