// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The body of the FIRMWARE page of the system Config screen (private to kit/app/src; system_firmware_page.cpp).

#include <o3sdk/app/firmware_view.h>
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

}  // namespace o3sdk::app
