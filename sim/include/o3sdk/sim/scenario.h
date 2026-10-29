// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Scenario scripts for the simulator: one command a line, `#` starts a comment.
//
//   wait <ms>                 let time pass
//   press <button>            a button down (names: app/panel.h button_name: f1, play, shift, sampleload ...)
//   release <button>          and up
//   tap <button> [<ms>]       down, <ms> (default 60), up
//   pad <n> [<ms>]            pad n (0..47, row by row from the top left) down, <ms> (default 150), up
//   paddown <n> / padup <n>
//   turn <detents>            the encoder (+ clockwise)
//   snap <name>               <out>/<name>.png: the screen above the pads
//   boot <name>               the boot screen (the logo of this image, ui/boot_logo.h) as the first frame, to <out>/<name>.png
//   record on|off             keep the rendered audio
//   wav <name>                <out>/<name>.wav: what has been recorded so far
//   midi <port> <hex...>      MIDI bytes arriving on din or usb now, e.g. `midi din 90 3c 64` (a note on, channel 1, middle C): channel
//                             messages with running status; they land in the block that renders next
//   clock <bpm> start|stop    a MIDI master at <bpm> (decimals allowed: 140.5) sends Start, or Stop; `continue <beats>` is a song position
//                             (in sixteenths) and Continue; `clock off` takes the master away. The engine follows it when FOLLOW is on in
//                             the system Config's MIDI section (or the app set it)
//
//   card out|in               the SD card out of its slot (every card call answers NO CARD, a file being written stays torn) and back in (mounted at once)
//   card protect on|off       the card's write-protect switch
//   reboot                    the unit restarted on the same card: the runtime and the engine made again, App::setup, the kit's settings read from the card
//   expect file <path>        the card holds the file `path` (relative to its root, "Apps/hello/riff.o3a"); `expect nofile <path>` it does not
//   expect state <text>       the console's `state` line holds `text` ("files=done", "card=none", "section=display"); a scenario that fails an expect fails with its line
// `card` and `expect file` need a card (<app>_sim --card DIR, or a Harness given one); `reboot` works without one (and keeps nothing).
//
// `midi` and `clock` need a MIDI device (Harness(app, firmware, &fake_midi); the simulator makes one when the scenario uses them or with
// --midi). Each command that touches the panel is followed by one millisecond, so that the runtime sees events in order.

#include <o3sdk/sim/harness.h>

#include <string>

namespace o3sdk::sim {

struct ScenarioResult {
  bool ok = true;
  int line = 0;          // the failing line, 1-based
  std::string error;     // what was wrong
  unsigned snapshots = 0;
};

// Runs `text` against a started harness. Snapshots and WAV files go to `out_dir` (which must exist).
ScenarioResult run_scenario(Harness& h, const std::string& text, const std::string& out_dir);

}  // namespace o3sdk::sim
