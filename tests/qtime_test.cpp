// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/qtime.h (OS-005c): Q32.32 sample time. The division against 128-bit arithmetic (host) and against constants worked out in exact rational arithmetic (host and
// Cortex-M4F), the tempo as samples per tick for both clocks (4080/1 and the 44,100 Hz retune), the tempo recovered from it, the cycle / sample / microsecond
// conversions, and the claim of the ADR: a sum of exact durations does not drift.
#include <doctest/doctest.h>

#include <o3sdk/core/qtime.h>

#include <cstdint>

#include "support/rng.h"

namespace core = o3sdk::core;
using core::Q32;

namespace {
constexpr core::CyclesPerSample kToday{4080u, 1u};
constexpr core::CyclesPerSample kRetune{1800000u, 441u};

#if defined(__SIZEOF_INT128__)
Q32 q32_div_oracle(std::uint64_t num, std::uint64_t den) {
  return static_cast<Q32>((static_cast<unsigned __int128>(num) << 32) / den);
}
#endif
}  // namespace

TEST_CASE("qtime: q32_div is floor(num * 2^32 / den), exactly") {
  CHECK(core::q32_div(1, 1) == core::kQ32One);
  CHECK(core::q32_div(1, 2) == core::kQ32Half);
  CHECK(core::q32_div(3, 4) == (core::kQ32One / 4) * 3);
  CHECK(core::q32_div(0, 7) == 0u);
  CHECK(core::q32_div(7, 7) == core::kQ32One);
  CHECK(core::q32_div(1, 3) == 0x55555555u);  // floor(2^32 / 3)
  CHECK(core::q32_div(2, 3) == 0xAAAAAAAAu);
  CHECK(core::q32_div(10, 3) == (std::uint64_t{3} << 32) + 0x55555555u);
#if defined(__SIZEOF_INT128__)
  o3sdk::test::Rng rng(7);
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t den = (static_cast<std::uint64_t>(rng.next()) << (rng.next() % 31u)) + 1u;       // up to 2^62
    const std::uint64_t whole = rng.next() % 0x10000u;
    const std::uint64_t num = den * whole + (static_cast<std::uint64_t>(rng.next()) % den);         // result < 2^16 whole
    if (num / den >= (std::uint64_t{1} << 31) || den >= (std::uint64_t{1} << 62)) {
      continue;
    }
    CHECK(core::q32_div(num, den) == q32_div_oracle(num, den));
  }
#endif
}

TEST_CASE("qtime: samples per tick at known tempos, both clocks, against exact rational arithmetic") {
  // Values from Python integer arithmetic: (kCpuHz * den * 125 << 32) // (4 * num * bpm_x100).
  CHECK(core::samples_per_tick_q32(12000, kToday) == 0x72e3c3c3c3ull);    // 114.8897 samples: 120 BPM
  CHECK(core::samples_per_tick_q32(4000, kToday) == 0x158ab4b4b4bull);    // 344.669: 40 BPM
  CHECK(core::samples_per_tick_q32(80000, kToday) == 0x113bc3c3c3ull);    // 17.2335: 800 BPM
  CHECK(core::samples_per_tick_q32(13000, kToday) == 0x6a0d523e8dull);
  CHECK(core::samples_per_tick_q32(12345, kToday) == 0x6fadcec7a0ull);
  CHECK(core::samples_per_tick_q32(12000, kRetune) == 0x72d8000000ull);   // 114.84375: 44,100 Hz is a round number of samples at 120 BPM
  CHECK(core::samples_per_tick_q32(4000, kRetune) == 0x15888000000ull);
  CHECK(core::samples_per_tick_q32(80000, kRetune) == 0x113a000000ull);
  CHECK(core::samples_per_tick_q32(13000, kRetune) == 0x6a02762762ull);
  CHECK(core::samples_per_tick_q32(12345, kRetune) == 0x6fa25f2ef8ull);
}

TEST_CASE("qtime: the tempo is recovered from samples per tick, for every tempo of this OS's range and both clocks") {
  for (std::uint32_t b = 4000; b <= 80000; ++b) {
    CHECK(core::bpm_x100_from_tick_q32(core::samples_per_tick_q32(b, kToday), kToday) == b);
    CHECK(core::bpm_x100_from_tick_q32(core::samples_per_tick_q32(b, kRetune), kRetune) == b);
  }
  CHECK(core::bpm_x100_from_tick_q32(0, kToday) == 0u);
}

TEST_CASE("qtime: cycles, samples and microseconds") {
  CHECK(core::q32_from_cycles(57600, kToday) == 0xe1e1e1e1eull);           // a DIN byte is 14.1176 samples
  CHECK(core::q32_from_cycles(180000, kToday) == 0x2c1e1e1e1eull);         // a USB frame is 44.1176 samples
  CHECK(core::q32_from_micros(1000, kToday) == 0x2c1e1e1e1eull);
  CHECK(core::cycles_from_samples(1, kToday) == 4080u);
  CHECK(core::cycles_from_samples(441, kRetune) == 1800000u);               // 441 samples of the 44.1 kHz clock are 1,800,000 cycles
  CHECK(core::cycles_from_samples(44100, kRetune) == 180000000u);           // exactly one second
  CHECK(core::cycles_from_samples_frac(10, 0, kToday) == 40800u);
  CHECK(core::cycles_from_samples_frac(10, 0x80000000u, kToday) == 42840u);  // half a sample more: 2040 cycles
  CHECK(core::cycles_from_samples_sfrac(10, 0, kToday) == 40800u);
  CHECK(core::cycles_from_samples_sfrac(10, static_cast<std::int32_t>(0x40000000), kToday) == 40800u + 1020u);   // a quarter sample after
  CHECK(core::cycles_from_samples_sfrac(10, -static_cast<std::int32_t>(0x40000000), kToday) == 40800u - 1020u);  // a quarter before
  CHECK(core::cycles_from_samples_sfrac(0, -1, kToday) == 0u);  // never below the start
  // Round trip: cycles -> Q32 samples -> (whole samples, fraction) -> cycles, within one cycle.
  o3sdk::test::Rng rng(11);
  for (int i = 0; i < 5000; ++i) {
    const std::uint64_t cycles = rng.next() % 0x7FFFFFFFu;
    const Q32 q = core::q32_from_cycles(cycles, kToday);
    const std::uint64_t back = core::cycles_from_samples_frac(core::q32_floor(q), core::q32_frac(q), kToday);
    CHECK(back + 1u >= cycles);
    CHECK(back <= cycles + 1u);
    const Q32 q2 = core::q32_from_cycles(cycles, kRetune);
    const std::uint64_t back2 = core::cycles_from_samples_frac(core::q32_floor(q2), core::q32_frac(q2), kRetune);
    CHECK(back2 + 1u >= cycles);
    CHECK(back2 <= cycles + 1u);
  }
}

TEST_CASE("qtime: rounding of a time to a sample") {
  CHECK(core::q32_round(core::q32_from_samples(5)) == 5u);
  CHECK(core::q32_round(core::q32_from_samples(5) + core::kQ32Half - 1u) == 5u);
  CHECK(core::q32_round(core::q32_from_samples(5) + core::kQ32Half) == 6u);  // halves up
  CHECK(core::sq32_round(-static_cast<core::SQ32>(core::kQ32Half)) == 0);    // -0.5 rounds up to 0
  CHECK(core::sq32_round(-static_cast<core::SQ32>(core::kQ32Half) - 1) == -1);
  CHECK(core::sq32_round(static_cast<core::SQ32>(core::q32_from_samples(9)) + 1) == 9);
}

TEST_CASE("qtime: a sum of exact durations does not drift (ADR 0013 D5)") {
  // 300,000 steps at 123.45 BPM (about 11 hours: a Q32 holds 2^32 samples, 27 hours of audio, so a longer run is kept as an origin plus a Q32 offset): the sum of
  // the steps' lengths is the exact product, to within the one-ulp truncation of the tick itself.
  const Q32 spt = core::samples_per_tick_q32(12345, kToday);
  Q32 pos = 0;
  const std::uint32_t steps = 300000;
  for (std::uint32_t i = 0; i < steps; ++i) {
    pos += 48u * spt;
  }
  CHECK(pos == static_cast<Q32>(steps) * 48u * spt);
  // The true tick is spt + e with 0 <= e < 1 Q32 unit: after 300,000 steps the position is off by less than 14.4e6 units = 0.0034 sample.
  const double ideal = 300000.0 * 48.0 * static_cast<double>(core::kCpuHz) * 125.0 / (4.0 * 4080.0 * 12345.0);
  const double got = static_cast<double>(pos) / 4294967296.0;
  CHECK(got <= ideal);
  CHECK(ideal - got < 0.004);
}
