// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one screen of the template: the pads play the voice and record what they play into a 16-step pattern, F1..F3 + the encoder edit
// the voice's three values (the kit's controls convention, o3-sdk app/param_keys.h), LEFT / RIGHT move the keyboard an octave, DELETE
// clears the pattern. It also shows the card and the last file job. The file keys (FILE saves, SAMPLE LOAD loads) are the app's, in
// src/app.cpp. Everything here runs in the main loop (o3-sdk app/screen.h).

#include <@APP_ID@/pattern.h>

#include <o3sdk/app/help.h>
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
  // HELP (Shift + Config) draws what the buttons and the pads do here from the data this gives (o3-sdk app/help.h, docs/FRAMEWORK.md "Help"). Keep it true when you change a control:
  // tests/app_test.cpp checks it against the kit's limits (help_check) and app/scenarios/help.txt shows it.
  void help(const o3sdk::app::Context& ctx, o3sdk::app::Help& help) const noexcept override;

  // For the console's `state` (the app's describe_state) and the app's file jobs.
  int octave() const noexcept { return octave_; }
  void set_octave(int octave) noexcept { octave_ = clamp_octave(octave); }
  const Pattern& pattern() const noexcept { return pattern_; }
  void set_pattern(const Pattern& p) noexcept { pattern_ = p; }
  // A line of the app's own about the last file job, shown in place of the files service's text ("BAD FILE": the load worked and the file
  // was not a pattern). nullptr: none.
  void set_note(const char* note) noexcept { note_ = note; }
  const o3sdk::app::ParamKeys& keys() const noexcept { return keys_; }

 private:
  o3sdk::dsp::UnitId voice_ = o3sdk::dsp::kNoUnit;
  o3sdk::app::ParamPage page_ = {};
  o3sdk::app::ParamKeys keys_;
  int octave_ = 0;
  Pattern pattern_;
  const char* note_ = nullptr;
  mutable o3sdk::app::HelpPadNames help_names_ = {};  // help(): the pads' labels ("C3", "F#4"), which move with the octave; Screen::help() is const
  int held_[o3sdk::app::kPadCount] = {};  // the note each held pad started (-1: none), so the release names it even after an octave shift
};

}  // namespace @APP_ID@
