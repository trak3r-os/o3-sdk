// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SYNTH: a small polyphonic subtractive voice, the kit's example instrument (docs/FRAMEWORK.md, "Units"). Eight voices; one
// oscillator (sine, triangle, PolyBLEP saw or square); a 12 dB state-variable low-pass (the TPT form, Zavalishin); an ADSR on the
// amplitude. Every parameter is on one page of eight, so F1..F8 reach them all.
//
// Cost [est]: about 40 cycles a voice a sample on the Cortex-M4F; eight sounding voices near 8 % of a block. Not measured on a unit.

#include <o3sdk/dsp/unit.h>

#include <cstdint>

namespace o3sdk::units {

class Synth final : public dsp::Unit {
 public:
  enum Param : std::uint16_t { Wave, Cutoff, Reso, Attack, Decay, Sustain, Release, Volume, kParamCount };
  static constexpr unsigned kVoices = 8;

  const dsp::UnitInfo& info() const noexcept override;
  void prepare(float sample_rate) noexcept override;
  void process(const dsp::ProcessArgs& args) noexcept override;

  // Voices sounding now (any stage but idle): for a test or a screen.
  unsigned active_voices() const noexcept;

 private:
  enum class Stage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };
  struct Voice {
    Stage stage = Stage::Idle;
    std::uint8_t note = 0;
    std::uint32_t age = 0;  // note-on order, for stealing
    float phase = 0.0f;
    float inc = 0.0f;
    float env = 0.0f;
    float vel = 0.0f;
    float ic1 = 0.0f;  // the filter's two integrator states
    float ic2 = 0.0f;
  };

  void apply(const dsp::Event& e) noexcept;
  void set(std::uint16_t id, float v) noexcept;
  void note_on(std::uint8_t note, std::uint8_t velocity) noexcept;
  void note_off(std::uint8_t note) noexcept;
  void render(float* out, std::uint32_t from, std::uint32_t to) noexcept;
  float coeff_ms(float ms) const noexcept;  // one-pole coefficient reaching 1/e in `ms`

  Voice voices_[kVoices];
  float values_[kParamCount] = {};
  float fs_ = 44100.0f;
  float g_ = 0.1f;        // the filter's tan(pi fc / fs), reached at the end of the last run
  float g_target_ = 0.1f;
  float k_ = 1.6f;        // the filter's damping (2 - 2 * resonance, floored)
  float attack_step_ = 0.01f;
  float decay_coef_ = 0.999f;
  float release_coef_ = 0.999f;
  float gain_ = 0.35f;
  std::uint32_t clock_ = 0;
};

}  // namespace o3sdk::units
