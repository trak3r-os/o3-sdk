// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB layout every boot presents (docs/M2B-PLAN.md D5): track3r-os's default, the AUDIO set with profile 8 and the stereo return in ("8 OUT / 2 IN"): the master on channels 7 and 8, channels 1
// to 6 silent, the return stream's endpoint present and what arrives discarded. One place: main.cpp starts the driver with it (usb0::start), and tests/mk66_usb_bringup_test.cpp builds the
// descriptors of the same three values and pins them to track3r-os's golden bytes, so the test is about what the image boots with and not about a literal that could drift from it.
// (usb0.cpp keeps its own constant for the profile of the STANDARD set it builds before start().)

#include <o3sdk/usb/usb_types.h>

namespace o3sdk::platform::mk66::usb0::boot_layout {

inline constexpr o3sdk::usb::Mode kMode = o3sdk::usb::Mode::Audio;
inline constexpr o3sdk::usb::Profile kProfile = o3sdk::usb::Profile::P8;
inline constexpr bool kReturn = true;

}  // namespace o3sdk::platform::mk66::usb0::boot_layout
