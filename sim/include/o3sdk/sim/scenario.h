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
//                             (both also note the digest of the frame, ScenarioResult::digests)
//   record on|off             keep the rendered audio
//   wav <name>                <out>/<name>.wav: what has been recorded so far
//   midi <port> <hex...>      MIDI bytes arriving on din or usb now, e.g. `midi din 90 3c 64` (a note on, channel 1, middle C): channel
//                             messages with running status; they land in the block that renders next
//   clock <bpm> start|stop    a MIDI master at <bpm> (decimals allowed: 140.5) sends Start, or Stop; `continue <beats>` is a song position
//                             (in sixteenths) and Continue; `clock off` takes the master away. The engine follows it when FOLLOW is on in
//                             the system Config's MIDI section (or the app set it)
//
//   input tone <hz> [<dbfs>]  what the codec hears at the line jack and the microphone: a sine (default -12 dBFS); `input wav <file>` a 16-bit PCM WAV, looped;
//                             `input off` silence (the default)
//   radio stations <f...>     the stations the fake receiver finds, in MHz (93.7) or 10 kHz units (9370); a tuned one is a sine at freq / 10 Hz in the input while the
//                             source is Radio. `radio absent` is a board with no receiver (the power-up ends in Absent), `radio present` takes it back
//
//   card out|in               the SD card out of its slot (every card call answers NO CARD, a file being written stays torn) and back in (mounted at once)
//   card protect on|off       the card's write-protect switch
//   card disk on|off          the card as a USB disk (docs/M2C-PLAN.md section 7): `on` is what the Config's CARD AS DISK does (Context::usb_audio().share_disk()): the port re-enumerates and the card is
//                             the computer's (card=computer in `state`, every save refused with CARD ON COMPUTER - EJECT FIRST) after about 2.5 s of simulated time; `off` is the eject. A refusal
//                             (the card in use, no card) fails the scenario with its text
//   usb listen on|off         the computer opens (or closes) the capture stream of the USB port: nothing streams until it does
//   usb profile <2|8|10|11> [out|duplex]   the USB layout (the console's `usb profile`): the port re-enumerates, about a second of simulated time, and the factory map of the profile is back;
//                             `usb audio on|off` is the AUDIO set or STANDARD; `usb map <slot 1..> <sources>` (T1+T3, ML, MR, SA, SB, -) edits a slot, `usb map reset`; `usb level <0..100>` is RETURN LEVEL
//   usb return tone <hz> [<dbfs>]|wav <file>|off   what the computer plays to the unit (the return stream: open while the layout has it and the computer listens)
//   usbwav <name>             <out>/<name>.wav: what the computer captured since its stream opened, one channel per channel of the profile (16 bit, 44.1 kHz)
//   reboot                    the unit restarted on the same card: the runtime and the engine made again, App::setup, the kit's settings read from the card
//   expect file <path>        the card holds the file `path` (relative to its root, "Apps/hello/riff.o3a"); `expect nofile <path>` it does not
//   expect backlight <duty>   the platform's backlight was last told this duty (0: never; `--backlight` or this line gives the harness one)
//   expect state <text>       the console's `state` line holds `text` ("files=done", "card=none", "section=display"); a scenario that fails an expect fails with its line
// `card` and `expect file` need a card (<app>_sim --card DIR, or a Harness given one); `reboot` works without one (and keeps nothing).
//
// `input` and `radio` need a capture device (platform/host/fake_capture.h: the simulator makes one when the scenario uses them or with --audio-in); the AUDIO section of the
// system Config and Context::audio_in() / radio() exist only with one.
// `usb`, `usbwav` and `card disk` need a USB audio device (platform/host/fake_usb_audio.h: the simulator makes one when the scenario uses them or with --usb); `card disk` needs a card too.
// `midi` and `clock` need a MIDI device (Harness(app, firmware, &fake_midi); the simulator makes one when the scenario uses them or with
// --midi). Each command that touches the panel is followed by one millisecond, so that the runtime sees events in order.

#include <o3sdk/sim/harness.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace o3sdk::sim {

struct ScenarioResult {
  bool ok = true;
  int line = 0;          // the failing line, 1-based
  std::string error;     // what was wrong
  unsigned snapshots = 0;
  // One entry per snap and boot, in order: the name and EveRaster::last_frame_digest() of the picture it took (the display-list words, FNV-1a).
  std::vector<std::pair<std::string, std::uint32_t>> digests;
};

// Runs `text` against a started harness. Snapshots and WAV files go to `out_dir` (which must exist).
ScenarioResult run_scenario(Harness& h, const std::string& text, const std::string& out_dir);

}  // namespace o3sdk::sim
