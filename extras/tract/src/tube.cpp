// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/tube.h>

#include <cstdint>
#include <cstring>

namespace o3sdk::extras::tract {

namespace {
constexpr float kTinyState = 1.0e-15f;  // below this a state is zero: no denormals in the loop
constexpr float kHugeState = 1.0e6f;    // a state above this has blown up (or is not finite): the tube is cleared

constexpr std::uint32_t float_bits(float v) noexcept { return __builtin_bit_cast(std::uint32_t, v); }
constexpr std::uint32_t kTinyBits = float_bits(kTinyState);
constexpr std::uint32_t kHugeAdd = 0x80000000u - float_bits(kHugeState);   // a + kHugeAdd has bit 31 set exactly when a >= float_bits(kHugeState)
}  // namespace

Tube::Tube() noexcept { set_ends(0.8f, 0.8f); }

void Tube::reset() noexcept {
  for (float& v : b_) v = 0.0f;
  f_ = 0.0f;
}

void Tube::set_size(unsigned n) noexcept {
  if (n < 2) n = 2;
  if (n > kMaxSections) n = kMaxSections;
  for (unsigned i = n; i < kMaxSections; ++i) {
    b_[i] = 0.0f;
    k_[i] = 0.0f;
  }
  n_ = n;
}

void Tube::set_ends(float r_glottis, float r_lips) noexcept {
  // A NaN fails every comparison and lands on the default.
  rg_ = (r_glottis >= 0.0f && r_glottis <= 0.99f) ? r_glottis : (r_glottis > 0.99f ? 0.99f : 0.8f);
  rl_ = (r_lips >= 0.0f && r_lips <= 0.99f) ? r_lips : (r_lips > 0.99f ? 0.99f : 0.8f);
  out_gain_ = (1.0f - rl_) * lip_gain_;
}

void Tube::set_diameters(const float* d) noexcept {
  float a_prev = d[0] * d[0];
  for (unsigned i = 0; i + 1 < n_; ++i) {
    const float a_next = d[i + 1] * d[i + 1];
    k_[i] = (a_prev - a_next) / (a_prev + a_next);
    a_prev = a_next;
  }
  lip_gain_ = a_prev >= kLipRef ? 1.0f : a_prev * (1.0f / kLipRef);
  out_gain_ = (1.0f - rl_) * lip_gain_;
}

void Tube::set_reflection(unsigned i, float k) noexcept {
  if (i >= kMaxSections) return;
  k_[i] = (k > -0.9999f && k < 0.9999f) ? k : 0.0f;
}

bool Tube::sanitize() noexcept {
  // On the bits: for a float's magnitude a (the sign cleared) the integer order is the float order and a NaN is above every number, so
  // `a >= huge` is also the not-finite test and `a < tiny` includes zero and the denormals. kHugeAdd turns the first into the sign bit of a
  // sum, so one OR over the states carries the verdict.
  std::uint32_t bad = 0;
  for (unsigned i = 0; i < n_; ++i) {
    std::uint32_t a;
    std::memcpy(&a, &b_[i], sizeof a);
    a &= 0x7fffffffu;
    bad |= a + kHugeAdd;
    if (a < kTinyBits) b_[i] = 0.0f;
  }
  std::uint32_t a;
  std::memcpy(&a, &f_, sizeof a);
  a &= 0x7fffffffu;
  bad |= a + kHugeAdd;
  if (bad >> 31) {
    reset();
    return true;
  }
  return false;
}

}  // namespace o3sdk::extras::tract
