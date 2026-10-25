// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The boot screen (OS-003f, docs/specs/OS-003-ui-toolkit.md): the maintainer's logo, the first frame the display shows, centred on black
// and scaled by an integer factor, with one line under it in the Micro face: the device name and the build. It looks the same in
// every theme (boot_theme(), theme.h): the device has not read its settings yet, and a stored theme must not decide what the
// first picture is.
//
//   * The logo is the bitmap of ui/boot_logo.h, at kBootLogoScale = 3 through BITMAP_TRANSFORM_A and _E (nearest sampling, the
//     two coefficients are 85, the nearest 8.8 value to a third: guide section 4.12, p.108) in the white of the theme's Text
//     colour (COLOR_RGB): 124 x 124 pixels become 372 x 372, centred on the panel. [A] The coefficient is 0.4 % under a third,
//     so of the 124 blocks of 3 px one is 4 px wide and the last is 2 px wide (tests/raster_transform_test.cpp works it out);
//     which columns they are depends on how the chip rounds, which only a unit can say (the hardware checklist, README of
//     platform/mk66).
//   * The line is the device name (the app name) in Text and "o3sdk <build id>" in TextDim, centred, 14 px under
//     the logo. The caller passes both: the build id is the platform's (the one `diag version` prints), and ui does not see usb.
//   * The bitmap must be in RAM_G, where ui::upload_assets() puts it (ui/plot/ram_g_map.h kRamGBootLogo), and the Micro face too.
//   * How long it stays is core::BootHold's business (core/boot_hold.h), driven by the platform.

#include <o3sdk/hal/display.h>
#include <o3sdk/ui/boot_logo.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui {

// Where things go, in screen pixels: the logo exactly centred, the line a fixed gap under it.
inline constexpr int kBootLogoDrawW = static_cast<int>(kBootLogoW * kBootLogoScale);
inline constexpr int kBootLogoDrawH = static_cast<int>(kBootLogoH * kBootLogoScale);
inline constexpr int kBootLogoX = (kScreenW - kBootLogoDrawW) / 2;
inline constexpr int kBootLogoY = (kScreenH - kBootLogoDrawH) / 2;
inline constexpr int kBootTextGap = 14;
inline constexpr int kBootTextY = kBootLogoY + kBootLogoDrawH + kBootTextGap;
// BITMAP_TRANSFORM_A and _E of an enlargement by kBootLogoScale: 256 / scale in signed 8.8 fixed point (the guide's example:
// 128 shows the bitmap twice as wide).
inline constexpr std::int32_t kBootLogoTransform = 256 / static_cast<std::int32_t>(kBootLogoScale);
static_assert(kBootLogoX >= 0 && kBootLogoY >= 0 && kBootTextY + 12 <= kScreenH, "the logo and its line fit the panel");
static_assert(kBootLogoScale >= 1 && kBootLogoScale <= 16, "the transform coefficient 256 / scale is at least 16");

// The longest build id the line shows (the rest is cut): the platform's is a 12-digit hash and "-dirty".
inline constexpr std::size_t kBootBuildChars = 24;

// Draws the content of the boot frame into a renderer that has been begun (the registry's convention, sim/screens.h): the logo,
// then the line. `name` and `build_id` are ASCII, not null.
void draw_boot_screen(Renderer& r, const char* name, const char* build_id) noexcept;

// The display-list words of the boot frame, DISPLAY included, are far below this (tests/boot_screen_test.cpp measures it in
// both translate modes); it is the size of the buffer show_boot_screen() builds in.
inline constexpr std::size_t kBootScreenMaxWords = 192;

// Builds the boot frame with boot_theme() and commits it to `display`: begin_frame(), the frame, end_frame(). Main loop only,
// no heap (the list is built in a buffer on the stack, 768 bytes). false: the display refused a frame (the previous swap is
// pending: try again) or the list did not fit; nothing is committed and the screen is as it was.
bool show_boot_screen(hal::Display& display, const char* name, const char* build_id) noexcept;

}  // namespace o3sdk::ui
