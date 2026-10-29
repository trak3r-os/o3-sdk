// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel-ownership table (resources.h, OS-005a; hazard H11): every eDMA channel and PIT timer has one owner on paper, the audio
// channels are where the audio code says, their interrupt lines carry the audio priorities. Host.
#include <doctest/doctest.h>

#include <o3sdk/hal/irq.h>
#include <o3sdk/platform/mk66/resources.h>

#include <cstdint>
#include <cstring>

namespace res = o3sdk::platform::mk66::resources;
namespace mk66 = o3sdk::platform::mk66;
namespace irq = o3sdk::hal::irq;

TEST_CASE("resources: eDMA channels, one row each, one owner each, arbitration a permutation") {
  CHECK(res::kDmaChannelCount == 16u);
  std::uint32_t seen = 0;
  unsigned claimed = 0;
  unsigned reserved = 0;
  for (unsigned i = 0; i < res::kDmaChannelCount; ++i) {
    const res::DmaRow& row = res::kDma[i];
    CHECK(row.channel == i);
    CHECK(row.arbitration <= 15u);
    CHECK((seen & (1u << row.arbitration)) == 0u);   // no two channels share an arbitration priority (the chip flags that as an error)
    seen |= 1u << row.arbitration;
    CHECK(row.name != nullptr);
    for (unsigned j = i + 1; j < res::kDmaChannelCount; ++j) {
      if (row.owner != res::DmaOwner::Free) {
        CHECK(res::kDma[j].owner != row.owner);      // an owner has one channel
      }
    }
    claimed += row.claimed ? 1u : 0u;
    reserved += (row.owner != res::DmaOwner::Free && !row.claimed) ? 1u : 0u;
  }
  CHECK(seen == 0xFFFFu);
  CHECK(claimed == 4u);    // the two I2S channels and the display's two (DSPI2 TX and RX)
  CHECK(reserved == 0u);   // nothing is only named for a later slice among the eDMA channels
}

TEST_CASE("resources: the audio channels are 13 (transmit) and 12 (receive), clear of the display's 14 and 15, and rank above it") {
  CHECK(res::kDmaTxChannel == 13);
  CHECK(res::kDmaRxChannel == 12);
  CHECK(res::dma_channel_of(res::DmaOwner::I2sTx) == 13);
  CHECK(res::dma_channel_of(res::DmaOwner::I2sRx) == 12);
  // OS-007c (the FT812 display) owns 14 for DSPI2 TX and 15 for DSPI2 RX: the table and the driver's own constants must agree.
  CHECK(res::dma_channel_of(res::DmaOwner::Spi2Tx) == 14);
  CHECK(res::dma_channel_of(res::DmaOwner::Spi2Rx) == 15);
  // The display driver's own constants (display/edma_config.h) are compared with these rows by the display slice's tests: this suite
  // links the base library only. The completion line is the receive channel's, in the Completion row of the priority table.
  CHECK(mk66::role_of(res::dma_irq(15)) == irq::Role::Completion);
  CHECK(irq::priority(irq::Role::Completion) == 80);
  CHECK(res::kDma[13].owner == res::DmaOwner::I2sTx);
  CHECK(res::kDma[13].claimed);
  CHECK(res::kDma[12].claimed);
  CHECK(res::kDma[14].claimed);
  CHECK(res::kDma[15].claimed);
  // Arbitration (the DCHPRI values boot::dma_init() writes): audio highest (TX 15, RX 14), the display next (13, 12), the free channels
  // below keep their own number; a permutation of 0..15.
  CHECK(res::kDma[13].arbitration == 15u);
  CHECK(res::kDma[12].arbitration == 14u);
  CHECK(res::kDma[14].arbitration == 13u);
  CHECK(res::kDma[15].arbitration == 12u);
  CHECK(res::kDma[13].arbitration > res::kDma[12].arbitration);   // transmit outranks receive
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(res::kDma[i].arbitration == i);                          // free channels: below the display's
    CHECK(res::kDma[i].arbitration < res::kDma[15].arbitration);
  }
  for (unsigned i = 14; i < 16; ++i) {
    CHECK(res::kDma[i].arbitration < res::kDma[12].arbitration);   // the display's two are below both audio channels
  }
  CHECK(std::strcmp(res::kDma[13].name, "i2s0_tx") == 0);
  CHECK(std::strcmp(res::kDma[12].name, "i2s0_rx") == 0);
  CHECK(std::strstr(res::kDma[14].name, "OS-007c") != nullptr);
  CHECK(std::strstr(res::kDma[15].name, "OS-007c") != nullptr);
}

TEST_CASE("resources: the channels' interrupt lines are the right vectors and carry the right priority") {
  CHECK(res::dma_irq(13) == mk66::Irq::DMA13_DMA29);
  CHECK(res::dma_irq(12) == mk66::Irq::DMA12_DMA28);
  CHECK(static_cast<unsigned>(res::dma_irq(13)) == 13u);
  CHECK(mk66::role_of(res::dma_irq(res::kDmaTxChannel)) == irq::Role::AudioDma);
  CHECK(mk66::role_of(res::dma_irq(res::kDmaRxChannel)) == irq::Role::AudioDma);
  CHECK(irq::priority(mk66::role_of(res::dma_irq(res::kDmaTxChannel))) == 16u);
  // The display's receive line (OS-007c: DMA15_DMA31 at 80) is the completion row: flags only, below the audio ISR, above the render.
  CHECK(mk66::role_of(mk66::Irq::DMA15_DMA31) == irq::Role::Completion);
  CHECK(mk66::role_of(mk66::Irq::DMA14_DMA30) == irq::Role::Unassigned);   // the transmit channel raises no interrupt
  CHECK(irq::priority(mk66::role_of(mk66::Irq::DMA15_DMA31)) == 80u);
  CHECK(irq::preempts(irq::Role::AudioDma, irq::Role::Completion));
  CHECK(irq::preempts(irq::Role::Completion, irq::Role::Render));
  // The remaining DMA lines are unclaimed: below everything with a role.
  for (unsigned ch = 0; ch < 12; ++ch) {
    CHECK(mk66::role_of(res::dma_irq(static_cast<std::uint8_t>(ch))) == irq::Role::Unassigned);
  }
}

TEST_CASE("resources: four PIT timers, one the MIDI-out emitter's (claimed by OS-005c2), three free") {
  CHECK(res::kPitChannelCount == 4u);
  unsigned emitters = 0;
  for (unsigned i = 0; i < res::kPitChannelCount; ++i) {
    CHECK(res::kPit[i].channel == i);
    CHECK(res::kPit[i].claimed == (i == 0u));
    emitters += res::kPit[i].owner == res::PitOwner::MidiOutEmitter ? 1u : 0u;
    CHECK(res::pit_irq(static_cast<std::uint8_t>(i)) == static_cast<mk66::Irq>(static_cast<unsigned>(mk66::Irq::PIT0) + i));
  }
  CHECK(emitters == 1u);
  CHECK(res::kPit[0].owner == res::PitOwner::MidiOutEmitter);
  CHECK(mk66::role_of(res::pit_irq(0)) == irq::Role::MidiOutTimer);
  for (std::uint8_t ch = 1; ch < 4; ++ch) {
    CHECK(res::kPit[ch].owner == res::PitOwner::Free);
    CHECK(mk66::role_of(res::pit_irq(ch)) == irq::Role::Unassigned);   // an unclaimed PIT line can never preempt audio
  }
}

TEST_CASE("resources: the render line is the K66's software interrupt, IRQ 64, at the render priority") {
  CHECK(res::kRenderLine == mk66::Irq::SWI);
  CHECK(static_cast<unsigned>(res::kRenderLine) == 64u);
  CHECK(mk66::role_of(res::kRenderLine) == irq::Role::Render);
  CHECK(irq::priority(mk66::role_of(res::kRenderLine)) == 128u);
  // The slot-boundary line preempts the render line it pends.
  CHECK(irq::preempts(mk66::role_of(res::dma_irq(res::kDmaTxChannel)), mk66::role_of(res::kRenderLine)));
}
