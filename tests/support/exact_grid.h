// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The transport's grid in exact rational arithmetic, independent of the engine's fixed point: a test compares the frame the engine lands a tick on with this one.
//
// A tick is reached by sample n when n * ticks-per-sample >= tick, with ticks per sample = bpm * 960 / (60 * rate) = mbpm * 960 / (60 * rate_mhz) (dsp/transport.h). So the frame of a
// whole tick is the first integer n with n * mbpm * 960 >= tick * 60 * rate_mhz: a ceiling division of two integers, nothing rounded before it.

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/dsp/transport.h>

#include <cstdint>

namespace o3sdk::test {

// The engine's rate in millihertz (44,100,000 at the default clock), computed from the one rate constant the way dsp/transport.cpp does.
inline constexpr std::uint64_t kRateMilliHz =
    (static_cast<std::uint64_t>(core::kCpuHz) * 1000u * core::kRefCyclesPerSample.den + core::kRefCyclesPerSample.num / 2u) / core::kRefCyclesPerSample.num;

// The sample (counted from Play's first, which is sample 0) on which the transport at `mbpm` (clamped to the legal range) reaches whole tick `tick`. `tick` stays below 2^30 (the product
// fits 64 bits).
inline std::uint64_t exact_frame(std::uint32_t mbpm, std::uint64_t tick) {
  const std::uint64_t t = mbpm < dsp::kMinTempoMilliBpm ? dsp::kMinTempoMilliBpm : (mbpm > dsp::kMaxTempoMilliBpm ? dsp::kMaxTempoMilliBpm : mbpm);
  const std::uint64_t num = tick * 60u * kRateMilliHz;
  const std::uint64_t den = t * dsp::kPpqn;
  return (num + den - 1u) / den;
}

}  // namespace o3sdk::test
