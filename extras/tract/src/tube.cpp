// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/tube.h>

#include <cmath>

namespace o3sdk::extras::tract {

namespace {
constexpr float kTinyState = 1.0e-15f;  // below this a state is zero: no denormals in the loop
constexpr float kHugeState = 1.0e6f;    // a state above this has blown up (or is not finite): the tube is cleared
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
  for (unsigned i = 0; i < n_; ++i) {
    const float a = std::fabs(b_[i]);
    if (!(a < kHugeState)) {  // also true for NaN
      reset();
      return true;
    }
    if (a < kTinyState) b_[i] = 0.0f;
  }
  const float a = std::fabs(f_);
  if (!(a < kHugeState)) {
    reset();
    return true;
  }
  return false;
}

}  // namespace o3sdk::extras::tract
