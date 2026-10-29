// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What an app and its screens may use: the engine, the panel's state, MIDI, the card's files, audio in and the FM radio, time, the screen switch and the theme. Handed to every call;
// owned by the runtime.

#include <o3sdk/app/audio_in.h>
#include <o3sdk/app/files.h>
#include <o3sdk/app/midi.h>
#include <o3sdk/app/panel.h>
#include <o3sdk/app/radio.h>
#include <o3sdk/core/tick.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/ui/theme.h>

#include <cstdint>

namespace o3sdk::app {

class App;
class Screen;

class Context {
 public:
  dsp::Engine& engine() noexcept { return *engine_; }
  const dsp::Engine& engine() const noexcept { return *engine_; }
  const Panel& panel() const noexcept { return panel_; }
  const App& app() const noexcept { return *app_; }
  // MIDI (midi.h): the routes of notes to units, the user's settings, the traffic. Main loop only.
  Midi& midi() noexcept { return midi_; }
  const Midi& midi() const noexcept { return midi_; }
  // The app's files on the SD card (files.h): save, load, list, in the app's own folder, as jobs the runtime advances; the render never waits for the card. Main loop only.
  Files& files() noexcept { return files_; }
  const Files& files() const noexcept { return files_; }
  // Audio in (audio_in.h): the codec's source and gain, the input's gate, the monitor and the peaks. An app that never calls it pays nothing in the render. Main loop only.
  AudioIn& audio_in() noexcept { return audio_in_; }
  const AudioIn& audio_in() const noexcept { return audio_in_; }
  // The FM radio (radio.h): power, region, tune and seek; its sound is AudioIn's Source::Radio. Main loop only.
  Radio& radio() noexcept { return radio_; }
  const Radio& radio() const noexcept { return radio_; }

  core::Tick now() const noexcept { return now_; }          // microseconds, wraps (core/tick.h)
  std::uint64_t uptime_ms() const noexcept { return ms_; }  // does not wrap

  // Shows `screen` from the next pass on (the current screen's leave(), then the new one's enter()).
  void show(Screen& screen) noexcept { next_ = &screen; }
  Screen* screen() const noexcept { return screen_; }

  ui::ThemeId theme() const noexcept { return theme_; }
  void set_theme(ui::ThemeId id) noexcept { theme_ = id; }

  // Asks for a frame at the next pass instead of waiting for the frame clock (a value changed and should show now).
  void redraw() noexcept { redraw_ = true; }

 private:
  friend class Runtime;
  dsp::Engine* engine_ = nullptr;
  const App* app_ = nullptr;
  Panel panel_;
  Midi midi_;
  Files files_;
  AudioIn audio_in_;
  Radio radio_;
  core::Tick now_ = 0;
  std::uint64_t ms_ = 0;
  Screen* screen_ = nullptr;
  Screen* next_ = nullptr;
  ui::ThemeId theme_ = ui::ThemeId::Amiga;
  bool redraw_ = false;
};

}  // namespace o3sdk::app
