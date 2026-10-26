// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The body of the FIRMWARE page of the system Config screen (private to kit/app/src; system_firmware_page.cpp).

#include <o3sdk/app/firmware_view.h>
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

}  // namespace o3sdk::app
