// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VOICE: the template's one unit, a small monophonic instrument (o3-sdk docs/FRAMEWORK.md, "Units"). Notes in, a mono signal out
// (written to both channels): one oscillator (sine, triangle or a naive saw), a click-free attack and an exponential decay that ends the
// note by itself, a fast release when the key goes up. It is a starting point: replace it with your engine, keep the shape.
//
// A unit is DSP only: a parameter table, prepare() and process(). It has no screen and no buttons, and it never owns the values the user
// sees (the engine's main side stores them, formats them and steps them with the encoder; the unit gets each change as a Param event).
//
// Parameters (the id is the index in the table: append, never insert). Values are in the descriptor's own units:
//   0 Wave   0 SIN, 1 TRI, 2 SAW (naive: it aliases; units/synth in o3-sdk has a PolyBLEP saw)
//   1 Decay  ms, 20..4000: about the whole fall of the note (four time constants)
//   2 Level  dB, -48..+6
//
// Cost [est]: about 25 cycles a sample on the Cortex-M4F (one phase step, a shape, an envelope step, a gain ramp), so a block of 64 frames
// is about 1,600 cycles, 0.6 % of the 261,224 a block has at 44.1 kHz. Not measured on a unit: the render's CPU % in the header is the
// figure (o3-sdk skill rt-optimize).

#include <o3sdk/dsp/smooth.h>
#include <o3sdk/dsp/unit.h>

#include <cstdint>

namespace @APP_ID@ {

class Voice final : public o3sdk::dsp::Unit {
 public:
  enum Param : std::uint16_t { Wave, Decay, Level, kParamCount };

  const o3sdk::dsp::UnitInfo& info() const noexcept override;
  void prepare(float sample_rate) noexcept override;
  void process(const o3sdk::dsp::ProcessArgs& args) noexcept override;

  // True while a note sounds (any stage but idle): for a test or a screen.
  bool sounding() const noexcept { return stage_ != Stage::Idle; }

 private:
  enum class Stage : std::uint8_t { Idle, Attack, Decay, Release };

  void apply(const o3sdk::dsp::Event& e) noexcept;
  void set(std::uint16_t id, float v) noexcept;
  void render(float* out, std::uint32_t from, std::uint32_t to) noexcept;
  float coeff_ms(float ms) const noexcept;  // one-pole coefficient reaching 1/e in `ms`

  float values_[kParamCount] = {};
  float fs_ = 44100.0f;
  Stage stage_ = Stage::Idle;
  std::uint8_t note_ = 0;
  float phase_ = 0.0f;
  float inc_ = 0.0f;  // phase increment of the sounding note
  float env_ = 0.0f;
  float vel_ = 0.0f;
  float attack_step_ = 0.01f;
  float decay_coef_ = 0.999f;
  float release_coef_ = 0.999f;
  o3sdk::dsp::Ramp gain_;  // the level, ramped over 5 ms: a turn of the encoder does not click
};

}  // namespace @APP_ID@
