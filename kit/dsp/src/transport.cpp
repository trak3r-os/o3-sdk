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

// The divisor is below 2^32, so a remainder shifted by 32 fits 64 bits, and rounding the next 32 bits up cannot reach 2^32 (it is at most 2^32 - 1, see ticks_per_sample_lo).
static_assert(60u * kRateMilliHz < (1ull << 32), "the Q32.64 remainder step fits in 64 bits");

}  // namespace

std::uint64_t ticks_per_sample_q32(std::uint32_t tempo_mbpm) noexcept {
  const std::uint64_t num = (static_cast<std::uint64_t>(clamp_tempo(tempo_mbpm)) * kPpqn) << 32;
  return num / (60u * kRateMilliHz);
}

std::uint32_t ticks_per_sample_lo(std::uint32_t tempo_mbpm) noexcept {
  const std::uint64_t den = 60u * kRateMilliHz;
  const std::uint64_t rem = ((static_cast<std::uint64_t>(clamp_tempo(tempo_mbpm)) * kPpqn) << 32) % den;  // the part of the Q32 division that was dropped, in units of 1 / den
  const std::uint64_t num = rem << 32;
  // Round up: rem < den < 2^32, so num / den <= 2^32 - 2^32 / den < 2^32 - 1 (den < 2^32), and the ceiling is at most 2^32 - 1.
  return static_cast<std::uint32_t>(num / den + (num % den != 0u ? 1u : 0u));
}

float samples_per_sixteenth(std::uint32_t tempo_mbpm) noexcept {
  // 60 / bpm / 4 seconds, in samples.
  return 15.0f * core::kRefRateHz * 1000.0f / static_cast<float>(clamp_tempo(tempo_mbpm));
}

int frame_at(const Transport& t, std::uint64_t tick) noexcept {
  if (!t.playing || t.inc == 0u || t.frames == 0u) {
    return -1;
  }
  // The block owns the whole ticks after the previous block's last frame and up to its own: (prev_last(), last()] with the previous block's real bound (an exact partition
  // whatever the increments), else (pos - inc, last()] in Q32.64, which is that bound while the increment is the same.
  const std::uint64_t q = tick << 32;
  bool before = false;  // another block reached it first
  if (t.has_prev()) {
    before = q <= t.prev_last();  // (q is whole: the position reaches it exactly when its rounded-down value does)
  } else {
    const std::uint64_t borrow = t.pos_lo < t.inc_lo ? 1u : 0u;  // (the borrow of pos - inc)
    before = q + t.inc + borrow <= t.pos;
  }
  if (before || q > t.last()) {
    return -1;
  }
  if (q <= t.pos) {
    return 0;
  }
  // The first f with at(f) >= q. With inc (the high words alone) it is ceil((q - pos) / inc); the low words add at most a part in 2^32 of a sample's step a frame, which is
  // less than one frame in a block (inc >= 2^24 at the slowest tempo, a block is a few hundred frames): at most one frame earlier.
  std::uint64_t f = (q - t.pos + t.inc - 1u) / t.inc;
  if (f > 0u && f - 1u < t.frames && t.at(static_cast<std::uint32_t>(f - 1u)) >= q) {
    --f;
  }
  return static_cast<int>(f);
}

}  // namespace o3sdk::dsp
