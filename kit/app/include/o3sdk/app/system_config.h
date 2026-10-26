// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The system layer of the runtime (docs/M2B2-PLAN.md section 4): the Config screen every image carries, and the strip it shares with the
// platform. The runtime takes the Config button, so an app never sees it; while the screen is open the app gets no new input (a release of
// something pressed before it opened still reaches the app, so no note is left sounding), and the app's own screen goes on running and
// sounding underneath. The sections, in this order:
//
//   FIRMWARE  switch the unit to another image from the card: the list of Firmware/*.ptf, a sliced check, two questions (F1 each), then
//             the install through hal::firmware::FirmwareSink. Nothing is written before INSTALL. Hidden when Platform::firmware is null.
//   DISPLAY   the theme.
//   INFO      the app's name and version, the build, the uptime.
//
// (The MIDI section of the plan goes between FIRMWARE and DISPLAY when the kit has MIDI; App::config_pages, the pages an app adds, is
// declared and not drawn yet.)
//
// Keys. CONFIG opens and closes it; LEFT / RIGHT change section (not while a question or a check is up); F8 is BACK (one level up, and
// the last level closes the screen); in FIRMWARE: UP / DOWN and the encoder choose a file, F1 asks (UPDATE, YES, INSTALL), any other
// control takes a question back; in DISPLAY: the encoder, UP / DOWN and F1 change the theme. Past INSTALL no key acts and CONFIG does not
// close it.
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
  enum class Section : std::uint8_t { Firmware, Display, Info };
  static constexpr unsigned kSectionCount = 3;

  static constexpr std::uint32_t kStripMs = 400;          // a strip that is not fed goes this long after its last call
  static constexpr std::uint32_t kNoticeMs = 3000;        // a refusal the screen says on the strip
  static constexpr unsigned kStripChars = 72;

  // `firmware`: the update path (null, or one that is not available(): no FIRMWARE section); `build`: the build id INFO shows (null: none).
  SystemConfig(hal::firmware::FirmwareSink* firmware, const char* build) noexcept;

  bool open() const noexcept { return open_; }
  // Past INSTALL: the hand-over has started, the screen cannot be closed and no key acts.
  bool locked() const noexcept { return view_.phase == FirmwareView::Phase::Installing; }
  Section section() const noexcept { return section_; }
  const FirmwareView& firmware() const noexcept { return view_; }

  // Opens the screen on the section it was left on (FIRMWARE the first time, DISPLAY without a firmware path), and closes it. close()
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

  // The names of the console's `state`: "firmware", "display", "info".
  const char* section_name() const noexcept;

 private:
  bool has(Section s) const noexcept;
  void move_section(Context& ctx, int dir) noexcept;
  void enter(Context& ctx) noexcept;
  void leave_section() noexcept;
  void open_list(Context& ctx) noexcept;
  void back(Context& ctx) noexcept;
  bool firmware_input(Context& ctx, const Input& in) noexcept;
  bool display_input(Context& ctx, const Input& in) noexcept;
  void step_theme(Context& ctx, int dir) noexcept;

  hal::firmware::FirmwareSink* sink_;
  const char* build_;
  FirmwareView view_;
  Section section_;
  bool open_ = false;
  core::FixedString<kStripChars> strip_text_;
  int strip_progress_ = -1;
  core::Tick strip_until_ = 0;
  bool strip_on_ = false;
};

}  // namespace o3sdk::app
