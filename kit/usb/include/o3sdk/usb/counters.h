// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::usb {

// Counters for the future `diag usb` command (ADR 0012 Decision 2: IN size histogram, OUT ring min/max fill,
// under/overruns, feedback min/max). Plain 32-bit words: every field has ONE writer context (named below) and
// any context may read it; a reader sees each word whole (aligned 32-bit access) but not a consistent set
// across fields. That is enough for diagnostics and is why nothing here is atomic or locked.
struct Counters {
  // Written by the USB driver's SOF ISR (OS-006b), not by this library: it calls note_sof() and note_feedback().
  std::uint32_t sof = 0;                // SOF interrupts seen
  std::uint32_t fb_min = 0xFFFFFFFFu;   // feedback values sent (native format), min / max / last
  std::uint32_t fb_max = 0;
  std::uint32_t fb_last = 0;
  // Written by InStream / OutStream, in the USB ISR.
  std::uint32_t in_size_lo = 0;         // IN packets of the low frame count (44 at FS)
  std::uint32_t in_size_hi = 0;         // IN packets of the high frame count (45 at FS)
  std::uint32_t in_size_other = 0;      // any other size: must stay 0
  std::uint32_t in_prime_packets = 0;   // silence packets sent while the stem ring builds its start-up fill
  std::uint32_t in_underruns = 0;       // packets that found fewer frames than they needed (zero filled)
  std::uint32_t in_dropped_blocks = 0;  // stem blocks overwritten before the USB side read them (ring overrun)
  std::uint32_t in_fill_min = 0xFFFFFFFFu;  // frames buffered at packet time, min / max since reset
  std::uint32_t in_fill_max = 0;
  std::uint32_t out_packets = 0;        // OUT (return) packets received
  std::uint32_t out_bad_packets = 0;    // not a whole number of frames, or longer than the ring accepts
  std::uint32_t out_fill_min = 0xFFFFFFFFu;  // return ring fill seen at packet time, frames
  std::uint32_t out_fill_max = 0;
  // Written by the engine's pull (ReturnReader).
  std::uint32_t out_dropped_frames = 0;  // return frames overwritten before the engine read them (ring overrun)
  std::uint32_t out_underruns = 0;       // pulls that found fewer frames than asked (silence for the rest)

  void note_sof() noexcept { ++sof; }
  void note_feedback(std::uint32_t value) noexcept {
    fb_last = value;
    fb_min = value < fb_min ? value : fb_min;
    fb_max = value > fb_max ? value : fb_max;
  }

  void reset() noexcept { *this = Counters{}; }
  // Restart only the min/max fill tracking (after the start-up transient, in tests and in `diag usb reset`).
  void reset_fill_range() noexcept {
    in_fill_min = 0xFFFFFFFFu;
    in_fill_max = 0;
    out_fill_min = 0xFFFFFFFFu;
    out_fill_max = 0;
    fb_min = 0xFFFFFFFFu;
    fb_max = 0;
  }
};

}  // namespace o3sdk::usb
