// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small helpers a unit uses to avoid zipper noise: a linear ramp to a target over a fixed number of samples, and a one-pole
// smoother. Plain structs, no heap; pure arithmetic in float.

#include <cstdint>

namespace o3sdk::dsp {

// Moves from the current value to a target in `length` samples, linearly; then holds the target.
class Ramp {
 public:
  explicit Ramp(float value = 0.0f) noexcept : value_(value), target_(value) {}

  void set_length(std::uint32_t samples) noexcept { length_ = samples != 0u ? samples : 1u; }
  void jump(float value) noexcept {
    value_ = value;
    target_ = value;
    left_ = 0;
  }
  void target(float value) noexcept {
    target_ = value;
    left_ = length_;
    step_ = (target_ - value_) / static_cast<float>(length_);
  }
  float next() noexcept {
    if (left_ != 0u) {
      value_ += step_;
      if (--left_ == 0u) {
        value_ = target_;
      }
    }
    return value_;
  }
  float value() const noexcept { return value_; }
  float target_value() const noexcept { return target_; }
  bool moving() const noexcept { return left_ != 0u; }

 private:
  float value_;
  float target_;
  float step_ = 0.0f;
  std::uint32_t length_ = 64;
  std::uint32_t left_ = 0;
};

// y += k * (x - y): k = 1 - exp(-1 / (time * rate)); the caller computes k once (set_coeff) where the time is known.
class OnePole {
 public:
  void set_coeff(float k) noexcept { k_ = k; }
  void jump(float value) noexcept { y_ = value; }
  float next(float x) noexcept {
    y_ += k_ * (x - y_);
    return y_;
  }
  float value() const noexcept { return y_; }

 private:
  float k_ = 1.0f;
  float y_ = 0.0f;
};

}  // namespace o3sdk::dsp
