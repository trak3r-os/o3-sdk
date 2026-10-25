// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SDRAM controller (SDRAMC) configuration of the Tracker's 16 MiB SDRAM, as the register words the boot code
// writes, with their arithmetic, so the compiler and the host tests check them and the boot code only writes them.
//
// Register layout, bit positions and the command sequence: K66 Sub-Family Reference Manual, "SDRAM Controller (SDRAMC)"
// (CTRL: RC, RTIM, IS; ACn: IP, PS, IMRS, CBM, CASL, RE, BA; CMn: V, WP, BAM); the register addresses and field positions
// were cross-checked against NXP's MK66F18 device header, which is a fact table.
//
// Board timings (this image's values for the board's SDRAM part): the SDRAM controller is programmed with
// refresh row time 15,625 ns (4096 rows in 64 ms), refresh timing
//       six clocks, 16-bit port, command bit 20, CAS latency 3 (the part's tRCD, tRP and tRAS minimums are met at 90 MHz), block
//       mask 0xFC0000 (16 MiB at 0x70000000), mode register: burst length 1, sequential, CAS latency 3, standard
//       operation, programmed burst length;
// and its clock: the controller's clock is taken as 90,000,000 Hz (the FlexBus
//       clock, SIM_CLKDIV1 OUTDIV3 = 1), CLKOUT selects the FlexBus clock
//       (SIM_SOPT2[CLKOUTSEL] = 0), SIM_SOPT2[FBSL] = 3, FB_CSPMCR groups 2-5 = 2 (byte enables), and the 39 pins below
//       at mux 5;
// and how the SDRAM's own address pins A0..A13 are wired to the controller's address
//       bits (the board's PCB wiring), which fixes where the command bit (A10) is and which address carries the mode
//       register word.

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/mem_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::sdram {

// ---- Controller registers -------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kControllerBase = 0x4000F000u;
inline constexpr std::uint32_t kCtrlAddress = kControllerBase + 0x42u;  // 16-bit
inline constexpr std::uint32_t kAc0Address = kControllerBase + 0x48u;   // 32-bit, block 0 address and control
inline constexpr std::uint32_t kCm0Address = kControllerBase + 0x4Cu;   // 32-bit, block 0 control mask

// CTRL.
inline constexpr std::uint32_t kRefreshRowNs = 15'625u;  // 64 ms / 4096 rows
inline constexpr std::uint32_t kRefreshTimingSixClocks = 1u;  // CTRL[RTIM]: 0 = 3, 1 = 6, 2 = 9 bus clocks
// CTRL[RC]: a refresh every (RC + 1) * 16 FlexBus clocks, the longest interval that does not exceed the row time.
constexpr std::uint32_t refresh_count(std::uint32_t flexbus_hz, std::uint32_t row_ns) noexcept {
  const std::uint64_t clocks = static_cast<std::uint64_t>(row_ns) * (flexbus_hz / 1000u) / 1'000'000u;
  return static_cast<std::uint32_t>(clocks / 16u) - 1u;
}
inline constexpr std::uint32_t kRc = refresh_count(clock::kFlexBusHz, kRefreshRowNs);
inline constexpr std::uint32_t kCtrl = kRc | (kRefreshTimingSixClocks << 9);  // RC [8:0], RTIM [10:9], IS [11] clear

// AC0 and CM0.
inline constexpr std::uint32_t kPortSize16Bit = 2u;  // ACn[PS]: 0 = 32-bit, 1 = 8-bit, 2 = 16-bit
inline constexpr std::uint32_t kCasLatency = 3u;     // ACn[CASL]
inline constexpr std::uint32_t kAcIp = 1u << 3;      // initiate precharge (all banks) on the next access
inline constexpr std::uint32_t kAcImrs = 1u << 6;    // initiate mode register set on the next access
inline constexpr std::uint32_t kAcRe = 1u << 15;     // auto refresh enable
inline constexpr std::uint32_t kBaMask = 0xFFFC0000u;  // ACn[BA] and CMn[BAM]: address bits 31:18

// The SDRAM's own address pins A0..A13 are wired to these controller address bits .
inline constexpr std::uint8_t kPinToAddressBit[14] = {16, 15, 14, 13, 12, 11, 10, 9, 18, 19, 20, 21, 22, 23};
// The command bit (the SDRAM's A10, which selects "all banks" in a precharge) is therefore controller address bit 20;
// ACn[CBM] encodes address bit 17 + CBM.
inline constexpr std::uint32_t kCommandBit = kPinToAddressBit[10];
inline constexpr std::uint32_t kCbm = kCommandBit - 17u;

inline constexpr std::uint32_t kAc0 = (kPortSize16Bit << 4) | (kCbm << 8) | (kCasLatency << 12) | (mem::kSdramBase & kBaMask);
// Block size 16 MiB: address bits 23:18 are not compared (CMn[BAM] = 1 means "do not care"), V = 1 makes the block valid.
inline constexpr std::uint32_t kBlockMask = 0x00FC0000u;
inline constexpr std::uint32_t kCm0 = (kBlockMask & kBaMask) | 1u;

// OS-PERF3: the ICode alias. The controller answers where the unmasked bits of BA match: with address bits 30:27 also "do not care" (CM0[BAM] bits 30:27) an access to 0x0800_0000 + n
// (mem_map.h kSdramAliasBase, the RM's "SDRAM (Aliased area)", a code-bus address the LMEM code cache can hold) reaches the same SDRAM byte as 0x7000_0000 + n. Only addresses that the
// crossbar sends to the SDRAM/FlexBus slave get here (flash, SRAM and the peripherals are decoded before), so the extra bits make no other address answer; the RM's own recipe is "set the
// address mask bit 31" for a block at 0x8800_0000, the same idea from the other side. Written after the memory tests, by boot::init_sdram.
inline constexpr std::uint32_t kAliasBam = 0x78000000u;
inline constexpr std::uint32_t kCm0WithAlias = kCm0 | kAliasBam;

// Mode register: burst length 1 (A2:A0 = 000), sequential (A3 = 0), CAS latency 3 (A6:A4 = 011), standard operation
// (A8:A7 = 00), programmed burst length (A9 = 0). The word is presented on the SDRAM's address pins by reading or
// writing an address whose controller address bits are wired to those pins.
inline constexpr std::uint32_t kModeRegister = (kCasLatency << 4);
constexpr std::uint32_t mode_register_offset(std::uint32_t mode) noexcept {
  std::uint32_t offset = 0;
  for (unsigned pin = 0; pin < 10u; ++pin) {
    if ((mode & (1u << pin)) != 0u) {
      offset |= 1u << kPinToAddressBit[pin];
    }
  }
  return offset;
}
inline constexpr std::uint32_t kMrsAddress = mem::kSdramBase + mode_register_offset(kModeRegister);
// The value of the byte written to trigger a command (any value does).
inline constexpr std::uint8_t kCommandAccessValue = 0xAA;

// ---- Everything around the controller -------------------------------------------------------------------------------
inline constexpr std::uint32_t kSimSopt2Fbsl = 3u << 8;       // FlexBus security level 3: off-chip data and instruction access allowed
inline constexpr std::uint32_t kSimSopt2ClkoutFlexBus = 0u;   // CLKOUTSEL = 0: CLKOUT carries the FlexBus clock to the SDRAM
// FB_CSPMCR groups 2..5 = 2: the shared pins carry the byte enables (the SDRAM's DQM2 and DQM3 among them).
inline constexpr std::uint32_t kCspmcrMask = 0x0FFFF000u;
inline constexpr std::uint32_t kCspmcrValue = (2u << 24) | (2u << 20) | (2u << 16) | (2u << 12);

// Waits, longer than a part needs, on purpose: the precharge must not follow the clock start by a few microseconds, and the
// "at least 8 refresh cycles" of the mode register sequence take 8 x 15.5 us at one refresh per 15.5 us.
// A longer wait cannot break a part that initialises with a shorter one.
inline constexpr std::uint32_t kPowerUpDelayMs = 10u;      // after the power key goes high
inline constexpr std::uint32_t kClockStableDelayUs = 200u;  // CLKOUT and CKE running, before the precharge (JEDEC: >= 100-200 us)
inline constexpr std::uint32_t kRefreshSettleDelayUs = 200u;  // after refresh is enabled, before the mode register set: 8 refreshes need 125 us
static_assert(kRefreshSettleDelayUs * 1000u >= 8u * (kRc + 1u) * 16u * 1000u / (clock::kFlexBusHz / 1'000'000u), "at least eight refresh intervals");

// The 39 pins at mux 5 (ALT5) that make up the SDRAM interface: CLKOUT, the control lines, 15 address lines A9..A23 and
// the 16 data lines D16..D31 (a 16-bit port on the upper data lanes). The tests hold an independent copy with the signal names.
inline constexpr std::uint32_t kPinMux = 5u;
inline constexpr Pin kPins[] = {
    // CLKOUT, SDRAM_CAS_b, SDRAM_RAS_b, SDRAM_WE, SDRAM_CS0_b, SDRAM_DQM2, SDRAM_DQM3, SDRAM_CKE
    {Port::A, 6}, {Port::B, 0}, {Port::B, 1}, {Port::B, 2}, {Port::B, 3}, {Port::C, 16}, {Port::C, 17}, {Port::D, 7},
    // SDRAM_A23 .. SDRAM_A9
    {Port::B, 18}, {Port::C, 0}, {Port::C, 1}, {Port::C, 2}, {Port::C, 4}, {Port::C, 5}, {Port::C, 6}, {Port::C, 7},
    {Port::C, 8}, {Port::C, 9}, {Port::C, 10}, {Port::D, 2}, {Port::D, 3}, {Port::D, 4}, {Port::D, 5},
    // SDRAM_D31 .. SDRAM_D16
    {Port::B, 20}, {Port::B, 21}, {Port::B, 22}, {Port::B, 23}, {Port::C, 12}, {Port::C, 13}, {Port::C, 14}, {Port::C, 15},
    {Port::B, 6}, {Port::B, 7}, {Port::B, 8}, {Port::B, 9}, {Port::B, 10}, {Port::B, 11}, {Port::B, 16}, {Port::B, 17},
};
inline constexpr unsigned kPinCount = sizeof(kPins) / sizeof(kPins[0]);
static_assert(kPinCount == 39u, "8 control + 15 address + 16 data pins");

// ---- Checked here so a mistyped constant is a compile error --------------------------------------------------------
static_assert(kRc == 86u, "refresh every 87 x 16 = 1392 FlexBus clocks = 15.47 us at 90 MHz (row time 15.625 us)");
static_assert(kCtrl == 0x0256u, "SDRAMC_CTRL value");
static_assert(kCbm == 3u && kCommandBit == 20u, "the command bit is controller address bit 20 (SDRAM A10)");
static_assert(kAc0 == 0x70003320u, "SDRAMC_AC0: PS 16-bit, CBM 20, CASL 3, base 0x70000000");
static_assert(kCm0 == 0x00FC0001u, "SDRAMC_CM0: 16 MiB, valid");
static_assert(kMrsAddress == 0x70001800u, "mode register word 0x030 (CAS 3) lands on controller address bits 12 and 11");
static_assert((mem::kSdramBase & ~kBaMask) == 0u, "SDRAM base is aligned to the block address field");
static_assert(((kBlockMask >> 18) & 0x3Fu) == 0x3Fu && (1u << (18u + 6u)) == mem::kSdramSize, "BAM masks six bits above bit 18: 2^24 = 16 MiB");
static_assert(sizeof(kPinToAddressBit) == 14, "SDRAM A0..A13");
// The alias: the base compare still holds for 0x7000_0000 and now also for 0x0800_0000, and for no address of the flash or SRAM windows below 0x2000_0000 other than the alias window.
static_assert(kCm0WithAlias == 0x78FC0001u, "SDRAMC_CM0 with the ICode alias: bits 30:27 and 23:18 do not care");
static_assert(((mem::kSdramBase ^ mem::kSdramAliasBase) & ~kCm0WithAlias & kBaMask) == 0u, "0x7000_0000 and 0x0800_0000 compare equal under the alias mask");
static_assert((kAliasBam & kBlockMask) == 0u && (kAliasBam & 0x80000000u) == 0u, "the alias mask adds bits 30:27 only: bit 31 and the block size are as they were");
static_assert(mem::sdram_alias(mem::kSdramBase + mem::kSdramSize - 1u) == mem::kSdramAliasBase + mem::kSdramSize - 1u, "the alias is the SDRAM's 16 MiB");

}  // namespace o3sdk::platform::mk66::sdram
