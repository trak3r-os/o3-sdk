// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DELAY: a stereo feedback delay synced to the transport's tempo, the kit's example effect (docs/FRAMEWORK.md, "Units"). Meant for a
// send bus: the output is the wet signal only. Mono (each side its own echo) or ping-pong (the echoes alternate sides); a one-pole
// low-pass in the feedback path darkens each repeat. The time is a note value; a tempo change glides the read point (a tape-like
// pitch bend, no click).
//
// Memory: the delay lines are the app's (constructor), because they are large and belong in the SDRAM on the unit
// (core/placement.h). kMaxSeconds of line holds the longest time at the slowest tempo the app allows; longer times are clamped.

#include <o3sdk/dsp/unit.h>

#include <cstdint>

namespace o3sdk::units {

class Delay final : public dsp::Unit {
 public:
  enum Param : std::uint16_t { Time, Feedback, Tone, Mode, kParamCount };

  // `left` and `right` hold `capacity` samples each, and stay valid for the unit's life.
  Delay(float* left, float* right, std::uint32_t capacity) noexcept : left_(left), right_(right), capacity_(capacity) {}

  const dsp::UnitInfo& info() const noexcept override;
  void prepare(float sample_rate) noexcept override;
  void process(const dsp::ProcessArgs& args) noexcept override;

 private:
  void set(std::uint16_t id, float v) noexcept;
  float read(const float* line, float delay) const noexcept;

  float* left_;
  float* right_;
  std::uint32_t capacity_;
  std::uint32_t write_ = 0;
  float values_[kParamCount] = {};
  float fs_ = 44100.0f;
  float delay_ = 0.0f;  // samples, gliding towards the tempo's
  float tone_k_ = 0.5f;
  float lp_l_ = 0.0f;
  float lp_r_ = 0.0f;
};

}  // namespace o3sdk::units
