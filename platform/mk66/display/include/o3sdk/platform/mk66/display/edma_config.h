// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The eDMA side of the FT812 bus as values: which channels, which request sources, and the transfer descriptors (TCDs)
// for one buffer going out through DSPI2. Pure, so the host tests check every field; src/device.cpp only writes them.
// Chip facts: K66 Sub-Family Reference Manual, "Enhanced direct memory access (eDMA)" (TCD layout, ATTR and CSR fields),
// "DMA Multiplexer (DMAMUX)" and the DMAMUX source table (DSPI2 receive = 38, transmit = 39; the Teensy core lists the
// same, a fact table).
//
// Ownership (the channel-ownership table of ADR 0013 D1 / D9 is platform/mk66 resources.h; mk66_resources_test.cpp checks that its
// display rows and these constants agree):
//   * eDMA channels 14 (SPI2 transmit) and 15 (SPI2 receive), DMAMUX sources 39 and 38. Interrupt: DMA15_DMA31, from the
//     receive channel's major loop (the last received frame is the last frame shifted out), at the Completion priority
//     (80, hal/irq.h). The transmit channel raises no interrupt.
//   * Arbitration priority: below the audio engine's (channels 12/13 highest; resources.h gives these two 13 and 12 of 0..15). The
//     eDMA's global configuration, DMA_CR and every DCHPRI, belongs to one boot function, `boot::dma_init()`, written from that
//     table; this driver writes neither (tools/check_dma_owner.py, ctest mk66_dma_owner, fails the build if it ever does).
//   * Minor loops: every descriptor moves at most kMaxMinorLoopBytes = 4 bytes a request (TX 1, RX 4). The audio engine's FIFO
//     margin is computed on that: a display request can hold the eDMA for at most one 4-byte beat before an audio request is
//     arbitrated in. Do not raise it (larger NBYTES, minor-loop mapping, bursts) without redoing that arithmetic.
//
// Shape of a transfer: DSPI2 in master mode, 8-bit frames (CTAR0), chip select a GPIO held low by software. TX: one eDMA
// request per byte (DSPI TX FIFO fill request), SSIZE = DSIZE = 8 bit, source increment 1, destination the low byte of
// SPI2_PUSHR; the command half of PUSHR stays as the last full 32-bit write left it (all zero: CTAS 0, no CONT, no EOQ, PCS 0).
// RX: one request per received frame, 32-bit read of SPI2_POPR into a one-word sink (no address increment), count equal to the
// TX count; its major-loop interrupt is the completion event. Whether the byte-wide PUSHR write behaves as described is the
// one thing here only the hardware can show; the display's DMA self-test (display.h) proves it before the path is trusted and
// the polled path is the fallback.

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::display::edma {

inline constexpr unsigned kTxChannel = 14;
inline constexpr unsigned kRxChannel = 15;
inline constexpr unsigned kTxSource = 39;  // DMAMUX: SPI2 transmit
inline constexpr unsigned kRxSource = 38;  // DMAMUX: SPI2 receive
inline constexpr unsigned kChannelCount = 16;
static_assert(kTxChannel < kChannelCount && kRxChannel < kChannelCount && kTxChannel != kRxChannel, "two distinct eDMA channels");

// Peripheral register addresses the descriptors point at (K66 RM memory map: SPI2 at 0x400A_C000, PUSHR +0x34, POPR +0x38).
inline constexpr std::uint32_t kSpi2Base = 0x400AC000u;
inline constexpr std::uint32_t kPushrAddr = kSpi2Base + 0x34u;  // byte 0 of the little-endian register = TXDATA[7:0]
inline constexpr std::uint32_t kPoprAddr = kSpi2Base + 0x38u;

// The most one descriptor moves: CITER / BITER are 15 bits without channel linking.
inline constexpr std::uint32_t kMaxCount = 0x7FFFu;

// TCD field encodings.
inline constexpr std::uint16_t kSize8 = 0;   // ATTR SSIZE / DSIZE: 0 = 8 bit, 1 = 16 bit, 2 = 32 bit
inline constexpr std::uint16_t kSize32 = 2;
constexpr std::uint16_t attr(std::uint16_t source_size, std::uint16_t dest_size) noexcept {
  // SMOD [15:11] = 0, SSIZE [10:8], DMOD [7:3] = 0, DSIZE [2:0]
  return static_cast<std::uint16_t>((source_size << 8) | dest_size);
}
inline constexpr std::uint16_t kCsrIntMajor = 1u << 1;  // interrupt when the major loop completes
inline constexpr std::uint16_t kCsrDreq = 1u << 3;      // clear this channel's request enable when the major loop completes

// The audio FIFO margin depends on this (see the header comment): no descriptor of this driver has a minor loop above it.
inline constexpr std::uint32_t kMaxMinorLoopBytes = 4;

struct Tcd {
  std::uint32_t saddr;
  std::int16_t soff;
  std::uint16_t attr;
  std::uint32_t nbytes;   // minor loop byte count (minor-loop mapping off)
  std::int32_t slast;     // source address adjustment at major loop end
  std::uint32_t daddr;
  std::int16_t doff;
  std::uint16_t citer;    // current major iteration count
  std::int32_t dlastsga;  // destination adjustment at major loop end (scatter-gather off)
  std::uint16_t csr;
  std::uint16_t biter;    // starting major iteration count (equal to citer)
  bool valid;             // false: the request was out of range, nothing may be written to a channel
};

// TX: `count` bytes from `src` into the low byte of PUSHR, one byte per request.
constexpr Tcd tx_tcd(std::uint32_t src, std::uint32_t count) noexcept {
  const bool ok = src != 0 && count != 0 && count <= kMaxCount;
  return Tcd{src,        1,   attr(kSize8, kSize8), 1, 0, kPushrAddr, 0, static_cast<std::uint16_t>(count),
             0,          kCsrDreq, static_cast<std::uint16_t>(count), ok};
}

// RX: `count` frames, each read as a word from POPR into the one-word sink at `sink` (no increment).
constexpr Tcd rx_tcd(std::uint32_t sink, std::uint32_t count) noexcept {
  const bool ok = sink != 0 && sink % 4u == 0u && count != 0 && count <= kMaxCount;
  return Tcd{kPoprAddr,  0,   attr(kSize32, kSize32), 4, 0, sink, 0, static_cast<std::uint16_t>(count),
             0,          static_cast<std::uint16_t>(kCsrDreq | kCsrIntMajor), static_cast<std::uint16_t>(count), ok};
}

// A transfer of `len` bytes out of a buffer with `capacity` bytes left from its source address: non-empty and inside it.
constexpr bool within_buffer(std::uint32_t len, std::uint32_t capacity) noexcept { return len != 0 && len <= capacity; }

// DMAMUX channel configuration byte: ENBL [7], TRIG [6] (periodic trigger: never), SOURCE [5:0].
constexpr std::uint8_t mux_config(unsigned source, bool enable) noexcept {
  return static_cast<std::uint8_t>((enable ? 0x80u : 0u) | (source & 0x3Fu));
}

// DSPI2's request enables (SPI2_RSER) while a transfer runs: TFFF_RE [25] with TFFF_DIRS [24] (DMA, not interrupt) for the
// transmit FIFO, RFDF_RE [17] with RFDF_DIRS [16] for the receive FIFO. All other bits zero: no interrupt of the DSPI itself.
inline constexpr std::uint32_t kRserDma = (1u << 25) | (1u << 24) | (1u << 17) | (1u << 16);

static_assert(attr(kSize8, kSize8) == 0x0000 && attr(kSize32, kSize32) == 0x0202, "ATTR: SSIZE in [10:8], DSIZE in [2:0]");
static_assert(kPushrAddr == 0x400AC034u && kPoprAddr == 0x400AC038u, "DSPI2 PUSHR and POPR");
static_assert(tx_tcd(0x20000100u, 100).citer == 100 && tx_tcd(0x20000100u, 100).biter == 100, "one request per byte");
static_assert(!tx_tcd(0x20000100u, 0).valid && !tx_tcd(0x20000100u, kMaxCount + 1).valid && !tx_tcd(0, 4).valid,
              "an empty, oversized or null transfer is not a descriptor");
static_assert(!rx_tcd(0x20000102u, 4).valid, "the sink is a word");

}  // namespace o3sdk::platform::mk66::display::edma
