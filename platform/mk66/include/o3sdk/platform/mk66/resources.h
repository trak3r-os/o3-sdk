// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Who owns which shared hardware channel (ADR 0013 D1, hazard H11): the eDMA channels, the PIT timers and I2S0. A channel has an owner
// written down, in one table, and
// a compile-time check that no channel has two and that each owner's interrupt line has the priority of its role. A new user adds
// a row; there is no allocator to run out of.
//
// "claimed" rows are used by code that exists; "reserved" rows are named for a later slice so that nobody else takes them.

#include <o3sdk/hal/irq.h>
#include <o3sdk/platform/mk66/irq_list.h>
#include <o3sdk/platform/mk66/irq_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::resources {

// ---- eDMA channels: 16 channels (DMAMUX0), each with its own interrupt line (IRQ n for channel n) ---------------------------------
enum class DmaOwner : std::uint8_t {
  Free,
  I2sTx,    // I2S0 transmit: TX ring to the data register (OS-005a)
  I2sRx,    // I2S0 receive (OS-005a)
  Spi2Tx,   // DSPI2 transmit to the FT812: the display driver, OS-007c (DMAMUX source 39)
  Spi2Rx,   // DSPI2 receive (DMAMUX source 38); its major-loop interrupt (line DMA15_DMA31) is the transfer's completion
};

struct DmaRow {
  std::uint8_t channel;
  DmaOwner owner;
  bool claimed;            // code uses it today; false: reserved for a later slice, or free
  std::uint8_t arbitration;  // DCHPRIn: the fixed arbitration priority, higher wins; the table is a permutation of 0..15
  const char* name;
};

// Channels 14 and 15 are the display's (OS-007c: DSPI2 TX source 39 on 14, DSPI2 RX source 38 on 15, interrupt line DMA15_DMA31 at
// priority 80; display/edma_config.h holds the same numbers and mk66_resources_test.cpp compares the two), so the I2S channels are
// 12 (receive) and 13 (transmit).
//
// ARBITRATION. `arbitration` is the value of that channel's DCHPRI register (higher wins; fixed priority, the chip flags a duplicate).
// The audio channels are the two highest: TX 15, RX 14; the display's follow: 13 and 12; the free channels keep their own number below.
// **This table is the only place the eDMA's global configuration comes from**: boot::dma_init() (src/boot.cpp) writes DMA_CR and
// every DCHPRI from it, once, before any driver runs; no driver writes either (neither the audio start() nor the display's EdmaEngine
// does; tools/check_dma_owner.py, ctest mk66_dma_owner, fails the build if another file in platform/mk66 touches DMA_CR or a DCHPRI).
// The eDMA arbitrates at minor-loop boundaries, so what a lower-priority channel can cost an audio request is one of its minor loops.
// **the display's minor loops must therefore be at most 4 bytes** (display/edma_config.h kMaxMinorLoopBytes, static_asserted there) (one DSPI FIFO word): that is a few bus cycles, under a microsecond,
// against a TX FIFO margin of about 2 words = 11-22 us with the watermark of 1 (audio_config.h kTxWatermark); a display channel with
// a longer minor loop (a burst of 32 B or more) would eat that margin and has to be reviewed against it.
inline constexpr DmaRow kDma[16] = {
    {0, DmaOwner::Free, false, 0, "free"},
    {1, DmaOwner::Free, false, 1, "free"},
    {2, DmaOwner::Free, false, 2, "free"},
    {3, DmaOwner::Free, false, 3, "free"},
    {4, DmaOwner::Free, false, 4, "free"},
    {5, DmaOwner::Free, false, 5, "free"},
    {6, DmaOwner::Free, false, 6, "free"},
    {7, DmaOwner::Free, false, 7, "free"},
    {8, DmaOwner::Free, false, 8, "free"},
    {9, DmaOwner::Free, false, 9, "free"},
    {10, DmaOwner::Free, false, 10, "free"},
    {11, DmaOwner::Free, false, 11, "free"},
    {12, DmaOwner::I2sRx, true, 14, "i2s0_rx"},
    {13, DmaOwner::I2sTx, true, 15, "i2s0_tx"},
    {14, DmaOwner::Spi2Tx, true, 13, "spi2_tx (display, OS-007c)"},
    {15, DmaOwner::Spi2Rx, true, 12, "spi2_rx (display, OS-007c)"},
};
inline constexpr unsigned kDmaChannelCount = sizeof(kDma) / sizeof(kDma[0]);

inline constexpr std::uint8_t kDmaTxChannel = 13;
inline constexpr std::uint8_t kDmaRxChannel = 12;

constexpr std::uint8_t dma_channel_of(DmaOwner owner) noexcept {
  for (const DmaRow& row : kDma) {
    if (row.owner == owner) {
      return row.channel;
    }
  }
  return 0xFFu;
}
// The interrupt line of a channel below 16: IRQ n (DMAn_DMAn+16 in the vector table).
constexpr Irq dma_irq(std::uint8_t channel) noexcept { return static_cast<Irq>(channel); }

constexpr bool dma_table_valid() noexcept {
  unsigned seen_priority = 0;  // bit mask of arbitration values
  for (unsigned i = 0; i < kDmaChannelCount; ++i) {
    if (kDma[i].channel != i || kDma[i].arbitration > 15u || (seen_priority & (1u << kDma[i].arbitration)) != 0u) {
      return false;
    }
    seen_priority |= 1u << kDma[i].arbitration;
    // One owner per channel; a non-Free owner appears once.
    if (kDma[i].owner != DmaOwner::Free) {
      for (unsigned j = i + 1; j < kDmaChannelCount; ++j) {
        if (kDma[j].owner == kDma[i].owner) {
          return false;
        }
      }
    }
  }
  return seen_priority == 0xFFFFu;
}
static_assert(dma_table_valid(), "every eDMA channel appears once, no owner has two, the arbitration priorities are a permutation of 0..15");
static_assert(dma_channel_of(DmaOwner::I2sTx) == kDmaTxChannel && dma_channel_of(DmaOwner::I2sRx) == kDmaRxChannel, "the audio channels are where the constants say");
static_assert(kDma[kDmaTxChannel].arbitration > kDma[kDmaRxChannel].arbitration,
              "transmit outranks receive: a late transmit word is audible, a late receive word is a counted glitch");
constexpr bool audio_outranks_everything_else() noexcept {
  for (const DmaRow& row : kDma) {
    if (row.owner != DmaOwner::I2sTx && row.owner != DmaOwner::I2sRx && row.arbitration >= kDma[kDmaRxChannel].arbitration) {
      return false;
    }
  }
  return true;
}
static_assert(audio_outranks_everything_else(), "the two audio channels hold the two highest arbitration priorities");
static_assert(dma_channel_of(DmaOwner::Spi2Tx) == 14 && dma_channel_of(DmaOwner::Spi2Rx) == 15,
              "OS-007c's display channels (DSPI2 TX 14, RX 15): the audio channels must stay clear of them");
// The audio channels' interrupt lines carry the audio DMA priority (irq_map.h): a channel's interrupt can never be below its owner's role.
// The display's receive channel (the transfer's completion event) carries the completion priority (80): the same table row that SDHC,
// SPI2 and I2C completions use; its transmit channel raises no interrupt, so its line stays Unassigned.
static_assert(role_of(dma_irq(kDmaTxChannel)) == hal::irq::Role::AudioDma && role_of(dma_irq(kDmaRxChannel)) == hal::irq::Role::AudioDma,
              "the I2S DMA channels' interrupt lines are in the AudioDma row of the priority table");
static_assert(role_of(dma_irq(15)) == hal::irq::Role::Completion && role_of(dma_irq(14)) == hal::irq::Role::Unassigned,
              "the display's receive channel's interrupt line is in the Completion row of the priority table, its transmit channel's is not used");

// USB0 and the SDHC (track3r-os OS-006b and OS-007e; milestone M2b here) claim no eDMA channel and no PIT: the USB OTG module is a bus
// master of its own and its line (IRQ 53) is in the Usb row of the priority table; the SDHC driver is polled, and its line (IRQ 81)
// keeps its Completion row so that a later ADMA2 version cannot preempt the audio path.
static_assert(role_of(Irq::SDHC) == hal::irq::Role::Completion, "the SDHC line is in the Completion row: flags only, below the audio path");

// ---- PIT timers: four channels (H11), one interrupt line each --------------------------------------------------------------------
enum class PitOwner : std::uint8_t {
  Free,
  MidiOutEmitter,  // the one channel of the MIDI-out timed emitter (ADR 0013 D5, open item 6): OS-005c
};
struct PitRow {
  std::uint8_t channel;
  PitOwner owner;
  bool claimed;
  const char* name;
};
// One PIT channel is in use: channel 0, the MIDI-out emitter (OS-005c2: platform/mk66/src/midi_din.cpp, a one-shot compare whose interrupt writes the due byte
// to UART4). Other PIT users are gone by design: the sequencer ticks inside the render (D5), the engine control rate is the block rate, and MIDI input is
// timestamped in the UART ISR.
inline constexpr PitRow kPit[4] = {
    {0, PitOwner::MidiOutEmitter, true, "midi_out emitter (ADR 0013 D5, OS-005c2)"},
    {1, PitOwner::Free, false, "free"},
    {2, PitOwner::Free, false, "free"},
    {3, PitOwner::Free, false, "free"},
};
inline constexpr unsigned kPitChannelCount = sizeof(kPit) / sizeof(kPit[0]);
constexpr Irq pit_irq(std::uint8_t channel) noexcept { return static_cast<Irq>(static_cast<unsigned>(Irq::PIT0) + channel); }
constexpr bool pit_table_valid() noexcept {
  unsigned emitters = 0;
  for (unsigned i = 0; i < kPitChannelCount; ++i) {
    if (kPit[i].channel != i) {
      return false;
    }
    emitters += kPit[i].owner == PitOwner::MidiOutEmitter ? 1u : 0u;
  }
  return emitters == 1u;  // exactly one dedicated MIDI-out channel
}
static_assert(pit_table_valid(), "four PIT channels in order, exactly one of them the MIDI-out emitter");
static_assert(role_of(pit_irq(0)) == hal::irq::Role::MidiOutTimer, "the MIDI-out emitter's PIT line is in the MidiOutTimer row");
static_assert(role_of(pit_irq(1)) == hal::irq::Role::Unassigned && role_of(pit_irq(2)) == hal::irq::Role::Unassigned &&
                  role_of(pit_irq(3)) == hal::irq::Role::Unassigned,
              "an unclaimed PIT line can never preempt the audio path");

// ---- I2S0 and the render line -----------------------------------------------------------------------------------------------
// I2S0 (both directions) belongs to the audio engine alone; there is no other I2S user on this board. The render runs on the
// K66's software interrupt line (IRQ 64, hal::irq::Role::Render), which nothing else uses; PendSV is the fallback of ADR 0013 D1.
inline constexpr Irq kRenderLine = Irq::SWI;
static_assert(role_of(kRenderLine) == hal::irq::Role::Render, "the render line has the render priority");

}  // namespace o3sdk::platform::mk66::resources
