// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/test_signal.h>

namespace o3sdk::platform::mk66::audio {

void float_to_q15_dup(const float* mono, std::int16_t* out, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    const std::int32_t v = q15_from_float(mono[i]);
    out[2u * i] = static_cast<std::int16_t>(v);
    out[2u * i + 1u] = static_cast<std::int16_t>(v);
  }
}

void float_to_q15_stereo(const float* left, const float* right, std::int16_t* out, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    out[2u * i] = static_cast<std::int16_t>(q15_from_float(left[i]));
    out[2u * i + 1u] = static_cast<std::int16_t>(q15_from_float(right[i]));
  }
}

void TestTone::configure(std::uint32_t phase_inc, float amplitude, std::uint32_t ramp_samples) noexcept {
  phase_inc_q32_ = phase_inc;
  phase_inc_ = static_cast<float>(phase_inc) * (1.0f / 4294967296.0f);
  amplitude_ = amplitude;
  ramp_samples_ = ramp_samples == 0u ? 1u : ramp_samples;
  ramp_step_ = amplitude / static_cast<float>(ramp_samples_);
  ramp_pos_ = 0;
  on_ = false;
}

void TestTone::generate(float* out, std::uint32_t n) noexcept {
  // The block starts at the exact phase; the top 24 bits fit a float's mantissa.
  float p = static_cast<float>(phase_q32_ >> 8) * (1.0f / 16777216.0f);
  std::uint32_t pos = ramp_pos_;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (on_) {
      pos += pos < ramp_samples_ ? 1u : 0u;
    } else {
      pos -= pos != 0u ? 1u : 0u;
    }
    out[i] = gain_of(pos) * sin_turns(p);
    p += phase_inc_;
    if (p >= 1.0f) {
      p -= 1.0f;
    }
  }
  ramp_pos_ = pos;
  phase_q32_ += n * phase_inc_q32_;
}

void TestTone::generate_noise(float* out, std::uint32_t n, std::uint32_t& state) noexcept {
  std::uint32_t pos = ramp_pos_;
  std::uint32_t x = state != 0u ? state : 0x9E3779B9u;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (on_) {
      pos += pos < ramp_samples_ ? 1u : 0u;
    } else {
      pos -= pos != 0u ? 1u : 0u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = gain_of(pos) * (static_cast<float>(static_cast<std::int32_t>(x)) * (1.0f / 2147483648.0f));
  }
  ramp_pos_ = pos;
  state = x;
}

}  // namespace o3sdk::platform::mk66::audio
