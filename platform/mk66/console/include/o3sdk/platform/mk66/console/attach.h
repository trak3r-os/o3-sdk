// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What main.cpp calls beyond usb/console.h (the declarations identical to track3r-os's): the o3-sdk console is composed once the app's Runtime exists. Implemented in the MK66 image
// (console/src/console.cpp); the declarations compile everywhere.

namespace o3sdk::app {
class Runtime;
}
namespace o3sdk::platform::mk66::midi {
class MidiService;
}
namespace o3sdk::hal::capture {
class CaptureHw;
}

namespace o3sdk::platform::mk66::console {

// Builds the command table (the kit's commands, then the app's App::diag_commands, a row that breaks a rule skipped and logged), the shell over the CDC port and its pump, and lets
// `state` read the runtime. Once, in the main thread, after rt.start() and before usb0::start(): console::service() does nothing until then.
void attach(app::Runtime& runtime) noexcept;

// The MIDI service `midi` reports on (docs/M2B2-PLAN.md, slice MD): once, main thread, any time before the first command runs (the image's static, which lives for ever). Without it `midi` says
// it is not in this image.
void attach_midi(const midi::MidiService& service) noexcept;

// The capture device `radio` reports on (slice AI of M2c: capture.h's, the one main handed to the Runtime as Platform::capture): once, main thread, any time before the first command runs. Without it `radio`
// says this image has no radio.
void attach_capture(hal::capture::CaptureHw& capture) noexcept;

// usb0::start() has run and put the pull-up on: the mode guard watches the set the device started in (usb_mode_guard.h).
void note_usb_started() noexcept;

}  // namespace o3sdk::platform::mk66::console
