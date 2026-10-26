// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pads as a chromatic keyboard: the 12 columns are one octave, each row up is the next octave. Pure (no hardware, no state), so a
// host test covers it (tests/pad_map_test.cpp). A sequencer's step pads, modifier pads and so on are decided here too, in one place,
// so the screen's input, its LEDs and its drawing agree (o3-303 does it in app/include/o3_303/pad_map.h).

#include <o3sdk/app/panel.h>

namespace @APP_ID@ {

inline constexpr int kBottomLeftNote = 48;  // C3, the bottom left pad at octave shift 0
inline constexpr int kMinOctave = -3;
inline constexpr int kMaxOctave = 2;

// The MIDI note of `pad` (0..47, row by row from the top left) with the keyboard shifted by `octave` octaves, or -1 when the pad is past
// the panel or the note falls outside 0..127.
constexpr int key_note(unsigned pad, int octave) noexcept {
  if (pad >= o3sdk::app::kPadCount) {
    return -1;
  }
  const int octaves_up = static_cast<int>(o3sdk::app::kPadRows - 1u - o3sdk::app::pad_row(pad));  // 0 on the bottom row
  const int note = kBottomLeftNote + 12 * (octave + octaves_up) + static_cast<int>(o3sdk::app::pad_col(pad));
  return note >= 0 && note <= 127 ? note : -1;
}

constexpr int clamp_octave(int octave) noexcept {
  return octave < kMinOctave ? kMinOctave : (octave > kMaxOctave ? kMaxOctave : octave);
}

// A white key of the piano (C D E F G A B), for the drawing and the LEDs.
constexpr bool is_natural(int note) noexcept {
  const int pc = note % 12;
  return pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
}

}  // namespace @APP_ID@
