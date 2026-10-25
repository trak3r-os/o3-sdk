// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FT812 facts and the board's panel and SPI numbers (display/ft812_regs.h, panel.h, dspi_config.h): the memory-write and
// read frames, the panel's register list, the refresh rate, the DSPI divider arithmetic and the two speed limits. Runs on the
// host. The same constants are cross-checked against the pinned Riedel header by track3r-os's host test `eve_riedel`.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/blink.h>
#include <o3sdk/platform/mk66/display/dspi_config.h>
#include <o3sdk/platform/mk66/display/ft812_regs.h>
#include <o3sdk/platform/mk66/display/panel.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace display = o3sdk::platform::mk66::display;
namespace ft = o3sdk::platform::mk66::display::ft812;
namespace spi = o3sdk::platform::mk66::display::spi;

TEST_CASE("display regs: write and read frames carry the address in three bytes, a read has the dummy byte") {
  std::uint8_t w[ft::kWriteHeaderBytes];
  ft::write_header(ft::kRamDl, w);  // 0x300000
  CHECK(w[0] == 0xB0);
  CHECK(w[1] == 0x00);
  CHECK(w[2] == 0x00);
  ft::write_header(ft::reg::kCmdbWrite, w);  // 0x302578: Riedel's burst header is B0 25 78
  CHECK(w[0] == 0xB0);
  CHECK(w[1] == 0x25);
  CHECK(w[2] == 0x78);
  ft::write_header(0x0FFFC0u, w);  // the top of RAM_G
  CHECK(w[0] == 0x8F);
  CHECK(w[1] == 0xFF);
  CHECK(w[2] == 0xC0);

  std::uint8_t r[ft::kReadHeaderBytes];
  ft::read_header(ft::reg::kId, r);  // 0x302000
  CHECK(r[0] == 0x30);
  CHECK(r[1] == 0x20);
  CHECK(r[2] == 0x00);
  CHECK(r[3] == 0x00);
  // A write has bit 7 set and bit 6 clear, a read has both clear, for every address of the 22-bit space.
  const std::uint32_t addresses[] = {0u, 0x0FFFFFu, ft::kRamDl, ft::kRamReg, ft::kRamCmd, 0x3FFFFFu};
  for (const std::uint32_t a : addresses) {
    ft::write_header(a, w);
    ft::read_header(a, r);
    CHECK((w[0] & 0xC0u) == 0x80u);
    CHECK((r[0] & 0xC0u) == 0x00u);
    CHECK(((static_cast<std::uint32_t>(w[0] & 0x3Fu) << 16) | (static_cast<std::uint32_t>(w[1]) << 8) | w[2]) == a);
  }
  CHECK(ft::addr_valid(0x3FFFFFu));
  CHECK_FALSE(ft::addr_valid(0x400000u));
}

TEST_CASE("display regs: the registers sit in the register block, word aligned, none twice") {
  const std::uint32_t regs[] = {ft::reg::kId, ft::reg::kFrames, ft::reg::kClock, ft::reg::kCpuReset, ft::reg::kHcycle,
                                ft::reg::kHoffset, ft::reg::kHsize, ft::reg::kHsync0, ft::reg::kHsync1, ft::reg::kVcycle,
                                ft::reg::kVoffset, ft::reg::kVsize, ft::reg::kVsync0, ft::reg::kVsync1, ft::reg::kDlSwap,
                                ft::reg::kRotate, ft::reg::kOutBits, ft::reg::kDither, ft::reg::kSwizzle, ft::reg::kCspread,
                                ft::reg::kPclkPol, ft::reg::kPclk, ft::reg::kVolPb, ft::reg::kVolSound, ft::reg::kSound,
                                ft::reg::kGpioXDir, ft::reg::kGpioX, ft::reg::kPwmHz, ft::reg::kPwmDuty, ft::reg::kCmdRead,
                                ft::reg::kCmdWrite, ft::reg::kCmdDl, ft::reg::kCmdbSpace, ft::reg::kCmdbWrite};
  const std::size_t n = sizeof(regs) / sizeof(regs[0]);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(regs[i] >= ft::kRamReg);
    CHECK(regs[i] < ft::kRamCmd);
    CHECK(regs[i] % 4u == 0u);
    for (std::size_t j = i + 1; j < n; ++j) {
      CHECK(regs[i] != regs[j]);
    }
  }
}

TEST_CASE("display panel: the 800x480 numbers, in the order the init writes them") {
  const display::PanelTiming& p = display::kPanel800x480;
  CHECK(p.hsize == 800);
  CHECK(p.vsize == 480);
  CHECK(p.hcycle == 1056);
  CHECK(p.hoffset == 46);
  CHECK(p.hsync0 == 0);
  CHECK(p.hsync1 == 10);
  CHECK(p.vcycle == 525);
  CHECK(p.voffset == 23);
  CHECK(p.vsync0 == 0);
  CHECK(p.vsync1 == 10);
  CHECK(p.pclk == 2);
  CHECK(p.pclk_pol == 1);
  CHECK(p.swizzle == 0);
  CHECK(p.cspread == 0);
  CHECK(p.dither == 1);
  CHECK(p.outbits == 0x1B6);

  struct Expect {
    std::uint32_t addr, value;
    std::uint8_t width;
  };
  const Expect expect[display::kPanelWriteCount] = {
      {ft::reg::kHsize, 800, 2},   {ft::reg::kHcycle, 1056, 2}, {ft::reg::kHoffset, 46, 2}, {ft::reg::kHsync0, 0, 2},
      {ft::reg::kHsync1, 10, 2},   {ft::reg::kVsize, 480, 2},   {ft::reg::kVcycle, 525, 2}, {ft::reg::kVoffset, 23, 2},
      {ft::reg::kVsync0, 0, 2},    {ft::reg::kVsync1, 10, 2},   {ft::reg::kSwizzle, 0, 1},  {ft::reg::kPclkPol, 1, 1},
      {ft::reg::kCspread, 0, 1},   {ft::reg::kRotate, 0, 1},    {ft::reg::kDither, 1, 1},   {ft::reg::kOutBits, 0x1B6, 2}};
  for (unsigned i = 0; i < display::kPanelWriteCount; ++i) {
    const display::RegWrite w = display::panel_write(p, display::Orientation::Landscape, i);
    CHECK(w.addr == expect[i].addr);
    CHECK(w.value == expect[i].value);
    CHECK(w.width == expect[i].width);
  }
  CHECK(display::panel_write(p, display::Orientation::Landscape, display::kPanelWriteCount).width == 0);  // past the end: nothing
  // REG_PCLK is not in the list: the pixel clock is the init's last step.
  for (unsigned i = 0; i < display::kPanelWriteCount; ++i) {
    CHECK(display::panel_write(p, display::Orientation::Landscape, i).addr != ft::reg::kPclk);
  }
}

TEST_CASE("display panel: orientation is REG_ROTATE and nothing else") {
  for (unsigned o = 0; o < 8; ++o) {
    const display::RegWrite w = display::panel_write(display::kPanel800x480, static_cast<display::Orientation>(o), 13);
    CHECK(w.addr == ft::reg::kRotate);
    CHECK(w.value == o);
    CHECK(w.width == 1);
    // the other fifteen writes do not depend on it
    for (unsigned i = 0; i < display::kPanelWriteCount; ++i) {
      if (i != 13) {
        const display::RegWrite a = display::panel_write(display::kPanel800x480, static_cast<display::Orientation>(o), i);
        const display::RegWrite b = display::panel_write(display::kPanel800x480, display::Orientation::Landscape, i);
        CHECK(a.value == b.value);
      }
    }
  }
  CHECK(static_cast<unsigned>(display::Orientation::LandscapeInverted) == 1u);
}

TEST_CASE("display panel: pixel clock and refresh") {
  CHECK(display::pixel_clock_hz(display::kPanel800x480) == 30'000'000u);
  CHECK(display::frame_millihz(display::kPanel800x480) == 54'112u);
  display::PanelTiming off = display::kPanel800x480;
  off.pclk = 0;
  CHECK(display::pixel_clock_hz(off) == 0u);
  CHECK(display::frame_millihz(off) == 0u);
  CHECK_FALSE(display::panel_valid(off));  // a panel that never gets a clock is not a panel
  display::PanelTiming bad = display::kPanel800x480;
  bad.hoffset = 300;  // the active line would run past the cycle
  CHECK_FALSE(display::panel_valid(bad));
  bad = display::kPanel800x480;
  bad.vcycle = 5000;  // a 12-bit counter
  CHECK_FALSE(display::panel_valid(bad));
}

TEST_CASE("display spi: dividers, the two speed classes and their limits at the 90 MHz bus") {
  CHECK(spi::pbr_divider(0) == 2u);
  CHECK(spi::pbr_divider(1) == 3u);
  CHECK(spi::pbr_divider(2) == 5u);
  CHECK(spi::pbr_divider(3) == 7u);
  CHECK(spi::br_divider(0) == 2u);
  CHECK(spi::br_divider(1) == 4u);
  CHECK(spi::br_divider(2) == 6u);
  CHECK(spi::br_divider(3) == 8u);
  CHECK(spi::br_divider(4) == 16u);
  CHECK(spi::br_divider(5) == 32u);
  CHECK(spi::br_divider(15) == 32768u);

  CHECK(spi::kBusHz == 90'000'000u);
  CHECK(spi::sck_hz(spi::kBusHz, spi::kInitRate) == 1'125'000u);
  CHECK(spi::sck_hz(spi::kBusHz, spi::kDataRate) == 18'000'000u);
  CHECK(spi::sck_hz(spi::kBusHz, spi::kInitRate) <= spi::kMaxInitHz);
  CHECK(spi::sck_hz(spi::kBusHz, spi::kDataRate) <= spi::kMaxDataHz);
  CHECK(spi::ctar(spi::kInitRate) == 0x38020004u);
  CHECK(spi::ctar(spi::kDataRate) == 0xB8020000u);
  // Mode 0, MSB first: CPOL, CPHA and LSBFE stay clear; 8-bit frames: FMSZ = 7.
  for (const spi::Rate r : {spi::kInitRate, spi::kDataRate}) {
    const std::uint32_t c = spi::ctar(r);
    CHECK(((c >> 24) & 7u) == 0u);        // LSBFE, CPHA, CPOL
    CHECK(((c >> 27) & 15u) == 7u);       // FMSZ
    CHECK((c & 0x0000FFF0u) == 0u);       // CSSCK, ASC, DT: chip select is a GPIO, no hardware delays
  }
}

TEST_CASE("display spi: no divider pair the init or data rate could be mistyped into stays legal by accident") {
  // Every (PBR, BR, DBR) combination: the ones at or under the init limit and the ones at or under the data limit are
  // exactly those the two constants may be chosen from, and the two constants are the fastest of each class that still has a
  // 50/50 or 40/60 duty cycle (no PBR /3 with DBR: 33/67).
  std::uint32_t fastest_init = 0;
  std::uint32_t fastest_data = 0;
  for (std::uint8_t pbr = 0; pbr < 4; ++pbr) {
    for (std::uint8_t br = 0; br < 16; ++br) {
      for (const bool dbr : {false, true}) {
        const std::uint32_t hz = spi::sck_hz(spi::kBusHz, spi::Rate{pbr, br, dbr});
        if (hz <= spi::kMaxInitHz && hz > fastest_init) {
          fastest_init = hz;
        }
        if (hz <= spi::kMaxDataHz && hz > fastest_data) {
          fastest_data = hz;
        }
      }
    }
  }
  CHECK(fastest_data == 18'000'000u);  // the data rate is the fastest the 20 MHz limit allows from the 90 MHz bus
  CHECK(fastest_init >= 10'000'000u);  // the init could run faster; it deliberately does not (margin for the warm-start part)
  CHECK(spi::sck_hz(spi::kBusHz, spi::kInitRate) < fastest_init);
}

TEST_CASE("display blink codes: the display reports are distinct from every other code and read by counting") {
  namespace blink = o3sdk::platform::mk66::blink;
  const std::uint8_t codes[] = {blink::kClockFailure, blink::kSdramFailure, blink::kDisplayFailure, blink::kDisplayPolled,
                                blink::kCrashSeen,    blink::kDrillRoundTrip, blink::kDisplayStalled, blink::kAppFailure};
  for (std::size_t i = 0; i < sizeof(codes); ++i) {
    CHECK(codes[i] >= 1);
    CHECK(codes[i] <= 9);
    for (std::size_t j = i + 1; j < sizeof(codes); ++j) {
      CHECK(codes[i] != codes[j]);
    }
  }
  CHECK(blink::kDisplayFailure == 4);
  CHECK(blink::kDisplayPolled == 5);
  CHECK(blink::kDisplayStalled == 8);
  CHECK(blink::kAppFailure == 9);
  // the safe-state codes are shown for good in place of the heartbeat: counted flashes, then a pause, never the heartbeat's shape
  for (const std::uint8_t code : {blink::kDisplayStalled, blink::kAppFailure}) {
    unsigned on_edges = 0;
    bool prev = false;
    for (std::uint32_t t = 0; t < blink::period_ms(code); ++t) {
      const bool on = blink::led_on(code, t);
      on_edges += (on && !prev) ? 1u : 0u;
      prev = on;
    }
    CHECK(on_edges == code);
  }
}
