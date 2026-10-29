// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The body of the FIRMWARE page of the system Config screen (private to kit/app/src; system_firmware_page.cpp).

#include <o3sdk/app/firmware_view.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/capture.h>
#include <o3sdk/hal/midi.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/usb_audio.h>
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
// One row after the config rows: CLEAR LEARN (the number of MIDI learn bindings, and F1 twice clears them). `bindings` and `asking` (the first F1 was pressed) are what it shows.
void draw_midi_page(ui::Renderer& r, const PageGrid& grid, const hal::midi::Config& c, const hal::midi::Status& s, unsigned row, unsigned bindings, bool asking) noexcept;

// The AUDIO page (system_audio_page.cpp): INPUT, GAIN and MONITOR, then FREQ, REGION and SEEK while the radio is the input (a platform with a capture device), then USB AUDIO, PROFILE, RETURN
// and RETURN LEVEL (a platform with a USB audio device).
enum AudioRow : unsigned { kAudioInput, kAudioGain, kAudioMonitor, kAudioFreq, kAudioRegion, kAudioSeek, kAudioUsb, kAudioProfile, kAudioReturn, kAudioReturnLevel };
inline constexpr unsigned kAudioBaseRows = 3;
inline constexpr unsigned kAudioRadioRows = 3;
inline constexpr unsigned kAudioUsbRows = 4;
inline constexpr unsigned kAudioMaxRows = kAudioBaseRows + kAudioRadioRows + kAudioUsbRows;
// The rows the page has, in order, as AudioRow kinds, for this source and these capabilities: how many.
unsigned audio_rows(AudioRow (&kinds)[kAudioMaxRows], hal::capture::Source source, bool has_input, bool has_usb) noexcept;
// The sources the INPUT row offers, in order: the line, the microphone, the radio and, with a USB audio device, the computer's return.
inline constexpr unsigned kAudioSourceChoices = 7;
// `source` moved by `steps` choices of the INPUT row: clamped, or wrapped round with `wrap`; the radio is skipped while no receiver is there (`radio_ok` false), unless it is where
// the move starts and goes nowhere else; the USB return is offered only with `usb_ok`.
hal::capture::Source audio_step_source(hal::capture::Source source, int steps, bool wrap, bool radio_ok, bool usb_ok = false) noexcept;
// `p` moved by `steps` choices of the PROFILE row (P2, P8, P10, P11): clamped, or wrapped round.
hal::usb_audio::Profile audio_step_profile(hal::usb_audio::Profile p, int steps, bool wrap) noexcept;
// A per cent value moved by `steps` (clamped to 0..100, or wrapped round with `wrap`).
unsigned audio_step_percent(unsigned value, int steps, bool wrap) noexcept;

// What the USB rows show: the layout (the proposal while a change waits for its second F1) and the streams.
struct UsbView {
  bool audio;
  hal::usb_audio::Profile profile;
  bool duplex;
  unsigned level;      // the return's level, per cent
  bool streaming;      // the computer has the capture stream open
  bool return_open;    // and the return stream
  bool changing;       // the port is re-enumerating
  bool proposed;       // audio, profile and duplex are a proposal: F1 again applies it, the port reconnects
};

struct AudioView {
  bool has_input;
  bool has_usb;
  UsbView usb;
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

// The CARD page (system_card_page.cpp): the card as the files service sees it, the volume, the files job, and CARD AS DISK and READ ONLY with a USB audio device.
enum CardRow : unsigned { kCardState, kCardVolume, kCardFiles, kCardDisk, kCardReadOnly };
inline constexpr unsigned kCardBaseRows = 3;
inline constexpr unsigned kCardUsbRows = 2;
enum class CardAsk : std::uint8_t { None, Share, Eject };  // the second F1 of CARD AS DISK is waited for
struct CardView {
  const char* state;  // the files service's view of the card, upper case
  const char* files;  // its job's text, "" when idle
  hal::storage::VolumeInfo volume;
  bool has_usb;
  hal::usb_audio::DiskStatus disk;
  bool read_only;
  CardAsk ask;
  unsigned row;
};
void draw_card_page(ui::Renderer& r, const PageGrid& grid, const CardView& v) noexcept;

}  // namespace o3sdk::app
