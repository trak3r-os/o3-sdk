// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The system layer of the runtime (docs/M2B2-PLAN.md section 4): the Config screen every image carries, and the strip it shares with the
// platform. The runtime takes the Config button, so an app never sees it; while the screen is open the app gets no new input (a release of
// something pressed before it opened still reaches the app, so no note is left sounding), and the app's own screen goes on running and
// sounding underneath. The sections, in this order:
//
//   FIRMWARE  switch the unit to another image from the card: the list of Firmware/*.ptf, a sliced check, two questions (F1 each), then
//             the install through hal::firmware::FirmwareSink. Nothing is written before INSTALL. Hidden when Platform::firmware is null.
//   AUDIO     the audio in (Context::audio_in(): INPUT, GAIN, MONITOR, a level meter) and, while INPUT is RADIO, the receiver (Context::radio(): FREQ, REGION, SEEK
//             with F5 / F6, and what the chip says); then, with Platform::usb_audio, USB AUDIO (the AUDIO set or STANDARD), PROFILE, RETURN and RETURN LEVEL (Context::usb_audio()). A change of
//             the set asks F1 twice: the first shows what would be asked, the second applies it and the port reconnects. Hidden when the platform has neither a capture device nor a USB
//             audio device; the radio rows say so when no receiver answers.
//   MIDI      the user's hal::midi::Config (the channel notes come in on, the channel and ports out, clock and transport out, FOLLOW and its
//             port, transport in) and the device's status. Hidden when Platform::midi is null.
//   CARD      with a card capability: the card as the files service sees it, the volume and the files job, and, with Platform::usb_audio, CARD AS DISK (the card shared to the computer as a
//             disk, F1 twice, never at boot; F1 twice again to eject) and READ ONLY (the policy of the next share).
//   DISPLAY   the theme and, with Platform::backlight, BRIGHTNESS and, with pad LEDs besides, PAD LIGHTS (LOW, MEDIUM, HIGH: brightness.h; the screen's lowest is a fifth of full and a pad never goes dark).
//   INFO      the app's name and version, the build, the uptime and, with Platform::crash, the last crash record.
//   (app)     one tab for each page the app adds (App::config_pages, config_page.h), after INFO: rows of values and actions in the look and with the keys of the other sections.
//
// Keys. CONFIG opens and closes it; LEFT / RIGHT change section (not while a question or a check is up); F8 is BACK (one level up, and
// the last level closes the screen); in FIRMWARE: UP / DOWN and the encoder choose a file, F1 asks (UPDATE, YES, INSTALL), any other
// control takes a question back; in AUDIO: UP / DOWN choose a row, the encoder and F1 change its value (SHIFT: coarse; the USB set's rows ask F1 again), F5 / F6 seek down / up while the radio is the
// input (F5 retries a receiver that failed or did not answer); in MIDI: UP / DOWN choose a row, the encoder and F1 change its value; in DISPLAY: with one row (the theme only) the encoder, UP / DOWN
// and F1 change the theme, with more UP / DOWN choose a row and the encoder and F1 change it; in an app's page: UP / DOWN choose a row, the encoder and F1 change a value (SHIFT: coarse),
// F1 on an action asks and the next F1, within kAskMs, runs it. Past INSTALL no key acts and CONFIG does not close it.
//
// The strip is one line over the bottom of the screen, in the app's screens too: a message and, optionally, a bar. A caller that keeps
// calling strip() (the platform's power-off hold) keeps it up; it goes kStripMs after the last call.
//
// Main loop only.

#include <o3sdk/app/firmware_view.h>
#include <o3sdk/app/input.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/core/fixed_string.h>
#include <o3sdk/core/tick.h>
#include <o3sdk/hal/firmware.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::app {

class Context;

class SystemConfig {
 public:
  enum class Section : std::uint8_t { Firmware, Audio, Midi, Card, Display, Info, App };  // App: a page of the app's (not one of kSectionCount)
  static constexpr unsigned kSectionCount = 6;  // the kit's own

  static constexpr std::uint32_t kStripMs = 400;          // a strip that is not fed goes this long after its last call
  static constexpr std::uint32_t kNoticeMs = 3000;        // a refusal the screen says on the strip
  static constexpr std::uint32_t kAskMs = 5000;           // a change that waits for its second F1 is dropped after this long
  static constexpr unsigned kStripChars = 72;

  // `firmware`: the update path (null, or one that is not available(): no FIRMWARE section); `build`: the build id INFO shows (null: none);
  // `midi`: the platform has a MIDI device (Context::midi() is live: the MIDI section is shown); `audio`: it has a capture device (Context::audio_in() is live: the AUDIO section is shown).
  // `usb`: it has a USB audio device (Context::usb_audio() is live: the USB rows and CARD AS DISK are shown); `card`: it has a card capability (the CARD section is shown).
  // `backlight`: Platform::backlight is set (the BRIGHTNESS row); `pads`: and the platform has pad LEDs (the PAD LIGHTS row, which the runtime asks for only together with the backlight); `crash`: INFO's crash line (null: none).
  SystemConfig(hal::firmware::FirmwareSink* firmware, const char* build, bool midi = false, bool audio = false, bool usb = false, bool card = false, bool backlight = false, bool pads = false,
               const char* crash = nullptr) noexcept;

  bool open() const noexcept { return open_; }
  // Past INSTALL: the hand-over has started, the screen cannot be closed and no key acts.
  bool locked() const noexcept { return view_.phase == FirmwareView::Phase::Installing; }
  Section section() const noexcept { return section_; }
  unsigned app_page() const noexcept { return app_page_; }  // which page of the app's, while section() is App
  const FirmwareView& firmware() const noexcept { return view_; }

  // Opens the screen on the section it was left on (the first one there is the first time: FIRMWARE, else AUDIO, else MIDI, else DISPLAY), and closes it. close()
  // cancels what the FIRMWARE section had started; it does nothing past INSTALL.
  void show(Context& ctx) noexcept;
  void close(Context& ctx) noexcept;

  // One input, while the screen is open.
  void input(Context& ctx, const Input& in) noexcept;
  // Every pass of the main loop, open or not: follows the check and the install, drops an expired strip.
  void update(Context& ctx) noexcept;
  // The card was taken out or put in: a list that is not past INSTALL closes and the update path is cancelled.
  void card_changed(Context& ctx) noexcept;

  void draw(const Context& ctx, ui::Renderer& r, const ui::Rect& area) const noexcept;
  void soft_keys(const Context& ctx, SoftKeys& keys) const noexcept;

  // The strip. `progress`: 0..1000 draws a bar, anything else none. Null `text` takes the strip down.
  void strip(const char* text, int progress, std::uint32_t hold_ms, core::Tick now) noexcept;
  bool strip_active() const noexcept { return strip_on_; }
  void draw_strip(ui::Renderer& r, const ui::Rect& area) const noexcept;

  // The names of the console's `state`: "firmware", "audio", "midi", "card", "display", "info", and "app" for any page of the app's.
  const char* section_name() const noexcept;

 private:
  bool has(Section s) const noexcept;
  void move_section(Context& ctx, int dir) noexcept;
  void enter(Context& ctx) noexcept;
  void leave_section(Context& ctx) noexcept;
  void open_list(Context& ctx) noexcept;
  void back(Context& ctx) noexcept;
  bool firmware_input(Context& ctx, const Input& in) noexcept;
  bool display_input(Context& ctx, const Input& in) noexcept;
  bool midi_input(Context& ctx, const Input& in) noexcept;
  bool audio_input(Context& ctx, const Input& in) noexcept;
  bool card_input(Context& ctx, const Input& in) noexcept;
  bool app_input(Context& ctx, const Input& in) noexcept;
  unsigned display_rows() const noexcept { return 1u + (backlight_ ? 1u : 0u) + (pads_ ? 1u : 0u); }
  void step_display(Context& ctx, unsigned row, int steps, bool wrap) noexcept;
  bool usb_row(Context& ctx, unsigned kind, int steps, bool f1) noexcept;
  void disarm() noexcept { ask_ = Ask::None; }
  void step_theme(Context& ctx, int dir) noexcept;

  hal::firmware::FirmwareSink* sink_;
  const char* build_;
  bool midi_;
  bool audio_;
  bool usb_;
  bool card_;
  bool backlight_;
  bool pads_;
  const char* crash_;
  FirmwareView view_;
  Section section_;
  std::uint8_t midi_row_ = 0;  // the MIDI section's cursor
  std::uint8_t audio_row_ = 0;  // the AUDIO section's
  std::uint8_t card_row_ = 0;   // the CARD section's
  std::uint8_t display_row_ = 0;  // the DISPLAY section's
  std::uint8_t app_page_ = 0;     // the page of the app's that Section::App shows
  std::uint8_t app_row_ = 0;      // and its cursor (an index among the page's valid items)
  std::uint16_t ask_key_ = 0;     // Ask::Action: the item that waits for its second F1
  // A change that waits for its second F1: the USB layout (the proposal is want_*), or the card's share or eject.
  enum class Ask : std::uint8_t { None, Layout, Share, Eject, Action };
  Ask ask_ = Ask::None;
  core::Tick ask_until_ = 0;
  bool want_audio_ = true;
  std::uint8_t want_profile_ = 1;  // hal::usb_audio::Profile
  bool want_duplex_ = true;
  bool open_ = false;
  core::FixedString<kStripChars> strip_text_;
  int strip_progress_ = -1;
  core::Tick strip_until_ = 0;
  bool strip_on_ = false;
};

}  // namespace o3sdk::app
