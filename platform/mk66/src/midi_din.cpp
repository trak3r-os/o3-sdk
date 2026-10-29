// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66's DIN MIDI port (OS-005c2): UART4 and the one PIT channel of the emitter. See midi.h for what runs where; the logic of the two interrupts is
// midi/din_io.h, the values written here are midi_config.h's (checked by the compiler and tests/mk66_midi_config_test.cpp).
//
// SEQUENCE (start(); the same order as the other drivers: gates, peripheral, pins, interrupt lines last):
//   1. SIM_SCGC1[UART4] and SIM_SCGC6[PIT] on (the port clock gates were opened by boot::early_pins).
//   2. PIT: the module on (MCR = 0), channel 0 stopped with its flag clear.
//   3. UART4: C2 = 0 (transmitter and receiver off), then the baud divisor (BDH, BDL, C4), C1 = 0 (8N1, no parity, no loopback), C3 = 0, PFIFO = 0 (the FIFO off:
//      the data register in front of the shift register is the whole transmit buffer, whatever depth the chip has; midi/din_io.h explains why that is enough).
//   4. RX pin to the UART (pull-up and passive filter: a floating input pin is a stream of false start bits), then C2 = TE | RE | RIE, then a read of S1 and D to drop
//      anything the line held, then the TX pin to the UART: with TE set the UART drives the line idle-high, which is the level the quiet-pin table has held the pin at
//      as a GPIO since boot, so the hand-over does not glitch the DIN output (a MIDI device would see a start bit).
//   5. The NVIC: pending cleared, UART4_RX_TX and PIT0 enabled (their priorities were written at boot from the table: 48 and 96). UART4_ERR stays disabled: the receive
//      interrupt reads S1 and the error flags in it (a framing, noise or overrun flag is cleared by the S1 / D read pair, midi/din_io.h DinRx).
#include "internal.h"
#include "regs.h"

#include <o3sdk/platform/mk66/midi/din_io.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/midi.h>
#include <o3sdk/platform/mk66/midi_config.h>
#include <o3sdk/platform/mk66/resources.h>
#include <o3sdk/platform/mk66/sections.h>

#include <cstdint>

namespace o3sdk::platform::mk66::midi_io {
namespace {

namespace u4 = midi_cfg::uart4;
namespace pit = midi_cfg::pit;
using regs::at;

constexpr unsigned kRxIrq = static_cast<unsigned>(Irq::UART4_RX_TX);
constexpr unsigned kPitIrq = static_cast<unsigned>(resources::pit_irq(pit::kChannel));
static_assert(role_of(Irq::UART4_RX_TX) == hal::irq::Role::MidiUart && role_of(resources::pit_irq(pit::kChannel)) == hal::irq::Role::MidiOutTimer,
              "the DIN lines carry the priorities of their rows in the table of ADR 0013 D1 (48 and 96)");
static_assert(resources::kPit[pit::kChannel].owner == resources::PitOwner::MidiOutEmitter, "the emitter's PIT channel is the one resources.h reserves for it");

volatile std::uint8_t& u4_reg(std::uint32_t offset) noexcept { return at<std::uint8_t>(u4::kBase + offset); }

// The register policy of midi/din_io.h: UART4 and PIT0, one instruction each.
struct Hw {
  static constexpr unsigned kCyclesPerTickShift = pit::kCyclesPerTickShift;
  std::uint32_t cycles() const noexcept { return regs::dwt_cyccnt(); }
  std::uint8_t status() const noexcept { return u4_reg(u4::kOffS1); }
  std::uint8_t data() const noexcept { return u4_reg(u4::kOffD); }
  bool tx_ready() const noexcept { return (u4_reg(u4::kOffS1) & midi::uart_s1::kTdre) != 0u; }
  void tx_write(std::uint8_t byte) const noexcept { u4_reg(u4::kOffD) = byte; }
  // One-shot: the channel off and its flag cleared. The flag is write-1-to-clear; the handler's later work gives the write time to land before it returns, so the
  // line is not still asserted when the interrupt exits.
  void timer_stop() const noexcept {
    at<std::uint32_t>(pit::tctrl(pit::kChannel)) = 0;
    at<std::uint32_t>(pit::tflg(pit::kChannel)) = pit::kTflgTif;
  }
  // LDVAL then the enable: the counter loads LDVAL when the channel is enabled and the interrupt comes after LDVAL + 1 ticks.
  void timer_start(std::uint32_t ticks) const noexcept {
    at<std::uint32_t>(pit::ldval(pit::kChannel)) = ticks - 1u;
    at<std::uint32_t>(pit::tctrl(pit::kChannel)) = pit::kTctrlTen | pit::kTctrlTie;
  }
};
constexpr Hw kHw{};

// What the interrupts work on, set once by start() before their lines are enabled; read by the interrupts. Pointers to the link's objects (SRAM_U, forever).
midi::DinEmitter* volatile g_din = nullptr;
midi::RtInRing* volatile g_rx = nullptr;
const midi::TimeRef* volatile g_ref = nullptr;
midi::DinRxStats g_rx_stats;
midi::DinTxStats g_tx_stats;
volatile bool g_started = false;

}  // namespace

bool start(midi::DinEmitter& din, midi::RtInRing& rx, const midi::TimeRef& ref) noexcept {
  if (g_started) {
    return true;
  }
  g_din = &din;
  g_rx = &rx;
  g_ref = &ref;

  // 1. Clock gates.
  at<std::uint32_t>(u4::kSimScgc1) = at<std::uint32_t>(u4::kSimScgc1) | u4::kScgc1Uart4;
  at<std::uint32_t>(pit::kSimScgc6) = at<std::uint32_t>(pit::kSimScgc6) | pit::kScgc6Pit;

  // 2. The timer: module on, channel 0 stopped, flag clear.
  at<std::uint32_t>(pit::kMcr) = pit::kMcrEnable;
  at<std::uint32_t>(pit::tctrl(pit::kChannel)) = 0;
  at<std::uint32_t>(pit::tflg(pit::kChannel)) = pit::kTflgTif;

  // 3. UART4 off and configured.
  u4_reg(u4::kOffC2) = 0;
  u4_reg(u4::kOffBdh) = u4::kBdh;
  u4_reg(u4::kOffBdl) = u4::kBdl;
  u4_reg(u4::kOffC4) = u4::kC4;
  u4_reg(u4::kOffC1) = 0;
  u4_reg(u4::kOffC3) = 0;
  u4_reg(u4::kOffPfifo) = 0;

  // 4. Pins and enable.
  regs::port_pcr(board::kMidiRx) = u4::kPcrRx;
  u4_reg(u4::kOffC2) = u4::kC2Run;
  const std::uint8_t s1 = u4_reg(u4::kOffS1);  // S1 then D drops a byte and the flags the line held (a read of a volatile register is the access)
  const std::uint8_t d = u4_reg(u4::kOffD);
  (void)s1;
  (void)d;
  regs::port_pcr(board::kMidiTx) = u4::kPcrTx;

  // 5. The interrupt lines, last.
  asm volatile("dsb" ::: "memory");
  regs::nvic_icpr(kRxIrq) = regs::irq_bit(kRxIrq);
  regs::nvic_icpr(kPitIrq) = regs::irq_bit(kPitIrq);
  regs::nvic_iser(kRxIrq) = regs::irq_bit(kRxIrq);
  regs::nvic_iser(kPitIrq) = regs::irq_bit(kPitIrq);
  g_started = true;
  return true;
}

bool started() noexcept { return g_started; }

void kick() noexcept {
  if (g_started) {
    regs::nvic_stir() = kPitIrq;
  }
}

const midi::DinRxStats& rx_stats() noexcept { return g_rx_stats; }
const midi::DinTxStats& tx_stats() noexcept { return g_tx_stats; }

}  // namespace o3sdk::platform::mk66::midi_io

// UART4 receive (vector 16 + 66, priority 48): every byte is stamped and queued. SRAM_L: nothing it calls is in flash (tools/check_ramfunc.py).
extern "C" O3SDK_RAMFUNC_L void UART4_RX_TX_IRQHandler() {
  namespace io = o3sdk::platform::mk66::midi_io;
  o3sdk::platform::mk66::midi::RtInRing* const rx = io::g_rx;
  if (rx == nullptr) {
    return;
  }
  o3sdk::platform::mk66::midi::DinRx::isr(io::kHw, *rx, io::g_rx_stats);
}

// PIT channel 0 (vector 16 + 48, priority 96): the emitter. Entered by the compare and by the render's software pend.
extern "C" O3SDK_RAMFUNC_L void PIT0_IRQHandler() {
  namespace io = o3sdk::platform::mk66::midi_io;
  o3sdk::platform::mk66::midi::DinEmitter* const din = io::g_din;
  const o3sdk::platform::mk66::midi::TimeRef* const ref = io::g_ref;
  if (din == nullptr || ref == nullptr) {
    io::kHw.timer_stop();
    return;
  }
  o3sdk::platform::mk66::midi::DinTx::on_irq(io::kHw, *din, *ref, io::g_tx_stats);
}
