// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::core {

// Monotonic clock value: unsigned 32-bit microseconds since an arbitrary epoch (boot).
// It WRAPS every 2^32 us (about 71.6 minutes), so never compare two ticks with < or >; use
// tick_elapsed() / tick_reached(), which are valid for spans below about 35.8 minutes. Anything that can
// run longer (idle or auto-save timers, session clocks) must use hal::Clock::uptime_ms(), a 64-bit
// millisecond counter that does not wrap. The source of ticks is the hal::Clock contract.
using Tick = std::uint32_t;

inline constexpr Tick kTicksPerMs = 1000;
inline constexpr Tick kTicksPerSecond = 1000000;

// Converts milliseconds to ticks; the result wraps modulo 2^32 like every other tick.
constexpr Tick ticks_from_ms(std::uint32_t ms) noexcept { return ms * kTicksPerMs; }

// Ticks from `since` to `now`, modulo 2^32: correct across a wrap while the true span is below 2^32.
constexpr std::uint32_t tick_elapsed(Tick now, Tick since) noexcept { return now - since; }

// True when `now` is at or after `deadline`. Valid while the two are less than 2^31 ticks
// (about 35.8 minutes) apart, which covers every UI and sequencer timeout.
constexpr bool tick_reached(Tick now, Tick deadline) noexcept { return (now - deadline) < 0x80000000u; }

// `stamp` unless it is ahead of `now` (OS-STAMPCLAMP), then `now`. For an event stamp that a source got wrong (a future one would hold a
// recogniser's time back until the clock caught up). Wrap-aware: ahead means less than 2^31 ticks in front of `now`; an in-range stamp is returned as it is.
constexpr Tick tick_clamp_to_now(Tick stamp, Tick now) noexcept { return tick_reached(now, stamp) ? stamp : now; }

}  // namespace o3sdk::core
