// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MIDI timing core's small parts (OS-005c): message encoding and the USB-MIDI words, the timed queue against a reference, the note-hang book against a
// reference, and the byte parser of MIDI in. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/midi/hang.h>
#include <o3sdk/platform/mk66/midi/in.h>
#include <o3sdk/platform/mk66/midi/message.h>
#include <o3sdk/platform/mk66/midi/timed_queue.h>

#include <cstdint>

#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
using o3sdk::test::Rng;

TEST_CASE("midi message: channel messages, real-time bytes and the data lengths") {
  const midi::Msg on = midi::note_on(2, 60, 100);
  CHECK(on.len == 3);
  CHECK(on.b[0] == 0x92);
  CHECK(on.b[1] == 60);
  CHECK(on.b[2] == 100);
  const midi::Msg off = midi::note_off(15, 0x7F);
  CHECK(off.b[0] == 0x8F);
  CHECK(off.b[2] == 0);  // velocity 0
  CHECK(midi::note_on(0, 200, 200).b[1] == (200 & 0x7F));  // data bytes are masked to 7 bits
  const midi::Msg pc = midi::program_change(4, 9);
  CHECK(pc.len == 2);
  CHECK(pc.b[0] == 0xC4);
  const midi::Msg spp = midi::song_position(0x1234);
  CHECK(spp.b[0] == 0xF2);
  CHECK(spp.b[1] == (0x1234 & 0x7F));
  CHECK(spp.b[2] == ((0x1234 >> 7) & 0x7F));
  CHECK(midi::is_realtime(0xF8));
  CHECK(midi::is_realtime(0xFF));
  CHECK_FALSE(midi::is_realtime(0xF7));
  CHECK_FALSE(midi::is_realtime(0x90));
  CHECK(midi::data_bytes_of(0x80) == 2);
  CHECK(midi::data_bytes_of(0xC0) == 1);
  CHECK(midi::data_bytes_of(0xD3) == 1);
  CHECK(midi::data_bytes_of(0xF2) == 2);
  CHECK(midi::data_bytes_of(0xF3) == 1);
  CHECK(midi::data_bytes_of(0xF6) == 0);
  CHECK(midi::data_bytes_of(0x40) == 0);
}

TEST_CASE("midi message: USB-MIDI event words (USB-MIDI 1.0 chapter 4), in wire order") {
  // note on, cable 0, channel 0: CIN 9, then the three bytes
  CHECK(midi::usb_event(0, midi::note_on(0, 60, 100)) == (0x09u | (0x90u << 8) | (60u << 16) | (100u << 24)));
  CHECK(midi::usb_event(0, midi::note_off(1, 61)) == (0x08u | (0x81u << 8) | (61u << 16)));
  CHECK(midi::usb_event(0, midi::control_change(0, 7, 127)) == (0x0Bu | (0xB0u << 8) | (7u << 16) | (127u << 24)));
  CHECK(midi::usb_event(0, midi::program_change(0, 5)) == (0x0Cu | (0xC0u << 8) | (5u << 16)));
  CHECK(midi::usb_event(1, midi::msg1(midi::kClock)) == (0x1Fu | (0xF8u << 8)));  // cable 1, single byte: CIN F
  CHECK(midi::usb_event(0, midi::msg1(midi::kStart)) == (0x0Fu | (0xFAu << 8)));
  CHECK(midi::usb_event(0, midi::song_position(5)) == (0x03u | (0xF2u << 8) | (5u << 16)));  // system common, three bytes: CIN 3
  const std::uint32_t w = midi::usb_event(0, midi::note_on(0, 60, 100));
  std::uint8_t bytes[4];
  __builtin_memcpy(bytes, &w, 4);
  CHECK(bytes[0] == 0x09);  // little endian: byte 0 first on the wire
  CHECK(bytes[1] == 0x90);
}

TEST_CASE("midi timed queue: key order, stable ties, bounded, cancel") {
  struct Item {
    std::uint32_t id;
    std::uint32_t pad;
  };
  midi::TimedQueue<Item, 32> q;
  CHECK(q.empty());
  CHECK(q.front() == nullptr);
  for (std::uint32_t i = 0; i < 32; ++i) {
    CHECK(q.push(100, Item{i, 0}));  // equal keys: first in, first out
  }
  CHECK(q.full());
  CHECK_FALSE(q.push(5, Item{99, 0}));  // full: refused, nothing changes
  CHECK(q.size() == 32);
  for (std::uint32_t i = 0; i < 32; ++i) {
    CHECK(q.front()->id == i);
    CHECK(q.front_key() == 100);
    q.pop();
  }
  CHECK(q.empty());

  // Against a reference (a sorted array, stable): random pushes and pops, with the head wrapping around the circular array many times.
  Rng rng(3);
  std::uint64_t ref_key[32];
  std::uint32_t ref_id[32];
  std::uint32_t ref_n = 0;
  std::uint32_t next_id = 0;
  for (int step = 0; step < 20000; ++step) {
    if (rng.below(100) < 55 && ref_n < 32u) {
      const std::uint64_t key = rng.below(40);
      // reference: after the last item whose key <= key
      std::uint32_t pos = ref_n;
      while (pos != 0 && ref_key[pos - 1] > key) {
        --pos;
      }
      for (std::uint32_t i = ref_n; i > pos; --i) {
        ref_key[i] = ref_key[i - 1];
        ref_id[i] = ref_id[i - 1];
      }
      ref_key[pos] = key;
      ref_id[pos] = next_id;
      ++ref_n;
      CHECK(q.push(key, Item{next_id, 0}));
      ++next_id;
    } else if (ref_n != 0u) {
      CHECK(q.front_key() == ref_key[0]);
      CHECK(q.front()->id == ref_id[0]);
      q.pop();
      for (std::uint32_t i = 1; i < ref_n; ++i) {
        ref_key[i - 1] = ref_key[i];
        ref_id[i - 1] = ref_id[i];
      }
      --ref_n;
    }
    CHECK(q.size() == ref_n);
  }
  // cancel_if: removes the matching items at or after a key, keeps the order of the rest.
  q.clear();
  for (std::uint32_t i = 0; i < 10; ++i) {
    CHECK(q.push(i * 10, Item{i, i % 2}));
  }
  CHECK(q.cancel_if(35, [](const Item& it) { return it.pad == 1u; }) == 3u);  // ids 5, 7, 9 (keys 50, 70, 90); id 3 (key 30) is before the limit
  CHECK(q.size() == 7);
  const std::uint32_t expect[7] = {0, 1, 2, 3, 4, 6, 8};
  for (std::uint32_t i = 0; i < 7; ++i) {
    CHECK(q.front()->id == expect[i]);
    q.pop();
  }
}

TEST_CASE("midi hang book: owed offs against a reference, the sweep, the channel filter") {
  midi::HangBook b;
  b.clear();
  CHECK(b.owed() == 0);
  std::uint8_t ch = 0;
  std::uint8_t note = 0;
  CHECK_FALSE(b.take_next(ch, note));
  b.note_on(3, 60);
  b.note_on(3, 60);  // two tracks, one note: two offs owed
  b.note_on(0, 127);
  CHECK(b.owed() == 3);
  CHECK(b.count(3, 60) == 2);
  CHECK(b.note_off(3, 60));
  CHECK(b.note_off(3, 60));
  CHECK_FALSE(b.note_off(3, 60));  // none owed
  CHECK(b.owed() == 1);
  // sweep order: channel then note, wrapping from the cursor
  b.clear();
  b.note_on(5, 10);
  b.note_on(1, 99);
  b.note_on(1, 3);
  b.note_on(15, 127);
  const std::uint8_t exp_ch[4] = {1, 1, 5, 15};
  const std::uint8_t exp_note[4] = {3, 99, 10, 127};
  for (int i = 0; i < 4; ++i) {
    CHECK(b.take_next(ch, note));
    CHECK(ch == exp_ch[i]);
    CHECK(note == exp_note[i]);
  }
  CHECK(b.owed() == 0);
  CHECK_FALSE(b.take_next(ch, note));
  // channel filter: only channel 7 and 9
  b.note_on(2, 1);
  b.note_on(7, 2);
  b.note_on(9, 3);
  b.note_on(12, 4);
  CHECK(b.take_next(ch, note, static_cast<std::uint16_t>((1u << 7) | (1u << 9))));
  CHECK(ch == 7);
  CHECK(b.take_next(ch, note, static_cast<std::uint16_t>((1u << 7) | (1u << 9))));
  CHECK(ch == 9);
  CHECK_FALSE(b.take_next(ch, note, static_cast<std::uint16_t>((1u << 7) | (1u << 9))));
  CHECK(b.owed() == 2);  // channels 2 and 12 still owe
  // saturation: 255 offs owed of one note and no further
  b.clear();
  for (int i = 0; i < 300; ++i) {
    b.note_on(0, 0);
  }
  CHECK(b.count(0, 0) == 255);
  CHECK(b.owed() == 255);

  // Random operations against a plain count array.
  b.clear();
  Rng rng(5);
  std::uint8_t ref[16][128] = {};
  std::uint32_t owed = 0;
  for (int i = 0; i < 40000; ++i) {
    const std::uint8_t c = static_cast<std::uint8_t>(rng.below(16));
    const std::uint8_t n = static_cast<std::uint8_t>(rng.below(128));
    const std::uint32_t op = rng.below(10);
    if (op < 5) {
      b.note_on(c, n);
      if (ref[c][n] != 255) {
        ++ref[c][n];
        ++owed;
      }
    } else if (op < 8) {
      const bool paid = b.note_off(c, n);
      CHECK(paid == (ref[c][n] != 0));
      if (ref[c][n] != 0) {
        --ref[c][n];
        --owed;
      }
    } else if (b.take_next(ch, note)) {
      CHECK(ref[ch][note] != 0);
      owed -= ref[ch][note];
      ref[ch][note] = 0;
    } else {
      CHECK(owed == 0);
    }
    CHECK(b.owed() == owed);
  }
}

namespace {
midi::InParser::Out feed_all(midi::InParser& p, const std::uint8_t* bytes, std::uint32_t n, midi::ByteKind want, std::uint32_t* count = nullptr) {
  midi::InParser::Out last{midi::ByteKind::Pass, 0};
  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const midi::InParser::Out o = p.feed(bytes[i]);
    if (o.kind == want) {
      last = o;
      ++c;
    }
  }
  if (count != nullptr) {
    *count = c;
  }
  return last;
}
}  // namespace

TEST_CASE("midi in parser: real-time bytes anywhere, SPP in three bytes, everything else passes") {
  midi::InParser p;
  CHECK(p.feed(0xF8).kind == midi::ByteKind::Clock);
  CHECK(p.feed(0xFA).kind == midi::ByteKind::Start);
  CHECK(p.feed(0xFB).kind == midi::ByteKind::Continue);
  CHECK(p.feed(0xFC).kind == midi::ByteKind::Stop);
  CHECK(p.feed(0xFE).kind == midi::ByteKind::Pass);  // active sensing is not ours
  // SPP, with clocks between its bytes
  CHECK(p.feed(0xF2).kind == midi::ByteKind::Consumed);
  CHECK(p.feed(0xF8).kind == midi::ByteKind::Clock);
  CHECK(p.feed(0x34).kind == midi::ByteKind::Consumed);
  CHECK(p.feed(0xF8).kind == midi::ByteKind::Clock);
  const midi::InParser::Out spp = p.feed(0x12);
  CHECK(spp.kind == midi::ByteKind::Spp);
  CHECK(spp.value == (0x34 | (0x12 << 7)));
  // a note on with running status, a clock inside: all of it passes, the clock is the clock
  const std::uint8_t note[] = {0x90, 60, 0xF8, 100, 61, 100, 0x80, 60, 0};
  std::uint32_t clocks = 0;
  feed_all(p, note, sizeof note, midi::ByteKind::Clock, &clocks);
  CHECK(clocks == 1);
  midi::InParser q;
  std::uint32_t passed = 0;
  feed_all(q, note, sizeof note, midi::ByteKind::Pass, &passed);
  CHECK(passed == sizeof note - 1);  // every byte but the clock
  // SysEx: its data bytes pass, and a data byte that looks like an SPP's is not one
  midi::InParser s;
  const std::uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
  passed = 0;
  feed_all(s, sysex, sizeof sysex, midi::ByteKind::Pass, &passed);
  CHECK(passed == sizeof sysex);
  // an SPP interrupted by another status byte is dropped, not completed by the next data
  midi::InParser t;
  CHECK(t.feed(0xF2).kind == midi::ByteKind::Consumed);
  CHECK(t.feed(0x05).kind == midi::ByteKind::Consumed);
  CHECK(t.feed(0x90).kind == midi::ByteKind::Pass);  // a new status: the SPP is abandoned
  CHECK(t.feed(60).kind == midi::ByteKind::Pass);
  CHECK(t.feed(100).kind == midi::ByteKind::Pass);
  // stray data bytes with no status pass
  midi::InParser u;
  CHECK(u.feed(0x10).kind == midi::ByteKind::Pass);
  // fuzz: no input makes it report a Spp without having consumed a status 0xF2 and two data bytes
  Rng rng(9);
  midi::InParser f;
  std::uint32_t since = 0;
  bool armed = false;
  for (int i = 0; i < 100000; ++i) {
    const std::uint8_t b = static_cast<std::uint8_t>(rng.next() >> 8);
    const midi::InParser::Out o = f.feed(b);
    if (midi::is_realtime(b)) {
      continue;
    }
    if (b == 0xF2) {
      armed = true;
      since = 0;
    } else if (midi::is_status(b)) {
      armed = false;
    } else if (armed) {
      ++since;
    }
    if (o.kind == midi::ByteKind::Spp) {
      CHECK(armed);
      CHECK(since == 2);
      armed = false;
    }
  }
}
