// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/ring.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace o3sdk::usb {

// The CDC ACM function behind the `diag` console (OS-006b): the line state a host sets, and the byte streams in both
// directions as rings of packet-sized chunks. Pure and host tested; the driver moves a chunk to or from a bulk endpoint.

// ---- Line state (CDC 1.2 PSTN: SET_LINE_CODING, GET_LINE_CODING, SET_CONTROL_LINE_STATE) ----------------------------
// The device stores what the host sets and ignores it: the "serial port" has no UART behind it, so the baud rate means
// nothing. GET_LINE_CODING returns what was last set (115200 8N1 before any SET).
inline constexpr std::size_t kLineCodingBytes = 7;

struct LineCoding {
  std::uint32_t dte_rate = 115200;  // bits per second
  std::uint8_t char_format = 0;     // 0 one stop bit, 1 one and a half, 2 two
  std::uint8_t parity_type = 0;     // 0 none, 1 odd, 2 even, 3 mark, 4 space
  std::uint8_t data_bits = 8;

  // The 7 bytes of the wire format: dwDTERate (little endian), bCharFormat, bParityType, bDataBits.
  void serialize(std::uint8_t out[kLineCodingBytes]) const noexcept {
    out[0] = static_cast<std::uint8_t>(dte_rate & 0xFFu);
    out[1] = static_cast<std::uint8_t>((dte_rate >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((dte_rate >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((dte_rate >> 24) & 0xFFu);
    out[4] = char_format;
    out[5] = parity_type;
    out[6] = data_bits;
  }
  static LineCoding parse(const std::uint8_t in[kLineCodingBytes]) noexcept {
    LineCoding c;
    c.dte_rate = static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
                 (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
    c.char_format = in[4];
    c.parity_type = in[5];
    c.data_bits = in[6];
    return c;
  }
};

inline constexpr std::uint16_t kControlLineDtr = 0x0001;  // SET_CONTROL_LINE_STATE wValue bit 0: a terminal has the port open
inline constexpr std::uint16_t kControlLineRts = 0x0002;

// Written by the USB interrupt (the control pipe), read by the main loop: `control` is an atomic, the rest are plain words
// with one writer (read as a diagnostic, never for a decision).
struct CdcLineState {
  LineCoding coding;
  std::atomic<std::uint8_t> control{0};
  std::uint32_t coding_sets = 0;   // SET_LINE_CODING requests seen
  std::uint32_t control_sets = 0;  // SET_CONTROL_LINE_STATE requests seen

  bool dtr() const noexcept { return (control.load(std::memory_order_relaxed) & kControlLineDtr) != 0u; }
  bool rts() const noexcept { return (control.load(std::memory_order_relaxed) & kControlLineRts) != 0u; }
};

// ---- Byte streams -----------------------------------------------------------------------------------------------------
// A chunk is what one bulk packet carries: up to 64 bytes. The device-to-host direction never fills the packet (63 bytes at
// most, kCdcTxChunkMax), so every packet it sends is short and no zero-length packet is ever needed to end a transfer (a
// host driver that waits for a short packet, as some do, never waits).
inline constexpr std::uint32_t kCdcPacketBytes = 64;
inline constexpr std::uint32_t kCdcTxChunkMax = kCdcPacketBytes - 1u;

struct ByteChunk {
  std::uint32_t len;
  std::uint8_t data[kCdcPacketBytes];
};
static_assert(sizeof(ByteChunk) % 4u == 0u && std::is_trivially_copyable<ByteChunk>::value, "the ring copies chunks in words");

// Device to host: the main loop writes, the USB interrupt reads. 64 chunks, 63 usable (OverwriteRing keeps one chunk of slack): about
// 4 KB, which holds the longest `diag` reply (`mem`, about 2.5 KB) with room to spare, because the main loop writes a whole reply in
// microseconds while the host takes a packet per millisecond or so. A host that does not read loses the OLDEST bytes (counted); the
// firmware never blocks.
using CdcTxRing = OverwriteRing<ByteChunk, 64, 1>;
// Host to device: the USB interrupt writes, the main loop reads. Typing is slow and a pasted script is a few hundred bytes; 16 chunks
// is plenty, overrun drops the oldest.
using CdcRxRing = OverwriteRing<ByteChunk, 16, 1>;
// Back-pressure on the host (OS-006b): while the receive ring holds this many chunks the interrupt does not re-arm the bulk OUT descriptors, so the
// host's next packets are NAKed (and retried by the host) instead of overwriting what the main loop has not read. Two descriptors can still be
// armed when the limit is reached, so the ring never holds more than this plus two: a pasted burst loses nothing.
inline constexpr std::uint32_t kCdcRxHoldChunks = 12;
static_assert(kCdcRxHoldChunks + 2u <= CdcRxRing::capacity(), "the ring has room for the packets still in flight when the hold starts");

// The writer side of CdcTxRing: collects bytes into a chunk, publishes full chunks and, on flush(), the partial one. Single
// context (the main loop). Never blocks and never fails: a full ring drops its oldest chunk, and the writer counts it itself
// (the ring only learns of a drop when its consumer next reads, which may be never when no host is attached).
class CdcTxWriter {
 public:
  explicit CdcTxWriter(CdcTxRing& ring) noexcept : ring_(ring) {}

  void write(const char* s, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      cur_.data[cur_.len++] = static_cast<std::uint8_t>(s[i]);
      ++bytes_;
      if (cur_.len == kCdcTxChunkMax) {
        publish();
      }
    }
  }
  void flush() noexcept {
    if (cur_.len != 0u) {
      publish();
    }
  }

  std::uint32_t bytes_written() const noexcept { return bytes_; }
  std::uint32_t dropped_chunks() const noexcept { return dropped_chunks_; }
  // Bytes buffered here and not yet published.
  std::uint32_t pending() const noexcept { return cur_.len; }
  // Bytes that can be written now without overwriting an unread chunk: a LOWER bound (the interrupt may drain more at any moment). The chunk being
  // filled will need a slot of the ring when it is published, so it is counted against the free slots, not added to them.
  std::uint32_t room_bytes() const noexcept {
    const std::uint32_t used = ring_.size_from_producer();
    const std::uint32_t free_bytes = (used < CdcTxRing::capacity() ? CdcTxRing::capacity() - used : 0u) * kCdcTxChunkMax;
    return free_bytes > cur_.len ? free_bytes - cur_.len : 0u;
  }

 private:
  void publish() noexcept {
    if (ring_.size_from_producer() >= CdcTxRing::capacity()) {
      ++dropped_chunks_;  // this write overwrites the oldest chunk
    }
    ring_.write(&cur_, 1);
    cur_.len = 0;
  }

  CdcTxRing& ring_;
  ByteChunk cur_{};
  std::uint32_t bytes_ = 0;
  std::uint32_t dropped_chunks_ = 0;
};

// ---- USB-MIDI event packets (USB-MIDI 1.0 chapter 4) ------------------------------------------------------------------
// A bulk packet carries up to 16 four-byte events (cable and code index, three message bytes). An event is one word whose
// memory order is the wire order (little endian target and host, usb_types.h asserts it). One ring per direction; the MIDI
// engine binding is OS-005. An overrun drops the oldest events (counted by whoever notices: see the driver's counters).
inline constexpr std::uint32_t kMidiEventsPerPacket = kCdcPacketBytes / 4u;
using MidiRing = OverwriteRing<std::uint32_t, 128, kMidiEventsPerPacket>;

// Code index numbers 0 and 1 are reserved ("miscellaneous" and "cable events", USB-MIDI 1.0 table 4-1): hosts never send them
// meaningfully and a device must ignore them. Zero words also appear as padding.
constexpr bool midi_event_valid(std::uint32_t event) noexcept { return (event & 0x0Fu) >= 2u; }

struct MidiPacketResult {
  std::uint32_t accepted = 0;  // events pushed to the ring
  std::uint32_t skipped = 0;   // reserved or padding events
  std::uint32_t bad_bytes = 0; // trailing bytes that were not a whole event
};

// Bulk OUT packet -> ring. `n` bytes at `packet` (4-byte aligned).
inline MidiPacketResult midi_packet_to_ring(MidiRing& ring, const std::uint8_t* packet, std::uint32_t n) noexcept {
  MidiPacketResult r;
  std::uint32_t events[kMidiEventsPerPacket];
  const std::uint32_t whole = n / 4u > kMidiEventsPerPacket ? kMidiEventsPerPacket : n / 4u;
  r.bad_bytes = n - (n / 4u) * 4u;
  std::uint32_t kept = 0;
  for (std::uint32_t i = 0; i < whole; ++i) {
    std::uint32_t e;
    std::memcpy(&e, packet + 4u * i, 4);
    if (midi_event_valid(e)) {
      events[kept++] = e;
    } else {
      ++r.skipped;
    }
  }
  if (kept != 0u) {
    ring.write(events, kept);
  }
  r.accepted = kept;
  return r;
}

// Ring -> bulk IN packet buffer (room for 64 bytes); returns the bytes written (a multiple of 4, 0 when the ring is empty).
inline std::uint32_t midi_ring_to_packet(MidiRing& ring, std::uint8_t* packet) noexcept {
  std::uint32_t events[kMidiEventsPerPacket];
  const std::uint32_t n = ring.read(events, kMidiEventsPerPacket);
  std::memcpy(packet, events, n * 4u);
  return n * 4u;
}

}  // namespace o3sdk::usb
