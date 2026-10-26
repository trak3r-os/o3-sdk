// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MIDI-in ring of the app modules (OS-H1, ADR 0014 decision 4): a MIDI MONITOR app (and any app that holds O3M_CAP_MIDI_IN) reads the channel messages that arrive on DIN and USB.
//
// The producer is the render, which already walks every byte the timing code did not take (CcInput::feed, called by MidiLink): the core parser stays the only reader of the ports. Each
// port has a running-status parser here that turns the bytes into whole channel messages (any status 0x80..0xEF: notes, controllers, program change, pressure, pitch bend) and pushes
// one 32-bit word, port | status << 8 | data1 << 16 | data2 << 24, into a 64-entry SPSC ring. The consumer is the main loop (ModuleUi, from the app's own io.midi_read call).
//
// Off unless asked: the ring is filled only while an app that wants it runs (set_enabled, written by the main loop, read relaxed by the render: a flag, no ordering needed), so a
// session without such an app pays one relaxed load per byte. Every enable also resets the render's parsers (R1 of the OS-H1 review). Real-time bytes (0xF8..0xFF) are ignored without disturbing running status; system common (0xF1..0xF6) cancels running
// status; SysEx (0xF0) is not delivered, only counted (sysex_dropped, one per message, at its F0). A full ring drops the new message and counts it (overflow). Every counter has one
// writer (the render) and is read relaxed by the main loop.
//
// Real time and storage: bounded (one parser step a byte, one push a message), no allocation, no blocking; plain storage (about 300 bytes) that lives inside midi::CcInput, in SDRAM.

#include <o3sdk/core/ring.h>
#include <o3sdk/platform/mk66/midi/message.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::midi {

class MidiMonitor {
 public:
  static constexpr std::uint32_t kRingSize = 64;  // power of two; ADR 0014: 64 events of 4 bytes
  static constexpr unsigned kPorts = 2;           // DIN, USB

  // One message as the consumer sees it. `port` 0 DIN, 1 USB; `status` 0x80..0xEF; data2 is 0 for a one-data-byte message.
  struct Event {
    std::uint8_t port;
    std::uint8_t status;
    std::uint8_t data1;
    std::uint8_t data2;
  };
  static constexpr std::uint32_t pack(const Event& e) noexcept {
    return static_cast<std::uint32_t>(e.port) | (static_cast<std::uint32_t>(e.status) << 8) | (static_cast<std::uint32_t>(e.data1) << 16) | (static_cast<std::uint32_t>(e.data2) << 24);
  }
  static constexpr Event unpack(std::uint32_t w) noexcept {
    return Event{static_cast<std::uint8_t>(w & 0xFFu), static_cast<std::uint8_t>((w >> 8) & 0xFFu), static_cast<std::uint8_t>((w >> 16) & 0xFFu), static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
  }

  // ---- the render (the one producer) --------------------------------------------------------------------------------------------
  void feed(std::uint32_t port, std::uint8_t b) noexcept {
    const std::uint32_t ctl = ctl_.load(std::memory_order_relaxed);
    if ((ctl & 1u) == 0u) {
      return;
    }
    if (ctl != seen_ctl_) {  // an enable edge (a new app): forget the running status and half message of the last one, so it never completes a stale message
      seen_ctl_ = ctl;
      parser_[0] = Parser{};
      parser_[1] = Parser{};
    }
    Parser& p = parser_[port < kPorts ? port : 0u];
    if (is_realtime(b)) {
      return;  // may sit inside a message; never touches the state
    }
    if (is_status(b)) {
      if (b == kSysExStart) {
        sysex_.store(sysex_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
      }
      p.status = b < 0xF0u ? b : std::uint8_t{0};  // system common and SysEx cancel running status
      p.have = 0;
      return;
    }
    if (p.status == 0u) {
      return;  // a data byte of SysEx or system common, or with no status yet
    }
    const std::uint8_t need = data_bytes_of(p.status);
    if (need == 1u) {
      push(port, p.status, b, 0);
      return;
    }
    if (p.have == 0u) {
      p.first = b;
      p.have = 1;
      return;
    }
    p.have = 0;  // running status: the next data byte starts another message of this kind
    push(port, p.status, p.first, b);
  }

  // ---- the main loop (the one consumer) -----------------------------------------------------------------------------------------
  // Start or stop collecting. Starting empties nothing by itself: the consumer drains (clear()) first; it does make the render reset its parsers at the next byte (the enable bit and a
  // generation counter share one word, written only here by the main loop, so the render sees the edge and the flag together in one relaxed load).
  void set_enabled(bool on) noexcept {
    const std::uint32_t ctl = ctl_.load(std::memory_order_relaxed);
    ctl_.store(on ? (((ctl >> 1) + 1u) << 1) | 1u : (ctl & ~1u), std::memory_order_relaxed);
  }
  bool enabled() const noexcept { return (ctl_.load(std::memory_order_relaxed) & 1u) != 0u; }
  bool pop(Event& out) noexcept {
    std::uint32_t w = 0;
    if (!ring_.pop(w)) {
      return false;
    }
    out = unpack(w);
    return true;
  }
  void clear() noexcept {
    std::uint32_t w = 0;
    for (std::uint32_t i = 0; i < kRingSize && ring_.pop(w); ++i) {
    }
  }
  // Cumulative since start (the consumer subtracts what it saw when its app launched): messages that did not fit, and SysEx messages dropped.
  std::uint32_t overflow() const noexcept { return overflow_.load(std::memory_order_relaxed); }
  std::uint32_t sysex_dropped() const noexcept { return sysex_.load(std::memory_order_relaxed); }

 private:
  struct Parser {
    std::uint8_t status = 0;  // the running channel status, 0 when there is none
    std::uint8_t have = 0;    // data bytes of the message seen so far (0 or 1)
    std::uint8_t first = 0;
  };
  void push(std::uint32_t port, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) noexcept {
    const Event e{static_cast<std::uint8_t>(port < kPorts ? port : 0u), status, d1, d2};
    if (!ring_.push(pack(e))) {
      overflow_.store(overflow_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
    }
  }

  core::SpscRing<std::uint32_t, kRingSize> ring_;
  Parser parser_[kPorts];  // render only
  std::uint32_t seen_ctl_ = 0;  // render only: the ctl_ the parsers were last reset for
  std::atomic<std::uint32_t> ctl_{0};  // bit 0: enabled; the rest: a generation, bumped at every enable (main loop writes, render reads)
  std::atomic<std::uint32_t> overflow_{0};
  std::atomic<std::uint32_t> sysex_{0};
};

}  // namespace o3sdk::platform::mk66::midi
