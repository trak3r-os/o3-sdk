// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 hardware half of the display driver: DSPI2 as the FT812's Link (polled bytes), the eDMA as its BulkEngine (one
// buffer per chip-select cycle), the completion interrupt, bring_up() and the main loop's calls (device.h). Everything above this
// file is pure and tested on the host; this file is what only a hardware session can prove (track3r-os platform/mk66/README.md,
// checklist DSP-*).
//
// Nothing here blocks without a bound: every wait is a counted loop, and a wait that runs out reports an error flag that the
// driver collects (Link::take_error) instead of hanging the main loop.
#include "dev_regs.h"
#include "../../src/internal.h"

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/display/chip.h>
#include <o3sdk/platform/mk66/display/device.h>
#include <o3sdk/platform/mk66/display/display.h>
#include <o3sdk/platform/mk66/display/dspi_config.h>
#include <o3sdk/platform/mk66/display/edma_config.h>
#include <o3sdk/platform/mk66/irq_list.h>
#include <o3sdk/platform/mk66/mem_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::display::device {
namespace {

// ---- Shared state of the DMA path --------------------------------------------------------------------------------------
// One writer per transition: the main loop moves Idle -> Busy before it enables a request and Done / Failed -> Idle when it
// collects the result; the completion handler moves Busy -> Done / Failed and nothing else. abort() runs with interrupts masked.
enum : std::uint8_t { kIdle = 0, kBusy = 1, kDone = 2, kFailed = 3 };
volatile std::uint8_t g_state = kIdle;
// The receive channel's one-word sink: the frames it reads are not used (a master's receive of a write-only transfer).
alignas(4) volatile std::uint32_t g_rx_sink = 0;

// True while the driver itself waits through Link::delay_ms() (the chip's bring-up and re-initialisation): boot::delay_ms() calls the
// idle hook once a millisecond, and the hook must not enter the driver then (it is not re-entrant). Main thread only, like both.
bool g_driver_waiting = false;

// Spin bounds, in polls. The slowest SPI byte is 8 / 1.125 MHz = 7.1 us = 1,280 core cycles; a poll is a few cycles, so 20,000
// polls are about a hundred times that.
constexpr std::uint32_t kByteSpinLimit = 20'000;
constexpr std::uint32_t kHaltSpinLimit = 20'000;

// Interrupt mask save / restore, for the few places where the main loop and the completion handler could meet.
std::uint32_t irq_save() noexcept {
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  return primask;
}
void irq_restore(std::uint32_t primask) noexcept { asm volatile("msr primask, %0" ::"r"(primask) : "memory"); }

// ---- Chip select, PD#: GPIO ---------------------------------------------------------------------------------------------
// About ten NOPs (55 ns at 180 MHz) separate chip select from the first clock, and the last clock from
// chip select high, as setup and hold.
inline void settle_nops() noexcept {
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
}
inline void cs_low() noexcept {
  regs::gpio_pcor(board::kFt812ChipSelect) = regs::mask_of(board::kFt812ChipSelect);
  settle_nops();
}
inline void cs_high() noexcept {
  settle_nops();
  regs::gpio_psor(board::kFt812ChipSelect) = regs::mask_of(board::kFt812ChipSelect);
}

// Waits for `done()`, at most `limit` polls. false: ran out.
template <typename Predicate>
bool spin(std::uint32_t limit, Predicate done) noexcept {
  for (std::uint32_t i = 0; i < limit; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// Empties both DSPI FIFOs and clears every status flag, with the module halted so that nothing is mid-frame. Main loop.
void flush_dspi() noexcept {
  dev::spi2_mcr() = dev::spi2_mcr() | dev::kMcrHalt;
  spin(kHaltSpinLimit, [] { return (dev::spi2_sr() & dev::kSrTxrxs) == 0; });
  dev::spi2_mcr() = dev::spi2_mcr() | dev::kMcrClrTxf | dev::kMcrClrRxf;
  dev::spi2_sr() = dev::kSrFlushClear;  // not TFFF (soc_facts.h): the DMA's first request must survive the flush
  dev::spi2_mcr() = dev::spi2_mcr() & ~dev::kMcrHalt;
}

// ---- The Link: DSPI2, polled ---------------------------------------------------------------------------------------------
class DspiLink final : public Link {
 public:
  // Clock gate, module in master mode at the init speed, then the pins (the module drives SCK idle low before the pin
  // leaves GPIO input), then clean FIFOs. Chip select and PD# are already outputs, high (boot::early_pins).
  void begin() noexcept {
    dev::sim_scgc3() = dev::sim_scgc3() | dev::kScgc3Spi2;
    const std::uint32_t gate = dev::sim_scgc3();  // read back: the gate is open before the first register access
    (void)gate;
    dev::spi2_mcr() = dev::kMcrMdis | dev::kMcrHalt | dev::kMcrPcsisAll;  // stopped while it is configured
    dev::spi2_ctar0() = spi::ctar(spi::kInitRate);
    dev::spi2_mcr() = dev::kMcrMstr | dev::kMcrPcsisAll;  // master, running, FIFOs on, no continuous clock, mode 0
    regs::port_pcr(board::kFt812Sck) = dev::kPcrMuxAlt2;
    regs::port_pcr(board::kFt812Mosi) = dev::kPcrMuxAlt2;
    regs::port_pcr(board::kFt812Miso) = dev::kPcrMuxAlt2;
    flush_dspi();
    error_ = false;
  }

  // While a DMA transfer is in flight it owns the bus and chip select: a polled access now would cut it short (chip select high)
  // or corrupt it (a byte in its stream). The driver never does it (display.cpp keeps its polled accesses out of the Sending
  // state, and a test proves it); this is the second line, and it says so through the error flag.
  void select() noexcept override {
    if (g_state == kBusy) {
      error_ = true;
      return;
    }
    cs_low();
  }
  void deselect() noexcept override {
    if (g_state == kBusy) {
      error_ = true;
      return;
    }
    cs_high();
  }

  std::uint8_t xfer(std::uint8_t out) noexcept override {
    if (g_state == kBusy) {
      error_ = true;
      return 0xFF;
    }
    dev::spi2_sr() = dev::kSrRfdf;
    // A full 32-bit write: command half zero (CTAS 0 = CTAR0, no CONT, no EOQ, PCS 0), data byte in the low half.
    dev::spi2_pushr() = out;
    if (!spin(kByteSpinLimit, [] { return (dev::spi2_sr() & dev::kSrRxctrMask) != 0; })) {
      error_ = true;
      flush_dspi();
      return 0xFF;
    }
    const auto in = static_cast<std::uint8_t>(dev::spi2_popr());
    dev::spi2_sr() = dev::kSrRfdf;
    return in;
  }

  void power_down(bool asserted) noexcept override {
    // PD# is active low: asserted = driven low.
    if (asserted) {
      regs::gpio_pcor(board::kFt812PowerDown) = regs::mask_of(board::kFt812PowerDown);
    } else {
      regs::gpio_psor(board::kFt812PowerDown) = regs::mask_of(board::kFt812PowerDown);
    }
  }

  void set_speed(SpiSpeed speed) noexcept override {
    if (g_state == kBusy) {
      error_ = true;
      return;
    }
    const std::uint32_t ctar = spi::ctar(speed == SpiSpeed::Init ? spi::kInitRate : spi::kDataRate);
    // CTAR is written with the module halted (RM: CTARs are writable only in the stopped state).
    dev::spi2_mcr() = dev::spi2_mcr() | dev::kMcrHalt;
    spin(kHaltSpinLimit, [] { return (dev::spi2_sr() & dev::kSrTxrxs) == 0; });
    dev::spi2_ctar0() = ctar;
    dev::spi2_mcr() = dev::spi2_mcr() & ~dev::kMcrHalt;
  }

  void delay_ms(std::uint32_t ms) noexcept override {
    const bool was = g_driver_waiting;  // (nested only if a wait ever waited: restore what was found)
    g_driver_waiting = true;
    boot::delay_ms(ms);
    g_driver_waiting = was;
  }
  std::uint32_t now_ms() const noexcept override { return boot::uptime_ms_low(); }
  bool take_error() noexcept override {
    const bool e = error_;
    error_ = false;
    return e;
  }

 private:
  bool error_ = false;
};

// ---- The BulkEngine: eDMA channels 14 (TX) and 15 (RX) ------------------------------------------------------------------
void write_tcd(unsigned channel, const edma::Tcd& t) noexcept {
  dev::tcd_u16(channel, 0x1C) = 0;  // CSR first: no DONE, no flags, while the rest is written
  dev::tcd_u32(channel, 0x00) = t.saddr;
  dev::tcd_u16(channel, 0x04) = static_cast<std::uint16_t>(t.soff);
  dev::tcd_u16(channel, 0x06) = t.attr;
  dev::tcd_u32(channel, 0x08) = t.nbytes;
  dev::tcd_u32(channel, 0x0C) = static_cast<std::uint32_t>(t.slast);
  dev::tcd_u32(channel, 0x10) = t.daddr;
  dev::tcd_u16(channel, 0x14) = static_cast<std::uint16_t>(t.doff);
  dev::tcd_u16(channel, 0x16) = t.citer;
  dev::tcd_u32(channel, 0x18) = static_cast<std::uint32_t>(t.dlastsga);
  dev::tcd_u16(channel, 0x1E) = t.biter;
  dev::tcd_u16(channel, 0x1C) = t.csr;
}

// Stops both channels and the DSPI's requests. Safe in the handler and in the main loop.
inline void stop_requests() noexcept {
  dev::spi2_rser() = 0;
  dev::dma_cerq() = static_cast<std::uint8_t>(edma::kTxChannel);
  dev::dma_cerq() = static_cast<std::uint8_t>(edma::kRxChannel);
  dev::dmamux_chcfg(edma::kTxChannel) = edma::mux_config(edma::kTxSource, false);
  dev::dmamux_chcfg(edma::kRxChannel) = edma::mux_config(edma::kRxSource, false);
}

// Clears the error indicators of this driver's two channels only (never another owner's).
inline void clear_own_errors() noexcept {
  dev::dma_cerr() = static_cast<std::uint8_t>(edma::kTxChannel);
  dev::dma_cerr() = static_cast<std::uint8_t>(edma::kRxChannel);
}

class EdmaEngine final : public BulkEngine {
 public:
  void begin() noexcept {
    regs::sim_scgc7() = regs::sim_scgc7() | dev::kScgc7Dma;
    dev::sim_scgc6() = dev::sim_scgc6() | dev::kScgc6Dmamux;
    const std::uint32_t gate = dev::sim_scgc6();
    (void)gate;
    // The eDMA's global configuration (DMA_CR, every DCHPRI) is not written here: one boot function owns it (OS-005a's dma_init(),
    // see device.h). This engine programs only its own two channels' TCDs, the DMAMUX and its interrupt line.
    stop_requests();
    clear_own_errors();
    dev::dma_cdne() = static_cast<std::uint8_t>(edma::kTxChannel);
    dev::dma_cdne() = static_cast<std::uint8_t>(edma::kRxChannel);
    dev::dma_cint() = static_cast<std::uint8_t>(edma::kRxChannel);
    g_state = kIdle;
    constexpr unsigned kLine = static_cast<unsigned>(Irq::DMA15_DMA31);
    dev::nvic_icpr(kLine) = 1u << (kLine & 31u);
    dev::nvic_iser(kLine) = 1u << (kLine & 31u);  // its priority (Completion, 80) was written by boot::init_interrupts
  }

  std::size_t max_len() const noexcept override {
    return Ft812Display::kFrameStageBytes - Ft812Display::kHeaderOffset;  // the staging buffer, never more
  }

  bool start(const std::uint8_t* src, std::size_t len, std::size_t capacity) noexcept override {
    const auto address = reinterpret_cast<std::uintptr_t>(src);
    // Bounded to its own buffer: `capacity` is what is left of the staging buffer this transfer comes from (the frame buffer or
    // the smaller upload buffer, whichever it is), so a transfer is never longer than the buffer it reads. And the source must
    // lie wholly inside SRAM.
    if (g_state != kIdle || src == nullptr || !edma::within_buffer(static_cast<std::uint32_t>(len), static_cast<std::uint32_t>(capacity)) ||
        len > max_len() || !mem::in_sram(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(len))) {
      return false;
    }
    const edma::Tcd tx = edma::tx_tcd(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(len));
    const edma::Tcd rx = edma::rx_tcd(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_rx_sink)), static_cast<std::uint32_t>(len));
    if (!tx.valid || !rx.valid) {
      return false;
    }
    stop_requests();
    dev::dma_cdne() = static_cast<std::uint8_t>(edma::kTxChannel);
    dev::dma_cdne() = static_cast<std::uint8_t>(edma::kRxChannel);
    dev::dma_cint() = static_cast<std::uint8_t>(edma::kRxChannel);
    clear_own_errors();
    flush_dspi();  // empty FIFOs, clear flags: the first request must be the first byte's
    write_tcd(edma::kTxChannel, tx);
    write_tcd(edma::kRxChannel, rx);
    dev::dmamux_chcfg(edma::kTxChannel) = edma::mux_config(edma::kTxSource, true);
    dev::dmamux_chcfg(edma::kRxChannel) = edma::mux_config(edma::kRxSource, true);
    g_state = kBusy;  // before any request can complete
    cs_low();
    asm volatile("dsb" ::: "memory");  // the staged bytes are in SRAM before the first DMA read
    dev::dma_serq() = static_cast<std::uint8_t>(edma::kRxChannel);
    dev::dma_serq() = static_cast<std::uint8_t>(edma::kTxChannel);
    dev::spi2_rser() = edma::kRserDma;  // the DSPI's requests start the transfer
    return true;
  }

  BulkStatus poll() noexcept override {
    switch (g_state) {
      case kBusy:
        return BulkStatus::Busy;
      case kDone:
        g_state = kIdle;
        return BulkStatus::Done;
      case kFailed:
        flush_dspi();  // not in the handler: a halt-and-wait
        g_state = kIdle;
        return BulkStatus::Failed;
      default:
        return BulkStatus::Idle;
    }
  }

  void abort() noexcept override {
    const std::uint32_t primask = irq_save();
    stop_requests();
    dev::dma_cint() = static_cast<std::uint8_t>(edma::kRxChannel);
    clear_own_errors();
    cs_high();
    g_state = kIdle;
    irq_restore(primask);
    flush_dspi();
  }
};

DspiLink g_link;
EdmaEngine g_engine;
Chip g_chip(g_link);
Ft812Display g_display(g_chip, g_engine);

}  // namespace

// The receive channel's major loop is done: every byte of the buffer has been shifted out and its answer taken. Flags only
// (Completion priority, ADR 0013 D1): requests off, chip select released, the result published.
void edma_complete_isr() noexcept {
  dev::dma_cint() = static_cast<std::uint8_t>(edma::kRxChannel);
  if (g_state != kBusy) {
    return;  // a stale interrupt of a transfer that was aborted
  }
  stop_requests();
  const bool error = (dev::dma_es() & dev::kEsVld) != 0 || (dev::spi2_sr() & (dev::kSrTfuf | dev::kSrRfof)) != 0;
  if (error) {
    clear_own_errors();
  }
  cs_high();
  asm volatile("dsb" ::: "memory");
  g_state = error ? kFailed : kDone;
}

static BringUpResult g_last_result;

const BringUpResult& last_bring_up() noexcept { return g_last_result; }

Ft812Display& display() noexcept { return g_display; }
Chip& chip() noexcept { return g_chip; }

Status service() noexcept {
  if (!g_display.ready()) {
    return Status::Down;
  }
  g_display.service();
  if (g_display.failed()) {
    return Status::Failed;
  }
  return g_display.needs_assets() ? Status::NeedsAssets : Status::Ok;
}

void idle() noexcept {
  if (g_driver_waiting || !g_display.ready()) {
    return;
  }
  g_display.service();
}

void hold_dark() noexcept { g_display.hold_dark(); }

BringUpResult bring_up() noexcept {
  BringUpResult result;
  g_link.begin();
  g_engine.begin();
  InitConfig config;
  config.orientation = kBuildOrientation;
  for (unsigned attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    result.attempts = attempt;
    result.init = g_chip.init(config);
    if (result.init.status == InitStatus::Ok) {
      break;
    }
  }
  result.up = result.init.status == InitStatus::Ok;
  if (result.up) {
    g_display.start(true, kBacklightDefault);
    result.dma = g_display.self_test();
  }  // else the display stays refused and dark; the image carries on headless
  g_last_result = result;
  return result;
}

}  // namespace o3sdk::platform::mk66::display::device

extern "C" void DMA15_DMA31_IRQHandler() { o3sdk::platform::mk66::display::device::edma_complete_isr(); }
