// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sample time as Q32.32 integers (ADR 0013 D5: "tempo is samples-per-tick in Q32.32, no float drift"; OS-005c).
//
// A time or a span is a count of audio samples with 32 fractional bits in a uint64 (Q32) or an int64 (SQ32, for errors that can be
// negative). One tick of the grid (192 PPQN: 48 to a step, 4 steps to a beat) is a few hundred samples, and a step is a few thousand, so
// a duration fits 47 bits at the slowest tempo. An absolute position on the timeline is NOT a Q32: 2^32 samples is only 27 hours, and
// the timeline is a uint64. Whoever needs sub-sample precision keeps an origin (a whole uint64 sample) and a Q32 offset from it, and
// moves the origin on when the offset gets large (seq::Sequencer and midi::ClockDll both do).
//
// Everything here is exact integer arithmetic, constexpr, no heap, no libm, no float: a tempo is a rational, and a sum of exact
// durations does not drift (a float timer would drift; this replaces it).
//
// The rate comes from core/audio_clock.h: CPU cycles per sample as a rational (1800000/441 for 44,100 Hz, or 4080/1 for the pre-1.0 clock), so a
// cycle count and a sample count convert by one multiplication and one division, with no clock-domain estimation.

#include <o3sdk/core/audio_clock.h>

#include <cstdint>

namespace o3sdk::core {

using Q32 = std::uint64_t;   // unsigned Q32.32 samples
using SQ32 = std::int64_t;   // signed Q32.32 samples

inline constexpr unsigned kQ32Frac = 32;
inline constexpr Q32 kQ32One = Q32{1} << kQ32Frac;
inline constexpr Q32 kQ32Half = Q32{1} << (kQ32Frac - 1u);

constexpr Q32 q32_from_samples(std::uint32_t whole) noexcept { return Q32{whole} << kQ32Frac; }
constexpr std::uint64_t q32_floor(Q32 q) noexcept { return q >> kQ32Frac; }
// Nearest whole sample, halves up: what an event's sample offset is.
constexpr std::uint64_t q32_round(Q32 q) noexcept { return (q + kQ32Half) >> kQ32Frac; }
constexpr std::uint32_t q32_frac(Q32 q) noexcept { return static_cast<std::uint32_t>(q & 0xFFFFFFFFu); }
// Signed, nearest whole sample (halves up).
constexpr std::int64_t sq32_round(SQ32 q) noexcept { return (q + static_cast<SQ32>(kQ32Half)) >> kQ32Frac; }

// floor(num * 2^32 / den), exactly: the whole part by one division, the 32 fractional bits by shift and subtract (a bounded loop of 32
// steps, so it is safe wherever 100 cycles are). den must not be zero and be below 2^62; the result must fit 64 bits (num / den < 2^32).
constexpr Q32 q32_div(std::uint64_t num, std::uint64_t den) noexcept {
  const std::uint64_t whole = num / den;
  std::uint64_t r = num % den;  // < den
  std::uint64_t frac = 0;
  for (unsigned i = 0; i < kQ32Frac; ++i) {
    r <<= 1u;  // < 2 den < 2^63
    frac <<= 1u;
    if (r >= den) {
      r -= den;
      frac |= 1u;
    }
  }
  return (whole << kQ32Frac) | frac;
}

// Samples per tick of the 192 PPQN grid (one microstep) at `bpm_x100` BPM x 100 (4000..80000 are 40..800 BPM, our range), as Q32:
//     ticks per second = bpm * 192 / 60 = bpm_x100 * 4 / 125,   samples per second = kCpuHz * den / num
// so   spt = kCpuHz * den * 125 / (4 * num * bpm_x100).
// 114.89 samples at 120 BPM and 4080/1. bpm_x100 must not be zero.
constexpr Q32 samples_per_tick_q32(std::uint32_t bpm_x100, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  return q32_div(std::uint64_t{kCpuHz} * r.den * 125u, std::uint64_t{4u} * r.num * bpm_x100);
}

// The inverse: the tempo, BPM x 100, of a span of `spt` samples per tick (rounded to nearest). 0 for a zero span.
constexpr std::uint32_t bpm_x100_from_tick_q32(Q32 spt, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  if (spt == 0u) {
    return 0u;
  }
  // bpm_x100 = kCpuHz * den * 125 / (4 * num * spt) with spt in Q32. The numerator times 2^32 does not fit a uint64, so the quotient of
  // the constants is taken as a Q32 first (one q32_div), and dividing it by the Q32 span gives the integer.
  const Q32 q = q32_div(std::uint64_t{kCpuHz} * r.den * 125u, std::uint64_t{4u} * r.num);
  return static_cast<std::uint32_t>((q + spt / 2u) / spt);
}

// A span of CPU cycles as samples (Q32): cycles * den / num. Valid for spans below 2^32 samples; cycles * den must fit 64 bits.
constexpr Q32 q32_from_cycles(std::uint64_t cycles, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  return q32_div(cycles * r.den, r.num);
}

// Whole samples as CPU cycles, nearest: samples * num / den. Valid below 2^43 samples (about 6 years of audio at 44.1 kHz).
constexpr std::uint64_t cycles_from_samples(std::uint64_t samples, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  return (samples * r.num + r.den / 2u) / r.den;
}

// A span of microseconds as samples (Q32), exact: us * (kCpuHz / 1e6 = 180) * den / num. For the user's millisecond offsets of the MIDI ports.
constexpr Q32 q32_from_micros(std::uint32_t us, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  return q32_div(std::uint64_t{us} * (kCpuHz / 1000000u) * r.den, r.num);
}

// A whole sample plus a SIGNED fraction (in 2^-32 sample, within half a sample either way: what an event carries when its sample is the nearest one to its exact time) as
// CPU cycles, nearest: the exact due time of the MIDI clock. samples < 2^43, as above.
constexpr std::uint64_t cycles_from_samples_sfrac(std::uint64_t samples, std::int32_t sfrac, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  const std::int64_t whole = static_cast<std::int64_t>(samples * r.num);                                  // cycles * den
  const std::int64_t frac = (static_cast<std::int64_t>(sfrac) * static_cast<std::int64_t>(r.num) + (std::int64_t{1} << 31)) >> kQ32Frac;  // cycles * den, rounded
  const std::int64_t sum = whole + frac + static_cast<std::int64_t>(r.den / 2u);
  return static_cast<std::uint64_t>(sum < 0 ? 0 : sum) / r.den;
}

// A whole sample plus an UNSIGNED fraction (the low 32 bits of a Q32: the floor and what is beyond it) as CPU cycles, nearest. samples < 2^43, as above.
constexpr std::uint64_t cycles_from_samples_frac(std::uint64_t samples, std::uint32_t frac, CyclesPerSample r = kRefCyclesPerSample) noexcept {
  const std::uint64_t whole_cycles_x_den = samples * r.num;                               // cycles * den
  const std::uint64_t frac_cycles_x_den = (std::uint64_t{frac} * r.num + kQ32Half) >> kQ32Frac;  // 0..num, cycles * den, rounded
  return (whole_cycles_x_den + frac_cycles_x_den + r.den / 2u) / r.den;
}

}  // namespace o3sdk::core
