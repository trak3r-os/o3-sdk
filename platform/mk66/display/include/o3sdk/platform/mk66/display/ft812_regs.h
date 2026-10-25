// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FT812 facts the display driver needs: memory map, the registers it touches, host commands, the SPI frame layout, a
// few display-list words. Written from the FT81X Series Programmers Guide v1.2 (BRT_000031: "Memory Map" p.253,
// "Registers" pp.27-34, "Host commands" p.11-12); facts only, no text. Every value is cross-checked against the
// pinned Riedel library's own header (track3r-os third_party/eve-riedel/EVE.h, MIT) by track3r-os's host test `eve_riedel`, so a typo
// here fails a test instead of a display. No header or source of the official firmware's display driver was used (ADR 0011 amendment 1).

#include <cstdint>

namespace o3sdk::platform::mk66::display::ft812 {

// ---- Memory map (Guide "Memory Map") --------------------------------------------------------------------------------
inline constexpr std::uint32_t kRamG = 0x000000u;      // graphics memory, 1 MiB
inline constexpr std::uint32_t kRamGBytes = 0x100000u;
inline constexpr std::uint32_t kRamDl = 0x300000u;     // display-list memory, 8 KiB = 2048 words
inline constexpr std::uint32_t kRamDlBytes = 0x2000u;
inline constexpr std::uint32_t kRamReg = 0x302000u;    // registers
inline constexpr std::uint32_t kRamCmd = 0x308000u;    // coprocessor command FIFO, 4 KiB
inline constexpr std::uint32_t kCmdFifoBytes = 0x1000u;

// ---- Registers ------------------------------------------------------------------------------------------------------
namespace reg {
inline constexpr std::uint32_t kId = 0x302000u;        // reads 0x7C once the chip is up
inline constexpr std::uint32_t kFrames = 0x302004u;    // frames scanned out since reset
inline constexpr std::uint32_t kClock = 0x302008u;     // 60 MHz cycles since reset
inline constexpr std::uint32_t kCpuReset = 0x302020u;  // bit 0 coprocessor, 1 touch, 2 audio engine: 1 = held in reset
inline constexpr std::uint32_t kHcycle = 0x30202Cu;
inline constexpr std::uint32_t kHoffset = 0x302030u;
inline constexpr std::uint32_t kHsize = 0x302034u;
inline constexpr std::uint32_t kHsync0 = 0x302038u;
inline constexpr std::uint32_t kHsync1 = 0x30203Cu;
inline constexpr std::uint32_t kVcycle = 0x302040u;
inline constexpr std::uint32_t kVoffset = 0x302044u;
inline constexpr std::uint32_t kVsize = 0x302048u;
inline constexpr std::uint32_t kVsync0 = 0x30204Cu;
inline constexpr std::uint32_t kVsync1 = 0x302050u;
inline constexpr std::uint32_t kDlSwap = 0x302054u;    // write 2: swap at the next frame; reads 0 when the list buffer is safe to write
inline constexpr std::uint32_t kRotate = 0x302058u;    // bits 2:0, takes effect at once, does not touch the touch matrix
inline constexpr std::uint32_t kOutBits = 0x30205Cu;
inline constexpr std::uint32_t kDither = 0x302060u;
inline constexpr std::uint32_t kSwizzle = 0x302064u;
inline constexpr std::uint32_t kCspread = 0x302068u;
inline constexpr std::uint32_t kPclkPol = 0x30206Cu;
inline constexpr std::uint32_t kPclk = 0x302070u;      // pixel clock divider of the 60 MHz system clock; 0 = no clock to the panel
inline constexpr std::uint32_t kVolPb = 0x302080u;
inline constexpr std::uint32_t kVolSound = 0x302084u;
inline constexpr std::uint32_t kSound = 0x302088u;
inline constexpr std::uint32_t kGpioXDir = 0x302098u;  // bit 15: DISP
inline constexpr std::uint32_t kGpioX = 0x30209Cu;
inline constexpr std::uint32_t kPwmHz = 0x3020D0u;     // backlight PWM frequency, 250..10000 Hz, default 250
inline constexpr std::uint32_t kPwmDuty = 0x3020D4u;   // backlight, 0..128
inline constexpr std::uint32_t kCmdRead = 0x3020F8u;   // coprocessor FIFO read pointer; 0xFFF = fault
inline constexpr std::uint32_t kCmdWrite = 0x3020FCu;
inline constexpr std::uint32_t kCmdDl = 0x302100u;
inline constexpr std::uint32_t kCmdbSpace = 0x302574u; // free bytes in the command FIFO (bits 1:0 non-zero = fault)
inline constexpr std::uint32_t kCmdbWrite = 0x302578u; // burst window into the command FIFO
}  // namespace reg

inline constexpr std::uint8_t kChipId = 0x7C;
inline constexpr std::uint16_t kGpioXDisp = 0x8000u;
inline constexpr std::uint8_t kPwmDutyMax = 128;
inline constexpr std::uint8_t kDlSwapFrame = 2;        // swap at the next frame, no tearing ("recommended in most cases")
inline constexpr std::uint16_t kSoundMute = 0x0060u;   // REG_SOUND: effect "mute"
inline constexpr std::uint16_t kCmdFault = 0x0FFFu;    // REG_CMD_READ value that says the coprocessor stopped

// ---- The snapshot (OS-HIL, `diag shot`): CMD_SNAPSHOT2 fmt, ptr, x | y << 16, w | h << 16 (the Guide's CMD_SNAPSHOT2), and the two
// output formats the console asks for. RGB565 is EVE.h's; ARGB8 is the code 0x20, which EVE.h does not name.
inline constexpr std::uint32_t kCmdSnapshot2 = 0xFFFFFF37u;
inline constexpr std::uint32_t kFmtRgb565 = 7u;
inline constexpr std::uint32_t kFmtArgb8 = 0x20u;

// ---- Host commands: three bytes, command / parameter / 0 ---------------------------------------------------------
namespace host {
inline constexpr std::uint8_t kActive = 0x00;
inline constexpr std::uint8_t kClkExt = 0x44;
inline constexpr std::uint8_t kClkInt = 0x48;
inline constexpr std::uint8_t kRstPulse = 0x68;  // reset the core, every register back to its default
}  // namespace host

// ---- Display-list words used by the driver itself (the app's lists come from eve/dl.h) ----------------------------
inline constexpr std::uint32_t kDlDisplay = 0x00000000u;
inline constexpr std::uint32_t kDlClearColorBlack = 0x02000000u;  // CLEAR_COLOR_RGB(0, 0, 0)
inline constexpr std::uint32_t kDlClearAll = 0x26000007u;         // CLEAR(colour, stencil, tag)

// ---- SPI frames: memory write = (0x80 | addr[21:16]), addr[15:8], addr[7:0], data...; read = (addr[21:16]), addr[15:8],
// addr[7:0], one dummy byte, then data. Addresses are 22 bits; a write has bit 7 set (and bit 6 clear).
inline constexpr std::uint8_t kWriteFlag = 0x80;
inline constexpr unsigned kWriteHeaderBytes = 3;
inline constexpr unsigned kReadHeaderBytes = 4;

constexpr bool addr_valid(std::uint32_t addr) noexcept { return addr < 0x400000u; }

constexpr void write_header(std::uint32_t addr, std::uint8_t (&out)[kWriteHeaderBytes]) noexcept {
  out[0] = static_cast<std::uint8_t>(kWriteFlag | ((addr >> 16) & 0x3Fu));
  out[1] = static_cast<std::uint8_t>(addr >> 8);
  out[2] = static_cast<std::uint8_t>(addr);
}
constexpr void read_header(std::uint32_t addr, std::uint8_t (&out)[kReadHeaderBytes]) noexcept {
  out[0] = static_cast<std::uint8_t>((addr >> 16) & 0x3Fu);
  out[1] = static_cast<std::uint8_t>(addr >> 8);
  out[2] = static_cast<std::uint8_t>(addr);
  out[3] = 0;  // the dummy byte
}

static_assert(kRamDl + kRamDlBytes == kRamReg, "the registers follow the display-list memory");
static_assert(kRamDlBytes / 4u == 2048u, "RAM_DL holds 2048 display-list words (Guide: 8 KiB)");
static_assert(kRamGBytes == 1024u * 1024u, "RAM_G is 1 MiB on the FT812");
static_assert(reg::kPwmDuty > reg::kPwmHz && reg::kCmdbWrite > reg::kCmdbSpace, "register block sanity");

}  // namespace o3sdk::platform::mk66::display::ft812
