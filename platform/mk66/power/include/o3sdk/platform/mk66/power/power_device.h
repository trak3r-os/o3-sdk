// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER P1: the image's Power switch and shutdown. The pin PTC3 (board.h kPowerSwitch), the core::PowerHold fed from it once per pass of the main loop, the PowerService over the image's
// PowerHost (the engine and audio stop, the card, USB, the lights, the restart) and the injection `diag power press|release|pin`. Declared everywhere, defined only in the image
// (power/src/power_host_mk66.cpp). Main loop only.

#include <o3sdk/core/power_hold.h>
#include <o3sdk/platform/mk66/power/power_report.h>
#include <o3sdk/platform/mk66/power/power_service.h>

#include <cstdint>

namespace o3sdk::platform::mk66::power::device {

// Once, before the first pass: PTC3 as a GPIO input with the board's pull (board.h kPowerPull); nothing is ever driven on it.
void init() noexcept;

// One pass: reads the pin (or the injected level), feeds the hold, steps the shutdown, and returns the line and the strip the app is told (App::set_power). `shift_down`: the Shift button;
// `ready`: the app has the screen (the boot gate has let go, no load phase). Logs the "held or stuck since boot" line once.
core::PowerShow pass(std::uint32_t now_ms, bool shift_down, bool ready) noexcept;

// OS-SCREENSHOT: Power went down with Shift already down since the last call (core::PowerHold::Result::screenshot, a one-shot that this call reads and clears), and whether the switch is
// down now (the debounced level: the picture waits for both Power and Shift to be released).
bool take_screenshot_request() noexcept;
bool pressed() noexcept;

// The one-way commit (D2): the stages are running. The input, the MIDI input and the console are locked from here.
bool locked() noexcept;

// OS-POWER P2: whether the end of a real off (a hold, `power off`) is the VLPR off (the build option O3SDK_POWER_VLPR_OFF, ON by default) or the restart of P1 (OFF: the fallback of the decision that
// ships P1's restart-off if the wake fails on the unit). `dry` is always the restart.
bool vlpr_off() noexcept;

// `diag power off [dry]`: the stages without a hold (the refusals of stage 0 apply, with no second hold).
Refusal request_off(std::uint32_t now_ms, bool dry) noexcept;

// `diag power press|release|pin`: what feeds the hold in place of the pin (Real: the pin).
void inject(Inject how) noexcept;

// `diag power`: the numbers, the pin read now in the three pulls.
PowerReport report() noexcept;

}  // namespace o3sdk::platform::mk66::power::device
