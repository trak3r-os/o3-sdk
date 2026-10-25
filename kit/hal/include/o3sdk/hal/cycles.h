// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::hal {

// Free-running CPU cycle counter, to time one stage of the audio path (ADR 0013 D1/D8: `diag cpu`, the render budget,
// the module guard). 32 bits: it wraps every 2^32 cycles (23.9 s at 180 MHz), so only differences are meaningful, taken
// with unsigned subtraction. Wait-free and callable from any context, ISRs included.
//
// Implemented by the platform: platform/mk66 reads the DWT cycle counter. The host and QEMU platforms (QEMU does not
// model DWT, ADR 0013 D8) provide theirs when the first layer that calls it needs one; until then a host or QEMU build
// that references it fails to link, which is the intended signal.
std::uint32_t cycles() noexcept;

}  // namespace o3sdk::hal
