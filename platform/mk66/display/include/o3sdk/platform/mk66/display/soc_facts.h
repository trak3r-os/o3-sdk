// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MK66 register bits the display's hardware half (src/device.cpp) writes, as plain constants in a header the host tests can
// read: a wrong bit position here is a peripheral that silently never comes up (the eDMA clock gate was once written at the
// MK66's neighbour-family bit), so every value is pinned by a test against a table cited from a fact source (NXP's MK66F18 device header, read as a fact
// table, nothing copied): tests/mk66_display_soc_test.cpp. K66 Sub-Family Reference Manual: "SIM", "DSPI", "eDMA".

#include <cstdint>

namespace o3sdk::platform::mk66::display::soc {

// ---- SIM clock gates ------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kScgc3Spi2 = 1u << 12;    // SIM_SCGC3[SPI2]
inline constexpr std::uint32_t kScgc6Dmamux = 1u << 1;   // SIM_SCGC6[DMAMUX]
inline constexpr std::uint32_t kScgc7Dma = 1u << 1;      // SIM_SCGC7[DMA]: bit 1 on the K66 (bit 8 is the Kinetis-L position)

// ---- DSPI module configuration and status ----------------------------------------------------------------------------
inline constexpr std::uint32_t kMcrMstr = 1u << 31;
inline constexpr std::uint32_t kMcrMdis = 1u << 14;
inline constexpr std::uint32_t kMcrClrTxf = 1u << 11;
inline constexpr std::uint32_t kMcrClrRxf = 1u << 10;
inline constexpr std::uint32_t kMcrHalt = 1u << 0;
inline constexpr std::uint32_t kMcrPcsisAll = 0x3Fu << 16;  // every peripheral chip select idles high (none is used: CS is a GPIO)
inline constexpr std::uint32_t kSrTcf = 1u << 31;
inline constexpr std::uint32_t kSrTxrxs = 1u << 30;
inline constexpr std::uint32_t kSrEoqf = 1u << 28;
inline constexpr std::uint32_t kSrTfuf = 1u << 27;
inline constexpr std::uint32_t kSrTfff = 1u << 25;
inline constexpr std::uint32_t kSrRfof = 1u << 19;
inline constexpr std::uint32_t kSrRfdf = 1u << 17;
inline constexpr std::uint32_t kSrRxctrMask = 0xFu << 4;  // entries in the receive FIFO

// The status flags a flush clears. **Not TFFF**: it is the transmit FIFO's fill request, the very thing the DMA waits for.
// Clearing it on a FIFO that is empty and then enabling the DMA request could leave the first request unasserted and the
// transfer never starting (the driver would time out and latch the polled path for good); the hardware keeps it set while the
// FIFO has room and the DMA acknowledge clears it.
inline constexpr std::uint32_t kSrFlushClear = kSrTcf | kSrEoqf | kSrTfuf | kSrRfof | kSrRfdf;

// ---- eDMA ---------------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kEsVld = 1u << 31;   // DMA_ES[VLD]: an error is flagged (any channel)
inline constexpr std::uint16_t kCsrDone = 1u << 7;  // TCD CSR[DONE]

static_assert((kSrFlushClear & kSrTfff) == 0, "a flush never clears the transmit FIFO fill request");

}  // namespace o3sdk::platform::mk66::display::soc
