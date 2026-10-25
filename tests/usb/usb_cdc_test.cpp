// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the CDC pieces that need no controller: the line coding wire format, the chunking and drop policy of the transmit
// writer, and the USB-MIDI event helpers. Runs on the host and on QEMU (qemu_usb_device).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/cdc.h>

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb;

TEST_CASE("usb cdc: line coding wire format") {
  LineCoding c;
  std::uint8_t b[kLineCodingBytes];
  c.serialize(b);
  const std::uint8_t def[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};  // 115200 8N1
  CHECK(std::memcmp(b, def, 7) == 0);
  const std::uint8_t in[7] = {0x40, 0x4B, 0x4C, 0x00, 0x01, 0x04, 0x05};  // 5,000,000 baud, 1.5 stop bits, space parity, 5 bits
  c = LineCoding::parse(in);
  CHECK(c.dte_rate == 5000000u);
  CHECK((c.char_format == 1 && c.parity_type == 4 && c.data_bits == 5));
  std::uint8_t out[7];
  c.serialize(out);
  CHECK(std::memcmp(in, out, 7) == 0);
  // The maximum rate and every byte value round-trip.
  const std::uint8_t full[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  c = LineCoding::parse(full);
  CHECK(c.dte_rate == 0xFFFFFFFFu);
  c.serialize(out);
  CHECK(std::memcmp(full, out, 7) == 0);
}

TEST_CASE("usb cdc: control lines") {
  CdcLineState s;
  CHECK_FALSE(s.dtr());
  s.control.store(kControlLineDtr, std::memory_order_relaxed);
  CHECK((s.dtr() && !s.rts()));
  s.control.store(kControlLineRts, std::memory_order_relaxed);
  CHECK((!s.dtr() && s.rts()));
}

TEST_CASE("usb cdc: the writer packs 63-byte chunks and publishes the rest on flush") {
  CdcTxRing ring;
  CdcTxWriter w(ring);
  char text[130];
  for (unsigned i = 0; i < sizeof text; ++i) {
    text[i] = static_cast<char>('A' + i % 26u);
  }
  w.write(text, 100);
  CHECK(ring.size() == 1u);   // 63 bytes went out as a chunk, 37 are waiting
  CHECK(w.pending() == 37u);
  w.write(text + 100, 30);
  CHECK(ring.size() == 2u);   // 126 bytes: two full chunks, 4 waiting
  CHECK(w.pending() == 4u);
  w.flush();
  CHECK(w.pending() == 0u);
  CHECK(ring.size() == 3u);
  w.flush();                  // nothing pending: no empty chunk
  CHECK(ring.size() == 3u);
  ByteChunk c;
  std::uint32_t total = 0;
  const std::uint32_t expect[3] = {63, 63, 4};
  for (unsigned i = 0; i < 3u; ++i) {
    USB_REQUIRE(ring.read(&c, 1) == 1u);
    CHECK(c.len == expect[i]);
    CHECK(c.len <= kCdcTxChunkMax);
    CHECK(std::memcmp(c.data, text + total, c.len) == 0);
    total += c.len;
  }
  CHECK(total == 130u);
  CHECK(w.bytes_written() == 130u);
  CHECK(w.dropped_chunks() == 0u);
}

TEST_CASE("usb cdc: the writer's drop count is its own, whether or not anything reads") {
  CdcTxRing ring;
  CdcTxWriter w(ring);
  for (unsigned i = 0; i < CdcTxRing::capacity() + 10u; ++i) {
    w.write("z", 1);
    w.flush();
  }
  CHECK(w.dropped_chunks() == 10u);
  CHECK(ring.dropped() == 0u);  // the ring only learns of a lap when its reader reads
  ByteChunk c;
  CHECK(ring.read(&c, 1) == 1u);
  CHECK(ring.dropped() == 10u);
}

TEST_CASE("usb cdc: MIDI event helpers") {
  CHECK_FALSE(midi_event_valid(0x00000000u));  // padding
  CHECK_FALSE(midi_event_valid(0x00000001u));  // cable event: reserved
  CHECK(midi_event_valid(0x00000002u));        // two-byte system common
  CHECK(midi_event_valid(0x7F3C9009u));
  CHECK(midi_event_valid(0x0000000Fu));        // single byte
  MidiRing ring;
  alignas(4) std::uint8_t pkt[64] = {};
  const std::uint32_t good = 0x7F3C9009u;
  for (unsigned i = 0; i < 16u; ++i) {
    std::memcpy(pkt + 4u * i, &good, 4);
  }
  MidiPacketResult r = midi_packet_to_ring(ring, pkt, 64);
  CHECK((r.accepted == 16u && r.skipped == 0u && r.bad_bytes == 0u));
  r = midi_packet_to_ring(ring, pkt, 7);  // one event and three stray bytes
  CHECK((r.accepted == 1u && r.bad_bytes == 3u));
  r = midi_packet_to_ring(ring, pkt, 3);  // not even one event
  CHECK((r.accepted == 0u && r.bad_bytes == 3u));
  r = midi_packet_to_ring(ring, pkt, 0);
  CHECK((r.accepted == 0u && r.bad_bytes == 0u));
  std::uint8_t out[64];
  std::uint32_t n = midi_ring_to_packet(ring, out);
  CHECK(n == 64u);
  n = midi_ring_to_packet(ring, out);
  CHECK(n == 4u);
  CHECK(midi_ring_to_packet(ring, out) == 0u);
}

TEST_CASE("usb cdc: the writer reports the room that is left, a lower bound that never overstates") {
  CdcTxRing ring;
  CdcTxWriter w(ring);
  constexpr std::uint32_t kAll = CdcTxRing::capacity() * kCdcTxChunkMax;
  CHECK(w.room_bytes() == kAll);
  w.write("0123456789", 10);
  CHECK(w.room_bytes() == kAll - 10u);  // the chunk being filled will take a slot when it is published
  w.flush();
  CHECK(w.room_bytes() == kAll - kCdcTxChunkMax);  // a slot of the ring is used, whatever the chunk's fill
  // Fill the ring to the brim: the room is exactly what is left, and writing exactly that drops nothing.
  char blob[kCdcTxChunkMax];
  std::memset(blob, 'x', sizeof blob);
  for (std::uint32_t left = w.room_bytes(); left != 0u;) {
    const std::uint32_t n = left < sizeof blob ? left : static_cast<std::uint32_t>(sizeof blob);
    w.write(blob, n);
    left -= n;
  }
  CHECK(w.room_bytes() == 0u);
  CHECK(w.dropped_chunks() == 0u);
  w.write("y", 1);  // one more byte: now a chunk is overwritten, and the room stays 0
  w.flush();
  CHECK(w.dropped_chunks() == 1u);
  CHECK(w.room_bytes() == 0u);
  // After the reader drains some chunks the room grows again.
  ByteChunk c;
  for (int i = 0; i < 10; ++i) {
    CHECK(ring.read(&c, 1) == 1u);
  }
  CHECK(w.room_bytes() >= 10u * kCdcTxChunkMax - kCdcTxChunkMax);
  const std::uint32_t dropped = w.dropped_chunks();
  std::uint32_t room = w.room_bytes();
  while (room != 0u) {  // writing it all in small pieces drops nothing either
    const std::uint32_t n = room < 7u ? room : 7u;
    w.write(blob, n);
    room -= n;
  }
  CHECK(w.dropped_chunks() == dropped);
}
