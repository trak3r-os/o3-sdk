// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDRAM power-on tests (memtest.h) against memories that are broken on purpose: a test that cannot fail proves nothing
// about the wiring on hardware. Runs on the host (ASan, UBSan) and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/memtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace mt = o3sdk::platform::mk66::memtest;

namespace {

// A word memory with optional faults. Each fault is applied where real hardware would apply it: on the way in or on the way
// out of a cell, on the data bus, on the address bus, or on a byte lane.
struct FaultyMem {
  std::vector<std::uint32_t> cells;
  std::uint32_t stuck0 = 0;        // data bits that always read 0
  std::uint32_t stuck1 = 0;        // data bits that always read 1
  int short_a = -1, short_b = -1;  // two data lines shorted: both read the OR of the two
  int addr_stuck0_bit = -1;        // an address line stuck low: indices alias to the one with that bit clear
  int addr_short_a = -1, addr_short_b = -1;  // two address lines shorted: they carry the same value (the OR)
  int dead_lane = -1;              // a byte lane whose enable never fires: byte writes to it are lost
  std::uint32_t bad_cell = 0xFFFFFFFFu;  // one cell that cannot hold bit 0 (a failed bit)

  explicit FaultyMem(std::uint32_t words) : cells(words, 0u) {}

  std::uint32_t index(std::uint32_t i) const {
    if (addr_stuck0_bit >= 0) {
      i &= ~(1u << addr_stuck0_bit);
    }
    if (addr_short_a >= 0) {
      const bool either = ((i >> addr_short_a) & 1u) || ((i >> addr_short_b) & 1u);
      i = (i & ~((1u << addr_short_a) | (1u << addr_short_b))) | (either ? ((1u << addr_short_a) | (1u << addr_short_b)) : 0u);
    }
    return i;
  }
  std::uint32_t data_out(std::uint32_t v) const {
    if (short_a >= 0) {
      const std::uint32_t either = ((v >> short_a) | (v >> short_b)) & 1u;
      v = (v & ~((1u << short_a) | (1u << short_b))) | (either ? ((1u << short_a) | (1u << short_b)) : 0u);
    }
    return (v & ~stuck0) | stuck1;
  }
  std::uint32_t read(std::uint32_t i) const {
    std::uint32_t v = cells[index(i)];
    if (index(i) == bad_cell) {
      v &= ~1u;
    }
    return data_out(v);
  }
  void write(std::uint32_t i, std::uint32_t v) { cells[index(i)] = data_out(v); }
  void write8(std::uint32_t byte_index, std::uint8_t v) {
    const std::uint32_t lane = byte_index & 3u;
    if (static_cast<int>(lane) == dead_lane) {
      return;
    }
    std::uint32_t& cell = cells[index(byte_index / 4u)];
    cell = (cell & ~(0xFFu << (8u * lane))) | (static_cast<std::uint32_t>(v) << (8u * lane));
  }
};

constexpr std::uint32_t kWords = 1u << 12;  // 4096 words: big enough for 12 address lines, small enough for ASan

}  // namespace

TEST_CASE("memtest: a good memory passes everything") {
  FaultyMem m(kWords);
  CHECK(mt::data_bus_test(m, 0).ok);
  CHECK(mt::data_bus_test(m, kWords - 1).ok);
  CHECK(mt::byte_lane_test(m, 0).ok);
  CHECK(mt::byte_lane_test(m, kWords - 1).ok);
  CHECK(mt::address_bus_test(m, kWords).ok);
  CHECK(mt::fill_test(m, kWords, 0x5EED5EEDu).ok);
  CHECK(mt::wiring_test(m, kWords).ok);
}

TEST_CASE("memtest: a data line stuck at 0 or 1 is found, with the bit that is wrong") {
  for (unsigned bit = 0; bit < 32; ++bit) {
    FaultyMem low(kWords);
    low.stuck0 = 1u << bit;
    const mt::Result a = mt::data_bus_test(low);
    CHECK_FALSE(a.ok);
    CHECK((a.expected ^ a.actual) == (1u << bit));
    CHECK_FALSE(mt::wiring_test(low, kWords).ok);
    CHECK_FALSE(mt::fill_test(low, kWords, 1u).ok);

    FaultyMem high(kWords);
    high.stuck1 = 1u << bit;
    const mt::Result b = mt::data_bus_test(high);
    CHECK_FALSE(b.ok);
    CHECK((b.expected ^ b.actual) == (1u << bit));
    CHECK_FALSE(mt::wiring_test(high, kWords).ok);
  }
}

TEST_CASE("memtest: two data lines shorted together are found") {
  for (int a = 0; a < 32; a += 5) {
    for (int b = a + 1; b < 32; b += 7) {
      FaultyMem m(kWords);
      m.short_a = a;
      m.short_b = b;
      CHECK_FALSE(mt::data_bus_test(m).ok);
      CHECK_FALSE(mt::wiring_test(m, kWords).ok);
    }
  }
}

TEST_CASE("memtest: a dead byte lane (an enable that never fires) is found") {
  for (int lane = 0; lane < 4; ++lane) {
    FaultyMem m(kWords);
    m.dead_lane = lane;
    const mt::Result r = mt::byte_lane_test(m, 0);
    CHECK_FALSE(r.ok);
    CHECK(r.index == static_cast<std::uint32_t>(lane));
    CHECK_FALSE(mt::wiring_test(m, kWords).ok);
    // A word memory test alone cannot see it: the full-word writes still work. That is why the lanes have their own test.
    FaultyMem words_only(kWords);
    words_only.dead_lane = lane;
    CHECK(mt::data_bus_test(words_only).ok);
    CHECK(mt::address_bus_test(words_only, kWords).ok);
  }
}

TEST_CASE("memtest: an address line stuck low is found") {
  for (int bit = 0; bit < 12; ++bit) {
    FaultyMem m(kWords);
    m.addr_stuck0_bit = bit;
    CHECK_FALSE(mt::address_bus_test(m, kWords).ok);
    CHECK_FALSE(mt::wiring_test(m, kWords).ok);
    CHECK_FALSE(mt::fill_test(m, kWords, 7u).ok);
  }
}

TEST_CASE("memtest: two address lines shorted together are found") {
  for (int a = 0; a < 12; ++a) {
    for (int b = a + 1; b < 12; ++b) {
      FaultyMem m(kWords);
      m.addr_short_a = a;
      m.addr_short_b = b;
      CHECK_FALSE(mt::address_bus_test(m, kWords).ok);
    }
  }
}

TEST_CASE("memtest: one bad cell is found by the fill test and only by it") {
  const std::uint32_t cells[] = {0u, 1u, 777u, kWords / 2u, kWords - 2u};
  for (const std::uint32_t cell : cells) {
    FaultyMem m(kWords);
    m.bad_cell = cell;
    // The fill values written at that cell are not all even, so one of the two passes must show the missing bit 0.
    const mt::Result r = mt::fill_test(m, kWords, 3u);
    CHECK_FALSE(r.ok);
    CHECK(r.index == cell);
  }
}

TEST_CASE("memtest: results say where and what") {
  FaultyMem m(kWords);
  m.stuck1 = 0x00000100u;
  const mt::Result r = mt::data_bus_test(m, 5);
  CHECK_FALSE(r.ok);
  CHECK(r.index == 5u);
  CHECK(r.expected != r.actual);
  CHECK(mt::pass().ok);
}

TEST_CASE("memtest: fill values differ between neighbours and between seeds") {
  CHECK(mt::fill_value(0, 1) != mt::fill_value(1, 1));
  CHECK(mt::fill_value(5, 1) != mt::fill_value(5, 2));
  // The scramble is a bijection on the index: no value twice in the first 100000 words.
  std::vector<std::uint32_t> values;
  values.reserve(100000);
  for (std::uint32_t i = 0; i < 100000; ++i) {
    values.push_back(mt::fill_value(i, 0x5EED5EEDu));
  }
  std::sort(values.begin(), values.end());
  CHECK(std::adjacent_find(values.begin(), values.end()) == values.end());
}
