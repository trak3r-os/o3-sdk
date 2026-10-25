// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// The `diag log` ring (ADR 0011 item 1, ADR 0013 D7 "diag log"): a fixed-record ring that boot code and drivers write short
// lines to and that the console reads back. Pure: the platform gives it the storage (an SDRAM region on the MK66, an array in the
// tests) and the millisecond stamp.
//
// Contexts. ANY context may write, ISRs included, concurrently: a writer claims the next sequence number with one atomic
// fetch_add (lock-free; on the Cortex-M4 an LDREX/STREX loop that an interrupt simply restarts), fills its own slot and publishes
// it with a release store of the sequence number into the slot. There is no lock, so an ISR can never wait for the main loop. ONE
// context reads (the console, in the main loop). A reader sees a record only when its slot holds exactly the sequence number it
// asked for before and after the copy, so a slot that is being rewritten or was lapped is skipped, not shown torn. The ring keeps
// the newest `capacity` records; older ones are overwritten, never blocking.
//
// Cost: a write is one fetch_add, a 64-byte copy and two stores (no formatting: callers pass finished text).
namespace o3sdk::usb::diag {

inline constexpr std::size_t kLogTextMax = 55;  // characters, a NUL follows
inline constexpr std::size_t kLogTextWords = 14;  // the text is stored as 14 words: 56 bytes

// The record is a sequence word, a stamp and the text, every word an atomic accessed with relaxed (or, under ThreadSanitizer, acquire /
// release) operations: a plain ldr / str on the Cortex-M4, and a reader that races a writer on a lapped slot reads defined values and
// discards them by the sequence check instead of invoking undefined behaviour (the same technique as OverwriteRing's slots).
struct LogRecord {
  std::atomic<std::uint32_t> seq;  // sequence number + 1 once the record is complete; 0 while empty or being written
  std::atomic<std::uint32_t> ms;   // the writer's millisecond stamp
  std::atomic<std::uint32_t> text[kLogTextWords];
};
static_assert(sizeof(LogRecord) == 64 && std::atomic<std::uint32_t>::is_always_lock_free, "64-byte records, lock-free words");
static_assert(kLogTextWords * 4u == kLogTextMax + 1u, "55 characters and the NUL fill the text words");

struct LogEntry {
  std::uint32_t sequence = 0;  // 0-based, counts every record ever written
  std::uint32_t ms = 0;
  char text[kLogTextMax + 1] = {};
};

class LogRing {
 public:
  LogRing() = default;
  LogRing(const LogRing&) = delete;
  LogRing& operator=(const LogRing&) = delete;

  // Gives the ring its storage (`count` records, a power of two, 2..2^20) and clears it. Call once, before the first write, in
  // one context. Until then write() does nothing and ready() is false. The storage need not be zeroed (SDRAM is not).
  void init(LogRecord* slots, std::uint32_t count) noexcept;
  bool ready() const noexcept { return slots_ != nullptr; }
  std::uint32_t capacity() const noexcept { return mask_ + 1u; }

  // Any context. Text longer than kLogTextMax is cut.
  void write(std::uint32_t ms, const char* text) noexcept;

  // Records ever written (the next sequence number).
  std::uint32_t written() const noexcept { return head_.load(std::memory_order_acquire); }
  // The oldest sequence number that can still be read (0 until the ring has wrapped).
  std::uint32_t oldest() const noexcept {
    const std::uint32_t head = written();
    return head > capacity() ? head - capacity() : 0u;
  }
  // Reader only. False when `sequence` is not readable now (not written yet, overwritten, or being rewritten).
  bool read(std::uint32_t sequence, LogEntry& out) const noexcept;

 private:
  LogRecord* slots_ = nullptr;
  std::uint32_t mask_ = 0;
  std::atomic<std::uint32_t> head_{0};
};

}  // namespace o3sdk::usb::diag
