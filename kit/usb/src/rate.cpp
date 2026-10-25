// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/rate.h>

namespace o3sdk::usb {
namespace {

constexpr unsigned kAlphaShift = 7;    // phase gain  alpha = 2^-7
constexpr unsigned kBetaShift = 17;    // period gain beta  = 2^-17
// The period estimate never leaves nominal +/- 0.5 %, in Q16 cycles (constants: no run-time division).
constexpr std::int64_t kSpanFull = static_cast<std::int64_t>(180000u / 200u) << 16;
constexpr std::int64_t kSpanHigh = static_cast<std::int64_t>(22500u / 200u) << 16;
static_assert(nominal_sof_cycles(Speed::Full) == 180000u && nominal_sof_cycles(Speed::High) == 22500u,
              "the span constants above follow nominal_sof_cycles()");

// v / 2^n truncated toward zero, like '/', but a shift: the compiler turns a 64-bit '/' into a libgcc call
// (about 1 KB of flash and 100+ cycles) even for a power of two at -Os.
constexpr std::int64_t shr_trunc(std::int64_t v, unsigned n) noexcept { return v >= 0 ? (v >> n) : -((-v) >> n); }

}  // namespace

std::uint32_t pack_feedback(Speed s, std::uint32_t value, std::uint8_t* out) noexcept {
  const std::uint32_t n = feedback_bytes(s);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu);
  }
  return n;
}

void RateEstimator::reset(Speed s, std::uint32_t multiplier) noexcept {
  speed_ = s;
  multiplier_ = multiplier;
  started_ = false;
  frame_mask_ = s == Speed::Full ? 0x7FFu : 0x3FFFu;
  last_stamp_ = 0;
  last_frame_ = 0;
  count_ = 0;
  run_ = 0;
  resyncs_ = 0;
  t_ = 0;
  pred_q16_ = 0;
  period_q16_ = static_cast<std::int64_t>(nominal_sof_cycles(s)) << 16;
}

void RateEstimator::on_sof(std::uint32_t cyccnt, std::uint32_t frame) noexcept {
  if (!started_) {
    started_ = true;
    t_ = cyccnt;
    last_stamp_ = cyccnt;
    last_frame_ = frame & frame_mask_;
    pred_q16_ = t_ << 16;  // the first SOF defines the phase; its prediction error is zero by definition
    pred_q16_ += static_cast<std::uint64_t>(period_q16_);
    count_ = 1;
    run_ = 1;
    return;
  }
  const std::uint32_t gap = (frame - last_frame_) & frame_mask_;
  if (gap == 0) {
    return;  // the same SOF reported twice
  }
  t_ += static_cast<std::uint32_t>(cyccnt - last_stamp_);
  last_stamp_ = cyccnt;
  last_frame_ = frame & frame_mask_;
  ++count_;
  if (gap > kMaxGap) {  // lost track: take the phase from this SOF, keep the period
    pred_q16_ = (t_ << 16) + static_cast<std::uint64_t>(period_q16_);
    ++resyncs_;
    run_ = 0;
    return;
  }
  // Predicted arrival of this SOF: the prediction for the next one made last time, plus the missed periods.
  const std::uint64_t arrival = pred_q16_ + static_cast<std::uint64_t>(static_cast<std::int64_t>(gap - 1u) * period_q16_);
  std::int64_t r = static_cast<std::int64_t>((t_ << 16) - arrival);  // phase error, Q16 cycles
  const std::int64_t limit = shr_trunc(period_q16_, 2);
  r = r > limit ? limit : (r < -limit ? -limit : r);

  const std::int64_t nominal = static_cast<std::int64_t>(nominal_sof_cycles(speed_)) << 16;
  period_q16_ += shr_trunc(r, kBetaShift);
  const std::int64_t span = speed_ == Speed::Full ? kSpanFull : kSpanHigh;
  period_q16_ = period_q16_ > nominal + span ? nominal + span : (period_q16_ < nominal - span ? nominal - span : period_q16_);
  // Unsigned arithmetic: the phase counters wrap modulo 2^64 together (only their difference is used).
  pred_q16_ = arrival + static_cast<std::uint64_t>(shr_trunc(r, kAlphaShift)) + static_cast<std::uint64_t>(period_q16_);
  if (run_ < kLockSofs) {
    ++run_;
  }
}

std::uint32_t RateEstimator::frames_per_sof_q16() const noexcept {
  const std::uint64_t q8 = static_cast<std::uint64_t>(period_q16_ >> 8);
  return static_cast<std::uint32_t>((q8 * multiplier_ + (std::uint64_t{1} << 31)) >> 32);
}

}  // namespace o3sdk::usb
