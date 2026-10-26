// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RAM_G map (ADR 0010 amendment 2): the one header that owns every fixed region of the FT81x's 1 MiB graphics
// memory. ui/assets.h places the faces, tiles and icons (OS-003b); this file adds the wave bitmaps (OS-003d) and names
// what is left as spare, and proves with static_asserts that nothing overlaps, that every region starts on a word
// (hal::Display::upload() asks for it, ADR 0011: alignment is declared, never placed) and that all of it fits.
//
//   0x00000  faces                       kRamGTiles bytes                                  (assets.h)
//   0x04000  hex pairs, notes, icons, the smaller tiles of the zoomed grid   kRamGEnd - kRamGTiles bytes   (assets.h)
//   0x0F000  wave slots 0..3             4 x 96 KiB: 2 lanes x 2 buffers, 800 x 240 at L4 (this file)
//   0x6F000  boot logo                   16 pages (kBootLogoMaxBytes, 64 KiB): the logo's bitmap and, for a colour one, its palette,
//                                        uploaded with the other assets (OS-003f; the default is 36 KB of them, boot_logo.h)
//   0x7F000  spare                       the rest, 516 KiB, for the screens to come. In it: the screenshot's bands (OS-HIL, `diag shot`: kRamGShot, up to the app's region),
//                                        the app's region (kRamGApp, from 0xD4000, 4 KB aligned, 175.9 KiB) and the last 64 bytes, the display driver's DMA self-test scratch
//
// A wave lane draws from one slot while the other is being filled and swaps when the new picture is complete: the chip
// re-reads RAM_G on every scan-out, so uploading over a bitmap that is on screen would tear (hal/display.h).

#include <o3sdk/hal/display.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/boot_logo.h>
#include <o3sdk/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui::plot {

struct RamGRegion {
  const char* name;
  std::uint32_t addr;
  std::uint32_t bytes;
};

constexpr std::uint32_t align_up(std::uint32_t v, std::uint32_t a) noexcept { return (v + a - 1u) / a * a; }

// The largest wave bitmap: the full panel width by kWaveMaxHeight rows, at L4 (the deeper of the two formats).
inline constexpr std::uint32_t kWaveMaxWidth = static_cast<std::uint32_t>(kScreenW);
inline constexpr std::uint32_t kWaveMaxHeight = 240;
inline constexpr std::uint32_t kWaveMaxBytes = kWaveMaxWidth * kWaveMaxHeight / 2u;  // L4: two pixels a byte
// A slot is a whole number of 4 KB pages so every slot starts on the upload chunk grid.
inline constexpr std::uint32_t kWaveSlotBytes = align_up(kWaveMaxBytes, 4096u);
inline constexpr unsigned kWaveLanes = 2;          // stereo: left and right
inline constexpr unsigned kWaveSlotsPerLane = 2;   // double buffered
inline constexpr unsigned kWaveSlots = kWaveLanes * kWaveSlotsPerLane;

inline constexpr std::uint32_t kRamGWave = align_up(kRamGEnd, 4096u);
inline constexpr std::uint32_t kRamGWaveEnd = kRamGWave + kWaveSlots * kWaveSlotBytes;
// The boot logo (OS-003f): the picture (the kit's or the app's own), uploaded by ui::upload_assets() and drawn by the boot screen. The
// region is the largest a logo may be (kBootLogoMaxBytes: its bitmap and palette), so the map does not depend on which logo an image
// carries; a whole number of 4 KB pages like the wave slots, so the spare region behind it stays on the upload grid.
inline constexpr std::uint32_t kRamGBootLogo = kRamGWaveEnd;
inline constexpr std::uint32_t kRamGBootLogoBytes = align_up(kBootLogoMaxBytes, 4096u);
inline constexpr std::uint32_t kRamGBootLogoEnd = kRamGBootLogo + kRamGBootLogoBytes;
inline constexpr std::uint32_t kRamGSpare = kRamGBootLogoEnd;
inline constexpr std::uint32_t kRamGSpareBytes = hal::kRamGBytes - kRamGSpare;
// The last 64 bytes of RAM_G are the DMA self-test's scratch (platform/mk66/display/display.h kSelfTestBytes, which the composition root checks against this). OS-HIL: the console's
// `diag shot` writes its snapshot bands (CMD_SNAPSHOT2, platform/mk66/usb/shot_stream.h) into the rest of the spare region, nothing else: every band it asks for is sized to fit
// kRamGShotBytes. A screen that needs memory takes it from the END of the spare region downwards and shortens kRamGShotBytes here.
inline constexpr std::uint32_t kRamGSelfTestBytes = 64;
// The app's region: the end of the spare region, from a 4 KB page to the self-test's scratch. The app owns it (its bitmaps, uploaded
// through hal::Display::upload()); the kit never writes it. Sized for o3-303's panel skin (S4 of o3-303 docs/M3B-PLAN.md: 115,632
// bytes with one palette, 177,072 with two); the fill-rate test (apps/fillrate) puts its two 800-wide test bitmaps (112.5 KiB) at its start.
inline constexpr std::uint32_t kRamGApp = hal::kRamGBytes - 0x2C000u;
inline constexpr std::uint32_t kRamGAppBytes = hal::kRamGBytes - kRamGSelfTestBytes - kRamGApp;
inline constexpr std::uint32_t kRamGShot = kRamGSpare;
inline constexpr std::uint32_t kRamGShotBytes = kRamGApp - kRamGShot;

// Bitmap handles of the two wave lanes: after the eight the renderer binds (assets.h), below the coprocessor's 15.
inline constexpr std::uint8_t kHandleWaveLane0 = 9;
inline constexpr std::uint8_t kHandleWaveLane1 = 10;
// The boot logo's bitmap handle (OS-003f): after the two wave lanes, below the coprocessor's 15. Bound by the boot screen's own
// raw section (ui/boot_screen.h), not by Renderer::begin(): the logo is drawn once, at boot.
inline constexpr std::uint8_t kHandleBootLogo = 11;

// The address of slot `slot` (0..kWaveSlots-1) of lane `lane`: lane 0 owns slots 0 and 1, lane 1 slots 2 and 3.
constexpr std::uint32_t wave_slot_addr(unsigned lane, unsigned slot) noexcept {
  return kRamGWave + (lane * kWaveSlotsPerLane + slot) * kWaveSlotBytes;
}
constexpr std::uint8_t wave_handle(unsigned lane) noexcept { return lane == 0 ? kHandleWaveLane0 : kHandleWaveLane1; }

// Every fixed region, in address order.
inline constexpr RamGRegion kRamGRegions[] = {
    {"faces", 0, kRamGTiles},
    {"tiles", kRamGTiles, kRamGEnd - kRamGTiles},
    {"wave0a", wave_slot_addr(0, 0), kWaveSlotBytes},
    {"wave0b", wave_slot_addr(0, 1), kWaveSlotBytes},
    {"wave1a", wave_slot_addr(1, 0), kWaveSlotBytes},
    {"wave1b", wave_slot_addr(1, 1), kWaveSlotBytes},
    {"boot_logo", kRamGBootLogo, kRamGBootLogoBytes},
    {"spare", kRamGSpare, kRamGSpareBytes},
};
inline constexpr unsigned kRamGRegionCount = sizeof(kRamGRegions) / sizeof(kRamGRegions[0]);

// True when every region is word aligned in address and size, lies inside RAM_G, and none overlaps another.
constexpr bool ram_g_regions_ok(const RamGRegion* regions, unsigned count) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const RamGRegion& a = regions[i];
    if (a.addr % hal::kUploadAlign != 0 || a.bytes % hal::kUploadAlign != 0 || a.bytes == 0 ||
        a.addr > hal::kRamGBytes || a.bytes > hal::kRamGBytes - a.addr) {
      return false;
    }
    for (unsigned j = i + 1; j < count; ++j) {
      const RamGRegion& b = regions[j];
      if (a.addr < b.addr + b.bytes && b.addr < a.addr + a.bytes) {
        return false;
      }
    }
  }
  return true;
}
static_assert(ram_g_regions_ok(kRamGRegions, kRamGRegionCount),
              "RAM_G regions are word aligned, inside RAM_G and disjoint");
static_assert(kRamGShot % hal::kUploadAlign == 0 && kRamGShot >= kRamGBootLogoEnd && kRamGShot + kRamGShotBytes == kRamGApp,
              "the screenshot's region is the spare region up to the app's region, on a word, behind every fixed region");
static_assert(kRamGApp % 4096u == 0 && kRamGApp > kRamGShot && kRamGApp + kRamGAppBytes + kRamGSelfTestBytes == hal::kRamGBytes &&
                  kRamGAppBytes % hal::kUploadAlign == 0,
              "the app's region is the end of the spare region, below the self-test's scratch, on a word, and leaves the screenshot's bands something");
static_assert(kRamGWave >= kRamGEnd, "the wave slots start after the tiles and icons");
static_assert(kRamGWaveEnd <= hal::kRamGBytes, "the wave slots fit RAM_G");
static_assert(kWaveSlotBytes >= kWaveMaxBytes && kWaveSlotBytes % 4096u == 0, "a slot holds the largest wave bitmap");
static_assert(kRamGBootLogo % 4096u == 0 && kRamGBootLogo % hal::kUploadAlign == 0 && kRamGBootLogo >= kRamGWaveEnd,
              "the boot logo starts on the upload grid, after the wave slots");
static_assert(kRamGBootLogoBytes >= kBootLogoMaxBytes && kRamGBootLogoBytes % 4096u == 0 && kRamGBootLogoEnd < hal::kRamGBytes,
              "the boot logo's region holds its bitmap and leaves the spare region something");
static_assert(kWaveMaxHeight < 512 && kWaveMaxWidth < 1024, "BITMAP_LAYOUT / BITMAP_SIZE fields (the _H commands cover the rest)");
static_assert(kHandleWaveLane0 > kHandleIcons && kHandleWaveLane0 < kHandleWaveLane1 && kHandleWaveLane1 <= 14,
              "wave handles follow the renderer's eight and stay below the coprocessor's 15");
static_assert(kHandleBootLogo > kHandleWaveLane1 && kHandleBootLogo <= 14, "the logo's handle follows the wave lanes and stays below 15");
static_assert(kHandleSmallHex > kHandleNotesHi && kHandleSmallNotes > kHandleNotesHi && kHandleSmallHex <= 14 && kHandleSmallNotes <= 14 && kHandleSmallHex != kHandleSmallNotes,
              "the smaller tiles' handles (OS-ZOOM) follow the second note-tile handle and stay below the coprocessor's 15");
static_assert(kHandleNotesHi > kHandleBootLogo && kHandleNotesHi <= 14, "the second note-tile handle follows the logo's and stays below 15");

}  // namespace o3sdk::ui::plot
