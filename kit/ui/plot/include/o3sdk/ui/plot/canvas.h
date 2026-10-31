// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app's bitmaps. Bitmap handle 0 is the app's: it holds the bitmaps and shapes an app uploads into its own RAM_G region (ram_g_map.h,
// kRamGApp), and the kit leaves it alone. Every other handle the kit draws with (assets.h: the glyph faces 1..4 and the tiles 5..8, 12..14;
// ram_g_map.h: the wave lanes 9, 10 and the boot logo 11) is named there and differs from it, and 15 is the coprocessor's. A glyph face's
// handle is generated (font_convert.py, 1 + the position in ui::Font), so tests/handle_app_test.cpp checks the four.
//
// Nothing else is here yet: the app-side drawing API (ui::plot::Canvas) lands in this header later, and Canvas::bind() is how an app will
// use the handle. The simulator counts every list that touches handle 0 (sim::EveRaster::handle_app_uses()); a kit app that has no Canvas
// must read zero.

#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include <cstdint>

namespace o3sdk::ui::plot {

inline constexpr std::uint8_t kHandleApp = 0;

static_assert(kHandleApp != kHandleHexLo && kHandleApp != kHandleHexHi && kHandleApp != kHandleNotes && kHandleApp != kHandleIcons &&
                  kHandleApp != kHandleWaveLane0 && kHandleApp != kHandleWaveLane1 && kHandleApp != kHandleBootLogo &&
                  kHandleApp != kHandleNotesHi && kHandleApp != kHandleSmallHex && kHandleApp != kHandleSmallNotes && kHandleApp != 15,
              "the app's handle is none of the kit's, and not the coprocessor's");

}  // namespace o3sdk::ui::plot
