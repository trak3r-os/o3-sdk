// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HW-A test tone and the float-to-int16 conversion (audio/test_signal.h, OS-005a) against a double-precision oracle: the
// phase increment from the rate constant for both clocks, the sine, the ramp, the long-term phase exactness, the rounding and
// saturation of the conversion. Host and Cortex-M4F (`qemu_audio`); the oracle uses `double` and the C library's sin(), which only
// tests may.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/test_signal.h>
#include <o3sdk/hal/audio.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace {

using o3sdk::platform::mk66::audio::TestTone;
namespace haudio = o3sdk::hal::audio;

constexpr double kTwoPi = 6.283185307179586476925287;

// double is software-emulated on the Cortex-M4F: the oracle loops are shorter there (the fake I2S suite checks 64 more blocks of the
// tone against the same oracle on the target).
#if defined(__arm__)
constexpr bool kTarget = true;
#else
constexpr bool kTarget = false;
#endif
constexpr std::uint32_t kSinPoints = kTarget ? 4000u : 100000u;
constexpr std::uint32_t kOracleBlocks = kTarget ? 200u : 3000u;

std::int32_t oracle_q15(double v) {
  double s = v * 32768.0;
  if (s > 32767.0) {
    s = 32767.0;
  }
  if (s < -32768.0) {
    s = -32768.0;
  }
  return static_cast<std::int32_t>(s >= 0.0 ? s + 0.5 : s - 0.5);
}

// The tone's definition, in double: sample n (0-based after the tone was switched on) has the ramp position min(n + 1, ramp), gain
// amplitude x position / ramp, and the phase (n x increment mod 2^32) / 2^32 turns.
std::int32_t oracle_sample(std::uint64_t n, std::uint64_t inc_q32, double amplitude, std::uint32_t ramp) {
  const std::uint64_t pos = n + 1u < ramp ? n + 1u : ramp;
  const double gain = pos >= ramp ? amplitude : static_cast<double>(pos) * (amplitude / static_cast<double>(ramp));
  const double turns = static_cast<double>((n * inc_q32) & 0xFFFFFFFFull) / 4294967296.0;
  return oracle_q15(gain * std::sin(kTwoPi * turns));
}

}  // namespace

TEST_CASE("test signal: the phase increment is the tone's turns per sample at the rate constant, for both clocks") {
  for (const haudio::CyclesPerSample rate : {haudio::CyclesPerSample{4080u, 1u}, haudio::CyclesPerSample{1800000u, 441u}}) {
    // hz x cycles-per-sample / cpu-hz turns per sample, to nearest, in Q32.
    const double exact = 4294967296.0 * 1000.0 * (static_cast<double>(rate.num) / static_cast<double>(rate.den)) / 180000000.0;
    const std::uint32_t inc = o3sdk::platform::mk66::audio::tone_phase_inc_q32(1000u, rate);
    CHECK(static_cast<double>(inc) >= std::floor(exact));
    CHECK(static_cast<double>(inc) <= std::floor(exact) + 1.0);
    // The retune: 1000 Hz at exactly 44,100 Hz is 2^32 x 10 / 441 turns per sample.
    if (rate.num == 1800000u) {
      CHECK(std::fabs(static_cast<double>(inc) - 4294967296.0 * 1000.0 / 44100.0) < 1.0);
    }
  }
  CHECK(o3sdk::platform::mk66::audio::tone_phase_inc_q32(1000u) == o3sdk::platform::mk66::audio::tone_phase_inc_q32(1000u, haudio::kCyclesPerSample));
  // A tone at or above the sample rate has no phase increment: refused, 0.
  CHECK(o3sdk::platform::mk66::audio::tone_phase_inc_q32(60000u) == 0u);
  // The tone frequency tracks the rate constant: the same phase increment against the retuned rate would read 0.04 % wrong, which
  // is the number a frequency counter sees at HW-A when MCLK is not what the constant says.
  const double ratio = static_cast<double>(o3sdk::platform::mk66::audio::tone_phase_inc_q32(1000u, {4080u, 1u})) /
                       static_cast<double>(o3sdk::platform::mk66::audio::tone_phase_inc_q32(1000u, {1800000u, 441u}));
  CHECK(std::fabs(ratio - 4080.0 / (1800000.0 / 441.0)) < 1e-6);
}

TEST_CASE("test signal: sin_turns is the sine to 4e-6 over the whole turn") {
  double worst = 0.0;
  for (std::uint32_t i = 0; i < kSinPoints; ++i) {
    const float p = static_cast<float>(i) / static_cast<float>(kSinPoints);
    const double err = std::fabs(static_cast<double>(o3sdk::platform::mk66::audio::sin_turns(p)) - std::sin(kTwoPi * static_cast<double>(p)));
    worst = err > worst ? err : worst;
  }
  CHECK(worst < 4e-6);
  CHECK(o3sdk::platform::mk66::audio::sin_turns(0.0f) == 0.0f);
  CHECK(std::fabs(o3sdk::platform::mk66::audio::sin_turns(0.25f) - 1.0f) < 4e-6f);
  CHECK(std::fabs(o3sdk::platform::mk66::audio::sin_turns(0.75f) + 1.0f) < 4e-6f);
  CHECK(std::fabs(o3sdk::platform::mk66::audio::sin_turns(0.5f)) < 4e-6f);
}

TEST_CASE("test signal: float to int16 rounds half away from zero, saturates, and survives NaN") {
  using o3sdk::platform::mk66::audio::q15_from_float;
  CHECK(q15_from_float(0.0f) == 0);
  CHECK(q15_from_float(1.0f / 32768.0f) == 1);
  CHECK(q15_from_float(-1.0f / 32768.0f) == -1);
  CHECK(q15_from_float(0.5f / 32768.0f) == 1);     // half away from zero
  CHECK(q15_from_float(-0.5f / 32768.0f) == -1);
  CHECK(q15_from_float(0.49f / 32768.0f) == 0);
  CHECK(q15_from_float(-0.49f / 32768.0f) == 0);
  CHECK(q15_from_float(0.5f) == 16384);
  CHECK(q15_from_float(-0.5f) == -16384);
  CHECK(q15_from_float(1.0f) == 32767);            // +1.0 is one LSB above the largest int16: saturated
  CHECK(q15_from_float(-1.0f) == -32768);
  CHECK(q15_from_float(2.0f) == 32767);
  CHECK(q15_from_float(-2.0f) == -32768);
  CHECK(q15_from_float(1.0e30f) == 32767);
  CHECK(q15_from_float(-1.0e30f) == -32768);
  CHECK(q15_from_float(std::numeric_limits<float>::infinity()) == 32767);
  CHECK(q15_from_float(-std::numeric_limits<float>::infinity()) == -32768);
  CHECK(q15_from_float(std::numeric_limits<float>::quiet_NaN()) == 32767);  // the comparison form: NaN takes the saturating branch
  // Against the oracle on a grid, including every value around a rounding boundary.
  for (std::int32_t k = -70000; k <= 70000; k += 3) {
    const float x = static_cast<float>(k) / 65536.0f;  // exact in float
    CHECK(q15_from_float(x) == oracle_q15(static_cast<double>(x)));
  }
}

TEST_CASE("test signal: the tone follows the double oracle within one LSB, ramp included, and then holds the frequency exactly") {
  TestTone tone;
  const std::uint32_t inc = o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz);
  tone.configure(inc, o3sdk::platform::mk66::audio::kToneAmplitude, o3sdk::platform::mk66::audio::kToneRampSamples);
  CHECK_FALSE(tone.active());
  tone.set_on(true);
  CHECK(tone.active());

  alignas(8) float mono[haudio::kBlock];
  alignas(8) std::int16_t out[haudio::kBlockSamples];
  std::int32_t worst = 0;
  std::uint64_t n = 0;
  // 3,000 blocks (200 on the target): through the 2,205-sample ramp and the steady tone after it.
  for (std::uint32_t block = 0; block < kOracleBlocks; ++block) {
    tone.generate(mono, haudio::kBlock);
    o3sdk::platform::mk66::audio::float_to_q15_dup(mono, out, haudio::kBlock);
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i, ++n) {
      const std::int32_t want = oracle_sample(n, inc, static_cast<double>(o3sdk::platform::mk66::audio::kToneAmplitude), o3sdk::platform::mk66::audio::kToneRampSamples);
      for (std::uint32_t ch = 0; ch < 2; ++ch) {
        const std::int32_t d = out[2u * i + ch] - want;
        worst = (d < 0 ? -d : d) > worst ? (d < 0 ? -d : d) : worst;
      }
    }
    CHECK(tone.phase_q32() == static_cast<std::uint32_t>((n * inc) & 0xFFFFFFFFull));  // the block phase is exact, never drifting
  }
  CHECK_MESSAGE(worst <= 1, "worst deviation from the oracle: " << worst << " LSB");
  CHECK(tone.ramp_pos() == o3sdk::platform::mk66::audio::kToneRampSamples);
  CHECK(tone.gain() == o3sdk::platform::mk66::audio::kToneAmplitude);
}

TEST_CASE("test signal: the long-term phase is exact after 10,000 blocks and the sample there still matches the oracle") {
  TestTone tone;
  const std::uint32_t inc = o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz);
  tone.configure(inc, o3sdk::platform::mk66::audio::kToneAmplitude, o3sdk::platform::mk66::audio::kToneRampSamples);
  tone.set_on(true);
  alignas(8) float mono[haudio::kBlock];
  for (std::uint32_t block = 0; block < 10000; ++block) {
    tone.generate(mono, haudio::kBlock);
  }
  const std::uint64_t n = 10000ull * haudio::kBlock;
  CHECK(tone.phase_q32() == static_cast<std::uint32_t>((n * inc) & 0xFFFFFFFFull));
  tone.generate(mono, haudio::kBlock);
  for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
    const std::int32_t want = oracle_sample(n + i, inc, static_cast<double>(o3sdk::platform::mk66::audio::kToneAmplitude), o3sdk::platform::mk66::audio::kToneRampSamples);
    const std::int32_t got = o3sdk::platform::mk66::audio::q15_from_float(mono[i]);
    const std::int32_t d = got - want;
    CHECK((d < 0 ? -d : d) <= 1);
  }
}

TEST_CASE("test signal: positive-going zero crossings give the frequency against the rate constant") {
  // The tone's frequency in Hz is kToneHz x (true rate / rate constant): with the rate equal to the constant it is exactly
  // kToneHz. Count crossings over a whole number of seconds' worth of samples at the constant's rate.
  TestTone tone;
  const std::uint32_t inc = o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz);
  tone.configure(inc, o3sdk::platform::mk66::audio::kToneAmplitude, 1);  // a 1-sample ramp: full tone at once
  tone.set_on(true);
  const std::uint64_t samples_per_second = (static_cast<std::uint64_t>(haudio::kCpuHz) * haudio::kCyclesPerSample.den) / haudio::kCyclesPerSample.num;
  alignas(8) float mono[haudio::kBlock];
  std::uint64_t crossings = 0;
  float previous = 0.0f;
  std::uint64_t done = 0;
  const std::uint64_t total = samples_per_second * 2u;
  while (done + haudio::kBlock <= total) {
    tone.generate(mono, haudio::kBlock);
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
      if (previous < 0.0f && mono[i] >= 0.0f) {
        ++crossings;
      }
      previous = mono[i];
    }
    done += haudio::kBlock;
  }
  // 2 s at the constant's rate: 2,000 cycles, to within a crossing at either end.
  const double seconds = static_cast<double>(done) / (static_cast<double>(haudio::kCpuHz) / (static_cast<double>(haudio::kCyclesPerSample.num) / static_cast<double>(haudio::kCyclesPerSample.den)));
  const double hz = static_cast<double>(crossings) / seconds;
  CHECK(std::fabs(hz - 1000.0) < 1.0);
}

TEST_CASE("test signal: the ramp is linear, exact, and fades out to a stop") {
  TestTone tone;
  tone.configure(o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz), 0.1f, 100);
  alignas(8) float mono[haudio::kBlock];
  tone.set_on(true);
  tone.generate(mono, haudio::kBlock);
  CHECK(tone.ramp_pos() == 64u);
  tone.generate(mono, haudio::kBlock);
  CHECK(tone.ramp_pos() == 100u);                    // clamped at the top
  CHECK(tone.gain() == 0.1f);
  tone.set_on(false);
  CHECK(tone.active());                              // fading
  tone.generate(mono, haudio::kBlock);
  CHECK(tone.ramp_pos() == 36u);
  tone.generate(mono, haudio::kBlock);
  CHECK(tone.ramp_pos() == 0u);
  CHECK_FALSE(tone.active());                        // silent: the engine stops generating
  // The phase kept running through the fade.
  CHECK(tone.phase_q32() == static_cast<std::uint32_t>((4ull * haudio::kBlock * o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz)) & 0xFFFFFFFFull));
  tone.skip(10);
  CHECK(tone.phase_q32() == static_cast<std::uint32_t>((4ull * haudio::kBlock + 10u) * o3sdk::platform::mk66::audio::tone_phase_inc_q32(o3sdk::platform::mk66::audio::kToneHz)));
  tone.set_on(true);
  tone.generate(mono, 4);
  tone.reset_gain();
  CHECK(tone.ramp_pos() == 0u);
}
