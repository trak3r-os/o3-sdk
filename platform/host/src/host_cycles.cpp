// SPDX-License-Identifier: GPL-3.0-or-later
// hal::cycles() on the host: a monotonic nanosecond count scaled to the MK66's 180 MHz, so that a figure the simulator prints
// is in the same unit as the unit's (and is NOT a measurement of the unit: the host is a different machine).
#include <o3sdk/hal/cycles.h>

#include <chrono>
#include <cstdint>

namespace o3sdk::hal {

std::uint32_t cycles() noexcept {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(ns) * 180u / 1000u);
}

}  // namespace o3sdk::hal
