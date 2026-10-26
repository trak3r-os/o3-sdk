// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The DIN MIDI interrupts' logic (OS-005c2; ADR 0013 D1 rows "UART4 RX" (priority 48) and "MIDI-out timer compare, one PIT" (priority 96), D5), written
// against a register policy so that the same code runs on the MK66's UART4 and PIT0 (platform/mk66/src/midi_din.cpp) and on the fake of the host tests
// (the pattern of usb/device.h). What is here is WHAT each interrupt does; the registers, their addresses and the pin are the platform's.
//
// ---- RX: DinRx::isr, UART4 receive interrupt, priority kMidiUart (48) ----------------------------------------------------------------------------
// Stamps every byte with the cycle counter and pushes `(stamp, byte, Port::Din)` into the port's `midiRt` ring (RtInRing): the render drains it at block start
// (midi/in.h). The stamp is the counter read as the FIRST thing the interrupt does, so it carries the interrupt's entry latency, which is the platform's to
// bound (only the audio DMA and SysTick can preempt it; HW-B measures it). MidiIn subtracts the byte time (320 us: a byte raises the interrupt at its end).
// A byte with a framing, noise or parity error or an overrun is dropped and counted (reading S1 and then D clears the flags); the parser sees the gap. The ring
// is single producer: this interrupt is its ONE producer (the USB interrupt has a ring of its own: midi/usb_bind.h), the render its one consumer. Bounded: at
// most kMaxBytesPerIsr bytes (the receiver has one data register: more than one pending means an interrupt that was held off for a byte time); no allocation,
// no blocking, no printf.
//
// ---- TX: DinTx::on_irq, PIT compare interrupt, priority kMidiOutTimer (96) -----------------------------------------------------------------------
// ONE interrupt does everything on the transmit side, and it is the ONE consumer of the DinEmitter's rings (single-consumer: poll() and next_wake() keep
// state that only this context may touch):
//   1. stop the timer (a one-shot: it is re-armed below, never free-running);
//   2. read the counter and map it to the audio timeline (TimeRef); with no time base yet nothing is due: look again in a millisecond;
//   3. if the UART's data register is empty, ask the emitter what goes next at this instant (DinEmitter::poll) and write that byte;
//   4. ask when the emitter wants to be woken (next_wake) and arm the timer for it, or leave it stopped when both queues are empty.
// Entered also by a software pend: the render pends the line after it has handed items to the ring (platform kick()), which is how a byte that is due
// soon gets a timer that was not armed for it (the timer may be stopped). The pend is idempotent: the handler only decides from the queues and the clock.
//
// COMMIT DEPTH 1 (din.h): the next byte of a message is written when the previous has gone (the emitter's model, byte = 57,600 cycles), so the compare time
// of the next wake is the end of the byte in flight. The UART has a data register in front of its shift register (the FIFO is switched off): a write made while
// the shifter is still sending (a few microseconds early: baud-clock alignment of the real start against the model) waits in the data register and goes out
// with no gap, so an early timer costs nothing. TDRE low at entry (a write made in the last few microseconds: it cannot happen with depth 1 and a model that
// holds) postpones the whole step by kNotReadyTicks without popping anything from the emitter.
//
// Real time. Bounded and short: one poll, one next_wake, a handful of register accesses; the emitter's functions are O3SDK_AUDIO_HOT (SRAM_L) and this body is
// folded into the platform's interrupt handler (O3SDK_AUDIO_INLINE), also SRAM_L; nothing here calls flash (tools/check_ramfunc.py).
//
// Register policy of RX:
//   std::uint32_t cycles();            // the free-running cycle counter
//   std::uint8_t status();             // UART S1
//   std::uint8_t data();               // UART D (reading clears RDRF; S1 must have been read first)
// Register policy of TX (also `cycles()`):
//   static constexpr unsigned kCyclesPerTickShift;  // log2 of the CPU cycles per timer tick (core / bus clock = 2 on the MK66: 1): a shift, so that the
//                                                   // interrupt never divides (the 64-bit division is a libgcc call in flash)
//   bool tx_ready();                   // S1.TDRE: the data register is empty
//   void tx_write(std::uint8_t byte);  // D
//   void timer_stop();                 // disable the channel and clear its flag (one-shot semantics)
//   void timer_start(std::uint32_t ticks);  // load the channel (ticks >= 1) and enable it with its interrupt

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/platform/mk66/midi/din.h>
#include <o3sdk/platform/mk66/midi/in.h>
#include <o3sdk/platform/mk66/midi/time_ref.h>

#include <cstdint>

namespace o3sdk::platform::mk66::midi {

// UART S1 (K66 RM "UART": status register 1; the Teensy core's names UART_S1_*).
namespace uart_s1 {
inline constexpr std::uint8_t kTdre = 0x80;
inline constexpr std::uint8_t kTc = 0x40;
inline constexpr std::uint8_t kRdrf = 0x20;
inline constexpr std::uint8_t kIdle = 0x10;
inline constexpr std::uint8_t kOverrun = 0x08;
inline constexpr std::uint8_t kNoise = 0x04;
inline constexpr std::uint8_t kFraming = 0x02;
inline constexpr std::uint8_t kParity = 0x01;
inline constexpr std::uint8_t kErrors = kOverrun | kNoise | kFraming | kParity;
}  // namespace uart_s1

struct DinRxStats {
  std::uint32_t isrs = 0;
  std::uint32_t bytes = 0;         // pushed
  std::uint32_t errors = 0;        // dropped for a framing, noise, parity error
  std::uint32_t overruns = 0;      // the receiver lost a byte (OR): the interrupt came too late
  std::uint32_t ring_full = 0;     // dropped: the render has not drained the ring
  std::uint32_t multi = 0;         // interrupts that found more than one byte
};

struct DinTxStats {
  std::uint32_t irqs = 0;
  std::uint32_t bytes = 0;
  std::uint32_t rt_bytes = 0;      // real-time bytes among them
  std::uint32_t no_time = 0;       // woken before the render published a time base
  std::uint32_t not_ready = 0;     // TDRE low: postponed
  std::uint32_t timer_floor = 0;   // a wake nearer than kMinTicks, armed at the floor
  std::uint32_t timer_ceiling = 0; // a wake farther than kMaxTicks, armed at the ceiling (looks again then)
  std::uint64_t rt_late_max = 0;   // cycles: the worst start - due of a real-time byte (din.h Tx): interrupt latency + timer granularity + a byte in flight
};

inline constexpr std::uint32_t kMaxBytesPerIsr = 4;

struct DinRx {
  template <typename Hw>
  static O3SDK_AUDIO_INLINE void isr(Hw& hw, RtInRing& ring, DinRxStats& st) noexcept {
    const std::uint32_t stamp = hw.cycles();  // first: the byte's time
    ++st.isrs;
    for (std::uint32_t i = 0; i < kMaxBytesPerIsr; ++i) {
      const std::uint8_t s1 = hw.status();
      if ((s1 & uart_s1::kRdrf) == 0u) {
        break;
      }
      const std::uint8_t byte = hw.data();  // S1 then D: clears RDRF and any error flag S1 showed
      if (i != 0u) {
        ++st.multi;
      }
      if ((s1 & uart_s1::kOverrun) != 0u) {
        ++st.overruns;  // an earlier byte was lost; this one is whole
      }
      if ((s1 & (uart_s1::kNoise | uart_s1::kFraming | uart_s1::kParity)) != 0u) {
        ++st.errors;
        continue;
      }
      if (ring.push(RtItem{stamp, byte, static_cast<std::uint8_t>(Port::Din), 0})) {
        ++st.bytes;
      } else {
        ++st.ring_full;
      }
    }
  }
};

struct DinTx {
  // Timer limits, in ticks of the policy's timer: at least ~1 us (an ISR entry is about that), at most 100 ms (a wake farther away than the
  // longest port offset is looked at again then; the handler recomputes it from the queues every time).
  static constexpr std::uint32_t kMinTicks = 90;
  static constexpr std::uint32_t kMaxTicks = 9000000;
  static constexpr std::uint32_t kNotReadyTicks = 180;   // 2 us
  static constexpr std::uint32_t kNoTimeTicks = 90000;   // 1 ms

  // The tick count for a wake `delta_cycles` away, clamped to [kMinTicks, kMaxTicks]; `shift` is the policy's kCyclesPerTickShift.
  static constexpr O3SDK_AUDIO_INLINE std::uint32_t ticks_for(std::uint64_t delta_cycles, unsigned shift) noexcept {
    const std::uint64_t t = delta_cycles >> shift;
    return t < kMinTicks ? kMinTicks : (t > kMaxTicks ? kMaxTicks : static_cast<std::uint32_t>(t));
  }

  template <typename Hw>
  static O3SDK_AUDIO_INLINE void on_irq(Hw& hw, DinEmitter& em, const TimeRef& ref, DinTxStats& st) noexcept {
    hw.timer_stop();
    const std::uint32_t entry = hw.cycles();
    ++st.irqs;
    std::uint64_t now = 0;
    if (!ref.to_timeline(entry, now)) {
      ++st.no_time;
      hw.timer_start(kNoTimeTicks);
      return;
    }
    if (!hw.tx_ready()) {
      ++st.not_ready;
      hw.timer_start(kNotReadyTicks);
      return;
    }
    const DinEmitter::Tx tx = em.poll(now);
    if (tx.valid) {
      hw.tx_write(tx.byte);
      ++st.bytes;
      if (tx.realtime) {
        ++st.rt_bytes;
        const std::uint64_t late = tx.start - tx.due;
        if (late > st.rt_late_max) {
          st.rt_late_max = late;
        }
      }
    }
    const std::uint64_t wake = em.next_wake(now);
    if (wake == kNever) {
      return;  // both queues empty and no message open: the timer stays stopped until the render pends the line
    }
    const std::uint64_t raw = (wake > now ? wake - now : 0u) >> Hw::kCyclesPerTickShift;
    if (raw < kMinTicks) {
      ++st.timer_floor;
    } else if (raw > kMaxTicks) {
      ++st.timer_ceiling;
    }
    hw.timer_start(ticks_for(wake > now ? wake - now : 0u, Hw::kCyclesPerTickShift));
  }
};

}  // namespace o3sdk::platform::mk66::midi
