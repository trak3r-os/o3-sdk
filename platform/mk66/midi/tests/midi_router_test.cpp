// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MIDI out router (midi/out.h, OS-005c): sequencer events to bytes and due times, the CC table, the port offsets, the staging order, the hand-over to the
// ports, the hang bookkeeping (stop, panic, a muted channel), the suppression of an export. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3sdk/core/qtime.h>
#include <o3sdk/platform/mk66/midi/out.h>

#include <cstdint>

#include "support/need.h"
#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
namespace audio = o3sdk::platform::mk66::audio;
namespace core = o3sdk::core;
using audio::EventKind;
using o3sdk::test::Rng;

namespace {

audio::SeqEvent ev(EventKind k, std::uint16_t offset, std::uint8_t a = 0, std::uint8_t b = 0, std::uint8_t c = 0, std::int32_t value = 0) {
  audio::SeqEvent e{};
  e.offset = offset;
  e.kind = k;
  e.track = 0;
  e.a = a;
  e.b = b;
  e.c = c;
  e.value = value;
  return e;
}

struct Wire {
  std::uint8_t bytes[64];
  std::uint64_t start[64];
  std::uint32_t n = 0;
  std::uint32_t total = 0;  // every byte, whether or not the arrays had room
};

// Drains a DinEmitter completely, recording the bytes and their start times.
Wire drain(midi::DinEmitter& em, std::uint64_t now = 0) {
  Wire w;
  for (int i = 0; i < 200; ++i) {
    const std::uint64_t t = em.next_wake(now);
    if (t == midi::kNever) {
      break;
    }
    now = t;
    const midi::DinEmitter::Tx tx = em.poll(now);
    if (tx.valid) {
      ++w.total;
      if (w.n < 64u) {
        w.bytes[w.n] = tx.byte;
        w.start[w.n] = tx.start;
        ++w.n;
      }
    }
  }
  return w;
}

constexpr std::uint64_t kBlockEnd(std::uint64_t block_start) { return core::cycles_from_samples(block_start + 64u); }

}  // namespace

TEST_CASE("midi router: a note becomes three bytes due at its sample, on the DIN port and as one USB word") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin | midi::kPortUsb;
  out.configure(cfg);
  out.attach(&din, &usb);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 5, 60, 100, 2), 1000));
  CHECK(out.staged() == 2u);  // one for each port
  out.flush(kBlockEnd(1000));
  CHECK(out.staged() == 0u);
  const Wire w = drain(din);
  NEED(w.n == 3u);
  CHECK(w.bytes[0] == 0x92);
  CHECK(w.bytes[1] == 60);
  CHECK(w.bytes[2] == 100);
  CHECK(w.start[0] == core::cycles_from_samples(1005));  // due at sample 1005, in cycles
  std::uint32_t words[16];
  CHECK(usb.frame(core::cycles_from_samples(1005) + 180000u, words, 16) == 1u);
  CHECK(words[0] == midi::usb_event(0, midi::note_on(2, 60, 100)));
  CHECK(out.book().count(2, 60) == 1);
  // its off
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 60, 0, 2), 2000));
  out.flush(kBlockEnd(2000));
  const Wire w2 = drain(din, w.start[2] + 100000);
  NEED(w2.n == 3u);
  CHECK(w2.bytes[0] == 0x82);
  CHECK(out.book().owed() == 0u);
  // a MIDI off that is not owed is not sent
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 60, 0, 2), 3000));
  CHECK(out.stats().stray_offs == 1u);
  CHECK(out.staged() == 0u);
  // other kinds are not the router's
  CHECK_FALSE(out.send(ev(EventKind::NoteOn, 0), 0));
  CHECK_FALSE(out.send(ev(EventKind::Step, 0), 0));
}

TEST_CASE("midi router: the clock is placed to a fraction of a sample, as a real-time byte, only on the clock ports") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin;
  cfg.clock_ports = midi::kPortDin;  // not USB
  out.configure(cfg);
  out.attach(&din, &usb);
  // sample 5001 - 0.5 (the nearest sample is 5001, the exact time half a sample before it): 5000.5 * 4080 cycles
  CHECK(out.send(ev(EventKind::MidiClock, 0, 0, 0, 0, static_cast<std::int32_t>(0x80000000u)), 5001));
  out.flush(kBlockEnd(5000));
  const Wire w = drain(din);
  NEED(w.n == 1u);
  CHECK(w.bytes[0] == midi::kClock);
  CHECK(w.start[0] == core::cycles_from_samples(5000) + core::cycles_from_samples(1) / 2u);  // 5000.5 samples
  std::uint32_t words[16];
  CHECK(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 0u);
  // start, continue, stop and SPP
  CHECK(out.send(ev(EventKind::MidiStart, 0), 6000));
  CHECK(out.send(ev(EventKind::MidiContinue, 0), 6001));
  CHECK(out.send(ev(EventKind::MidiStop, 0), 6002));
  CHECK(out.send(ev(EventKind::MidiSpp, 0, 0, 0, 0, 0x1234), 6003));
  out.flush(kBlockEnd(6003));
  const Wire w2 = drain(din, w.start[0] + 1);
  NEED(w2.n == 6u);
  CHECK(w2.bytes[0] == midi::kStart);
  CHECK(w2.bytes[1] == midi::kContinue);
  CHECK(w2.bytes[2] == midi::kStop);
  CHECK(w2.bytes[3] == 0xF2);
  CHECK(w2.bytes[4] == (0x1234 & 0x7F));
  CHECK(w2.bytes[5] == ((0x1234 >> 7) & 0x7F));
}

TEST_CASE("midi router: CC numbers from the table, the default channel, unassigned slots send nothing; program change") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.cc_channel = 9;
  cfg.cc_number[0] = 74;
  cfg.cc_number[5] = 11;
  out.configure(cfg);
  out.attach(&din, nullptr);
  CHECK(out.send(ev(EventKind::MidiCc, 0, 0, 99, audio::kChannelDefault), 100));  // CC A: number 74 on channel 10 (index 9)
  CHECK(out.send(ev(EventKind::MidiCc, 0, 5, 7, 4), 101));                       // CC F on channel 5
  CHECK(out.send(ev(EventKind::MidiCc, 0, 1, 7, 4), 102));                       // CC B: unassigned
  CHECK(out.send(ev(EventKind::MidiProgram, 0, 33, 0, audio::kChannelDefault), 103));
  CHECK(out.stats().dropped_cc == 1u);
  out.flush(kBlockEnd(103));
  const Wire w = drain(din);
  NEED(w.n == 3u + 3u + 2u);
  CHECK(w.bytes[0] == 0xB9);
  CHECK(w.bytes[1] == 74);
  CHECK(w.bytes[2] == 99);
  CHECK(w.bytes[3] == 0xB4);
  CHECK(w.bytes[4] == 11);
  CHECK(w.bytes[6] == 0xC9);
  CHECK(w.bytes[7] == 33);
}

TEST_CASE("midi router: the port offset delays or advances, and never reaches before the start") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin | midi::kPortUsb;
  cfg.offset_cycles[0] = 90000;    // DIN: +0.5 ms
  cfg.offset_cycles[1] = -180000;  // USB: -1 ms
  out.configure(cfg);
  out.attach(&din, &usb);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 60, 100, 0), 1000));
  out.flush(kBlockEnd(1000));
  const Wire w = drain(din);
  NEED(w.n == 3u);
  CHECK(w.start[0] == core::cycles_from_samples(1000) + 90000u);
  std::uint32_t words[16];
  CHECK(usb.frame(core::cycles_from_samples(1000) - 180000u - 1u, words, 16) == 0u);
  CHECK(usb.frame(core::cycles_from_samples(1000) - 180000u, words, 16) == 1u);  // advanced by a millisecond
  // an advance past the start of the timeline clamps at 0
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 61, 100, 0), 10));  // 10 samples = about 40,800 cycles, less than the 180,000 advance
  out.flush(kBlockEnd(10));
  CHECK(usb.frame(0, words, 16) == 1u);
}

TEST_CASE("midi router: staging puts out-of-order items in due order, and a full port holds the rest") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.offset_cycles[0] = 0;
  out.configure(cfg);
  out.attach(&din, nullptr);
  // three notes sent in the order 300, 100, 200
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 63, 100, 0), 300));
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 61, 100, 0), 100));
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 62, 100, 0), 200));
  out.flush(kBlockEnd(300));
  const Wire w = drain(din);
  NEED(w.n == 9u);
  CHECK(w.bytes[1] == 61);
  CHECK(w.bytes[4] == 62);
  CHECK(w.bytes[7] == 63);
  // a port that is full holds the rest, in order, for the next flush
  midi::DinEmitter din2;
  midi::MidiOut out2;
  out2.configure(cfg);
  out2.attach(&din2, nullptr);
  for (std::uint32_t i = 0; i < 200; ++i) {
    CHECK(out2.send(ev(EventKind::MidiCc, 0, 0, static_cast<std::uint8_t>(i & 0x7F), 0), 1000 + i));
  }
  midi::MidiOut::Config c2 = cfg;
  c2.cc_number[0] = 7;
  out2.configure(c2);
  for (std::uint32_t i = 0; i < 200; ++i) {
    CHECK(out2.send(ev(EventKind::MidiCc, 0, 0, static_cast<std::uint8_t>(i & 0x7F), 0), 1000 + i));
  }
  CHECK(out2.staged() <= 200u);
  std::uint32_t rounds = 0;
  std::uint32_t sent = 0;
  while (out2.staged() != 0u && rounds < 100u) {
    out2.flush(core::cycles_from_samples(1000 + 64u * (rounds + 20u)));
    const Wire dw = drain(din2, 0);
    sent += dw.total;
    ++rounds;
  }
  CHECK(out2.staged() == 0u);
  CHECK(sent == 200u * 3u);
}

TEST_CASE("midi router: stop drops queued notes and sends the offs that are owed, a few a block, behind everything handed over") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  out.configure(cfg);
  out.attach(&din, nullptr);
  // 40 notes already handed over (sounding), then 5 more queued far ahead (never sent)
  for (std::uint8_t n = 0; n < 40; ++n) {
    CHECK(out.send(ev(EventKind::MidiNoteOn, 0, n, 100, 1), 100));
  }
  out.flush(kBlockEnd(100));
  for (std::uint8_t n = 50; n < 55; ++n) {
    CHECK(out.send(ev(EventKind::MidiNoteOn, 0, n, 100, 1), 1000000));
  }
  CHECK(out.book().owed() == 45u);
  out.stop_all(core::cycles_from_samples(200));
  CHECK(out.book().owed() == 40u);  // the five that were not handed over are not owed
  CHECK(out.panic_pending());
  std::uint32_t blocks = 0;
  while (out.panic_pending() && blocks < 20u) {
    out.flush(core::cycles_from_samples(300 + 64u * blocks));
    ++blocks;
  }
  CHECK(blocks == 3u);  // 16 + 16 + 8
  CHECK(out.stats().panic_offs == 40u);
  CHECK(out.book().owed() == 0u);
  // On the wire: 40 note-ons, then 40 note-offs, each off after the on of its note.
  midi::DinEmitter& em = din;
  std::uint64_t now = 0;
  std::uint32_t ons = 0, offs = 0;
  std::uint64_t last_on = 0, first_off = ~std::uint64_t{0};
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t t = em.next_wake(now);
    if (t == midi::kNever) {
      break;
    }
    now = t;
    const midi::DinEmitter::Tx tx = em.poll(now);
    if (tx.valid && (tx.byte & 0xF0u) == 0x90u) {
      ++ons;
      last_on = tx.start;
    } else if (tx.valid && (tx.byte & 0xF0u) == 0x80u) {
      ++offs;
      first_off = first_off < tx.start ? first_off : tx.start;
    }
  }
  CHECK(ons == 40u);
  CHECK(offs == 40u);
  CHECK(first_off > last_on);  // no off before the last on
}

TEST_CASE("midi router: a note-off lost to a full staging queue is still owed, and the next panic releases it; a note-on lost owes nothing (OS-PFX)") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.cc_number[0] = 7;
  out.configure(cfg);
  out.attach(&din, nullptr);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 60, 100, 0), 100));
  out.flush(kBlockEnd(100));
  (void)drain(din);  // the note sounds on the wire
  CHECK(out.book().count(0, 60) == 1);
  // fill the staging queue (256 items) with controllers that are not due yet, so that nothing can leave it
  for (std::uint32_t i = 0; i < midi::MidiOut::kStagingSize; ++i) {
    CHECK(out.send(ev(EventKind::MidiCc, 0, 0, static_cast<std::uint8_t>(i & 0x7F), 0), 5000));
  }
  CHECK(out.staged() == midi::MidiOut::kStagingSize);
  CHECK(out.stats().staging_overflow == 0u);
  // the off of the sounding note finds no room: it is lost, and the book must keep owing it
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 60, 0, 0), 6000));
  CHECK(out.stats().staging_overflow == 1u);
  CHECK(out.stats().stray_offs == 0u);
  CHECK(out.book().count(0, 60) == 1);
  // a note-on that finds no room never sounds: it owes no off
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 61, 100, 1), 6001));
  CHECK(out.stats().staging_overflow == 2u);
  CHECK(out.book().count(1, 61) == 0);
  CHECK(out.book().owed() == 1u);
  // the ports drain; a stop then releases the note whose off was lost (before the fix the book had forgotten it: a stuck note)
  for (std::uint32_t round = 0; out.staged() != 0u && round < 50u; ++round) {
    out.flush(core::cycles_from_samples(6000 + 64u * round));
    (void)drain(din);
  }
  CHECK(out.staged() == 0u);
  out.stop_all(core::cycles_from_samples(7000));
  CHECK(out.panic_pending());
  for (std::uint32_t round = 0; out.panic_pending() && round < 10u; ++round) {
    out.flush(core::cycles_from_samples(8000 + 64u * round));
  }
  CHECK(out.stats().panic_offs == 1u);
  CHECK(out.book().owed() == 0u);
  const Wire w = drain(din);
  bool off60 = false;
  for (std::uint32_t i = 0; i + 2u < w.n; ++i) {
    off60 = off60 || (w.bytes[i] == 0x80 && w.bytes[i + 1] == 60);
  }
  CHECK(off60);
  // an off with room pays as ever, and an off that is not owed is still stray
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 62, 100, 2), 9000));
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 62, 0, 2), 9100));
  CHECK(out.book().owed() == 0u);
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 62, 0, 2), 9200));
  CHECK(out.stats().stray_offs == 1u);
}

TEST_CASE("midi router: with both ports, an off that only one port took stays owed (OS-PFX)") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = static_cast<std::uint8_t>(midi::kPortDin | midi::kPortUsb);
  cfg.clock_ports = midi::kPortDin;
  cfg.cc_number[0] = 7;
  out.configure(cfg);
  out.attach(&din, &usb);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 60, 100, 0), 100));  // two items, one a port; the count is one note
  CHECK(out.book().count(0, 60) == 1);
  out.flush(kBlockEnd(100));
  // 127 controllers (254 items) and a clock (1) leave one free slot: the off takes it on DIN and finds none on USB
  for (std::uint32_t i = 0; i < 127u; ++i) {
    CHECK(out.send(ev(EventKind::MidiCc, 0, 0, static_cast<std::uint8_t>(i), 0), 5000));
  }
  CHECK(out.send(ev(EventKind::MidiClock, 0), 5000));
  CHECK(out.staged() == midi::MidiOut::kStagingSize - 1u);
  CHECK(out.send(ev(EventKind::MidiNoteOff, 0, 60, 0, 0), 6000));
  CHECK(out.stats().staging_overflow == 1u);
  CHECK(out.staged() == midi::MidiOut::kStagingSize);
  CHECK(out.book().count(0, 60) == 1);  // USB never got the off: the note stays owed
}

TEST_CASE("midi router: a panic that finds the staging queue full retries its note-offs, every held note gets its off (OS-SMALL3)") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.cc_number[0] = 7;
  out.configure(cfg);
  out.attach(&din, nullptr);
  constexpr std::uint8_t kNotes = 40;
  for (std::uint8_t n = 0; n < kNotes; ++n) {
    CHECK(out.send(ev(EventKind::MidiNoteOn, 0, n, 100, 0), 100));
  }
  out.flush(kBlockEnd(100));
  CHECK(out.book().owed() == kNotes);
  // the DIN bytes are collected as the rounds go and parsed at the end (status + two data bytes, running status allowed)
  static std::uint8_t wire[4096];
  std::uint32_t wire_n = 0;
  std::uint64_t now = 0;
  const auto pull = [&]() {
    for (int i = 0; i < 4000; ++i) {
      const std::uint64_t t = din.next_wake(now);
      if (t == midi::kNever) {
        break;
      }
      now = t;
      const midi::DinEmitter::Tx tx = din.poll(now);
      if (tx.valid && wire_n < sizeof wire) {
        wire[wire_n++] = tx.byte;
      }
    }
  };
  pull();  // the note-ons
  CHECK(wire_n >= 2u * kNotes);
  // fill the staging queue with controllers that are due inside the horizon (they drain 64 a block), then ask for the panic: the pump finds no room
  for (std::uint32_t i = 0; i < midi::MidiOut::kStagingSize; ++i) {
    CHECK(out.send(ev(EventKind::MidiCc, 0, 0, static_cast<std::uint8_t>(i & 0x7F), 0), 200));
  }
  CHECK(out.staged() == midi::MidiOut::kStagingSize);
  out.stop_all(core::cycles_from_samples(250));
  CHECK(out.panic_pending());
  std::uint32_t rounds = 0;
  while ((out.panic_pending() || out.staged() != 0u) && rounds < 100u) {
    out.flush(core::cycles_from_samples(300 + 64u * rounds));
    pull();
    ++rounds;
  }
  CHECK(rounds < 100u);
  CHECK(out.stats().staging_overflow >= 1u);  // the overflow did happen
  CHECK(out.stats().panic_offs == kNotes);
  CHECK(out.book().owed() == 0u);
  CHECK_FALSE(out.panic_pending());
  bool off_seen[128] = {};
  std::uint32_t offs = 0;
  std::uint8_t status = 0;
  std::uint32_t k = 0;  // data bytes of the current message so far
  std::uint8_t first = 0;
  for (std::uint32_t i = 0; i < wire_n; ++i) {
    if ((wire[i] & 0x80u) != 0u) {
      status = wire[i];
      k = 0;
    } else if (k++ == 0u) {
      first = wire[i];
    } else {
      if ((status & 0xF0u) == 0x80u) {
        ++offs;
        off_seen[first & 0x7Fu] = true;
      }
      k = 0;
    }
  }
  CHECK(offs == kNotes);
  for (std::uint8_t n = 0; n < kNotes; ++n) {
    CHECK(off_seen[n]);
  }
}

TEST_CASE("midi router: a channel is released alone; a suppressed export queues nothing") {
  midi::DinEmitter din;
  midi::MidiOut out;
  out.attach(&din, nullptr);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 60, 100, 1), 10));
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 61, 100, 2), 10));
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 62, 100, 1), 10));
  out.release_channel(1, core::cycles_from_samples(20));
  out.flush(kBlockEnd(20));
  CHECK(out.stats().panic_offs == 2u);
  CHECK(out.book().count(2, 61) == 1);  // channel 2 still sounds
  CHECK(out.book().owed() == 1u);
  out.set_suppress(true);
  CHECK(out.send(ev(EventKind::MidiNoteOn, 0, 70, 100, 0), 30));
  CHECK(out.stats().suppressed == 1u);
  CHECK(out.book().owed() == 1u);  // the book is untouched
  CHECK(out.staged() == 0u);
}

// ---- OS-INSTUI S8b: the route of an instrument's messages (SeqEvent::flags = the ports, ::value = the OFFSET in microseconds) and the new message kinds -------------------------------------

namespace {

audio::SeqEvent evr(EventKind k, std::uint16_t offset, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t flags, std::int32_t value = 0) {
  audio::SeqEvent e = ev(k, offset, a, b, c, value);
  e.flags = flags;
  return e;
}

}  // namespace

TEST_CASE("midi router: an event's own ports override the device's; 0 is the device's; a note-off on the same route pays the book") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin;  // the device's MIDI OUT is DIN
  out.configure(cfg);
  out.attach(&din, &usb);
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 60, 100, 0, audio::kRoutePorts & 2), 100));  // USB only
  CHECK(out.staged() == 1u);
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 61, 100, 0, 3), 100));                         // both
  CHECK(out.staged() == 3u);
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 62, 100, 0, 0), 100));                         // the device's: DIN
  CHECK(out.staged() == 4u);
  out.flush(kBlockEnd(100));
  const Wire w = drain(din);
  NEED(w.n == 6u);  // 61 and 62 on DIN
  CHECK((w.bytes[1] == 61 && w.bytes[4] == 62));
  std::uint32_t words[16];
  NEED(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 2u);  // 60 and 61 on USB
  CHECK(words[0] == midi::usb_event(0, midi::note_on(0, 60, 100)));
  CHECK(words[1] == midi::usb_event(0, midi::note_on(0, 61, 100)));
  // the offs, each on the route of its on
  CHECK(out.send(evr(EventKind::MidiNoteOff, 0, 60, 0, 0, 2), 200));
  CHECK(out.send(evr(EventKind::MidiNoteOff, 0, 61, 0, 0, 3), 200));
  CHECK(out.send(evr(EventKind::MidiNoteOff, 0, 62, 0, 0, 0), 200));
  CHECK(out.book().owed() == 0u);
  CHECK(out.stats().stray_offs == 0u);
}

TEST_CASE("midi router: an event's OFFSET delays it by that many microseconds, held to 100 ms; the port's own offset adds") {
  midi::DinEmitter din;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin;
  cfg.offset_cycles[0] = 1000;  // the port's OUT OFFSET
  out.configure(cfg);
  out.attach(&din, nullptr);
  const std::uint64_t base = core::cycles_from_samples(1000);
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 60, 100, 0, 0, 40000), 1000));
  out.flush(kBlockEnd(1000) + 40000u * 180u);
  const Wire w = drain(din);
  NEED(w.n == 3u);
  CHECK(w.start[0] == base + 1000u + 40000u * 180u);  // 40 ms of 180 cycles a microsecond
  // a delay past the bound is the bound, a negative one is none
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 61, 100, 0, 0, 5000000), 5000));
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 62, 100, 0, 0, -7), 5000));
  out.flush(core::cycles_from_samples(5000) + 500000u * 180u);
  const Wire w2 = drain(din);
  NEED(w2.n == 6u);
  CHECK(w2.bytes[1] == 62);
  CHECK(w2.start[0] == core::cycles_from_samples(5000) + 1000u);                  // 62 first: no delay
  CHECK(w2.bytes[4] == 61);
  CHECK(w2.start[3] == core::cycles_from_samples(5000) + 1000u + 100000u * 180u);  // 61 at the bound, 100 ms
  // a delayed item is not handed over before it is due (the horizon is two blocks)
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 63, 100, 0, 0, 50000), 9000));
  out.flush(kBlockEnd(9000));
  CHECK(out.staged() == 1u);
  out.flush(kBlockEnd(9000) + 50000u * 180u);
  CHECK(out.staged() == 0u);
}

TEST_CASE("midi router: a controller named by the event, program change, pitch bend, channel and polyphonic aftertouch are the bytes of MIDI 1.0, on both ports") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin | midi::kPortUsb;  // (no CC table: every slot unassigned)
  out.configure(cfg);
  out.attach(&din, &usb);
  CHECK(out.send(evr(EventKind::MidiCc, 0, static_cast<std::uint8_t>(audio::kMidiCcExplicit | 74), 100, 5, 0), 100));
  CHECK(out.stats().dropped_cc == 0u);
  CHECK(out.send(ev(EventKind::MidiCc, 0, 2, 100, 5), 100));  // a table slot with no number: dropped, as before
  CHECK(out.stats().dropped_cc == 1u);
  CHECK(out.send(evr(EventKind::MidiProgram, 1, 9, 0, 6, 0), 100));
  CHECK(out.send(evr(EventKind::MidiPitchBend, 2, 0x55, 0x30, 7, 0), 100));  // 0x30 << 7 | 0x55 = 6229
  CHECK(out.send(evr(EventKind::MidiChanAt, 3, 77, 0, 8, 0), 100));
  CHECK(out.send(evr(EventKind::MidiPolyAt, 4, 60, 88, 9, 0), 100));
  out.flush(kBlockEnd(100));
  const Wire w = drain(din);
  NEED(w.n == 3u + 2u + 3u + 2u + 3u);
  CHECK((w.bytes[0] == 0xB5 && w.bytes[1] == 74 && w.bytes[2] == 100));
  CHECK((w.bytes[3] == 0xC6 && w.bytes[4] == 9));
  CHECK((w.bytes[5] == 0xE7 && w.bytes[6] == 0x55 && w.bytes[7] == 0x30));
  CHECK((w.bytes[8] == 0xD8 && w.bytes[9] == 77));
  CHECK((w.bytes[10] == 0xA9 && w.bytes[11] == 60 && w.bytes[12] == 88));
  std::uint32_t words[16];
  NEED(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 5u);
  CHECK(words[0] == midi::usb_event(0, midi::control_change(5, 74, 100)));
  CHECK(words[1] == midi::usb_event(0, midi::program_change(6, 9)));
  CHECK(words[2] == midi::usb_event(0, midi::pitch_bend(7, 6229)));
  CHECK(words[3] == midi::usb_event(0, midi::channel_pressure(8, 77)));
  CHECK(words[4] == midi::usb_event(0, midi::poly_pressure(9, 60, 88)));
  CHECK((words[2] & 0xFFu) == 0x0Eu);  // the code index number of a pitch bend
  CHECK((words[3] & 0xFFu) == 0x0Du);
  CHECK((words[4] & 0xFFu) == 0x0Au);
  // a new kind adds no hang bookkeeping
  CHECK(out.book().owed() == 0u);
}

TEST_CASE("midi router: a stop drops a delayed note-on of a note that never sounded and pays its book; the offs of a sounding note go to the port it went to") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config cfg;
  cfg.note_ports = midi::kPortDin;
  out.configure(cfg);
  out.attach(&din, &usb);
  // a USB-only note, delayed 50 ms: staged, not handed over
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 60, 100, 2, 2, 50000), 100));
  out.flush(kBlockEnd(100));
  CHECK(out.staged() == 1u);
  CHECK(out.book().count(2, 60) == 1);
  out.stop_all(core::cycles_from_samples(200));
  CHECK(out.staged() == 0u);        // dropped: it never sounded
  CHECK(out.book().owed() == 0u);   // ... so no off is owed (the copy that paid the book was the USB one, not the device's DIN port)
  CHECK_FALSE(out.panic_pending());
  // a USB-only note that did sound: the panic's off goes to USB (and not to the device's port)
  CHECK(out.send(evr(EventKind::MidiNoteOn, 0, 61, 100, 3, 2, 0), 300));
  out.flush(kBlockEnd(300));
  CHECK(out.staged() == 0u);
  std::uint32_t words[16];
  NEED(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 1u);
  out.stop_all(core::cycles_from_samples(400));
  NEED(out.panic_pending());
  for (std::uint32_t round = 0; round < 8u && (out.panic_pending() || out.staged() != 0u); ++round) {
    out.flush(core::cycles_from_samples(500 + 64u * round));
  }
  CHECK(out.stats().panic_offs == 1u);
  CHECK(out.book().owed() == 0u);
  NEED(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 1u);
  CHECK(words[0] == midi::usb_event(0, midi::note_off(3, 61)));
  const Wire w = drain(din);
  CHECK(w.n == 0u);  // nothing went to DIN
}
