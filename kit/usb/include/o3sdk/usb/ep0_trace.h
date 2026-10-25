// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

// OS-USBDISK U2a: a ring of endpoint 0's events, for the unit. The runtime DISK layout did not enumerate on a Linux host (int.12: the first 9-byte configuration read answered -71 while the device's counters
// saw no error), and nothing on the device says what the controller did in between; this keeps the first kEntries events of the enumeration: every SETUP (the request), every finished token (odd or even
// descriptor, DATA PID, byte count, buffer address), every transmit armed, every stall, the address, the bus reset and a bus error, each with the cycles since the previous event.
//
// Protocol (no lock): the USB interrupt is the only writer once the trace is armed; arm() and disarm() are called by the main loop with the USB interrupt masked (the detach / attach of usb0, or the cold
// start), so they never race the writer. A reader (the console's `usb ep0`) loads count() with acquire and reads entries below it: an entry is stored first and published by the release store of count,
// and nothing below count is ever rewritten until the next arm(). The ring does not wrap (the start of the enumeration is what matters): when full it stops. 12 bytes an entry, 384 bytes the ring plus a 4-byte retry counter (the STALL handshakes after the first of a stall, which the ring does not keep), in .bss.
namespace o3sdk::usb {

class Ep0Trace {
 public:
  static constexpr unsigned kEntries = 32;

  enum class Tag : std::uint8_t {
    Reset = 1,   // a bus reset (or the prepare() of an attach): endpoint 0 re-armed, address 0
    Setup,       // a SETUP finished: w0 = bytes 0..3 of the packet (bmRequestType, bRequest, wValue), extra = wIndex, bytes = wLength
    InDone,      // a transmit descriptor finished (the host took the packet): w0 = its buffer
    OutDone,     // a receive descriptor finished with an OUT (the status stage, or a data packet): w0 = its buffer
    ArmIn,       // a transmit descriptor handed to the controller: w0 = its buffer, bytes, the DATA PID in flags
    StallSet,    // the control pipe stalled endpoint 0 (EPSTALL set)
    StallSeen,   // the controller reported the first stall handshake of an endpoint 0 stall (ISTAT STALL); the later ones are the host's retries and only counted (stall_retries)
    Address,     // the address register written: w0 = the address
    Error,       // ERRSTAT was non-zero: w0 = its bits
    Sleep,       // the bus went idle (ISTAT SLEEP)
    SetupArmed,  // after the first stall sent: w0 = the receive descriptors the controller owns (bit 0 even, bit 1 odd), extra bit 0 = the token freeze was released (CTL written), bit 1 = TXSUSPENDTOKENBUSY read back after that (set: the controller is still deaf to tokens)
    StallCleared,  // a SETUP ended the protocol stall of endpoint 0 (the BD stall on the transmit descriptors): right after that SETUP's own event. The stall lives from `stall set` until this event
  };
  // flags: bit 0 odd descriptor, bit 1 transmit, bit 2 DATA1, bits 4..7 the token PID of a finished descriptor (0 when none).
  struct Entry {
    std::uint32_t w0 = 0;
    std::uint16_t bytes = 0;
    std::uint16_t dt64 = 0;  // cycles since the previous entry in units of 64, saturating (65535 = 23 ms or more); the first entry holds 0
    std::uint16_t extra = 0;
    std::uint8_t tag = 0;
    std::uint8_t flags = 0;
  };
  static_assert(sizeof(Entry) == 12, "12 bytes an entry");

  // Main loop, USB interrupt masked: forget the old events and record from now on.
  void arm() noexcept {
    count_.store(0, std::memory_order_relaxed);
    retries_.store(0, std::memory_order_relaxed);
    last_cycles_ = 0;
    armed_.store(true, std::memory_order_release);
  }
  // Main loop, USB interrupt masked: stop recording, keep what was recorded.
  void disarm() noexcept { armed_.store(false, std::memory_order_release); }
  bool armed() const noexcept { return armed_.load(std::memory_order_relaxed); }

  // USB interrupt (or the main loop before the interrupt line is on). Bounded, wait-free, no allocation.
  void record(Tag tag, std::uint32_t cycles, std::uint8_t flags, std::uint32_t w0, std::uint32_t bytes, std::uint32_t extra) noexcept {
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    if (n >= kEntries || !armed_.load(std::memory_order_relaxed)) {
      return;
    }
    const std::uint32_t gap = n == 0u ? 0u : (cycles - last_cycles_) >> 6;
    last_cycles_ = cycles;
    Entry& e = entries_[n];
    e.w0 = w0;
    e.bytes = static_cast<std::uint16_t>(bytes);
    e.dt64 = static_cast<std::uint16_t>(gap > 0xFFFFu ? 0xFFFFu : gap);
    e.extra = static_cast<std::uint16_t>(extra);
    e.tag = static_cast<std::uint8_t>(tag);
    e.flags = flags;
    count_.store(n + 1u, std::memory_order_release);
  }

  // USB interrupt, only while armed: one more STALL handshake of an endpoint 0 stall after its first (the host retried a token the BD stall answered). The ring keeps the first handshake only.
  void note_stall_retry() noexcept {
    if (armed_.load(std::memory_order_relaxed)) {
      retries_.store(retries_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    }
  }
  // The retries of the attach: greater than 0 means a host retried a stalled token and was stalled again instead of NAKed.
  std::uint32_t stall_retries() const noexcept { return retries_.load(std::memory_order_relaxed); }

  unsigned count() const noexcept { return count_.load(std::memory_order_acquire); }
  // Valid for index < count().
  const Entry& entry(unsigned index) const noexcept { return entries_[index < kEntries ? index : kEntries - 1u]; }

  static constexpr std::uint8_t make_flags(bool odd, bool tx, bool data1, std::uint32_t pid) noexcept {
    return static_cast<std::uint8_t>((odd ? 1u : 0u) | (tx ? 2u : 0u) | (data1 ? 4u : 0u) | ((pid & 0xFu) << 4));
  }

 private:
  std::atomic<std::uint32_t> count_{0};
  std::atomic<bool> armed_{false};
  std::atomic<std::uint32_t> retries_{0};  // writer: the interrupt, while armed; cleared by arm()
  std::uint32_t last_cycles_ = 0;  // writer only
  Entry entries_[kEntries] = {};
};

}  // namespace o3sdk::usb
