// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Registers the display driver's hardware half touches: DSPI2, the eDMA with its DMAMUX, the SIM clock gates it needs and
// the NVIC enable. Private to display/src. Written from the K66 Sub-Family Reference Manual (chapters "DSPI", "eDMA",
// "DMAMUX", "SIM", the Cortex-M4 NVIC); addresses and field positions were cross-checked against NXP's MK66F18 device header
// a fact table, not copied. The shared helpers (the `at<>`
// accessor, the pin and GPIO registers) come from the platform's own regs.h.

#include "../../src/regs.h"

#include <o3sdk/platform/mk66/display/edma_config.h>
#include <o3sdk/platform/mk66/display/soc_facts.h>

#include <cstdint>

namespace o3sdk::platform::mk66::display::dev {

using regs::at;
// The bit positions live in soc_facts.h, where a host test pins them: dev::kMcrHalt and the rest resolve to them.
using namespace ::o3sdk::platform::mk66::display::soc;

// ---- SIM clock gates ------------------------------------------------------------------------------------------------
inline volatile std::uint32_t& sim_scgc3() noexcept { return at<std::uint32_t>(0x40048030u); }
inline volatile std::uint32_t& sim_scgc6() noexcept { return at<std::uint32_t>(0x4004803Cu); }

// ---- DSPI2 (RM "DSPI memory map": base 0x400A_C000) ----------------------------------------------------------------------
inline constexpr std::uintptr_t kSpi2 = edma::kSpi2Base;
inline volatile std::uint32_t& spi2_mcr() noexcept { return at<std::uint32_t>(kSpi2 + 0x00u); }
inline volatile std::uint32_t& spi2_tcr() noexcept { return at<std::uint32_t>(kSpi2 + 0x08u); }
inline volatile std::uint32_t& spi2_ctar0() noexcept { return at<std::uint32_t>(kSpi2 + 0x0Cu); }
inline volatile std::uint32_t& spi2_sr() noexcept { return at<std::uint32_t>(kSpi2 + 0x2Cu); }
inline volatile std::uint32_t& spi2_rser() noexcept { return at<std::uint32_t>(kSpi2 + 0x30u); }
inline volatile std::uint32_t& spi2_pushr() noexcept { return at<std::uint32_t>(kSpi2 + 0x34u); }
inline volatile std::uint32_t& spi2_popr() noexcept { return at<std::uint32_t>(kSpi2 + 0x38u); }


// ---- eDMA (RM "eDMA memory map": base 0x4000_8000, TCDs from 0x4000_9000, 32 bytes each) ----------------------------
inline constexpr std::uintptr_t kDma = 0x40008000u;
inline volatile std::uint32_t& dma_es() noexcept { return at<std::uint32_t>(kDma + 0x04u); }
inline volatile std::uint32_t& dma_erq() noexcept { return at<std::uint32_t>(kDma + 0x0Cu); }
inline volatile std::uint8_t& dma_cerq() noexcept { return at<std::uint8_t>(kDma + 0x1Au); }
inline volatile std::uint8_t& dma_serq() noexcept { return at<std::uint8_t>(kDma + 0x1Bu); }
inline volatile std::uint8_t& dma_cdne() noexcept { return at<std::uint8_t>(kDma + 0x1Cu); }
inline volatile std::uint8_t& dma_cerr() noexcept { return at<std::uint8_t>(kDma + 0x1Eu); }
inline volatile std::uint8_t& dma_cint() noexcept { return at<std::uint8_t>(kDma + 0x1Fu); }
inline volatile std::uint32_t& dma_int() noexcept { return at<std::uint32_t>(kDma + 0x24u); }
inline volatile std::uint32_t& dma_err() noexcept { return at<std::uint32_t>(kDma + 0x2Cu); }

inline constexpr std::uintptr_t kTcdBase = 0x40009000u;
inline constexpr std::uintptr_t kTcdStride = 32u;
// A TCD is 32 bytes: SADDR 0x00, SOFF 0x04 (16), ATTR 0x06 (16), NBYTES 0x08, SLAST 0x0C, DADDR 0x10, DOFF 0x14 (16),
// CITER 0x16 (16), DLASTSGA 0x18, CSR 0x1C (16), BITER 0x1E (16).
inline volatile std::uint32_t& tcd_u32(unsigned channel, unsigned offset) noexcept {
  return at<std::uint32_t>(kTcdBase + kTcdStride * channel + offset);
}
inline volatile std::uint16_t& tcd_u16(unsigned channel, unsigned offset) noexcept {
  return at<std::uint16_t>(kTcdBase + kTcdStride * channel + offset);
}

// ---- DMAMUX0 (RM "DMAMUX memory map": base 0x4002_1000, one configuration byte a channel) --------------------------
inline volatile std::uint8_t& dmamux_chcfg(unsigned channel) noexcept { return at<std::uint8_t>(0x40021000u + channel); }

// ---- NVIC enable (ARMv7-M ARM B3: NVIC_ISER / ICER at 0xE000E100 / 0xE000E180, one word per 32 lines) -----------------
inline volatile std::uint32_t& nvic_iser(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E100u + 4u * (irq >> 5)); }
inline volatile std::uint32_t& nvic_icer(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E180u + 4u * (irq >> 5)); }
inline volatile std::uint32_t& nvic_icpr(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E280u + 4u * (irq >> 5)); }

// ---- Pin mux ----------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kPcrMuxAlt2 = 2u << regs::kPcrMuxShift;

}  // namespace o3sdk::platform::mk66::display::dev
