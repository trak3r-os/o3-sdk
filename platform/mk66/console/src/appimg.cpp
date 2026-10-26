// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's composition root, as the modules ported from track3r-os see it (src/internal.h, namespace appimg): what the firmware update path (update/src/fw_host_mk66.cpp) asks of
// the app, and the one flag main keeps with the console about the outputs. track3r-os's definitions are the tracker's (its sequencer, its project loader, its power switch); an o3-sdk
// app has none of those yet, so each is the smallest true thing:
//   storage_in_use       no app saves or loads from the card yet (the app's Config settings and the pattern store come with the card milestone): false.
//   stop_transport       the engine's transport (a Stop on its ring; the clocked hooks end their notes at the next block).
//   audio_closed         the outputs were muted behind main's back (`fw update`'s quiesce): main opens them again through the gate once the update path let go.
//   service_while_blocked  the idle hook (display service + the audio beat) stays installed for good in main.cpp, so a blocking call needs nothing put in place and nothing put back.
// The Power switch's hooks (docs/M2B2-PLAN.md, slice P; the shutdown of power/src/power_host_mk66.cpp asks them), the same smallest-true-thing rule:
//   power_off_latched      the latch of stage A: set first, never cleared (a restart ends the run). main.cpp's audio poll and its card watch read it.
//   power_export_*         no export exists (no WAV, stems or project export in an o3-sdk app): not running, nothing to stop.
//   power_quiesce          stage A: the latch FIRST, then the keys, pads and encoder dropped at the console's tap, the transport stopped (a Stop on its ring: the clocked hooks end their notes
//                          at the next block). The runtime's own lock and the strip are main.cpp's (it holds the Runtime); the app has no power mode of its own.
//   power_save_*           no save layer (docs/M2B2-PLAN.md D4: the settings are not saved): not busy, not committing, nothing to cancel.
//   power_display_off      the end of stage E: the display is dark and the chip in power-down; main.cpp asks power_display_is_off() and touches neither the display nor the runtime from there.
#include "../../src/internal.h"

#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/mk66/update/fw_device.h>
#include <o3sdk/platform/mk66/usb/console.h>

namespace o3sdk::platform::mk66::appimg {

namespace {

dsp::Engine* g_engine = nullptr;  // main loop only
bool g_outputs_open = false;      // main loop only: main sets it with the unmute, audio_closed() clears it
// OS-POWER P1: the power-off latch (set at stage A of the shutdown, never cleared) and the end of stage E (the display is dark and held, the chip in power-down). Main loop only: the shutdown's
// PowerHost and main.cpp's loop run in the same thread.
bool g_power_latched = false;
bool g_display_off = false;

}  // namespace

void bind_engine(dsp::Engine& engine) noexcept { g_engine = &engine; }

void service_while_blocked(bool) noexcept {}

bool storage_in_use() noexcept { return false; }

void stop_transport() noexcept {
  if (g_engine != nullptr) {
    (void)g_engine->stop();
  }
}

void audio_closed() noexcept { g_outputs_open = false; }
bool audio_outputs_open() noexcept { return g_outputs_open; }
void audio_opened() noexcept { g_outputs_open = true; }

// The update path holds the card from `fw put` to the end of `fw update`'s hand-over (an upload, its read-back, an arm, a re-check, the wait for the bootloader): the same
// answer track3r-os gives for its card claim and for the output gate (app_main.cpp firmware_has_card, firmware_update_active).
bool firmware_update_active() noexcept { return update::fw_phase_holds_card(update::device::service().phase()); }

// ---- OS-POWER P1 (internal.h has the contract; power/src/power_host_mk66.cpp is the caller) ----------------------------------------------------------------------
bool power_off_latched() noexcept { return g_power_latched; }
void power_display_off() noexcept { g_display_off = true; }
bool power_display_is_off() noexcept { return g_display_off; }

bool power_export_running() noexcept { return false; }
void power_export_stop() noexcept {}

void power_quiesce() noexcept {
  g_power_latched = true;  // FIRST: from here nothing reopens the outputs and the audio poll stands down
  console::lock_input();   // the keys, pads and the encoder are dropped at the tap (D2); the shell's gate refuses the commands that would start anything
  stop_transport();        // the transport ends; its notes end at the next block
}

bool power_save_busy() noexcept { return false; }
bool power_save_committing() noexcept { return false; }
void power_save_cancel() noexcept {}

}  // namespace o3sdk::platform::mk66::appimg
