// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// The faults the image found while starting and went on with (OS-006b): a start-up step that failed does not stop the image when the console can
// still be brought up, because `diag` is the way to find out what failed. `main` records each with console::note_boot_fault() and `diag stage` lists
// them; the LED shows the matching code (blink.h) for good, as the display's failures always have.
namespace o3sdk::platform::mk66::console {

enum BootFault : std::uint32_t {
  kFaultAudio = 1u << 0,    // the audio bring-up failed (codec, MCLK divider, no I2S boundaries): the outputs stay muted, the OS runs without sound
  kFaultDisplay = 1u << 1,  // the FT812 did not come up, or stopped swapping frames: the OS runs headless
  kFaultApp = 1u << 2,      // the app could not start (its bitmaps did not go up): the display is held dark
  kFaultSd = 1u << 3,       // OS-007e: a card is in the slot and did not mount (unsupported, failing): the OS runs without storage (no card at all is not a fault)
};

}  // namespace o3sdk::platform::mk66::console
