// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/units/delay/delay.h>

#include <cmath>

namespace o3sdk::units {

namespace {

using core::ParamCurve;
using core::ParamUnit;

constexpr const char* kTimeNames[] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
constexpr float kTimeSixteenths[] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
constexpr const char* kModeNames[] = {"MONO", "PING"};

// name, unit text, names, knots, min, max, default, detents, coarse, list count, display, curve, flags, page
constexpr core::ParamDesc kParams[Delay::kParamCount] = {
    {"TIME", "", kTimeNames, nullptr, 0.0f, 5.0f, 2.0f, 5, 1, 6, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"FEEDBACK", "%", nullptr, nullptr, 0.0f, 90.0f, 45.0f, 90, 10, 0, ParamUnit::Int, ParamCurve::Linear, core::kParamSmooth, 0},
    {"TONE", "Hz", nullptr, nullptr, 400.0f, 12000.0f, 4000.0f, 60, 10, 0, ParamUnit::Hz, ParamCurve::Log, core::kParamSmooth, 0},
    {"MODE", "", kModeNames, nullptr, 0.0f, 1.0f, 1.0f, 1, 1, 2, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
};

constexpr dsp::UnitInfo kInfo{"DELAY", dsp::UnitKind::Effect, kParams, Delay::kParamCount, nullptr, 0};

constexpr float kTwoPi = 6.28318531f;

}  // namespace

const dsp::UnitInfo& Delay::info() const noexcept { return kInfo; }

void Delay::prepare(float sample_rate) noexcept {
  fs_ = sample_rate;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    left_[i] = 0.0f;
    right_[i] = 0.0f;
  }
  write_ = 0;
  lp_l_ = 0.0f;
  lp_r_ = 0.0f;
  for (unsigned p = 0; p < kParamCount; ++p) {
    set(static_cast<std::uint16_t>(p), kParams[p].def);
  }
  delay_ = 0.0f;
}

void Delay::set(std::uint16_t id, float v) noexcept {
  if (id >= kParamCount) {
    return;
  }
  values_[id] = v;
  if (id == Tone) {
    tone_k_ = 1.0f - std::exp(-kTwoPi * v / fs_);
  }
}

float Delay::read(const float* line, float delay) const noexcept {
  float pos = static_cast<float>(write_) - delay;
  while (pos < 0.0f) {
    pos += static_cast<float>(capacity_);
  }
  const auto i0 = static_cast<std::uint32_t>(pos);
  const float frac = pos - static_cast<float>(i0);
  const std::uint32_t a = i0 % capacity_;
  const std::uint32_t b = (a + 1u) % capacity_;
  return line[a] + (line[b] - line[a]) * frac;
}

void Delay::process(const dsp::ProcessArgs& args) noexcept {
  if (capacity_ < 4u) {
    for (std::uint32_t i = 0; i < args.frames; ++i) {
      args.out_l[i] = 0.0f;
      args.out_r[i] = 0.0f;
    }
    return;
  }
  dsp::for_each_run(
      args,
      [&](std::uint32_t from, std::uint32_t to) {
        const auto t = static_cast<unsigned>(values_[Time] + 0.5f);
        float target = kTimeSixteenths[t < 6u ? t : 5u] * dsp::samples_per_sixteenth(args.transport->tempo_mbpm);
        const auto max = static_cast<float>(capacity_ - 2u);
        target = target > max ? max : target;
        if (delay_ <= 0.0f) {
          delay_ = target;
        }
        const float fb = values_[Feedback] * 0.01f;
        const bool ping = values_[Mode] >= 0.5f;
        for (std::uint32_t i = from; i < to; ++i) {
          delay_ += (target - delay_) * 0.0005f;
          const float wl = read(left_, delay_);
          const float wr = read(right_, delay_);
          lp_l_ += tone_k_ * (wl - lp_l_);
          lp_r_ += tone_k_ * (wr - lp_r_);
          const float in_l = args.in_l[i];
          const float in_r = args.in_r[i];
          if (ping) {
            left_[write_] = 0.5f * (in_l + in_r) + fb * lp_r_;
            right_[write_] = fb * lp_l_;
          } else {
            left_[write_] = in_l + fb * lp_l_;
            right_[write_] = in_r + fb * lp_r_;
          }
          write_ = write_ + 1u < capacity_ ? write_ + 1u : 0u;
          args.out_l[i] = wl;
          args.out_r[i] = wr;
        }
      },
      [&](const dsp::Event& e) {
        if (e.kind == dsp::EventKind::Param) {
          set(e.param, e.value);
        }
      });
}

}  // namespace o3sdk::units
