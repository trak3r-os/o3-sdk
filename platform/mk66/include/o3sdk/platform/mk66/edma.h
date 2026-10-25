// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The eDMA and DMAMUX register facts every user of the controller shares: where the blocks are, the layout of a channel's transfer
// control descriptor (TCD) and the DMA_CR value the reset sequence writes. K66 Sub-Family Reference Manual, "eDMA" (base 0x4000_8000,
// TCDs at 0x4000_9000 + 32 x channel) and "DMAMUX" (0x4002_1000). In the base library because the eDMA's global configuration is the
// boot's (boot::dma_init(), with resources.h as the table) and both the audio and the display drivers program channels of it; the
// values are track3r-os's audio_config.h `dma` facts, unchanged (OS-005a, OS-HW1).

#include <cstdint>

namespace o3sdk::platform::mk66::edma {

inline constexpr std::uint32_t kBase = 0x40008000u;
inline constexpr std::uint32_t kTcdBase = 0x40009000u;
inline constexpr std::uint32_t kMuxBase = 0x40021000u;
inline constexpr std::uint32_t kChannels = 16;  // DMAMUX0 and the first 16 eDMA channels; each has its own interrupt line (IRQ n)

// CR: GRP1PRI (group 1 at priority 1, group 0 at 0: the K66's 32 channels are two groups whose priorities must differ, else the eDMA
// records a group priority error (ES[GPE]) and services no channel at all: found on the first hardware boot, OS-HW1), EMLM (minor loop
// mapping: NBYTES is a 30-bit count here) and EDBG (keep running when a debugger halts the core); GRP1PRI exists on the parts with more
// than 16 channels. Written, with every DCHPRI, by boot::dma_init() and by nothing else (resources.h is the table). Because of EDBG a
// core halted by a debugger leaves the DMA replaying the last block: never halt with the amplifier open (README.md).
inline constexpr std::uint32_t kCrValue = 0x400u | 0x80u | 0x02u;
inline constexpr std::uint32_t kCrHalt = 0x20u;  // HALT: no channel starts while boot::dma_init() rewrites the priorities

// The TCD, in register order: one 32-byte descriptor per channel. Written field by field (its 16-bit fields need 16-bit stores).
struct Tcd {
  std::uint32_t saddr;
  std::int16_t soff;
  std::uint16_t attr;
  std::uint32_t nbytes;
  std::int32_t slast;
  std::uint32_t daddr;
  std::int16_t doff;
  std::uint16_t citer;
  std::int32_t dlastsga;
  std::uint16_t csr;
  std::uint16_t biter;
};
static_assert(sizeof(Tcd) == 32, "a TCD is 32 bytes");
static_assert(kTcdBase == kBase + 0x1000u, "the TCDs follow the control block");
static_assert(kCrValue == 0x482u && (kCrValue & kCrHalt) == 0u, "GRP1PRI | EMLM | EDBG, HALT clear");

}  // namespace o3sdk::platform::mk66::edma
