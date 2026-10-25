// SPDX-License-Identifier: GPL-3.0-or-later
//
// The eDMA side of the FT812 bus as values (display/edma_config.h): channels, request sources, the transfer descriptors for
// one buffer going out through DSPI2, the bounds that keep a transfer inside its buffer, and the interrupt line the receive
// channel owns. Runs on the host. What the hardware does with these values is the
// hardware session's (checklist DSP-5, DSP-6).
#include <doctest/doctest.h>

#include <o3sdk/hal/irq.h>
#include <o3sdk/platform/mk66/display/display.h>
#include <o3sdk/platform/mk66/display/edma_config.h>
#include <o3sdk/platform/mk66/irq_list.h>
#include <o3sdk/platform/mk66/irq_map.h>
#include <o3sdk/platform/mk66/resources.h>

#include <cstdint>
#include <initializer_list>

namespace display = o3sdk::platform::mk66::display;
namespace edma = o3sdk::platform::mk66::display::edma;
namespace mk66 = o3sdk::platform::mk66;

TEST_CASE("display edma: the channels, the DMAMUX sources and the interrupt line") {
  CHECK(edma::kTxChannel == 14u);
  CHECK(edma::kRxChannel == 15u);
  CHECK(edma::kTxSource == 39u);  // SPI2 transmit (K66 RM DMAMUX table; the Teensy core's DMAMUX_SOURCE_SPI2_TX)
  CHECK(edma::kRxSource == 38u);  // SPI2 receive
  // channels 0..15 raise the line of their own number (DMAn_DMAn+16): the receive channel's is DMA15_DMA31 at Completion (80)
  CHECK(static_cast<unsigned>(mk66::Irq::DMA15_DMA31) == edma::kRxChannel);
  CHECK(mk66::role_of(mk66::Irq::DMA15_DMA31) == o3sdk::hal::irq::Role::Completion);
  CHECK(o3sdk::hal::irq::priority(mk66::role_of(mk66::Irq::DMA15_DMA31)) == 80);
  // the transmit channel raises nothing: its line stays unassigned (lowest)
  CHECK(mk66::role_of(mk66::Irq::DMA14_DMA30) == o3sdk::hal::irq::Role::Unassigned);
}

TEST_CASE("display edma: the platform's channel table (resources.h) gives the display the channels this driver programs") {
  namespace res = o3sdk::platform::mk66::resources;
  CHECK(res::dma_channel_of(res::DmaOwner::Spi2Tx) == edma::kTxChannel);
  CHECK(res::dma_channel_of(res::DmaOwner::Spi2Rx) == edma::kRxChannel);
  CHECK(edma::kTxSource == 39u);
  CHECK(edma::kRxSource == 38u);
  CHECK(edma::kMaxMinorLoopBytes <= 4u);  // resources.h: a display minor loop is at most one DSPI FIFO word (the audio's margin)
  CHECK(mk66::role_of(res::dma_irq(edma::kRxChannel)) == o3sdk::hal::irq::Role::Completion);
}

TEST_CASE("display edma: the transmit descriptor moves bytes into the low byte of PUSHR, one per request") {
  const edma::Tcd t = edma::tx_tcd(0x20001001u, 3 + 4 * 100);
  CHECK(t.valid);
  CHECK(t.saddr == 0x20001001u);  // the source may start at any byte
  CHECK(t.soff == 1);
  CHECK(t.attr == 0x0000);        // 8-bit reads, 8-bit writes
  CHECK(t.nbytes == 1u);
  CHECK(t.slast == 0);
  CHECK(t.daddr == 0x400AC034u);  // SPI2_PUSHR
  CHECK(t.doff == 0);
  CHECK(t.citer == 403);
  CHECK(t.biter == 403);
  CHECK(t.dlastsga == 0);
  CHECK((t.csr & edma::kCsrDreq) != 0);       // the request switches itself off at the end
  CHECK((t.csr & edma::kCsrIntMajor) == 0);   // the transmit channel is silent: completion is the receive channel's
}

TEST_CASE("display edma: the receive descriptor reads every answer frame into one word and is the completion event") {
  const edma::Tcd r = edma::rx_tcd(0x20002000u, 403);
  CHECK(r.valid);
  CHECK(r.saddr == 0x400AC038u);  // SPI2_POPR
  CHECK(r.soff == 0);
  CHECK(r.attr == 0x0202);        // 32-bit reads, 32-bit writes
  CHECK(r.nbytes == 4u);
  CHECK(r.daddr == 0x20002000u);
  CHECK(r.doff == 0);             // no increment: a one-word sink, the count cannot run past anything
  CHECK(r.citer == 403);
  CHECK(r.biter == 403);          // as many frames as the transmit side sends
  CHECK((r.csr & edma::kCsrDreq) != 0);
  CHECK((r.csr & edma::kCsrIntMajor) != 0);
  CHECK(edma::tx_tcd(0x20001001u, 403).citer == r.citer);
}

TEST_CASE("display edma: out-of-range requests are not descriptors") {
  CHECK_FALSE(edma::tx_tcd(0, 10).valid);                          // no source
  CHECK_FALSE(edma::tx_tcd(0x20000000u, 0).valid);                 // nothing to send
  CHECK_FALSE(edma::tx_tcd(0x20000000u, edma::kMaxCount + 1).valid);  // CITER is 15 bits
  CHECK(edma::tx_tcd(0x20000000u, edma::kMaxCount).valid);
  CHECK_FALSE(edma::rx_tcd(0, 10).valid);
  CHECK_FALSE(edma::rx_tcd(0x20000002u, 10).valid);                // the sink is a word
  CHECK_FALSE(edma::rx_tcd(0x20000000u, 0).valid);
  CHECK_FALSE(edma::rx_tcd(0x20000000u, edma::kMaxCount + 1).valid);
}

TEST_CASE("display edma: the staging buffers' largest transfers fit one descriptor and end at their buffer's end") {
  using D = display::Ft812Display;
  const std::uint32_t frame_bytes = 3u + 4u * D::kMaxFrameWords;
  const std::uint32_t upload_bytes = 3u + o3sdk::hal::kMaxUploadBytes;
  CHECK(frame_bytes == 8195u);
  CHECK(upload_bytes == 4099u);
  CHECK(frame_bytes <= edma::kMaxCount);
  CHECK(edma::tx_tcd(0x20000001u, frame_bytes).valid);
  CHECK(frame_bytes == D::kFrameStageBytes - D::kHeaderOffset);
  CHECK(upload_bytes == D::kUploadStageBytes - D::kHeaderOffset);
}

TEST_CASE("display edma: DMAMUX and DSPI request bytes") {
  CHECK(edma::mux_config(39, true) == 0xA7);   // ENBL | 39
  CHECK(edma::mux_config(38, true) == 0xA6);
  CHECK(edma::mux_config(39, false) == 0x27);  // source kept, disabled: a channel is reconfigured only while off
  CHECK(edma::mux_config(0x7F, true) == 0xBF); // the source field is six bits
  // RSER: TFFF_RE | TFFF_DIRS | RFDF_RE | RFDF_DIRS and nothing else (no DSPI interrupt of its own)
  CHECK(edma::kRserDma == 0x03030000u);
}

TEST_CASE("display edma: minor loops of at most 4 bytes, the audio FIFO margin depends on it") {
  CHECK(edma::kMaxMinorLoopBytes == 4u);
  for (const std::uint32_t count : {1u, 3u, 403u, 8195u}) {
    CHECK(edma::tx_tcd(0x20000000u, count).nbytes <= edma::kMaxMinorLoopBytes);
    CHECK(edma::rx_tcd(0x20000000u, count).nbytes <= edma::kMaxMinorLoopBytes);
  }
  CHECK(edma::tx_tcd(0x20000000u, 8).nbytes == 1u);
  CHECK(edma::rx_tcd(0x20000000u, 8).nbytes == 4u);
}

TEST_CASE("display edma: a transfer is bounded by the buffer it comes from") {
  CHECK(edma::within_buffer(1, 1));
  CHECK(edma::within_buffer(8195, 8195));
  CHECK_FALSE(edma::within_buffer(8196, 8195));
  CHECK_FALSE(edma::within_buffer(0, 8195));
  CHECK_FALSE(edma::within_buffer(4100, 4099));  // an upload buffer's room is not a frame buffer's
}
