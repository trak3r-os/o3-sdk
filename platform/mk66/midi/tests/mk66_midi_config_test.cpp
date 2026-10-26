// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MIDI drivers' register values (midi_config.h, OS-005c2): the baud divisor from the clock tree and what baud rate it makes, the UART4 and PIT register
// addresses and bits against NXP's device header and the Teensy core's tables, the pin and clock-gate numbers, the interrupt lines' rows in the priority table (48 for the receiver,
// 96 for the emitter's timer, between the USB interrupt and the render), the PIT tick arithmetic, the audio lead. Host and Cortex-M4F (`qemu_mk66`). Nothing here
// touches a register: the wire, the interrupt latency and the timer are the HW-B list of the spec.
#include <doctest/doctest.h>

#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/irq.h>
#include <o3sdk/platform/mk66/midi/din.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/irq_map.h>
#include <o3sdk/platform/mk66/midi_config.h>
#include <o3sdk/platform/mk66/resources.h>

#include <cstdint>

namespace mk66 = o3sdk::platform::mk66;
namespace cfg = o3sdk::platform::mk66::midi_cfg;
namespace irq = o3sdk::hal::irq;

TEST_CASE("mk66 midi: UART4 divides the 90 MHz bus clock to exactly 31,250 baud") {
  CHECK(mk66::clock::kBusHz == 90000000u);
  CHECK(cfg::uart4::kDivisor32 == 5760u);  // the formula (F_BUS * 2 + baud / 2) / baud, in 1/32 units
  CHECK(cfg::uart4::kSbr == 180u);
  CHECK(cfg::uart4::kBrfa == 0u);
  CHECK(cfg::uart4::kBdh == 0u);
  CHECK(cfg::uart4::kBdl == 180u);
  CHECK(cfg::uart4::kC4 == 0u);
  // The rate the registers make, in 1/32 baud: 31,250 exactly, no error against a MIDI device.
  CHECK(cfg::uart4::actual_baud_x32(mk66::clock::kBusHz, cfg::uart4::kSbr, cfg::uart4::kBrfa) == 31250u);
  // The same formula at other clocks, to show the split into SBR and BRFA is right: a 60 MHz bus gives SBR 120, a 72 MHz one SBR 144, a whole-MHz clock is always exact
  // (31,250 = 2 * 5^6 divides it), and an 11.0592 MHz one needs a fractional adjust.
  CHECK((cfg::uart4::divisor32(60000000u, 31250u) >> 5) == 120u);
  CHECK(cfg::uart4::divisor32(72000000u, 31250u) == 4608u);
  CHECK((cfg::uart4::divisor32(48000000u, 31250u) & 0x1Fu) == 0u);
  CHECK((cfg::uart4::divisor32(11059200u, 31250u) >> 5) == 22u);
  CHECK((cfg::uart4::divisor32(11059200u, 31250u) & 0x1Fu) == 4u);
}

TEST_CASE("mk66 midi: the UART4 registers, bits, clock gate and pin values are the ones of the K66") {
  // KINETISK_UART4 0x400EA000; the register offsets of the K66 UART.
  CHECK(cfg::uart4::kBase == 0x400EA000u);
  CHECK(cfg::uart4::kOffBdh == 0u);
  CHECK(cfg::uart4::kOffBdl == 1u);
  CHECK(cfg::uart4::kOffC1 == 2u);
  CHECK(cfg::uart4::kOffC2 == 3u);
  CHECK(cfg::uart4::kOffS1 == 4u);
  CHECK(cfg::uart4::kOffD == 7u);
  CHECK(cfg::uart4::kOffC4 == 0xAu);
  CHECK(cfg::uart4::kOffPfifo == 0x10u);
  // C2: TE 0x08, RE 0x04, RIE 0x20 (kinetis.h); TIE and TCIE are not used.
  CHECK(cfg::uart4::kC2Run == 0x2Cu);
  CHECK((cfg::uart4::kC2Run & 0xC0u) == 0u);
  // SIM_SCGC1[UART4] (kinetis.h) and SIM_SCGC6[PIT].
  CHECK(cfg::uart4::kSimScgc1 == 0x40048028u);
  CHECK(cfg::uart4::kScgc1Uart4 == 0x00000400u);
  CHECK(cfg::pit::kSimScgc6 == 0x4004803Cu);
  CHECK(cfg::pit::kScgc6Pit == 0x00800000u);
  // Pin control: ALT3 on both pins of port E; RX pull-up (PE | PS) and passive filter (PFE), TX high drive (DSE) and slew-rate limited (SRE).
  CHECK(cfg::uart4::kPcrRx == (0x300u | 0x10u | 0x02u | 0x01u));
  CHECK(cfg::uart4::kPcrTx == (0x300u | 0x40u | 0x04u));
  const mk66::Pin tx{mk66::Port::E, 24};  // Teensy pin 33
  const mk66::Pin rx{mk66::Port::E, 25};  // Teensy pin 34
  CHECK(mk66::board::kMidiTx == tx);
  CHECK(mk66::board::kMidiRx == rx);
  CHECK(mk66::board::kMidiPinMux == 3u);
  CHECK_FALSE(mk66::board::kMidiRx == mk66::board::kMidiTx);
  // PTE25 is no other claimed pin of the board.
  CHECK_FALSE(mk66::board::kMidiRx == mk66::board::kInputSourceSelect);
  CHECK_FALSE(mk66::board::kMidiRx == mk66::board::kRadioReset);
  CHECK_FALSE(mk66::board::kMidiRx == mk66::board::kI2sRxd);
  CHECK_FALSE(mk66::board::kMidiRx == mk66::board::kI2sMclk);
}

TEST_CASE("mk66 midi: the PIT registers and the tick arithmetic: a tick is two cycles, a byte 28,800 ticks, channel 0 is the emitter's") {
  // PIT: MCR 0x40037000, LDVAL0 +0x100, CVAL0 +0x104, TCTRL0 +0x108, TFLG0 +0x10C, 16 bytes a channel.
  CHECK(cfg::pit::kBase == 0x40037000u);
  CHECK(cfg::pit::kMcr == 0x40037000u);
  CHECK(cfg::pit::ldval(0) == 0x40037100u);
  CHECK(cfg::pit::tctrl(0) == 0x40037108u);
  CHECK(cfg::pit::tflg(0) == 0x4003710Cu);
  CHECK(cfg::pit::ldval(3) == 0x40037130u);
  CHECK(cfg::pit::tctrl(3) == 0x40037138u);
  CHECK(cfg::pit::kTctrlTen == 1u);
  CHECK(cfg::pit::kTctrlTie == 2u);
  CHECK(cfg::pit::kTflgTif == 1u);
  CHECK(cfg::pit::kMcrEnable == 0u);
  CHECK(cfg::pit::kChannel == 0u);
  CHECK(mk66::resources::kPit[cfg::pit::kChannel].owner == mk66::resources::PitOwner::MidiOutEmitter);
  CHECK_FALSE(mk66::resources::kPit[cfg::pit::kChannel].claimed);  // o3sdk: resources.h keeps PIT0 reserved, unclaimed, until the MIDI driver is in the image (slice MD flips both; track3r-os: CHECK(...claimed))
  CHECK(cfg::pit::kCyclesPerTickShift == 1u);
  CHECK((mk66::clock::kCoreHz >> cfg::pit::kCyclesPerTickShift) == mk66::clock::kBusHz);
  CHECK(cfg::pit::kByteTicks == 28800u);
  CHECK((cfg::pit::kByteTicks << cfg::pit::kCyclesPerTickShift) == o3sdk::platform::mk66::midi::din::kByteCycles);
  // 1 us is 90 ticks (the interrupt's floor), 100 ms is 9 million: LDVAL is 32 bits, the ceiling is far inside it.
  CHECK(mk66::clock::kBusHz / 1000000u == 90u);
  CHECK(mk66::clock::kBusHz / 10u == 9000000u);
}

TEST_CASE("mk66 midi: the DIN lines have the rows of the priority table: receiver 48, USB 64, emitter timer 96, render 128") {
  CHECK(mk66::role_of(mk66::Irq::UART4_RX_TX) == irq::Role::MidiUart);
  CHECK(mk66::role_of(mk66::Irq::UART4_ERR) == irq::Role::MidiUart);
  CHECK(mk66::role_of(mk66::resources::pit_irq(cfg::pit::kChannel)) == irq::Role::MidiOutTimer);
  CHECK(irq::priority(irq::Role::MidiUart) == 48u);
  CHECK(irq::priority(irq::Role::Usb) == 64u);
  CHECK(irq::priority(irq::Role::MidiOutTimer) == 96u);
  CHECK(irq::priority(irq::Role::Render) == 128u);
  // The three interrupts that read the render's time base (midi/time_ref.h) are all above the render, the only writer of it.
  CHECK(irq::preempts(irq::Role::MidiUart, irq::Role::Render));
  CHECK(irq::preempts(irq::Role::Usb, irq::Role::Render));
  CHECK(irq::preempts(irq::Role::MidiOutTimer, irq::Role::Render));
  // The receiver can preempt the USB interrupt, the emitter's timer cannot: a USB pass delays a clock byte by the pass (bounded, HW-B).
  CHECK(irq::preempts(irq::Role::MidiUart, irq::Role::Usb));
  CHECK_FALSE(irq::preempts(irq::Role::MidiOutTimer, irq::Role::Usb));
  // The vector table's names of the two handlers the driver defines: UART4 RX/TX is IRQ 66, PIT channel 0 is IRQ 48.
  CHECK(static_cast<unsigned>(mk66::Irq::UART4_RX_TX) == 66u);
  CHECK(static_cast<unsigned>(mk66::Irq::PIT0) == 48u);
}

TEST_CASE("mk66 midi: the audio lead is the TX ring's render-ahead, two blocks with the three slots") {
  CHECK(cfg::kAudioLeadCycles == (o3sdk::hal::audio::kTxSlots - 1u) * o3sdk::hal::audio::kCyclesPerBlock);
  CHECK(o3sdk::hal::audio::kTxSlots == 3u);  // OS-CPU10 slice 3
  CHECK(cfg::kAudioLeadCycles == 2u * o3sdk::hal::audio::kCyclesPerBlock);  // 522,240 cycles: 2.9 ms
}
