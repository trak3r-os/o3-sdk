// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/test_tone.h>

namespace o3sdk::platform::mk66::audio {

void TestTone::init() noexcept {
  for (std::uint32_t i = 0; i < kTableSize; ++i) {
    const float v = static_cast<float>(kAmplitude) * sin_turns(static_cast<float>(i) / static_cast<float>(kTableSize));
    table_[i] = static_cast<std::int16_t>(v + (v >= 0.0f ? 0.5f : -0.5f));
  }
  table_[kTableSize] = table_[0];  // the interpolation's guard entry: one period wraps onto itself
  phase_ = 0;
  ramp_l_ = 0;
  ramp_r_ = 0;
  blocks_ = 0;
}

void TestTone::render(void* user, const hal::audio::BlockIo& io) noexcept {
  TestTone& t = *static_cast<TestTone*>(user);
  const bool silent = (io.flags & hal::audio::kFlagSilence) != 0u;
  const bool both = t.blocks_ >= kLeftOnlyBlocks;
  std::uint32_t phase = t.phase_;
  std::uint32_t ramp_l = t.ramp_l_;
  std::uint32_t ramp_r = t.ramp_r_;
  for (std::uint32_t i = 0; i < hal::audio::kBlock; ++i) {
    const std::uint32_t idx = phase >> (32u - kTableBits);
    const std::int32_t frac = static_cast<std::int32_t>((phase >> (16u - kTableBits)) & 0xFFFFu);
    const std::int32_t a = t.table_[idx];
    const std::int32_t b = t.table_[idx + 1u];
    const std::int32_t s = a + (((b - a) * frac) >> 16);
    phase += kPhaseInc;
    if (ramp_l < kRampSamples) {
      ++ramp_l;
    }
    std::int32_t right = 0;
    if (both) {
      if (ramp_r < kRampSamples) {
        ++ramp_r;
      }
      right = (s * static_cast<std::int32_t>(ramp_r)) >> kRampShift;
    }
    const std::int32_t left = (s * static_cast<std::int32_t>(ramp_l)) >> kRampShift;
    io.out[2u * i] = silent ? std::int16_t{0} : static_cast<std::int16_t>(left);
    io.out[2u * i + 1u] = silent ? std::int16_t{0} : static_cast<std::int16_t>(right);
  }
  t.phase_ = phase;
  t.ramp_l_ = ramp_l;
  t.ramp_r_ = ramp_r;
  if (t.blocks_ < kLeftOnlyBlocks) {
    ++t.blocks_;
  }

  std::uint32_t peak = 0;
  for (std::uint32_t i = 0; i < hal::audio::kBlockSamples; ++i) {
    const std::int32_t v = io.in[i];
    const std::uint32_t mag = static_cast<std::uint32_t>(v < 0 ? -v : v);
    peak = mag > peak ? mag : peak;
  }
  io.report->input_peak = peak;
}

}  // namespace o3sdk::platform::mk66::audio
