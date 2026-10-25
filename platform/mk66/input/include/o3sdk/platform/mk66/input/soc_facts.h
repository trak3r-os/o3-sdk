// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MK66 register values the input drivers' hardware half (src/device.cpp) writes, as constants in a header the host tests can read: a
// wrong bit is an encoder or a pin that silently does nothing, so every value is pinned by a test (mk66_input_pins_test.cpp) against a
// table cited from the fact source this tree reads: NXP's device header. K66 Sub-Family Reference
// Manual: "SIM", "PORT", "FlexTimer (FTM)", "Quadrature decoder mode".

#include <cstdint>

namespace o3sdk::platform::mk66::input::soc {

// ---- Port control register (PCR) fields ------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kPcrMuxShift = 8;
inline constexpr std::uint32_t kPcrPs = 1u << 0;   // pull select: 1 up, 0 down (with PE)
inline constexpr std::uint32_t kPcrPe = 1u << 1;   // pull enable
inline constexpr std::uint32_t kPcrPfe = 1u << 4;  // passive input filter
constexpr std::uint32_t pcr_mux(unsigned alt) noexcept { return static_cast<std::uint32_t>(alt) << kPcrMuxShift; }

// An INT line: a GPIO input with the pull-up on.
inline constexpr std::uint32_t kPcrInterruptInput = pcr_mux(1) | kPcrPe | kPcrPs;
// The encoder pins: ALT6 (FTM2_QD_PHA / PHB), passive filter, pull-DOWN: 0x00000612.
inline constexpr std::uint32_t kPcrEncoder = pcr_mux(6) | kPcrPfe | kPcrPe;

// ---- FTM2: the quadrature decoder -----------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kScgc3Register = 0x40048030u;  // SIM_SCGC3
inline constexpr std::uint32_t kScgc3Ftm2 = 1u << 24;
inline constexpr std::uintptr_t kFtm2Base = 0x400B8000u;
inline constexpr std::uintptr_t kFtmSc = 0x00;
inline constexpr std::uintptr_t kFtmCnt = 0x04;
inline constexpr std::uintptr_t kFtmMod = 0x08;
inline constexpr std::uintptr_t kFtmC0Sc = 0x0C;
inline constexpr std::uintptr_t kFtmC1Sc = 0x14;
inline constexpr std::uintptr_t kFtmCntin = 0x4C;
inline constexpr std::uintptr_t kFtmMode = 0x54;
inline constexpr std::uintptr_t kFtmCombine = 0x64;
inline constexpr std::uintptr_t kFtmFilter = 0x78;
inline constexpr std::uintptr_t kFtmQdctrl = 0x80;
inline constexpr std::uint32_t kModeWpdis = 0x04;   // write protection off (the registers below are write-protected otherwise)
inline constexpr std::uint32_t kModeFtmen = 0x01;   // the FTM-specific registers (QDCTRL, FILTER) are usable
inline constexpr std::uint32_t kFilterValue = 0x4444;  // a filter of 4 on all four channel inputs
inline constexpr std::uint32_t kQdctrlPhaFilter = 0x80;
inline constexpr std::uint32_t kQdctrlPhbFilter = 0x40;
inline constexpr std::uint32_t kQdctrlQuaden = 0x01;  // quadrature decoder on; QUADMODE = 0: phase A / phase B encoding, polarity normal
inline constexpr std::uint32_t kQdctrlValue = kQdctrlPhaFilter | kQdctrlPhbFilter | kQdctrlQuaden;  // 0xC1
inline constexpr std::uint32_t kModulo = 0xFFFF;  // the counter wraps at 16 bits
inline constexpr std::uint32_t kCountMask = 0xFFFF;

}  // namespace o3sdk::platform::mk66::input::soc
