// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one screen of the template: the pads play the voice, F1..F3 + the encoder edit its three values (the kit's controls convention,
// o3-sdk app/param_keys.h), LEFT / RIGHT move the keyboard an octave. Everything here runs in the main loop (o3-sdk app/screen.h).

#include <o3sdk/app/param_keys.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/dsp/engine.h>

namespace @APP_ID@ {

class PlayScreen final : public o3sdk::app::Screen {
 public:
  // Called by the app's setup(): the unit this screen plays and edits.
  void init(o3sdk::dsp::UnitId voice) noexcept;

  const char* title() const noexcept override { return "PLAY"; }
  void input(o3sdk::app::Context& ctx, const o3sdk::app::Input& in) noexcept override;
  void draw(o3sdk::app::Context& ctx, o3sdk::ui::Renderer& r, const o3sdk::ui::Rect& area) noexcept override;
  void soft_keys(o3sdk::app::Context& ctx, o3sdk::app::SoftKeys& out) noexcept override;
  void leds(o3sdk::app::Context& ctx, o3sdk::app::PadLeds& leds) noexcept override;

  // For the console's `state` (the app's describe_state).
  int octave() const noexcept { return octave_; }
  const o3sdk::app::ParamKeys& keys() const noexcept { return keys_; }

 private:
  o3sdk::dsp::UnitId voice_ = o3sdk::dsp::kNoUnit;
  o3sdk::app::ParamPage page_ = {};
  o3sdk::app::ParamKeys keys_;
  int octave_ = 0;
  int held_[o3sdk::app::kPadCount] = {};  // the note each held pad started (-1: none), so the release names it even after an octave shift
};

}  // namespace @APP_ID@
