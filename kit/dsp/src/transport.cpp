// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/dsp/transport.h>

#include <o3sdk/core/audio_clock.h>

namespace o3sdk::dsp {

namespace {

// The engine's rate in millihertz, rounded (44,100,000 at the default clock), from the one rate constant.
constexpr std::uint64_t kRateMilliHz =
    (static_cast<std::uint64_t>(core::kCpuHz) * 1000u * core::kRefCyclesPerSample.den + core::kRefCyclesPerSample.num / 2u) /
    core::kRefCyclesPerSample.num;

constexpr std::uint32_t clamp_tempo(std::uint32_t mbpm) noexcept {
  return mbpm < kMinTempoMilliBpm ? kMinTempoMilliBpm : (mbpm > kMaxTempoMilliBpm ? kMaxTempoMilliBpm : mbpm);
}

// ticks per sample = bpm * ppqn / (60 * rate) = mbpm * ppqn / (60 * rate_mhz). The numerator shifted by 32 stays below 2^61 at the
// fastest tempo (300,000 * 960 = 2.9e8 < 2^29), so the division is exact in 64 bits.
static_assert(static_cast<std::uint64_t>(kMaxTempoMilliBpm) * kPpqn < (1ull << 31), "the Q32 numerator fits in 64 bits");

}  // namespace

std::uint64_t ticks_per_sample_q32(std::uint32_t tempo_mbpm) noexcept {
  const std::uint64_t num = (static_cast<std::uint64_t>(clamp_tempo(tempo_mbpm)) * kPpqn) << 32;
  return num / (60u * kRateMilliHz);
}

float samples_per_sixteenth(std::uint32_t tempo_mbpm) noexcept {
  // 60 / bpm / 4 seconds, in samples.
  return 15.0f * core::kRefRateHz * 1000.0f / static_cast<float>(clamp_tempo(tempo_mbpm));
}

int frame_at(const Transport& t, std::uint64_t tick) noexcept {
  if (!t.playing || t.inc == 0u || t.frames == 0u) {
    return -1;
  }
  // Frame f reaches every position in (pos + (f - 1) inc, pos + f inc]: the block owns (pos - inc, pos + (frames - 1) inc].
  const std::uint64_t q = tick << 32;
  if (q + t.inc <= t.pos || q > t.last()) {
    return -1;
  }
  return q <= t.pos ? 0 : static_cast<int>((q - t.pos + t.inc - 1u) / t.inc);
}

}  // namespace o3sdk::dsp
