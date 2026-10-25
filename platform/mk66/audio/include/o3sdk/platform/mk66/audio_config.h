// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The register values of the audio path, with their arithmetic, so the compiler and the host tests check them and the boot
// code only writes them (the same discipline as clock_config.h): I2S0 (the SAI), its eDMA channels' transfer control
// descriptors (TCDs), the DMAMUX request sources, and the I2C0 divider for the codec's control bus.
//
// Sources. Register addresses, field positions and the settings are from the K66 Sub-Family Reference Manual chapters "I2S/SAI",
// "eDMA", "DMAMUX" and "I2C"; the *values* are the standard I2S master setup of the K66's SAI with the SGTL5000 as the slave
// (the one PJRC's Teensy Audio library, MIT, also uses) and this board's wiring; each value is described where it is written.
//
// The format: the K66 is the I2S master. Frame = two 32-bit words (left then right), MSB first, the frame
// sync one bit early and low for the left word (I2S), data changing on BCLK's falling edge. BCLK = MCLK / 4 = 64 Fs and
// LRCLK = BCLK / 64 = MCLK / 256: MCLK = 256 Fs. A 16-bit sample is the upper half of its 32-bit word: the DMA moves 16-bit
// halves to and from the word's upper two bytes (data register + 2). The codec is set to 16-bit slave I2S and reads the first
// 16 bits of each slot.

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/edma.h>
#include <o3sdk/platform/mk66/i2c_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::audio_cfg {

// What `hal::audio::kCpuHz` (core/audio_clock.h, a plain number in `core`) means here: the core clock of this image's clock tree.
// "Cycles per sample", the I2S divider, the block length in cycles and the DWT figures are all relative to it, so a change of the
// tree that is not made in the rate constant too is a compile error, not a silently wrong sample rate.
static_assert(clock::kCoreHz == hal::audio::kCpuHz, "the audio clock constant is relative to the 180 MHz core clock of clock_config.h");

// ---- I2S0 (SAI0): K66 RM "I2S" register map, base 0x4002_F000 --------------------------------------------------------------
namespace i2s {
inline constexpr std::uint32_t kBase = 0x4002F000u;
inline constexpr std::uint32_t kTcsr = kBase + 0x00, kTcr1 = kBase + 0x04, kTcr2 = kBase + 0x08, kTcr3 = kBase + 0x0C,
                               kTcr4 = kBase + 0x10, kTcr5 = kBase + 0x14, kTdr0 = kBase + 0x20, kTmr = kBase + 0x60;
inline constexpr std::uint32_t kRcsr = kBase + 0x80, kRcr1 = kBase + 0x84, kRcr2 = kBase + 0x88, kRcr3 = kBase + 0x8C,
                               kRcr4 = kBase + 0x90, kRcr5 = kBase + 0x94, kRdr0 = kBase + 0xA0, kRmr = kBase + 0xE0;
inline constexpr std::uint32_t kMcr = kBase + 0x100, kMdrAddress = kBase + 0x104;

// TCSR / RCSR bits
inline constexpr std::uint32_t kCsrEnable = 1u << 31;        // TE / RE
inline constexpr std::uint32_t kCsrBitClockEnable = 1u << 28;  // BCE
inline constexpr std::uint32_t kCsrFifoReset = 1u << 25;     // FR
inline constexpr std::uint32_t kCsrSoftReset = 1u << 24;     // SR
inline constexpr std::uint32_t kCsrSyncErrorFlag = 1u << 19;   // SEF, write 1 to clear
inline constexpr std::uint32_t kCsrFifoErrorFlag = 1u << 18;   // FEF (underrun on TX, overrun on RX), write 1 to clear
inline constexpr std::uint32_t kCsrWordStartFlag = 1u << 20;   // WSF, write 1 to clear
inline constexpr std::uint32_t kCsrFifoRequestDmaEnable = 1u << 0;  // FRDE
inline constexpr std::uint32_t kCsrErrorFlags = kCsrSyncErrorFlag | kCsrFifoErrorFlag;

// MCLK: MCR[MOE] drives the MCLK pin, MCR[MICS] = 0 selects the system clock (MCLK_SRC 0 at F_CPU = 180 MHz,
// as in PJRC's Audio library), MDR divides it by DIVIDE / FRACT. The register stores both minus one.
inline constexpr std::uint32_t kMcrMoe = 1u << 30;
inline constexpr std::uint32_t kMcrDivideUpdateFlag = 1u << 31;  // DUF: a new MDR is still being applied
inline constexpr std::uint32_t kMcrValue = kMcrMoe;              // MICS = 0: the core / system clock
constexpr std::uint32_t mdr_value(hal::audio::MclkRatio r) noexcept {
  return ((r.fract - 1u) << 12) | (r.divide - 1u);
}
inline constexpr std::uint32_t kMdr = mdr_value(hal::audio::kMclkRatio);
static_assert(hal::audio::kMclkRatio.fract <= 256u && hal::audio::kMclkRatio.divide <= 4096u, "FRACT is 8 bits and DIVIDE 12 bits, each stored minus one");
static_assert(kMdr >> 20 == 0u, "MDR[31:20] are reserved");

// The 16 bits of a sample are the upper half of the 32-bit word: the data register's address + 2 (little endian).
inline constexpr std::uint32_t kTxDataHalf = kTdr0 + 2u;
inline constexpr std::uint32_t kRxDataHalf = kRdr0 + 2u;

// Transmit configuration (PJRC's Audio library, config_i2s, uses the same values):
//   TCR1  TFW = 1 (kTxWatermark): the FIFO requests data at a watermark of one word.
//   TCR2  SYNC = 0 asynchronous (it generates the clocks), MSEL = 1 (the MCLK divider output), BCP = 1 (data changes on the
//         falling edge, so I2S timing holds), BCD = 1 (BCLK is an output: master), DIV = 1: BCLK = MCLK / ((1 + 1) x 2) = MCLK / 4.
//   TCR3  TCE: transmit data channel 0 enabled.
//   TCR4  FRSZ = 1 (two words per frame), SYWD = 31 (the sync is 32 BCLKs wide: one word), MF (MSB first), FSE (frame sync
//         one bit early, I2S), FSP (frame sync active low: low = left), FSD (frame sync is an output).
//   TCR5  WNW = W0W = 31 (32-bit words), FBT = 31 (the first bit shifted is bit 31).
inline constexpr std::uint32_t kBclkDiv = 1;
inline constexpr std::uint32_t kWordBits = 32;
inline constexpr std::uint32_t kFrameWords = 2;
// FIFO WATERMARKS AND THE MARGIN. The transmit FIFO raises its DMA request when it holds at most TFW words (the receive FIFO, at more
// than RFW). With TFW = 1 a request is raised when one word is left, so the DMA has about 2 word times, 11-22 us (a word is 32 BCLKs
// = 11.3 us), to deliver the next sample before the FIFO runs dry and the transmitter underruns. Against that the DMA's own latency
// is the crossbar arbitration plus one minor loop of any other channel (2-4 bytes, under a microsecond, resources.h): a margin of
// at least ten. TFW = 4 would raise it to about 4-5 words (45-57 us) at the price of that much more output latency (still 0.05 ms)
// and of leaving the value proven on this board, and the FIFO depth of this chip's I2S is not stated anywhere in the repository:
// so the choice is TFW = 1 and RFW = 1, and the real margin is a HW-A measurement
// (track3r-os platform/mk66 README, HW-A row A-8). Changing either is this one constant each, and the TCR1 / RCR1 test.
inline constexpr std::uint32_t kTxWatermark = 1;
inline constexpr std::uint32_t kRxWatermark = 1;
inline constexpr std::uint32_t kTcr1Value = kTxWatermark;
inline constexpr std::uint32_t kTcr2Value = (0u << 30) | (1u << 26) | (1u << 25) | (1u << 24) | kBclkDiv;
inline constexpr std::uint32_t kTcr3Value = 1u << 16;
inline constexpr std::uint32_t kTcr4Value = (1u << 16) | (31u << 8) | 0x10u | 0x08u | 0x02u | 0x01u;
inline constexpr std::uint32_t kTcr5Value = (31u << 24) | (31u << 16) | (31u << 8);
// Receive configuration: the same format, synchronous to the transmitter (RCR2 SYNC = 1: it uses the transmitter's BCLK and
// frame sync).
inline constexpr std::uint32_t kRcr1Value = kRxWatermark;
inline constexpr std::uint32_t kRcr2Value = (1u << 30) | (1u << 26) | (1u << 25) | (1u << 24) | kBclkDiv;
inline constexpr std::uint32_t kRcr3Value = 1u << 16;
inline constexpr std::uint32_t kRcr4Value = kTcr4Value;
inline constexpr std::uint32_t kRcr5Value = kTcr5Value;

// MCLK / (BCLK divide) / (bits per frame) = LRCLK: MCLK must be 256 LRCLKs, the codec's MCLK_FREQ = 256 Fs.
inline constexpr std::uint32_t kBclkDivide = (kBclkDiv + 1u) * 2u;
static_assert(kBclkDivide * kFrameWords * kWordBits == hal::audio::kMclkPerSample, "BCLK = MCLK / 4 and 64 BCLKs per frame make LRCLK = MCLK / 256");
static_assert(((kTcr4Value >> 16) & 0x1Fu) + 1u == kFrameWords && ((kTcr5Value >> 24) & 0x1Fu) + 1u == kWordBits, "TCR4/TCR5 say two 32-bit words per frame");
static_assert(((kTcr4Value >> 8) & 0x1Fu) + 1u == kWordBits, "the frame sync is one word wide");
}  // namespace i2s

// ---- eDMA and DMAMUX: K66 RM "eDMA" (base 0x4000_8000, TCDs at 0x4000_9000 + 32 x channel) and "DMAMUX" (0x4002_1000) -----------
namespace dma {
// The controller's shared facts (the block addresses, the TCD layout, DMA_CR with GRP1PRI | EMLM | EDBG) are the base library's edma.h,
// which the boot (boot::dma_init(), the one writer of DMA_CR and every DCHPRI) and the display share: named here so that the audio
// descriptors below read as they did in track3r-os.
using edma::kBase;
using edma::kChannels;
using edma::kCrHalt;
using edma::kCrValue;
using edma::kMuxBase;
using edma::kTcdBase;

// DMAMUX request sources of the K66: I2S0 receive 12,
// I2S0 transmit 13. CHCFGn = ENBL | source, written after the channel's TCD is complete.
inline constexpr std::uint8_t kSourceI2s0Rx = 12;
inline constexpr std::uint8_t kSourceI2s0Tx = 13;
inline constexpr std::uint8_t kMuxEnable = 0x80;

// The TCD, in register order: one 32-byte descriptor per channel (edma.h).
using Tcd = edma::Tcd;
static_assert(sizeof(Tcd) == 32, "a TCD is 32 bytes");

inline constexpr std::uint16_t kSize16 = 1;       // ATTR SSIZE / DSIZE: 16 bits
inline constexpr std::uint16_t kCsrIntMajor = 0x0002;  // CSR[INTMAJOR]: interrupt at the end of each major loop

constexpr unsigned log2_pow2(std::uint32_t v) noexcept {
  unsigned n = 0;
  while ((1u << n) < v) {
    ++n;
  }
  return n;
}

// TX channel: a circular read of the TX ring into the I2S data register, one 16-bit sample per FIFO request. The major loop is
// ONE slot (one block), so the channel interrupts once per slot; SOFF = 2 walks the ring and ATTR[SMOD] = log2(ring size) makes
// the source address wrap inside the ring (the lower SMOD address bits may change, the upper ones are frozen: the ring must sit
// on a multiple of its own size), SLAST = 0 so nothing else moves the address. The DMA can only read inside
// [ring_base, ring_base + ring_bytes). No scatter/gather, no DREQ: the channel runs for ever.
constexpr Tcd tx_tcd(std::uint32_t ring_base, std::uint32_t ring_bytes) noexcept {
  Tcd t{};
  t.saddr = ring_base;
  t.soff = 2;
  t.attr = static_cast<std::uint16_t>((log2_pow2(ring_bytes) << 11) | (kSize16 << 8) | kSize16);
  t.nbytes = 2;
  t.slast = 0;
  t.daddr = i2s::kTxDataHalf;
  t.doff = 0;
  t.citer = static_cast<std::uint16_t>(hal::audio::kBlockSamples);
  t.biter = t.citer;
  t.dlastsga = 0;
  t.csr = kCsrIntMajor;
  return t;
}

// RX channel: the same, mirrored: the data register into the RX ring, DOFF = 2, ATTR[DMOD] = log2(ring size), so it can only
// WRITE inside [ring_base, ring_base + ring_bytes). No interrupt of its own: the TX channel's boundary reads its position.
constexpr Tcd rx_tcd(std::uint32_t ring_base, std::uint32_t ring_bytes) noexcept {
  Tcd t{};
  t.saddr = i2s::kRxDataHalf;
  t.soff = 0;
  t.attr = static_cast<std::uint16_t>((kSize16 << 8) | (log2_pow2(ring_bytes) << 3) | kSize16);
  t.nbytes = 2;
  t.slast = 0;
  t.daddr = ring_base;
  t.doff = 2;
  t.citer = static_cast<std::uint16_t>(hal::audio::kBlockSamples);
  t.biter = t.citer;
  t.dlastsga = 0;
  t.csr = 0;
  return t;
}
}  // namespace dma

// ---- I2C0 for the codec: K66 RM "I2C", base 0x4006_6000, bus clock 90 MHz ----------------------------------------------------
// The bus is the I2C master's (src/i2c.cpp), shared with the input drivers, so its facts are the base library's i2c_config.h, one copy:
// F = 0x1F (MULT 0, ICR 0x1F), an SCL divider of 240: 90 MHz / 240 = 375 kHz, below the SGTL5000's 400 kHz limit (data sheet "I2C
// Serial Clock Frequency" max 400 kHz). Named here for the codec's checks.
namespace i2c {
inline constexpr std::uint32_t kBase = static_cast<std::uint32_t>(i2c_cfg::kI2c0Base);
using i2c_cfg::kFrequencyDivider;
using i2c_cfg::kSclDivider;
using i2c_cfg::kSclHz;
static_assert(kBase == 0x40066000u, "I2C0");
static_assert(kSclHz <= 400000u, "the SGTL5000 accepts at most 400 kHz");
static_assert(clock::kBusHz % kSclDivider == 0u, "an exact SCL frequency");
}  // namespace i2c

}  // namespace o3sdk::platform::mk66::audio_cfg
