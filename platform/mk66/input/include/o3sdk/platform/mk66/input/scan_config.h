// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Timing and bounds of the input scan, all in one place (OS-007d). Times are microseconds of hal::Clock::now().

#include <cstdint>

namespace o3sdk::platform::mk66::input::scan {

// Lockout debounce of a key (key_filter.h): the first edge of a key is accepted at once, then the key is deaf to the opposite
// edge for this long. The TCA8418 debounces in hardware already; this is the second line, sized for a tactile switch's bounce
// (a few milliseconds) and far below the shortest tap a hand makes (tens of ms). Zero added latency on a press.
inline constexpr std::uint32_t kKeyLockoutUs = 5'000;

// The safety poll: a chip is read this often even if its INT line says nothing, so an INT that is not wired as assumed, or an
// edge the chip raised while its status was being cleared, costs this much latency and no key.
inline constexpr std::uint32_t kSafetyPollUs = 10'000;

// A chip that is answering is also asked, this often, whether it still holds its configuration (a power glitch resets a TCA8418
// without any bus error, and it would then scan nothing for ever): CFG is read back. A chip found at its reset value lost its FIFO
// and the keys in it, so the keys believed down are released (a resync); any other mismatch is only configured again.
inline constexpr std::uint32_t kHealthUs = 500'000;

// The stamp of the last event (scanner.h, stamp()) is only meaningful against the clock while the two are less than 2^31 us apart. A
// stamp this far behind the clock (a silence of about 18 minutes or more) is dropped for the clock's value; otherwise the first
// event after a silence of 36 to 72 minutes would be stamped with that stale value, which the wrap-safe comparison reads as AHEAD of
// the clock, and the gesture layer would run its time forward and see no hold until the clock caught up (OS-BUG-HOLD).
inline constexpr std::uint32_t kStampStaleUs = 1u << 30;

// After a failed transfer a chip is left alone for the backoff, doubled after each further failure up to the maximum, so a dead
// or unplugged chip costs one bounded attempt every 320 ms and never starves the main loop. A success resets it.
inline constexpr std::uint32_t kBackoffMinUs = 10'000;
inline constexpr std::uint32_t kBackoffMaxUs = 320'000;
// The bus is recovered (nine SCL pulses and a STOP) from the second failure in a row on.
inline constexpr unsigned kRecoverAfterFailures = 2;

// Events one pass takes from one chip (the FIFO is ten deep): at 375 kHz a register read is about 115 us, so a pass spends at
// most about 0.6 ms a chip, and a FIFO holding more is finished on the next pass without waiting for INT.
inline constexpr unsigned kMaxEventsPerDrain = 4;

// Boot: the chip configuration is attempted this many times, with a bus recovery in between.
inline constexpr unsigned kConfigureAttempts = 3;

}  // namespace o3sdk::platform::mk66::input::scan
