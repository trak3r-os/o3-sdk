// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What main.cpp calls beyond usb/console.h (the declarations identical to track3r-os's): the o3-sdk console is composed once the app's Runtime exists. Implemented in the MK66 image
// (console/src/console.cpp); the declarations compile everywhere.

namespace o3sdk::app {
class Runtime;
}

namespace o3sdk::platform::mk66::console {

// Builds the command table (the kit's commands, then the app's App::diag_commands, a row that breaks a rule skipped and logged), the shell over the CDC port and its pump, and lets
// `state` read the runtime. Once, in the main thread, after rt.start() and before usb0::start(): console::service() does nothing until then.
void attach(app::Runtime& runtime) noexcept;

// usb0::start() has run and put the pull-up on: the mode guard watches the set the device started in (usb_mode_guard.h).
void note_usb_started() noexcept;

}  // namespace o3sdk::platform::mk66::console
