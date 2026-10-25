// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bring-up test tone: a hal::audio::BlockCallback that isolates the audio path from the app (the HW-A tone of track3r-os
// OS-005a). An image that hands it to audio_device().start() instead of the Runtime's callback proves the I2S, the eDMA, the codec
// and the output stage on their own: a clean 1 kHz on the outputs and a boundary count that keeps rising mean the platform's half
// works, whatever the app does.
//
// What it plays, as a pure function of the block count since init():
//   * a 1 kHz sine at -20 dBFS (peak 3277 of 32768) on the LEFT channel (the first word of every I2S frame) from the first block,
//     faded in over kRampSamples (46 ms) so that nothing clicks;
//   * from block kLeftOnlyBlocks (about 5.9 s) on, the same tone on the RIGHT channel too, with its own fade in. Left alone first
//     and then both: a swapped or dead channel is audible on the bench.
// The frequency comes from the rate constant (hal/audio.h), not from "44,100": a frequency counter on the line out then reads
// 1 kHz x (true sample rate / rate constant), so the tone measures the MCLK divider directly.
//
// How. A 256-entry int16 table of one period at the tone's amplitude, computed once by init() in float (a polynomial, no libm, no
// double), read with a Q32 phase accumulator and linear interpolation in integers (the interpolation error is below 0.3 LSB at this
// amplitude). The render does integer work only: no float, no division, 64 frames a block.
//
// It also reports the input block's peak (BlockReport::input_peak), so the status shows whether the RX path carries anything.
//
// Contexts: init() in the main thread before the callback is handed to the device; render() in the render line, once per block.
// The state is the render's alone after init().

#include <o3sdk/hal/audio.h>

#include <cstdint>

namespace o3sdk::platform::mk66::audio {

// Turns per sample in Q32 for a tone of `hz` at the given rate: hz x cycles-per-sample / cpu_hz, rounded to nearest, by long
// division (exact, no 128-bit intermediate). 0 if the tone would not be below the sample rate.
constexpr std::uint32_t tone_phase_inc_q32(std::uint32_t hz, hal::audio::CyclesPerSample r = hal::audio::kCyclesPerSample,
                                           std::uint32_t cpu_hz = hal::audio::kCpuHz) noexcept {
  const std::uint64_t num = static_cast<std::uint64_t>(hz) * r.num;
  const std::uint64_t den = static_cast<std::uint64_t>(r.den) * cpu_hz;
  if (num >= den) {
    return 0u;
  }
  std::uint64_t rem = num;
  std::uint32_t result = 0;
  for (int bit = 31; bit >= 0; --bit) {
    rem <<= 1;
    if (rem >= den) {
      rem -= den;
      result |= 1u << bit;
    }
  }
  if (rem * 2u >= den && result != 0xFFFFFFFFu) {
    ++result;
  }
  return result;
}

// sin(2 pi p) for p in [0, 1], float: the phase folded to [-1/4, 1/4] of a turn, then the Maclaurin series to x^9 (the first
// omitted term, x^11 / 11!, is 3.6e-6 at the edge: 0.01 LSB at the tone's amplitude).
constexpr float sin_turns(float p) noexcept {
  float t = p;
  if (p > 0.75f) {
    t = p - 1.0f;
  } else if (p > 0.25f) {
    t = 0.5f - p;
  }
  const float x = t * 6.28318531f;
  const float x2 = x * x;
  return x * (1.0f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f + x2 * 2.75573192e-6f))));
}

class TestTone {
 public:
  static constexpr std::uint32_t kHz = 1000;
  static constexpr std::int32_t kAmplitude = 3277;          // 0.1 of full scale (32768): -20 dBFS
  static constexpr std::uint32_t kLeftOnlyBlocks = 4096;    // about 5.9 s of the left channel alone, then both
  static constexpr unsigned kRampShift = 11;
  static constexpr std::uint32_t kRampSamples = 1u << kRampShift;  // 2048 samples, 46 ms: each channel's fade in
  static constexpr unsigned kTableBits = 8;
  static constexpr std::uint32_t kTableSize = 1u << kTableBits;
  static constexpr std::uint32_t kPhaseInc = tone_phase_inc_q32(kHz);
  static_assert(kPhaseInc != 0u, "the tone is below the sample rate");

  // Main thread, before callback() is handed to the device (or between two runs): the table, the phase, the fades and the block
  // count back to the start.
  void init() noexcept;
  hal::audio::BlockCallback callback() noexcept { return hal::audio::BlockCallback{&render, this}; }

  // The block callback: kBlock frames into io.out (zeros when io.flags asks for silence; the tone's clock runs on either way) and
  // the input block's peak into io.report.
  static void render(void* user, const hal::audio::BlockIo& io) noexcept;

  // For the tests (and a probe): the table entry i (0..kTableSize, the last is the first again), and the blocks since init(), which
  // stops counting at kLeftOnlyBlocks.
  std::int16_t table(std::uint32_t i) const noexcept { return table_[i <= kTableSize ? i : kTableSize]; }
  std::uint32_t blocks() const noexcept { return blocks_; }

 private:
  std::int16_t table_[kTableSize + 1u] = {};
  std::uint32_t phase_ = 0;      // Q32 turns of the next sample
  std::uint32_t ramp_l_ = 0;     // samples into each channel's fade (stops at kRampSamples)
  std::uint32_t ramp_r_ = 0;
  std::uint32_t blocks_ = 0;     // blocks rendered, up to kLeftOnlyBlocks
};

}  // namespace o3sdk::platform::mk66::audio
