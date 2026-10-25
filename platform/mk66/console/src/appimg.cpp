// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's composition root, as the modules ported from track3r-os see it (src/internal.h, namespace appimg): what the firmware update path (update/src/fw_host_mk66.cpp) asks of
// the app, and the one flag main keeps with the console about the outputs. track3r-os's definitions are the tracker's (its sequencer, its project loader, its power switch); an o3-sdk
// app has none of those yet, so each is the smallest true thing:
//   storage_in_use       no app saves or loads from the card yet (the app's Config settings and the pattern store come with the card milestone): false.
//   stop_transport       the engine's transport (a Stop on its ring; the clocked hooks end their notes at the next block).
//   audio_closed         the outputs were muted behind main's back (`fw update`'s quiesce): main opens them again through the gate once the update path let go.
//   service_while_blocked  the idle hook (display service + the audio beat) stays installed for good in main.cpp, so a blocking call needs nothing put in place and nothing put back.
#include "../../src/internal.h"

#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/mk66/update/fw_device.h>

namespace o3sdk::platform::mk66::appimg {

namespace {

dsp::Engine* g_engine = nullptr;  // main loop only
bool g_outputs_open = false;      // main loop only: main sets it with the unmute, audio_closed() clears it

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

}  // namespace o3sdk::platform::mk66::appimg
