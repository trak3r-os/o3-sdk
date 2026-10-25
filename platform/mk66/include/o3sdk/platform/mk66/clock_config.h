// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The clock tree at 180 MHz in HSRUN, as register values with their arithmetic, so the numbers are checked
// by the compiler and the host tests and the boot code only writes them. Sources: the K66 Sub-Family Reference Manual
// (MCG, SIM and SMC chapters). The targets are F_CPU = 180 MHz, F_BUS = 90 MHz and, for the SDRAM,
// a 90,000,000 Hz FlexBus clock (SIM_CLKDIV1 OUTDIV3 = 1). They are proven values on this board, not independent datasheet limits.
//
//   16 MHz crystal (OSC0, 8-32 MHz range, 10 pF load: SC8P + SC2P)
//     -> MCG FBE: external reference as the clock source, FLL reference divider 16 MHz / 512 (C1[FRDIV] = 4)
//     -> SMC: HSRUN (PMPROT[AHSRUN], PMCTRL[RUNM] = 11b, wait PMSTAT = HSRUN)
//     -> PLL: C5[PRDIV] = 1 (divide by 2: 8 MHz), C6[VDIV] = 29 (multiply by 45: VCO 360 MHz), MCGPLLCLK = VCO / 2
//     -> SIM_CLKDIV1: core /1, bus /2, FlexBus /2, flash /7
//     -> MCG PEE: C1[CLKS] = 0 (PLL as MCGOUTCLK)

#include <cstdint>

namespace o3sdk::platform::mk66::clock {

inline constexpr std::uint32_t kCrystalHz = 16'000'000u;

// MCG registers.
inline constexpr std::uint8_t kPrdiv = 1;   // MCG_C5[PRDIV]: the reference is divided by kPrdiv + 1
inline constexpr std::uint8_t kVdiv = 29;   // MCG_C6[VDIV]: the VCO multiplies by kVdiv + 16
inline constexpr std::uint8_t kFrdiv = 4;   // MCG_C1[FRDIV]: FLL reference divide, 512 with a high-range crystal

// SIM_CLKDIV1: each field divides MCGOUTCLK by its value + 1.
inline constexpr std::uint8_t kOutdiv1 = 0;  // core and system
inline constexpr std::uint8_t kOutdiv2 = 1;  // bus
inline constexpr std::uint8_t kOutdiv3 = 1;  // FlexBus: the SDRAM controller's clock and the SDRAM's CLKOUT
inline constexpr std::uint8_t kOutdiv4 = 6;  // flash

constexpr std::uint32_t pll_reference_hz() noexcept { return kCrystalHz / (kPrdiv + 1u); }
constexpr std::uint32_t vco_hz() noexcept { return pll_reference_hz() * (kVdiv + 16u); }
constexpr std::uint32_t mcgout_hz() noexcept { return vco_hz() / 2u; }
constexpr std::uint32_t core_hz() noexcept { return mcgout_hz() / (kOutdiv1 + 1u); }
constexpr std::uint32_t bus_hz() noexcept { return mcgout_hz() / (kOutdiv2 + 1u); }
constexpr std::uint32_t flexbus_hz() noexcept { return mcgout_hz() / (kOutdiv3 + 1u); }
constexpr std::uint32_t flash_hz() noexcept { return mcgout_hz() / (kOutdiv4 + 1u); }

inline constexpr std::uint32_t kCoreHz = core_hz();
inline constexpr std::uint32_t kBusHz = bus_hz();
inline constexpr std::uint32_t kFlexBusHz = flexbus_hz();
inline constexpr std::uint32_t kFlashHz = flash_hz();

// SIM_CLKDIV1 layout: OUTDIV1 [31:28], OUTDIV2 [27:24], OUTDIV3 [23:20], OUTDIV4 [19:16].
inline constexpr std::uint32_t kClkdiv1 = (static_cast<std::uint32_t>(kOutdiv1) << 28) | (static_cast<std::uint32_t>(kOutdiv2) << 24) |
                                          (static_cast<std::uint32_t>(kOutdiv3) << 20) | (static_cast<std::uint32_t>(kOutdiv4) << 16);

static_assert(pll_reference_hz() == 8'000'000u, "PLL reference 8 MHz (crystal / 2)");
static_assert(vco_hz() == 360'000'000u, "VCO 360 MHz (8 MHz x 45)");
static_assert(kCoreHz == 180'000'000u, "core 180 MHz (HSRUN)");
static_assert(kBusHz == 90'000'000u, "bus 90 MHz (F_BUS)");
static_assert(kFlexBusHz == 90'000'000u, "FlexBus / SDRAM 90 MHz (the SDRAM refresh maths assumes it)");
static_assert(kFlashHz == 25'714'285u, "flash 25.714 MHz (F_MEM)");
static_assert(kClkdiv1 == 0x01160000u, "SIM_CLKDIV1: 0x01060000 | OUTDIV3(1)");
static_assert(kPrdiv <= 7u && kVdiv <= 31u && kFrdiv <= 7u, "field widths: PRDIV 3 bits, VDIV 5 bits, FRDIV 3 bits");
static_assert(kOutdiv1 <= 15u && kOutdiv2 <= 15u && kOutdiv3 <= 15u && kOutdiv4 <= 15u, "OUTDIVn are 4 bits");

// OS-POWER P2: the step that leaves HSRUN (docs/specs/OS-POWER.md 4.4 step 2; RM 16.4.3.3 p352: in HSRUN a clock frequency change is limited to a factor of 2 per step). The HSRUN clock is 180 MHz and the
// crystal 16 MHz (x11), so the source is NOT switched in HSRUN: ONE step of SIM_CLKDIV1 doubles every divider (core /2, bus /4, FlexBus /4, flash /14, MCGOUTCLK still the PLL), which lands every domain
// well inside the RUN limits; HSRUN -> RUN follows; the switch to the crystal (x11) is made in RUN, where the rule does not apply. Every field only grows (a domain only gets slower).
inline constexpr std::uint8_t kExitOutdiv1 = 1;   // core and system /2
inline constexpr std::uint8_t kExitOutdiv2 = 3;   // bus /4
inline constexpr std::uint8_t kExitOutdiv3 = 3;   // FlexBus /4
inline constexpr std::uint8_t kExitOutdiv4 = 13;  // flash /14
inline constexpr std::uint32_t kExitClkdiv1 = (static_cast<std::uint32_t>(kExitOutdiv1) << 28) | (static_cast<std::uint32_t>(kExitOutdiv2) << 24) |
                                              (static_cast<std::uint32_t>(kExitOutdiv3) << 20) | (static_cast<std::uint32_t>(kExitOutdiv4) << 16);
inline constexpr std::uint32_t kExitCoreHz = mcgout_hz() / (kExitOutdiv1 + 1u);                   // on the PLL, after the step
inline constexpr std::uint32_t kExitBusHz = mcgout_hz() / (kExitOutdiv2 + 1u);
inline constexpr std::uint32_t kExitFlexBusHz = mcgout_hz() / (kExitOutdiv3 + 1u);
inline constexpr std::uint32_t kExitFlashHz = mcgout_hz() / (kExitOutdiv4 + 1u);
inline constexpr std::uint32_t kExitCrystalCoreHz = kCrystalHz / (kExitOutdiv1 + 1u);             // the same dividers on the crystal (PBE, in RUN)
static_assert((kExitOutdiv1 + 1u) <= 2u * (kOutdiv1 + 1u) && (kExitOutdiv2 + 1u) <= 2u * (kOutdiv2 + 1u) && (kExitOutdiv3 + 1u) <= 2u * (kOutdiv3 + 1u) && (kExitOutdiv4 + 1u) <= 2u * (kOutdiv4 + 1u),
              "RM 16.4.3.3: no clock of the HSRUN step changes by more than a factor of 2");
static_assert(kExitOutdiv1 >= kOutdiv1 && kExitOutdiv2 >= kOutdiv2 && kExitOutdiv3 >= kOutdiv3 && kExitOutdiv4 >= kOutdiv4 && kExitOutdiv1 <= 15u && kExitOutdiv2 <= 15u && kExitOutdiv3 <= 15u && kExitOutdiv4 <= 15u,
              "the step only slows a domain, and the fields are 4 bits");
static_assert(kExitClkdiv1 == 0x133D0000u, "RF9: SIM_CLKDIV1 of the HSRUN step: core /2, bus /4, FlexBus /4, flash /14");
static_assert(kExitCoreHz == 90'000'000u && kExitBusHz == 45'000'000u && kExitFlexBusHz == 45'000'000u && kExitFlashHz == 12'857'142u, "the state HSRUN is left in: 90 / 45 / 45 / 12.9 MHz");
static_assert(kExitCoreHz < kCoreHz && kExitBusHz < kBusHz && kExitFlashHz < kFlashHz, "every domain is slower than in HSRUN");
// The normal RUN limits, which the state HSRUN is left in must be inside (HSRUN -> RUN is allowed only at a clock RUN allows: RF4, RM Table 16-2 p347): core and system 120 MHz, bus 60 MHz, FlexBus 60 MHz, flash
// 28 MHz. Sources: K66 data sheet Rev 4, Table 10, pp. 17-18; K66 RM Rev 2, Table 6-1, p. 120.
inline constexpr std::uint32_t kRunMaxCoreHz = 120'000'000u;
inline constexpr std::uint32_t kRunMaxBusHz = 60'000'000u;
inline constexpr std::uint32_t kRunMaxFlexBusHz = 60'000'000u;
inline constexpr std::uint32_t kRunMaxFlashHz = 28'000'000u;
static_assert(kExitCoreHz <= kRunMaxCoreHz && kExitBusHz <= kRunMaxBusHz && kExitFlexBusHz <= kRunMaxFlexBusHz && kExitFlashHz <= kRunMaxFlashHz,
              "the state HSRUN is left in (90 / 45 / 45 / 12.9 MHz) is inside the normal RUN limits (120 / 60 / 60 / 28 MHz)");
static_assert(kExitCrystalCoreHz == 8'000'000u, "the same dividers on the 16 MHz crystal: core 8 MHz");

// OS-POWER P2: the clocks of the VLPR off (docs/specs/OS-POWER.md 4.4). In very-low-power RUN the MCG must be bypassed (BLPE: the 16 MHz crystal stays the source) and every domain sits under a
// far lower limit than in RUN: core and system 4 MHz, bus 4 MHz, FlexBus 4 MHz, flash 1 MHz [RF8: data sheet Table 10 p18, inclusive maxima]. SIM_CLKDIV1 is set
// while MCGOUTCLK is already the crystal (the dividers only ever divide further). The limits are not met at the dividers' maxima by accident: flash /16 of the 16 MHz crystal is exactly 1 MHz, the
// edge, so the core, bus and FlexBus take /8 (2 MHz, half the limit, half the draw).
inline constexpr std::uint8_t kVlprOutdiv1 = 7;   // core and system /8
inline constexpr std::uint8_t kVlprOutdiv2 = 7;   // bus /8
inline constexpr std::uint8_t kVlprOutdiv3 = 7;   // FlexBus /8
inline constexpr std::uint8_t kVlprOutdiv4 = 15;  // flash /16, the largest divider
inline constexpr std::uint32_t kVlprMaxCoreHz = 4'000'000u;     // [RF8]
inline constexpr std::uint32_t kVlprMaxBusHz = 4'000'000u;      // [RF8]
inline constexpr std::uint32_t kVlprMaxFlexBusHz = 4'000'000u;  // [RF8]
inline constexpr std::uint32_t kVlprMaxFlashHz = 1'000'000u;    // [RF8]
inline constexpr std::uint32_t kVlprCoreHz = kCrystalHz / (kVlprOutdiv1 + 1u);
inline constexpr std::uint32_t kVlprBusHz = kCrystalHz / (kVlprOutdiv2 + 1u);
inline constexpr std::uint32_t kVlprFlexBusHz = kCrystalHz / (kVlprOutdiv3 + 1u);
inline constexpr std::uint32_t kVlprFlashHz = kCrystalHz / (kVlprOutdiv4 + 1u);
inline constexpr std::uint32_t kVlprClkdiv1 = (static_cast<std::uint32_t>(kVlprOutdiv1) << 28) | (static_cast<std::uint32_t>(kVlprOutdiv2) << 24) |
                                              (static_cast<std::uint32_t>(kVlprOutdiv3) << 20) | (static_cast<std::uint32_t>(kVlprOutdiv4) << 16);
static_assert(kVlprOutdiv1 <= 15u && kVlprOutdiv2 <= 15u && kVlprOutdiv3 <= 15u && kVlprOutdiv4 <= 15u, "OUTDIVn are 4 bits");
static_assert(kVlprCoreHz <= kVlprMaxCoreHz && kVlprBusHz <= kVlprMaxBusHz && kVlprFlexBusHz <= kVlprMaxFlexBusHz && kVlprFlashHz <= kVlprMaxFlashHz,
              "every domain inside its VLPR limit");
static_assert(kVlprCoreHz % kVlprBusHz == 0u && kVlprBusHz % kVlprFlashHz == 0u && kVlprBusHz >= kVlprFlashHz, "the core is a whole multiple of the bus, the bus of the flash clock");
static_assert(kVlprOutdiv1 >= kExitOutdiv1 && kVlprOutdiv2 >= kExitOutdiv2 && kVlprOutdiv3 >= kExitOutdiv3 && kVlprOutdiv4 >= kExitOutdiv4, "the VLPR dividers only divide further than the HSRUN step's");
static_assert(kVlprClkdiv1 == 0x777F0000u, "RF9: SIM_CLKDIV1 (OUTDIV1 [31:28], OUTDIV2 [27:24], OUTDIV3 [23:20], OUTDIV4 [19:16]) of the VLPR off: core, bus, FlexBus /8, flash /16");

// SysTick: 1 kHz from the core clock. The reload register is 24 bits.
inline constexpr std::uint32_t kSysTickHz = 1000u;
inline constexpr std::uint32_t kSysTickReload = kCoreHz / kSysTickHz - 1u;
static_assert(kCoreHz % kSysTickHz == 0u && kSysTickReload <= 0x00FFFFFFu, "whole SysTick counts per millisecond, 24-bit reload");

// What the core runs at before the PLL, for the crude delays of the failure path: MCGOUTCLK after reset is the FLL on
// the internal 32.768 kHz reference (FEI, DMX32 = 0, DRS = 0: x640 = 20.97 MHz), CLKDIV1 = /1 for the core. After the
// crystal is selected (FBE) it is 16 MHz. Only ever used to size busy-wait loops, never for timing the product.
inline constexpr std::uint32_t kFeiHz = 20'971'520u;

}  // namespace o3sdk::platform::mk66::clock
