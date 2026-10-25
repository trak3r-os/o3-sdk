// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio path's register values (audio_config.h, OS-005a): I2S0 as audio_config.h sets it, the MDR divider for both clocks, the eDMA
// descriptors and, with a behavioural model of the eDMA's address arithmetic, the proof that the DMA can only read and write inside
// its rings; the pins, against the SDRAM and the quiet-pin tables. Nothing here touches a
// register: what only hardware can show is the HW-A checklist (README.md).
#include <doctest/doctest.h>

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio_config.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/sdramc_config.h>

#include <cstdint>
#include <initializer_list>

namespace cfg = o3sdk::platform::mk66::audio_cfg;
namespace haudio = o3sdk::hal::audio;
namespace mk66 = o3sdk::platform::mk66;

namespace {

// The part of the K66 eDMA engine the descriptors rely on, from the RM's description of a channel: each minor loop moves NBYTES from
// SADDR to DADDR, then SADDR += SOFF and DADDR += DOFF with the low SMOD / DMOD address bits the only ones allowed to change (a
// circular queue), CITER counts down, and at zero (the major loop's end) SLAST and DLASTSGA are added and CITER reloads from BITER;
// an interrupt is requested then if CSR[INTMAJOR] is set.
struct Edma {
  cfg::dma::Tcd t;
  std::uint32_t saddr;
  std::uint32_t daddr;
  std::uint16_t citer;
  std::uint32_t major_loops = 0;
  std::uint32_t interrupts = 0;

  explicit Edma(const cfg::dma::Tcd& tcd) : t(tcd), saddr(tcd.saddr), daddr(tcd.daddr), citer(tcd.citer) {}

  static std::uint32_t wrap_add(std::uint32_t a, std::int32_t off, unsigned modulo_bits) {
    if (modulo_bits == 0) {
      return a + static_cast<std::uint32_t>(off);
    }
    const std::uint32_t mask = (1u << modulo_bits) - 1u;
    return (a & ~mask) | ((a + static_cast<std::uint32_t>(off)) & mask);
  }

  // One minor loop: returns the source and destination of the transfer.
  void step(std::uint32_t& src, std::uint32_t& dst) {
    src = saddr;
    dst = daddr;
    const unsigned smod = (t.attr >> 11) & 0x1Fu;
    const unsigned dmod = (t.attr >> 3) & 0x1Fu;
    saddr = wrap_add(saddr, t.soff, smod);
    daddr = wrap_add(daddr, t.doff, dmod);
    if (--citer == 0) {
      ++major_loops;
      if ((t.csr & cfg::dma::kCsrIntMajor) != 0u) {
        ++interrupts;
      }
      saddr += static_cast<std::uint32_t>(t.slast);
      daddr += static_cast<std::uint32_t>(t.dlastsga);
      citer = t.biter;
    }
  }
};

}  // namespace

TEST_CASE("mk66 i2s: the MDR divider for today's clock and the 44,100 Hz retune") {
  // MDR = (FRACT - 1) << 12 | (DIVIDE - 1).
  CHECK(cfg::i2s::mdr_value(haudio::mclk_ratio({4080u, 1u})) == 0x0000F0FEu);       // 16 / 255: FRACT 15, DIVIDE 254
  CHECK(cfg::i2s::mdr_value(haudio::mclk_ratio({1800000u, 441u})) == 0x000C3C34u);  // 196 / 3125: FRACT 195, DIVIDE 3124
  CHECK(cfg::i2s::kMdr == cfg::i2s::mdr_value(haudio::kMclkRatio));
  CHECK((cfg::i2s::kMdr >> 20) == 0u);  // the reserved bits stay clear
  // MCLK = 180 MHz x FRACT / DIVIDE, and the frame clock is MCLK / 256 whichever constant is built.
  CHECK(haudio::mclk_hz(haudio::mclk_ratio({4080u, 1u})) == 11294117u);
  CHECK(haudio::mclk_hz(haudio::mclk_ratio({1800000u, 441u})) == 11289600u);
  CHECK(cfg::i2s::kBclkDivide == 4u);
  CHECK(cfg::i2s::kBclkDivide * cfg::i2s::kFrameWords * cfg::i2s::kWordBits == 256u);
}

TEST_CASE("mk66 i2s: the register values, field by field") {
  // The standard SAI master configuration, macros expanded by hand.
  CHECK(cfg::i2s::kMcrValue == 0x40000000u);                    // MOE, MICS = 0 (system clock)
  CHECK(cfg::i2s::kTxWatermark == 1u);                          // TFW(1) and RFW(1): the choice is stated in audio_config.h
  CHECK(cfg::i2s::kRxWatermark == 1u);
  CHECK(cfg::i2s::kTcr1Value == 0x00000001u);                   // TFW(1)
  CHECK(cfg::i2s::kTcr2Value == 0x07000001u);                   // SYNC(0) | MSEL(1) | BCP | BCD | DIV(1)
  CHECK(cfg::i2s::kTcr3Value == 0x00010000u);                   // TCE
  CHECK(cfg::i2s::kTcr4Value == 0x00011F1Bu);                   // FRSZ(1) | SYWD(31) | MF | FSE | FSP | FSD
  CHECK(cfg::i2s::kTcr5Value == 0x1F1F1F00u);                   // WNW(31) | W0W(31) | FBT(31)
  CHECK(cfg::i2s::kRcr1Value == 0x00000001u);                   // RFW(1)
  CHECK(cfg::i2s::kRcr2Value == 0x47000001u);                   // SYNC(1) | MSEL(1) | BCP | BCD | DIV(1)
  CHECK(cfg::i2s::kRcr3Value == 0x00010000u);                   // RCE
  CHECK(cfg::i2s::kRcr4Value == cfg::i2s::kTcr4Value);
  CHECK(cfg::i2s::kRcr5Value == cfg::i2s::kTcr5Value);
  // The register map (K66 RM): a few addresses that the whole driver hangs on.
  CHECK(cfg::i2s::kTcsr == 0x4002F000u);
  CHECK(cfg::i2s::kTdr0 == 0x4002F020u);
  CHECK(cfg::i2s::kRdr0 == 0x4002F0A0u);
  CHECK(cfg::i2s::kMcr == 0x4002F100u);
  CHECK(cfg::i2s::kMdrAddress == 0x4002F104u);
  CHECK(cfg::i2s::kTxDataHalf == 0x4002F022u);   // the upper halfword of the 32-bit word
  CHECK(cfg::i2s::kRxDataHalf == 0x4002F0A2u);
}

TEST_CASE("mk66 dma: the descriptors, field by field") {
  const cfg::dma::Tcd tx = cfg::dma::tx_tcd(0x20000800u, haudio::kTxRingBytes);
  CHECK(tx.saddr == 0x20000800u);
  CHECK(tx.soff == 2);
  // K66 eDMA ATTR: SMOD bits 15:11, SSIZE 10:8, DMOD 7:3, DSIZE 2:0. SMOD = n makes the low n bits of the source address wrap (a
  // modulo of 2^n bytes) and freezes the upper bits, so the ring must be 2^n bytes and start on a multiple of 2^n. The TX ring is
  // kTxPhysSlots = 4 slots of 256 B = 1024 B = 2^10: SMOD 10 -> 10 << 11 = 0x5000, | SSIZE 1 << 8 | DSIZE 1 = 0x5101. (The 2-slot
  // ring was 512 B, SMOD 9, 0x4901.) The ring's alignas(1024) (audio.cpp) and the 1 KiB aligned `.dma_rings` section (mk66.ld.in,
  // ASSERT on __dma_rings_start) meet that modulo.
  static_assert(haudio::kTxRingBytes == 1024u, "SMOD below is log2 of the 1 KiB ring of the three-slot default");
  CHECK(tx.attr == 0x5101u);              // SMOD 10 (1024 bytes), SSIZE 1, DSIZE 1
  CHECK((tx.attr >> 11) == cfg::dma::log2_pow2(haudio::kTxRingBytes));
  CHECK(tx.nbytes == 2u);
  CHECK(tx.slast == 0);
  CHECK(tx.daddr == 0x4002F022u);
  CHECK(tx.doff == 0);
  CHECK(tx.citer == 128u);                // one slot per major loop
  CHECK(tx.biter == 128u);
  CHECK(tx.dlastsga == 0);
  CHECK(tx.csr == 0x0002u);               // INTMAJOR only: no ESG, no DREQ, the channel never stops
  const cfg::dma::Tcd rx = cfg::dma::rx_tcd(0x20000400u, haudio::kRxRingBytes);
  CHECK(rx.saddr == 0x4002F0A2u);
  CHECK(rx.soff == 0);
  CHECK(rx.attr == 0x0151u);              // SSIZE 1, DMOD 10 (1024 bytes), DSIZE 1
  CHECK(rx.nbytes == 2u);
  CHECK(rx.slast == 0);
  CHECK(rx.daddr == 0x20000400u);
  CHECK(rx.doff == 2);
  CHECK(rx.citer == 128u);
  CHECK(rx.biter == 128u);
  CHECK(rx.dlastsga == 0);
  CHECK(rx.csr == 0x0000u);
  CHECK(cfg::dma::kSourceI2s0Rx == 12);
  CHECK(cfg::dma::kSourceI2s0Tx == 13);
  CHECK(cfg::dma::kCrValue == 0x482u);  // GRP1PRI | EMLM | EDBG (OS-HW1: without GRP1PRI the eDMA halts on ES[GPE])
  CHECK(cfg::dma::kCrHalt == 0x20u);
  CHECK(cfg::dma::kBase == 0x40008000u);
  CHECK(cfg::dma::kTcdBase == 0x40009000u);
  CHECK(cfg::dma::kMuxBase == 0x40021000u);
  CHECK(cfg::dma::log2_pow2(512) == 9u);
  CHECK(cfg::dma::log2_pow2(1024) == 10u);
}

TEST_CASE("mk66 dma: the TX channel only reads inside its ring, one slot per interrupt, for ever") {
  for (const std::uint32_t slots : {2u, 4u}) {
    const std::uint32_t bytes = slots * haudio::kBlockBytes;
    for (const std::uint32_t base : {0x20000800u, 0x20001000u, 0x1FFF0000u + 0x4000u, 0x20028000u}) {
      if (base % bytes != 0u) {
        continue;
      }
      INFO("slots " << slots << " base " << base);
      Edma dma(cfg::dma::tx_tcd(base, bytes));
      std::uint32_t outside = 0;
      std::uint32_t misaligned = 0;
      std::uint32_t wrong_destination = 0;
      std::uint32_t wrong_slot_at_interrupt = 0;
      // Five trips round the ring.
      const std::uint32_t transfers = 5u * slots * haudio::kBlockSamples;
      for (std::uint32_t i = 0; i < transfers; ++i) {
        std::uint32_t src;
        std::uint32_t dst;
        dma.step(src, dst);
        outside += (src < base || src + 2u > base + bytes) ? 1u : 0u;
        misaligned += (src & 1u) != 0u ? 1u : 0u;
        wrong_destination += dst != cfg::i2s::kTxDataHalf ? 1u : 0u;
        // The slot being read is (i / 128) mod slots, in order.
        if (((src - base) / haudio::kBlockBytes) != ((i / haudio::kBlockSamples) % slots)) {
          ++wrong_slot_at_interrupt;
        }
      }
      CHECK(outside == 0u);
      CHECK(misaligned == 0u);
      CHECK(wrong_destination == 0u);
      CHECK(wrong_slot_at_interrupt == 0u);
      CHECK(dma.interrupts == 5u * slots);   // one interrupt per slot
      CHECK(dma.major_loops == 5u * slots);
      // After the interrupt that ends slot k the DMA reads slot k + 1: the ISR's slot index from SADDR is the NEXT slot.
      CHECK((dma.saddr - base) / haudio::kBlockBytes == 0u);  // five whole trips: back at slot 0
    }
  }
}

TEST_CASE("mk66 dma: the RX channel only writes inside its ring, and the ISR's slot index tells the slot being written") {
  const std::uint32_t bytes = haudio::kRxRingBytes;
  const std::uint32_t base = 0x20000400u;
  Edma dma(cfg::dma::rx_tcd(base, bytes));
  std::uint32_t outside = 0;
  std::uint32_t wrong_source = 0;
  const std::uint32_t transfers = 7u * haudio::kRxPhysSlots * haudio::kBlockSamples;
  for (std::uint32_t i = 0; i < transfers; ++i) {
    std::uint32_t src;
    std::uint32_t dst;
    dma.step(src, dst);
    outside += (dst < base || dst + 2u > base + bytes || (dst & 1u) != 0u) ? 1u : 0u;
    wrong_source += src != cfg::i2s::kRxDataHalf ? 1u : 0u;
    // Between two of the TX channel's interrupts the DMA is writing slot ((i + 1) / 128) mod 4: the formula the ISR applies to
    // DADDR (slot = (DADDR - base) >> 8) names the slot being written, so the newest COMPLETE slot is the one behind it.
    if ((i + 1u) % haudio::kBlockSamples == 0u) {
      const std::uint32_t writing = (dma.daddr - base) >> 8;
      CHECK(writing == ((i + 1u) / haudio::kBlockSamples) % haudio::kRxPhysSlots);
    }
  }
  CHECK(outside == 0u);
  CHECK(wrong_source == 0u);
  CHECK(dma.interrupts == 0u);  // no interrupt of its own
  CHECK(dma.major_loops == 7u * haudio::kRxPhysSlots);
}

TEST_CASE("mk66 dma: a ring that is not aligned to its own size would escape, which is why alignment is declared") {
  // The modulo freezes the UPPER address bits: a 512-byte ring at 0x20000900 wraps inside [0x20000800, 0x20000A00), not inside
  // itself. The model must see that, or the checks above prove nothing.
  const std::uint32_t base = 0x20000900u;
  Edma dma(cfg::dma::tx_tcd(base, haudio::kTxRingBytes));
  std::uint32_t outside = 0;
  for (std::uint32_t i = 0; i < 4u * haudio::kBlockSamples; ++i) {
    std::uint32_t src;
    std::uint32_t dst;
    dma.step(src, dst);
    outside += (src < base || src + 2u > base + haudio::kTxRingBytes) ? 1u : 0u;
  }
  CHECK(outside > 0u);
}

TEST_CASE("mk66 clock: the audio rate constant is relative to this image's 180 MHz core clock") {
  CHECK(mk66::clock::kCoreHz == haudio::kCpuHz);
  CHECK(mk66::clock::kCoreHz == 180000000u);
  CHECK(mk66::clock::kBusHz == 90000000u);
}

TEST_CASE("mk66 i2c: 375 kHz from the 90 MHz bus clock, under the codec's 400 kHz") {
  CHECK(cfg::i2c::kFrequencyDivider == 0x1F);
  CHECK(cfg::i2c::kSclDivider == 240u);
  CHECK(cfg::i2c::kSclHz == 375000u);
  CHECK(cfg::i2c::kBase == 0x40066000u);
}

TEST_CASE("mk66 board: the audio pins are distinct from each other, the quiet pins and the SDRAM pins") {
  const mk66::Pin audio[] = {mk66::board::kI2sMclk, mk66::board::kI2sRxd, mk66::board::kI2sTxd, mk66::board::kI2sLrclk,
                             mk66::board::kI2sBclk, mk66::board::kCodecScl, mk66::board::kCodecSda};
  const unsigned n = sizeof(audio) / sizeof(audio[0]);
  for (unsigned i = 0; i < n; ++i) {
    for (unsigned j = i + 1; j < n; ++j) {
      CHECK_FALSE(audio[i] == audio[j]);
    }
    for (const mk66::board::QuietPin& quiet : mk66::board::kQuietPins) {
      CHECK_FALSE(audio[i] == quiet.pin);
    }
    CHECK_FALSE(audio[i] == mk66::board::kSdramPower);
    CHECK_FALSE(audio[i] == mk66::board::kBootloaderRequest);
    for (const mk66::Pin sdram_pin : mk66::sdram::kPins) {
      CHECK_FALSE(audio[i] == sdram_pin);
    }
  }
  // The five I2S signals are on port E at mux 4, the I2C bus on PTD8 / PTD9 at mux 2.
  CHECK(mk66::board::kI2sPinMux == 4);
  CHECK(mk66::board::kI2sMclk.port == mk66::Port::E);
  CHECK(mk66::board::kI2sMclk.bit == 6);
  CHECK(mk66::board::kI2sRxd.bit == 7);
  CHECK(mk66::board::kI2sTxd.bit == 10);
  CHECK(mk66::board::kI2sLrclk.bit == 11);
  CHECK(mk66::board::kI2sBclk.bit == 12);
  CHECK(mk66::board::kCodecScl.port == mk66::Port::D);
  CHECK(mk66::board::kCodecScl.bit == 8);
  CHECK(mk66::board::kCodecSda.bit == 9);
  CHECK(mk66::board::kCodecI2cMux == 2);
  CHECK(mk66::board::kCodecI2cAddress == 0x0A);
}
