// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI in, the timing side (midi/in.h, OS-005c): stamped bytes from the ISR through the midiRt ring into the clock-follow loop, the port's input latency,
// Start / Continue waiting for the next clock, Stop and SPP at once, the other port ignored, bytes that are not timing handed back, outputs that are full, the PAR-119
// hook. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3sdk/core/qtime.h>
#include <o3sdk/platform/mk66/midi/in.h>

#include <cstdint>

#include "support/need.h"

namespace midi = o3sdk::platform::mk66::midi;
namespace core = o3sdk::core;

namespace {

constexpr std::uint32_t kPeriodCycles = 3750000;  // a clock at 120 BPM: 20.83 ms
constexpr std::uint32_t kDinLatency = 57600;
constexpr std::uint32_t kUsbLatency = 90000;

struct Rig {
  midi::RtInRing ring;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  midi::TransportEvent ev[8];
  midi::RtItem pass[16];
  Rig() {
    map.set_ref(0, 0);
    dll.reset(0, core::q32_from_cycles(kPeriodCycles));
    midi::MidiIn::Config c;
    c.clock_enabled = true;
    c.clock_port = midi::Port::Din;
    in.configure(c);
  }
  void push(std::uint32_t cycles, std::uint8_t byte, midi::Port port = midi::Port::Din) {
    CHECK(ring.push(midi::RtItem{cycles, byte, static_cast<std::uint8_t>(port), 0}));
  }
  midi::MidiIn::Drained drain(std::uint32_t max_items = 256, std::uint32_t ev_cap = 8, std::uint32_t pass_cap = 16) {
    return in.drain(ring, map, dll, max_items, ev, ev_cap, pass, pass_cap);
  }
};

// The stamp of a clock sent at `true_cycles`: the ISR sees it complete, one byte time later.
std::uint32_t din_stamp(std::uint64_t true_cycles) { return static_cast<std::uint32_t>(true_cycles + kDinLatency); }

}  // namespace

TEST_CASE("midi in: clocks are stamped one byte late; the loop gets the time they were sent") {
  Rig r;
  for (std::uint32_t k = 0; k < 20; ++k) {
    r.push(din_stamp(1000000u + static_cast<std::uint64_t>(k) * kPeriodCycles), midi::kClock);
  }
  const midi::MidiIn::Drained d = r.drain();
  CHECK(d.items == 20u);
  CHECK(d.events == 0u);
  CHECK(d.passed == 0u);
  CHECK(r.in.stats().clocks == 20u);
  CHECK(r.dll.tick() == 19u);
  CHECK(r.dll.running());
  // the loop's time for tick 0 is the sample at which the clock was SENT: 1,000,000 cycles = 245.098 samples (the map's reference is sample 0 at cycle 0)
  const core::SQ32 t0 = r.dll.time_of_tick(0);
  const core::SQ32 want = static_cast<core::SQ32>(core::q32_from_cycles(1000000u));
  CHECK(t0 == want);
  CHECK(r.dll.period() == core::q32_from_cycles(kPeriodCycles));
  CHECK(r.dll.bpm_x100() == 12000u);
}

TEST_CASE("midi in: Start and Continue wait for the next clock, Stop and SPP do not") {
  Rig r;
  std::uint32_t cyc = 1000000;
  const auto clock = [&]() {
    r.push(din_stamp(cyc), midi::kClock);
    cyc += kPeriodCycles;
  };
  for (int i = 0; i < 14; ++i) {
    clock();  // clocks 0..13
  }
  r.push(din_stamp(cyc - 1000), midi::kStart);  // between clock 13 and 14
  midi::MidiIn::Drained d = r.drain();
  CHECK(d.events == 0u);  // armed, not yet an event
  CHECK(r.in.stats().transport == 0u);
  clock();  // clock 14: the transport begins on it
  d = r.drain();
  NEED(d.events == 1u);
  CHECK(r.ev[0].kind == midi::TransportEvent::Kind::Start);
  CHECK(r.ev[0].tick == 14u);
  CHECK(r.ev[0].port == 0u);
  // Continue after an SPP: the SPP is an event at once, Continue waits
  r.push(din_stamp(cyc), 0xF2);
  r.push(din_stamp(cyc), 0x10);
  r.push(din_stamp(cyc), 0x01);
  r.push(din_stamp(cyc), midi::kContinue);
  d = r.drain();
  NEED(d.events == 1u);
  CHECK(r.ev[0].kind == midi::TransportEvent::Kind::Spp);
  CHECK(r.ev[0].spp == (0x10 | (0x01 << 7)));
  clock();  // clock 15
  d = r.drain();
  NEED(d.events == 1u);
  CHECK(r.ev[0].kind == midi::TransportEvent::Kind::Continue);
  CHECK(r.ev[0].tick == 15u);
  // Stop is at once, and a Start that was waiting is forgotten
  r.push(din_stamp(cyc), midi::kStart);
  r.push(din_stamp(cyc), midi::kStop);
  clock();
  d = r.drain();
  NEED(d.events == 1u);
  CHECK(r.ev[0].kind == midi::TransportEvent::Kind::Stop);
  CHECK(r.in.stats().transport == 4u);
}

TEST_CASE("midi in: bytes that are not timing come back in order; a clock between the bytes of a note does not disturb it") {
  Rig r;
  r.push(din_stamp(1000), 0x90);
  r.push(din_stamp(1001), 60);
  r.push(din_stamp(1002), midi::kClock);
  r.push(din_stamp(1003), 100);
  r.push(din_stamp(1004), 0x80);
  r.push(din_stamp(1005), 60);
  r.push(din_stamp(1006), 0);
  const midi::MidiIn::Drained d = r.drain();
  NEED(d.passed == 6u);
  const std::uint8_t want[6] = {0x90, 60, 100, 0x80, 60, 0};
  for (std::uint32_t i = 0; i < 6; ++i) {
    CHECK(r.pass[i].byte == want[i]);
  }
  CHECK(r.pass[0].cycles == din_stamp(1000));  // the stamp is the ISR's, untouched
  CHECK(r.in.stats().clocks == 1u);
}

TEST_CASE("midi in: the timing of the port that is not followed is ignored, and everything is ignored with clock in off") {
  Rig r;
  for (std::uint32_t k = 0; k < 5; ++k) {
    r.push(1000000u + k * kPeriodCycles + kUsbLatency, midi::kClock, midi::Port::Usb);
  }
  r.push(5000, midi::kStart, midi::Port::Usb);
  r.push(5001, 0x90, midi::Port::Usb);
  midi::MidiIn::Drained d = r.drain();
  CHECK(d.events == 0u);
  CHECK(r.in.stats().ignored_port == 6u);
  CHECK(r.in.stats().clocks == 0u);
  CHECK(r.dll.has_phase() == false);
  CHECK(d.passed == 1u);  // the note of the USB port is not timing: it is handed back
  CHECK(r.pass[0].port == static_cast<std::uint8_t>(midi::Port::Usb));
  // the other port is followed
  midi::MidiIn::Config c = r.in.config();
  c.clock_port = midi::Port::Usb;
  r.in.configure(c);
  r.push(1000000u + kUsbLatency, midi::kClock, midi::Port::Usb);
  r.push(1000000u + kPeriodCycles + kUsbLatency, midi::kClock, midi::Port::Usb);
  d = r.drain();
  CHECK(r.in.stats().clocks == 2u);
  CHECK(r.dll.time_of_tick(0) == static_cast<core::SQ32>(core::q32_from_cycles(1000000u)));  // the USB latency is the half millisecond
  // clock in off: the timing bytes are neither followed nor passed on
  c.clock_enabled = false;
  r.in.configure(c);
  r.push(9000, midi::kClock, midi::Port::Usb);
  d = r.drain();
  CHECK(d.events == 0u);
  CHECK(d.passed == 0u);
  CHECK(r.in.stats().clocks == 2u);
}

TEST_CASE("midi in: full outputs hold the item for the next call; the work per call is bounded; the PAR-119 hook shifts the start") {
  Rig r;
  r.push(din_stamp(1000), midi::kStop);
  r.push(din_stamp(1001), midi::kStop);
  r.push(din_stamp(1002), midi::kStop);
  midi::MidiIn::Drained d = r.drain(256, 2, 16);  // room for two events
  CHECK(d.events == 2u);
  CHECK(d.items == 2u);
  d = r.drain(256, 2, 16);  // the third was held, not lost
  CHECK(d.events == 1u);
  r.push(din_stamp(2000), 0x90);
  r.push(din_stamp(2001), 60);
  r.push(din_stamp(2002), 100);
  d = r.drain(256, 8, 2);  // room for two passed bytes
  CHECK(d.passed == 2u);
  CHECK(r.in.stats().pass_overflow == 1u);
  d = r.drain(256, 8, 2);
  CHECK(d.passed == 1u);
  CHECK(r.pass[0].byte == 100);
  // bounded work
  for (int i = 0; i < 40; ++i) {
    r.push(din_stamp(3000 + static_cast<std::uint32_t>(i)), 0x90);
  }
  d = r.drain(16, 8, 16);
  CHECK(d.items == 16u);
  d = r.drain(256, 8, 16);
  CHECK(d.items == 16u);  // the pass array (16) is the limit now
  d = r.drain(256, 8, 16);
  CHECK(d.items == 8u);
  // PAR-119: +2 pulses on the clock number of a Start; -3 pulses cannot go below clock 0
  Rig q;
  midi::MidiIn::Config c = q.in.config();
  c.start_pulses = 2;
  q.in.configure(c);
  q.push(din_stamp(1000000), midi::kStart);
  q.push(din_stamp(1000000 + 10), midi::kClock);
  q.drain();
  CHECK(q.ev[0].kind == midi::TransportEvent::Kind::Start);
  CHECK(q.ev[0].tick == 2u);  // the first clock is clock 0, plus two
  Rig z;
  c.start_pulses = -3;
  z.in.configure(c);
  z.push(din_stamp(1000000), midi::kContinue);
  z.push(din_stamp(1000010), midi::kClock);
  z.drain();
  CHECK(z.ev[0].tick == 0u);
}
