// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's capture control (OS-011c): the hal::capture::CaptureHw of the board. The SGTL5000's input switch and gain (src/audio.cpp applies the register plans of
// sgtl5000.h), the analogue switch PTE26 between the line jack and the FM receiver (board.h kInputSourceSelect), and the Si4703 radio (si4703.h) on the control bus.
// Implemented in the MK66 image only (src/capture.cpp); the declaration compiles everywhere so the header check passes on every platform.
//
// Main thread only: every codec and radio transfer is I2C0's, whose clients are all the main loop (i2c_bus.h). service() is called once a pass of the main loop.

#include <o3sdk/hal/capture.h>

#include <cstddef>

namespace o3sdk::platform::mk66 {

// The capture device's own memory: about 130 bytes of state (the radio's register shadow and status), kept out of SRAM_U, which has no room: the composition root places it
// in SDRAM (app_main.cpp, behind the other parts of the sample arena's tail) and gives it to capture_create(). Sized with room to spare, checked by a static_assert in capture.cpp.
inline constexpr std::size_t kCaptureStorageBytes = 192;
inline constexpr std::size_t kCaptureStorageAlign = 8;

// Constructs the one capture device in `storage` (kCaptureStorageBytes, aligned to kCaptureStorageAlign) and returns it. Call it once (the composition root does, when the
// sound is built); the device has no pointer of its own in SRAM_U, the root keeps the address. Its radio is Off until a radio source is selected or radio_power(true).
hal::capture::CaptureHw& capture_create(void* storage) noexcept;

// `diag radio` (OS-RAD1), on the device capture_create() returned: the radio driver's text (si4703.h Radio::describe) followed by the levels of its lines (RST#, SDA, SCL, the source
// switch PTE26), at most `cap` bytes, and the length; and the power-up again from the reset (Radio::restart). Main loop only.
std::size_t capture_radio_report(hal::capture::CaptureHw& hw, char* out, std::size_t cap) noexcept;
void capture_radio_restart(hal::capture::CaptureHw& hw) noexcept;

}  // namespace o3sdk::platform::mk66
