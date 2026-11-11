// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/halfband.h>

#include <cstring>

namespace o3sdk::extras::tract {

namespace {

constexpr unsigned kRing = 16;   // the doubled ring of the even branch holds kRing floats twice

// The even branch over the 12 newest samples w[0] (oldest) .. w[11] (newest): the response is symmetric, so pairs are added first.
inline float even_branch(const float* w) noexcept {
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
inline const float* ring_push(float* ring, unsigned& pos, float v) noexcept {
  ring[pos] = v;
  ring[pos + kRing] = v;
  pos = (pos + 1u) & (kRing - 1u);
  // the newest is at index pos - 1 + kRing (the copy), the window starts 11 earlier
  return ring + pos + kRing - 12u;
}

}  // namespace

void HalfbandDown::reset() noexcept {
  std::memset(even_, 0, sizeof even_);
  std::memset(odd_, 0, sizeof odd_);
  epos_ = 0;
  opos_ = 0;
}

void HalfbandDown::push_odd(float x) noexcept {
  odd_[opos_ & 7u] = x;
  ++opos_;
}

float HalfbandDown::push_even(float x) noexcept {
  const float* w = ring_push(even_, epos_, x);
  // xo[m - 6]: opos_ counts the odd samples seen so far, the newest is xo[m - 1] at opos_ - 1
  return even_branch(w) + 0.5f * odd_[(opos_ - 6u) & 7u];
}

void HalfbandUp::reset() noexcept {
  std::memset(even_, 0, sizeof even_);
  std::memset(odd_, 0, sizeof odd_);
  epos_ = 0;
  opos_ = 0;
}

void HalfbandUp::push(float u, float* out2) noexcept {
  const float* w = ring_push(even_, epos_, u);
  odd_[opos_ & 7u] = u;
  out2[0] = 2.0f * even_branch(w);
  out2[1] = odd_[(opos_ - 5u) & 7u];
  ++opos_;
}

}  // namespace o3sdk::extras::tract
