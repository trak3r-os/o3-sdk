// SPDX-License-Identifier: GPL-3.0-or-later
// Pure logic gets a host test: the pad map has no hardware and no state, so a table of cases says what it must do.
#include <doctest/doctest.h>

#include <@APP_ID@/pad_map.h>

#include <o3sdk/app/panel.h>

using namespace @APP_ID@;
using namespace o3sdk::app;

TEST_CASE("pad map: the bottom left pad is C3 and each pad to the right is a semitone up") {
  CHECK(key_note(pad_at(kPadRows - 1u, 0u), 0) == 48);
  CHECK(key_note(pad_at(kPadRows - 1u, 1u), 0) == 49);
  CHECK(key_note(pad_at(kPadRows - 1u, 11u), 0) == 59);
}

TEST_CASE("pad map: each row up is an octave up, and the octave shift moves the whole keyboard") {
  CHECK(key_note(pad_at(kPadRows - 2u, 0u), 0) == 60);
  CHECK(key_note(pad_at(0u, 0u), 0) == 84);
  CHECK(key_note(pad_at(kPadRows - 1u, 0u), -1) == 36);
  CHECK(key_note(pad_at(kPadRows - 1u, 0u), 1) == 60);
}

TEST_CASE("pad map: a note outside 0..127 and a pad past the panel are not keys") {
  CHECK(key_note(kPadCount, 0) == -1);
  CHECK(key_note(pad_at(kPadRows - 1u, 0u), -5) == -1);    // 48 - 60 < 0
  CHECK(key_note(pad_at(0u, 11u), kMaxOctave + 3) == -1);  // far past 127
}

TEST_CASE("pad map: every pad of the panel is a key at every octave the keyboard reaches") {
  for (int octave = kMinOctave; octave <= kMaxOctave; ++octave) {
    for (unsigned pad = 0; pad < kPadCount; ++pad) {
      const int note = key_note(pad, octave);
      INFO("octave " << octave << " pad " << pad);
      CHECK(note >= 0);
      CHECK(note <= 127);
    }
  }
}

TEST_CASE("pad map: the octave is clamped and the white keys are the piano's") {
  CHECK(clamp_octave(-9) == kMinOctave);
  CHECK(clamp_octave(9) == kMaxOctave);
  CHECK(clamp_octave(1) == 1);
  CHECK(is_natural(60));
  CHECK_FALSE(is_natural(61));
  CHECK(is_natural(71));
  CHECK_FALSE(is_natural(70));
}
