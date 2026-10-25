// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::hal {

// Monotonic time source, in two widths. Implementations: platform/host (fake, manually advanced),
// platform/mk66 (hardware timer).
//
//   now()        32-bit microseconds. WRAPS every 2^32 us (about 71.6 minutes) and is only meaningful for
//                short spans compared through core::tick_elapsed / core::tick_reached (valid below about
//                35.8 minutes). Wait-free; callable from any context including ISRs.
//   uptime_ms()  64-bit milliseconds since boot. Does not wrap in practice (hundreds of millions of
//                years). Use it for anything that can outlive a wrap of now(): auto-save and idle timers,
//                session clocks, "time since" displays. Never torn: on a 32-bit target the implementation
//                composes the two halves with a retry loop or a short critical section, so it is also
//                safe from an ISR.
//
// Both are non-decreasing and refer to the same instant of the same clock.
class Clock {
 public:
  virtual core::Tick now() const noexcept = 0;
  virtual std::uint64_t uptime_ms() const noexcept = 0;

 protected:
  Clock() = default;
  ~Clock() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
};

}  // namespace o3sdk::hal
