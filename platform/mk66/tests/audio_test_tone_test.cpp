// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bring-up test tone (audio/test_tone.h): its table against a double oracle, its phase increment for both rate constants, what it
// plays block by block (1 kHz at -20 dBFS, the left channel alone and faded in, then both), the silence flag, the input peak it
// reports, and the tone behind the real pipeline. What only the unit shows (the tone on the line out, its frequency on a counter) is
// the bring-up image's check.
#include <doctest/doctest.h>

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio/pipeline.h>
#include <o3sdk/platform/mk66/audio/test_tone.h>

#include <cmath>
#include <cstdint>

namespace {

namespace haudio = o3sdk::hal::audio;
using o3sdk::platform::mk66::audio::Pipeline;
using o3sdk::platform::mk66::audio::TestTone;
using o3sdk::platform::mk66::audio::tone_phase_inc_q32;

constexpr double kTwoPi = 6.283185307179586476925287;

// The tone's sample n (from init()) before its fade: A sin(2 pi n inc / 2^32), the phase taken exactly in Q32.
double oracle(std::uint64_t n) {
  const double turns = static_cast<double>((n * TestTone::kPhaseInc) & 0xFFFFFFFFull) / 4294967296.0;
  return static_cast<double>(TestTone::kAmplitude) * std::sin(kTwoPi * turns);
}

struct Block {
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report{};
};

void run(TestTone& t, Block& b, std::uint32_t flags = 0) {
  const haudio::BlockCallback cb = t.callback();
  const haudio::BlockIo io{b.in, b.out, flags, &b.report};
  cb.fn(cb.user, io);
}

int abs_i(int v) { return v < 0 ? -v : v; }

}  // namespace

TEST_CASE("test tone: the table is one period at -20 dBFS, each entry within half an LSB of the true sine") {
  static TestTone t;
  t.init();
  CHECK(t.table(0) == 0);
  CHECK(t.table(TestTone::kTableSize / 4u) == TestTone::kAmplitude);
  CHECK(t.table(TestTone::kTableSize / 2u) == 0);
  CHECK(t.table(3u * TestTone::kTableSize / 4u) == -TestTone::kAmplitude);
  CHECK(t.table(TestTone::kTableSize) == t.table(0));  // the guard entry
  double worst = 0.0;
  for (std::uint32_t i = 0; i < TestTone::kTableSize; ++i) {
    const double want = static_cast<double>(TestTone::kAmplitude) * std::sin(kTwoPi * static_cast<double>(i) / static_cast<double>(TestTone::kTableSize));
    const double d = std::fabs(static_cast<double>(t.table(i)) - want);
    worst = d > worst ? d : worst;
  }
  CHECK(worst <= 0.5001);
  // -20 dBFS: the peak is a tenth of full scale.
  CHECK(std::fabs(20.0 * std::log10(static_cast<double>(TestTone::kAmplitude) / 32768.0) + 20.0) < 0.01);
}

TEST_CASE("test tone: the phase increment is 1 kHz at the rate constant, for both clocks") {
  const auto expect = [](haudio::CyclesPerSample r) {
    const double rate = static_cast<double>(haudio::kCpuHz) * static_cast<double>(r.den) / static_cast<double>(r.num);
    return static_cast<std::uint32_t>(std::floor(1000.0 / rate * 4294967296.0 + 0.5));
  };
  const haudio::CyclesPerSample exact{1800000u, 441u};  // 44,100 Hz
  const haudio::CyclesPerSample old{4080u, 1u};         // 44,117.647 Hz
  CHECK(tone_phase_inc_q32(1000, exact) == expect(exact));
  CHECK(tone_phase_inc_q32(1000, old) == expect(old));
  CHECK(TestTone::kPhaseInc == tone_phase_inc_q32(1000));
  CHECK(tone_phase_inc_q32(44100, exact) == 0u);  // not below the sample rate: no tone
}

TEST_CASE("test tone: the left channel fades in and then plays the oracle, the right is silent until kLeftOnlyBlocks, then joins with its own fade") {
  static TestTone t;
  t.init();
  static Block b;
  int worst_ramp = 0;
  int worst_steady = 0;
  int worst_right = 0;
  std::uint32_t right_nonzero_early = 0;
  int peak = 0;
  const std::uint32_t blocks = TestTone::kLeftOnlyBlocks + 200u;
  for (std::uint32_t k = 0; k < blocks; ++k) {
    run(t, b);
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
      const std::uint64_t n = static_cast<std::uint64_t>(k) * haudio::kBlock + i;
      const double s = oracle(n);
      const std::uint64_t ramp_l = n + 1u < TestTone::kRampSamples ? n + 1u : TestTone::kRampSamples;
      const double want_l = s * static_cast<double>(ramp_l) / static_cast<double>(TestTone::kRampSamples);
      const int left = b.out[2u * i];
      const int right = b.out[2u * i + 1u];
      const int d_l = abs_i(left - static_cast<int>(std::lround(want_l)));
      if (ramp_l < TestTone::kRampSamples) {
        worst_ramp = d_l > worst_ramp ? d_l : worst_ramp;
      } else {
        worst_steady = d_l > worst_steady ? d_l : worst_steady;
      }
      peak = abs_i(left) > peak ? abs_i(left) : peak;
      peak = abs_i(right) > peak ? abs_i(right) : peak;
      if (k < TestTone::kLeftOnlyBlocks) {
        right_nonzero_early += right != 0 ? 1u : 0u;
      } else {
        const std::uint64_t m = n - static_cast<std::uint64_t>(TestTone::kLeftOnlyBlocks) * haudio::kBlock;
        const std::uint64_t ramp_r = m + 1u < TestTone::kRampSamples ? m + 1u : TestTone::kRampSamples;
        const double want_r = s * static_cast<double>(ramp_r) / static_cast<double>(TestTone::kRampSamples);
        const int d_r = abs_i(right - static_cast<int>(std::lround(want_r)));
        worst_right = d_r > worst_right ? d_r : worst_right;
      }
    }
  }
  CHECK(worst_steady <= 1);  // table rounding (0.5) and interpolation (0.25 at this amplitude)
  CHECK(worst_ramp <= 2);    // and the fade's truncation
  CHECK(worst_right <= 2);
  CHECK(right_nonzero_early == 0u);
  CHECK(peak <= TestTone::kAmplitude);
  CHECK(peak >= TestTone::kAmplitude - 2);
  CHECK(t.blocks() == TestTone::kLeftOnlyBlocks);  // the count stops: the script is done
}

TEST_CASE("test tone: 1000 cycles in one second of samples, and init() rewinds it") {
  static TestTone t;
  t.init();
  static Block b;
  const std::uint32_t rate = static_cast<std::uint32_t>(haudio::sample_rate_mhz() / 1000u);
  std::uint32_t crossings = 0;
  std::int16_t prev = 0;
  std::uint32_t n = 0;
  while (n < rate) {
    run(t, b);
    for (std::uint32_t i = 0; i < haudio::kBlock && n < rate; ++i, ++n) {
      const std::int16_t v = b.out[2u * i];
      crossings += (prev < 0 && v >= 0) ? 1u : 0u;
      prev = v;
    }
  }
  CHECK(crossings >= 999u);
  CHECK(crossings <= 1000u);
  t.init();
  run(t, b);
  CHECK(b.out[0] == 0);  // phase 0, the fade's first step
  CHECK(t.blocks() == 1u);
}

TEST_CASE("test tone: the silence flag writes zeros and the tone's clock runs on; the input block's peak is reported") {
  static TestTone a;
  static TestTone ref;
  a.init();
  ref.init();
  static Block ba;
  static Block br;
  for (std::uint32_t k = 0; k < 40u; ++k) {
    run(a, ba, (k >= 10u && k < 20u) ? haudio::kFlagSilence : 0u);
    run(ref, br);
    bool zero = true;
    bool same = true;
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      zero = zero && ba.out[i] == 0;
      same = same && ba.out[i] == br.out[i];
    }
    if (k >= 10u && k < 20u) {
      CHECK(zero);
    } else {
      CHECK(same);  // after the silent blocks the tone is where it would have been
    }
  }
  static Block in;
  in.in[17] = 1234;
  in.in[40] = -900;
  run(a, in);
  CHECK(in.report.input_peak == 1234u);
  in.in[99] = -32768;
  run(a, in);
  CHECK(in.report.input_peak == 32768u);
}

namespace {

std::uint32_t g_tone_cycles = 0;
std::uint32_t tone_cycles() noexcept { return g_tone_cycles += 100u; }

struct alignas(haudio::kTxRingBytes) ToneTx {
  std::int16_t s[haudio::kTxPhysSlots * haudio::kBlockSamples];
};
struct alignas(haudio::kRxRingBytes) ToneRx {
  std::int16_t s[haudio::kRxPhysSlots * haudio::kBlockSamples];
};

}  // namespace

TEST_CASE("test tone behind the pipeline: every boundary from kTxSlots on plays the tone's next block, nothing underruns") {
  static TestTone tone;
  static TestTone ref;
  tone.init();
  ref.init();
  static ToneTx tx;
  static ToneRx rx;
  static Pipeline p;
  Pipeline::Config config{};
  config.tx = tx.s;
  config.rx = rx.s;
  config.render = tone.callback();
  config.cycles = &tone_cycles;
  config.cycles_per_block = haudio::kCyclesPerBlock;
  config.starve_guard = false;
  p.init(config);
  static Block expected;
  std::uint32_t mismatched = 0;
  for (std::uint32_t b = 1; b <= 3000u; ++b) {
    p.on_boundary(b % haudio::kTxPhysSlots, (b + 1u) % haudio::kRxPhysSlots, b * haudio::kCyclesPerBlock, 0);
    if (b >= haudio::kTxSlots) {
      run(ref, expected);  // the block rendered for boundary b - (kTxSlots - 1) plays now
      const std::int16_t* playing = tx.s + (b % haudio::kTxPhysSlots) * haudio::kBlockSamples;
      for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
        mismatched += playing[i] != expected.out[i] ? 1u : 0u;
      }
    }
    p.render();
  }
  CHECK(mismatched == 0u);
  const auto s = p.status().snapshot();
  CHECK(s.boundaries == 3000u);
  CHECK(s.renders == 3000u);
  CHECK(s.underruns == 0u);
  CHECK(s.late_drops == 0u);
}
