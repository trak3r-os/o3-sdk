// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The body of the FIRMWARE page of the system Config screen (private to kit/app/src; system_firmware_page.cpp).

#include <o3sdk/app/firmware_view.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/capture.h>
#include <o3sdk/hal/midi.h>
#include <o3sdk/ui/renderer.h>

namespace o3sdk::app {

// The layout the pages share: the first row of the body and the pitch.
struct PageGrid {
  int x;      // left edge of the text
  int w;      // width of the body
  int y0;     // top of row 0
  int pitch;  // row pitch of the theme
};

void draw_firmware_page(ui::Renderer& r, const PageGrid& grid, const FirmwareView& v) noexcept;

// The MIDI page (system_midi_page.cpp): the rows of hal::midi::Config, one value each, and the status of the device under them.
inline constexpr unsigned kMidiRowCount = 8;
// Row `row` (0 .. kMidiRowCount - 1) of `c` stepped by `steps` (positive or negative): a channel or the ports move by that many (clamped, or
// wrapped round with `wrap`), a switch toggles for any step.
hal::midi::Config midi_step(hal::midi::Config c, unsigned row, int steps, bool wrap) noexcept;
void draw_midi_page(ui::Renderer& r, const PageGrid& grid, const hal::midi::Config& c, const hal::midi::Status& s, unsigned row) noexcept;

// The AUDIO page (system_audio_page.cpp): INPUT, GAIN and MONITOR, then FREQ, REGION and SEEK while the radio is the input.
enum AudioRow : unsigned { kAudioInput, kAudioGain, kAudioMonitor, kAudioFreq, kAudioRegion, kAudioSeek };
inline constexpr unsigned kAudioBaseRows = 3;
inline constexpr unsigned kAudioRadioRows = 3;
// How many rows the page has for this source.
constexpr unsigned audio_row_count(hal::capture::Source source) noexcept {
  return source == hal::capture::Source::Radio ? kAudioBaseRows + kAudioRadioRows : kAudioBaseRows;
}
// The sources the INPUT row offers, in order (the USB return has no path yet: slice U adds it).
inline constexpr unsigned kAudioSourceChoices = 6;
// `source` moved by `steps` choices of the INPUT row: clamped, or wrapped round with `wrap`; the radio is skipped while no receiver is there (`radio_ok` false), unless it is where
// the move starts and goes nowhere else.
hal::capture::Source audio_step_source(hal::capture::Source source, int steps, bool wrap, bool radio_ok) noexcept;
// A per cent value moved by `steps` (clamped to 0..100, or wrapped round with `wrap`).
unsigned audio_step_percent(unsigned value, int steps, bool wrap) noexcept;

struct AudioView {
  hal::capture::Source source;
  unsigned gain;
  unsigned monitor;
  hal::capture::RadioStatus radio;
  dsp::InputPeaks meter;
  bool settling;
  bool failed;  // the codec refused the last write and it is being tried again
  unsigned row;
};
void draw_audio_page(ui::Renderer& r, const PageGrid& grid, const AudioView& v) noexcept;

}  // namespace o3sdk::app
