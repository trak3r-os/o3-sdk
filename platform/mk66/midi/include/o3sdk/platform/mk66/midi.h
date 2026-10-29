// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's MIDI drivers (OS-005c2): the DIN port (UART4 receive, UART4 transmit driven by the PIT-compare emitter) and the binding of the USB MIDI endpoint to the
// same timing core (midi/). Implemented in the MK66 image only (src/midi_din.cpp, usb/src/usb0.cpp); the declarations compile everywhere so that the header check
// passes on every platform. The logic of every interrupt is in midi/din_io.h and midi/usb_bind.h (host-tested against fakes); what is here is the registers.
//
// What runs where (ADR 0013 D1, the priority table of hal/irq.h; every line below is in the Role row irq_map.h gives it):
//   UART4 RX_TX    priority 48 (kMidiUart)      DinRx::isr: stamp the byte, push it into the DIN midiRt ring. The ONE producer of that ring.
//   PIT0           priority 96 (kMidiOutTimer)  DinTx::on_irq: the ONE consumer of the DinEmitter's rings; writes at most one byte to UART4 and re-arms the one-shot timer. Also
//                                               entered by a software pend (kick()) when the render has handed the emitter something new.
//   USB0           priority 64 (kUsb)           the USB device's interrupt with the MIDI binding around it (usb0.cpp): SOF -> UsbMidiOut::frame -> the to-host ring before the
//                                               device logic arms the IN buffer; the from-host ring -> stamped bytes in the USB midiRt ring after it.
//   render         priority 128 (kRender)       the producer of the emitter's rings and of UsbMidiOut, the consumer of both midiRt rings, the writer of the TimeRef.
// The three interrupts are above the render, which TimeRef's protocol (midi/time_ref.h) requires; none of them allocates, blocks, prints or loops without a bound.
//
// Order of bring-up: main thread, after the clock tree, the interrupt priorities (boot::init_interrupts) and the composition root's MidiLink; start() then enables
// the lines last. Calling it twice is harmless.

#include <o3sdk/platform/mk66/midi/din_io.h>
#include <o3sdk/platform/mk66/midi/usb_bind.h>

namespace o3sdk::platform::mk66::midi_io {

// Clock gates, the UART (31,250 baud, 8N1, FIFO off), both pins, the PIT module, the NVIC lines. `rx`: the DIN port's midiRt ring (its one producer from now on),
// `din`: the emitter, `ref`: the time base the render publishes.
bool start(o3sdk::platform::mk66::midi::DinEmitter& din, o3sdk::platform::mk66::midi::RtInRing& rx, const o3sdk::platform::mk66::midi::TimeRef& ref) noexcept;
bool started() noexcept;

// Pends the emitter's interrupt (render context: the NVIC's software trigger). The interrupt decides from the queues, so a pend is always safe and idempotent.
void kick() noexcept;

// Counters, for `diag` and the HW-B session: single words, one writer each (the interrupt), read as they are.
const o3sdk::platform::mk66::midi::DinRxStats& rx_stats() noexcept;
const o3sdk::platform::mk66::midi::DinTxStats& tx_stats() noexcept;

}  // namespace o3sdk::platform::mk66::midi_io
