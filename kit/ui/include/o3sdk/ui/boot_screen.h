// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The boot screen (OS-003f, docs/specs/OS-003-ui-toolkit.md): the logo, the first frame the display shows, centred on black
// and scaled by an integer factor, with one line under it in the Micro face: the device name and the build. It looks the same in
// every theme (boot_theme(), theme.h): the device has not read its settings yet, and a stored theme must not decide what the
// first picture is.
//
//   * The logo is the bitmap of ui/boot_logo.h, boot_logo(): the kit's default (the O3 logo) or the one the app's LOGO option
//     generated. Its width, height, format, stride, scale, palette and tint are read from that data, here at run time: the
//     bitmap is laid out by BITMAP_LAYOUT, enlarged by `scale` through BITMAP_TRANSFORM_A and _E (nearest sampling, the two
//     coefficients 256 / scale in 8.8 fixed point: guide section 4.12, p.108) and drawn through COLOR_RGB (a grey logo: its tint,
//     white by default) or, for a colour logo, through its PALETTE_SOURCE. A drawn width of 512 and more takes BITMAP_SIZE_H.
//     [A] A scale that is not a power of two (the coefficient is not exact: 85 for 3, 0.4 % under a third) makes some blocks of
//     `scale` px one px wider and the last narrower (tests/raster_transform_test.cpp works it out); which columns they are depends
//     on how the chip rounds, which only a unit can say (the hardware checklist, README of platform/mk66). Scale 1 draws with no
//     transform at all.
//   * The line is the device name (the app name) in Text and the build line in TextDim, centred, 14 px under the logo. The caller
//     passes both: the build line is the platform's (O3SDK_IMAGE_LABEL, the identity `diag version` prints) and ui does not see
//     usb: "o3sdk d8c4ede2c609" in o3sdk's own apps, "<app label> / o3sdk <kit label>" in an app of another repository (ASCII: the
//     faces stop at '~'; the app's identity first, because the name before it is the app's).
//   * The bitmap must be in RAM_G, where ui::upload_assets() puts it (ui/plot/ram_g_map.h kRamGBootLogo), and the Micro face too.
//   * How long it stays is core::BootHold's business (core/boot_hold.h), driven by the platform.

#include <o3sdk/hal/display.h>
#include <o3sdk/ui/boot_logo.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui {

// Where things go, in screen pixels: the logo exactly centred, the line a fixed gap under it. The limits of a logo
// (kBootLogoMaxDrawH, boot_logo.h) leave room for the line.
inline constexpr int kBootTextGap = 14;
inline constexpr int kBootTextH = 12;
struct BootLayout {
  int x, y;  // the logo's top left
  int w, h;  // its drawn size: native x scale
  int text_y;
};
constexpr BootLayout boot_layout(const BootLogo& logo) noexcept {
  const int w = static_cast<int>(logo.width) * logo.scale;
  const int h = static_cast<int>(logo.height) * logo.scale;
  const int y = (kScreenH - h) / 2;
  return BootLayout{(kScreenW - w) / 2, y, w, h, y + h + kBootTextGap};
}
static_assert(kBootLogoMaxDrawW <= static_cast<unsigned>(kScreenW) &&
                  (kScreenH - static_cast<int>(kBootLogoMaxDrawH)) / 2 + static_cast<int>(kBootLogoMaxDrawH) + kBootTextGap + kBootTextH <=
                      kScreenH,
              "the largest logo and its line fit the panel");

// The longest build line shown (the rest is cut). Two labels of 18 characters ("0123456789ab-dirty") joined by " / o3sdk " are 45
// characters, two with a version tag and a describe suffix ("v0.5.0-3-g0271ab6-dirty (0271ab60bea6-dirty)", 44) are 97, so 100 shows
// every form whole; that is 600 px in the Micro face, and a glyph is a word of the list (tests/boot_screen_test.cpp measures the
// longest line against kBootScreenMaxWords and the panel's width).
inline constexpr std::size_t kBootBuildChars = 100;

// Draws the content of the boot frame into a renderer that has been begun (the registry's convention, sim/screens.h): the logo,
// then the line. `name` and `build` (the build line) are ASCII, not null.
void draw_boot_screen(Renderer& r, const char* name, const char* build) noexcept;
// The same with the logo given: boot_logo() is the one a firmware shows, this is for the tests of the limits (a logo of the largest size
// the data may have). `logo` must be valid (boot_logo_valid()).
void draw_boot_screen(Renderer& r, const BootLogo& logo, const char* name, const char* build) noexcept;

// The display-list words of the boot frame, DISPLAY included, are 173 at the most (a 16-character name and a line of kBootBuildChars
// glyphs, in the worse of the two translate modes, under the largest colour logo, which has a palette and BITMAP_SIZE_H and a transform;
// tests/boot_screen_test.cpp measures it for the default and for that logo); this is the size of the buffer
// show_boot_screen() builds in.
inline constexpr std::size_t kBootScreenMaxWords = 192;

// Builds the boot frame with boot_theme() and commits it to `display`: begin_frame(), the frame, end_frame(). Main loop only,
// no heap (the list is built in a buffer on the stack, 768 bytes). false: the display refused a frame (the previous swap is
// pending: try again) or the list did not fit; nothing is committed and the screen is as it was.
bool show_boot_screen(hal::Display& display, const char* name, const char* build) noexcept;

}  // namespace o3sdk::ui
