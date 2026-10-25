// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Tracker's 800x480 panel as register values, and the orientation setting. The timing is that of the Riedel library's RVT70 preset
// (track3r-os third_party/eve-riedel/EVE_config.h), which differs only in CSPREAD = 1: it is 0 here until the bench says
// otherwise, hw checklist DSP-9. Nothing here is measured on a unit yet.
//
// The register list is a pure function of (panel, orientation), so the init sequence's writes are tested without a chip.

#include <o3sdk/platform/mk66/display/ft812_regs.h>

#include <cstdint>

namespace o3sdk::platform::mk66::display {

struct PanelTiming {
  std::uint16_t hsize, hcycle, hoffset, hsync0, hsync1;
  std::uint16_t vsize, vcycle, voffset, vsync0, vsync1;
  std::uint8_t pclk;        // divider of the 60 MHz system clock (REG_PCLK); written last, it starts the panel clock
  std::uint8_t pclk_pol;    // 1: data is clocked on the falling edge
  std::uint8_t swizzle;     // colour output pin order
  std::uint8_t cspread;     // 1: fewer outputs change at once (EMI)
  std::uint8_t dither;      // 1: dither on
  std::uint16_t outbits;    // REG_OUTBITS: 0x1B6 = 6 bits per colour line (with dither on); the chip's default is 0 = 8
};

inline constexpr PanelTiming kPanel800x480 = {
    800, 1056, 46, 0, 10,   // horizontal: size, cycle, offset, sync0, sync1
    480, 525, 23, 0, 10,    // vertical
    2, 1, 0, 0, 1, 0x1B6};  // pclk, pclk_pol, swizzle, cspread, dither, outbits

// REG_ROTATE (Guide section 2.5.3): 0 landscape, 1 inverted landscape (180 degrees), 2 portrait, 3 inverted portrait,
// 4..7 the mirrored versions. The register alone is enough: CMD_SETROTATE only adds the touch matrix (ADR 0011 amendment 1).
enum class Orientation : std::uint8_t {
  Landscape = 0,
  LandscapeInverted = 1,
  Portrait = 2,
  PortraitInverted = 3,
  LandscapeMirrored = 4,
  LandscapeInvertedMirrored = 5,
  PortraitMirrored = 6,
  PortraitInvertedMirrored = 7,
};

// A register write of the panel's configuration: `width` is 1, 2 or 4 bytes.
struct RegWrite {
  std::uint32_t addr;
  std::uint32_t value;
  std::uint8_t width;
};

// The panel's writes in the order the init performs them, REG_PCLK excluded (the pixel clock is the last thing the init
// starts). Order: the ten timing words, then swizzle, clock polarity, cspread, rotate, dither, outbits.
inline constexpr unsigned kPanelWriteCount = 16;

constexpr RegWrite panel_write(const PanelTiming& p, Orientation orientation, unsigned index) noexcept {
  switch (index) {
    case 0: return {ft812::reg::kHsize, p.hsize, 2};
    case 1: return {ft812::reg::kHcycle, p.hcycle, 2};
    case 2: return {ft812::reg::kHoffset, p.hoffset, 2};
    case 3: return {ft812::reg::kHsync0, p.hsync0, 2};
    case 4: return {ft812::reg::kHsync1, p.hsync1, 2};
    case 5: return {ft812::reg::kVsize, p.vsize, 2};
    case 6: return {ft812::reg::kVcycle, p.vcycle, 2};
    case 7: return {ft812::reg::kVoffset, p.voffset, 2};
    case 8: return {ft812::reg::kVsync0, p.vsync0, 2};
    case 9: return {ft812::reg::kVsync1, p.vsync1, 2};
    case 10: return {ft812::reg::kSwizzle, p.swizzle, 1};
    case 11: return {ft812::reg::kPclkPol, p.pclk_pol, 1};
    case 12: return {ft812::reg::kCspread, p.cspread, 1};
    case 13: return {ft812::reg::kRotate, static_cast<std::uint32_t>(orientation) & 7u, 1};
    case 14: return {ft812::reg::kDither, p.dither, 1};
    case 15: return {ft812::reg::kOutBits, p.outbits, 2};
    default: return {0, 0, 0};
  }
}

// Pixel clock and frame rate from the 60 MHz system clock the FT812 has at reset (REG_FREQUENCY default; the guide p.86).
inline constexpr std::uint32_t kSystemClockHz = 60'000'000u;
constexpr std::uint32_t pixel_clock_hz(const PanelTiming& p) noexcept { return p.pclk == 0 ? 0u : kSystemClockHz / p.pclk; }
// Frames per second times 1000 (54,112 for this panel: 30 MHz / (1056 x 525)).
constexpr std::uint32_t frame_millihz(const PanelTiming& p) noexcept {
  return p.pclk == 0 ? 0u
                     : static_cast<std::uint32_t>(static_cast<std::uint64_t>(pixel_clock_hz(p)) * 1000u /
                                                  (static_cast<std::uint64_t>(p.hcycle) * p.vcycle));
}

// The values the controller will reject or mis-scan: porches inside the line, 12-bit counters, a divider that exists.
constexpr bool panel_valid(const PanelTiming& p) noexcept {
  return p.hsize != 0 && p.vsize != 0 && p.hsize <= p.hcycle && p.vsize <= p.vcycle && p.hcycle < 4096u && p.vcycle < 4096u &&
         p.hoffset < p.hcycle && p.voffset < p.vcycle && p.hsync0 <= p.hsync1 && p.hsync1 <= p.hoffset &&
         p.vsync0 <= p.vsync1 && p.vsync1 <= p.voffset && p.hoffset + p.hsize <= p.hcycle && p.voffset + p.vsize <= p.vcycle &&
         p.pclk >= 1 && p.pclk <= 15 && p.swizzle <= 15 && p.pclk_pol <= 1 && p.cspread <= 1 && p.dither <= 1;
}

static_assert(panel_valid(kPanel800x480), "the panel timing is internally consistent");
static_assert(pixel_clock_hz(kPanel800x480) == 30'000'000u, "30 MHz pixel clock (60 MHz / 2)");
static_assert(frame_millihz(kPanel800x480) == 54'112u, "54.1 Hz refresh (the display notes)");

}  // namespace o3sdk::platform::mk66::display
