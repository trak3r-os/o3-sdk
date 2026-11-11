// SPDX-License-Identifier: GPL-3.0-or-later
// A half-band FIR (23 taps, 6 distinct coefficients and the centre 0.5), polyphase: the decimator by two and the interpolator by two the
// processor uses to run the tube at fs/2 (one stage) or fs/4 (two stages). The coefficients are from tools/design_halfband.py (a
// minimax design by linear programming) and frozen here. In the response of one stage (f in units of the stage's input rate fs):
// ripple +-0.048 dB below 0.2 fs and 45.1 dB of rejection above 0.3 fs (halfband_test asserts at most 0.5 dB and at least 45 dB).
// A 15-tap design reaches only 32.5 dB; that is why the plan's 15 taps became 23 (deviation recorded in the README).
//
// h[n], n = 0..22, centre 11: h[11 +- (2j+1)] = kHalfbandCoef[j], h[11] = 0.5, every other tap 0. Even taps (indices 0, 2, ..., 22) are the
// 12 of the "even branch", e[l] = h[2 l], symmetric; the one odd tap is the centre, a pure delay of 5 or 6 samples of the half rate.
//
//   decimate: y[m] = sum_l e[l] x[2 (m - l)] + 0.5 x[2 (m - 6) + 1]
//   interpolate: out[2 m] = 2 sum_l e[l] u[m - l], out[2 m + 1] = u[m - 5]       (gain 2: the zero-stuffed input has half the energy)
//
// Delay of a stage: 11 samples of the faster rate; down plus up 22. The phase of the decimation is the caller's: it pushes the samples of
// even position with push_even() (which returns an output) and those of odd position with push_odd(), so any block size gives the same
// samples. All state is members (under 200 bytes per stage); no heap, no double.
#pragma once

#include <cstdint>

namespace o3sdk::extras::tract {

inline constexpr unsigned kHalfbandSide = 6;   // odd taps per side of the centre
inline constexpr float kHalfbandCoef[kHalfbandSide] = {0.31499507f,   -0.0964709786f, 0.0485973006f,
                                                       -0.0262910236f, 0.0136096414f,  -0.00720961731f};

namespace detail {

inline constexpr unsigned kHalfbandRing = 16;   // the doubled ring of the even branch holds this many floats twice

// The even branch over the 12 newest samples w[0] (oldest) .. w[11] (newest): the response is symmetric, so pairs are added first.
inline float halfband_even_branch(const float* w) noexcept {
  const float* c = kHalfbandCoef;
  float s = c[5] * (w[11] + w[0]);
  s += c[4] * (w[10] + w[1]);
  s += c[3] * (w[9] + w[2]);
  s += c[2] * (w[8] + w[3]);
  s += c[1] * (w[7] + w[4]);
  s += c[0] * (w[6] + w[5]);
  return s;
}

// Appends v to the doubled ring: returns the pointer to the 12 newest in time order.
inline const float* halfband_ring_push(float* ring, unsigned& pos, float v) noexcept {
  ring[pos] = v;
  ring[pos + kHalfbandRing] = v;
  pos = (pos + 1u) & (kHalfbandRing - 1u);
  // the newest is at index pos - 1 + kHalfbandRing (the copy), the window starts 11 earlier
  return ring + pos + kHalfbandRing - 12u;
}

}  // namespace detail

// The pushes are defined here, inline: the processor's loops run them without a call and keep the coefficients in registers.
class HalfbandDown {
 public:
  void reset() noexcept;
  void push_odd(float x) noexcept {           // the sample at the odd position of the pair (the one after the output's)
    odd_[opos_ & 7u] = x;
    ++opos_;
  }
  float push_even(float x) noexcept {         // the sample at the even position: returns the decimated output
    const float* w = detail::halfband_ring_push(even_, epos_, x);
    // xo[m - 6]: opos_ counts the odd samples seen so far, the newest is xo[m - 1] at opos_ - 1
    return detail::halfband_even_branch(w) + 0.5f * odd_[(opos_ - 6u) & 7u];
  }

 private:
  static constexpr unsigned kTaps = 12;       // the even branch
  alignas(8) float even_[2 * 16] = {};        // a doubled ring: the window of kTaps newest is contiguous
  alignas(8) float odd_[8] = {};              // a ring of 8: the centre tap reads 6 back
  unsigned epos_ = 0;
  unsigned opos_ = 0;
};

class HalfbandUp {
 public:
  void reset() noexcept;
  void push(float u, float* out2) noexcept {   // one input, two outputs (out2[0], out2[1])
    const float* w = detail::halfband_ring_push(even_, epos_, u);
    odd_[opos_ & 7u] = u;
    out2[0] = 2.0f * detail::halfband_even_branch(w);
    out2[1] = odd_[(opos_ - 5u) & 7u];
    ++opos_;
  }

 private:
  static constexpr unsigned kTaps = 12;
  alignas(8) float even_[2 * 16] = {};
  alignas(8) float odd_[8] = {};
  unsigned epos_ = 0;
  unsigned opos_ = 0;
};

static_assert(alignof(HalfbandDown) >= 8 && alignof(HalfbandUp) >= 8, "the rings are 8-byte aligned");

}  // namespace o3sdk::extras::tract
