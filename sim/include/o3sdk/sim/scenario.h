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
//   record on|off             keep the rendered audio
//   wav <name>                <out>/<name>.wav: what has been recorded so far
//
// Each command that touches the panel is followed by one millisecond, so that the runtime sees events in order.

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
