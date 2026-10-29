// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What an app and its screens may use: the engine, the panel's state, MIDI, the card's files, audio in and the FM radio, recording, USB audio, time, the screen switch and the theme. Handed to every call;
// owned by the runtime.

#include <o3sdk/app/audio_in.h>
#include <o3sdk/app/config_page.h>
#include <o3sdk/app/files.h>
#include <o3sdk/app/midi.h>
#include <o3sdk/app/panel.h>
#include <o3sdk/app/radio.h>
#include <o3sdk/app/recorder.h>
#include <o3sdk/app/usb_audio.h>
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
  App& app() noexcept { return *app_; }
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
  // Recording (recorder.h): a take of the input or the master into a buffer the app owns, started on a step or a level, saved to the card as a WAV. An app that never arms one pays one load
  // a block in the render. Main loop only.
  Recorder& recorder() noexcept { return recorder_; }
  const Recorder& recorder() const noexcept { return recorder_; }

  // USB audio (usb_audio.h): the port's layout, the stems, the return and the card as a disk. An app that never calls it pays nothing in the render. Main loop only.
  UsbAudio& usb_audio() noexcept { return usb_audio_; }
  const UsbAudio& usb_audio() const noexcept { return usb_audio_; }

  core::Tick now() const noexcept { return now_; }          // microseconds, wraps (core/tick.h)
  std::uint64_t uptime_ms() const noexcept { return ms_; }  // does not wrap

  // Shows `screen` from the next pass on (the current screen's leave(), then the new one's enter()).
  void show(Screen& screen) noexcept { next_ = &screen; }
  Screen* screen() const noexcept { return screen_; }

  ui::ThemeId theme() const noexcept { return theme_; }
  void set_theme(ui::ThemeId id) noexcept { theme_ = id; }

  // DISPLAY and PAD LIGHTS of the system Config (brightness.h): 0 low, 1 medium, 2 high. The runtime tells the backlight and scales the pads' levels; both are kept in the settings. A value past
  // 2 is high. An app does not need them; it may set the defaults in setup() and read them to dim its own drawing.
  unsigned brightness() const noexcept { return brightness_; }
  void set_brightness(unsigned index) noexcept { brightness_ = static_cast<std::uint8_t>(index > 2u ? 2u : index); }
  unsigned pad_lights() const noexcept { return pad_lights_; }
  void set_pad_lights(unsigned index) noexcept { pad_lights_ = static_cast<std::uint8_t>(index > 2u ? 2u : index); }

  // The pages the app added to the Config screen as the kit read them (config_page.h), for the screen and a test.
  const ConfigView& config_view() const noexcept { return config_; }

  // Asks for a frame at the next pass instead of waiting for the frame clock (a value changed and should show now).
  void redraw() noexcept { redraw_ = true; }

 private:
  friend class Runtime;
  dsp::Engine* engine_ = nullptr;
  App* app_ = nullptr;
  Panel panel_;
  Midi midi_;
  Files files_;
  AudioIn audio_in_;
  Radio radio_;
  Recorder recorder_;
  UsbAudio usb_audio_;
  core::Tick now_ = 0;
  std::uint64_t ms_ = 0;
  Screen* screen_ = nullptr;
  Screen* next_ = nullptr;
  ui::ThemeId theme_ = ui::ThemeId::Amiga;
  std::uint8_t brightness_ = 2;
  std::uint8_t pad_lights_ = 2;
  ConfigView config_;
  bool redraw_ = false;
};

}  // namespace o3sdk::app
