// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the board's two key matrices mean (OS-007d). Both are TCA8418 keypad scanners, 8 rows x 10 columns each, which report a key
// as a code 1..80 = row * 10 + column + 1 with bit 7 set on a press (TCA8418 datasheet, KEY_EVENT_A: the key number is the low 7 bits, the state bit 7). In this file a key is the
// zero-based `row * 10 + column`, 0..79.
//
// The wiring is a board fact; the rules below are tested against the
// corners and the inverses (mk66_input_map_test.cpp).
//
//   Pads:    48 = 12 x 4. Pad index = 12 * pad_row + pad_column, pad row 0 first (the numbering of the unit's note
//            layout, which puts the highest notes on pads 0..11). Ten pad columns sit on matrix
//            columns 0..9 of rows 0..3; the last two pad columns sit on matrix rows 4..7, columns 9 and 8 (reversed): the 12 x 4
//            grid is wired as a 10 x 4 block plus a 2 x 4 block.
//   Buttons: 33 = codes 0..32 (the same numbers as the kit's o3sdk::app::Button, kit/app/include/o3sdk/app/panel.h; the platform
//            does not depend on kit/app, so mk66_input_map_test.cpp pins them by value). F1..F8 are matrix row 7, columns 7 down to 0;
//            the 25 others are a 5 x 5 block in rows 0..4, columns 0..4, row by row from code 8.
//   Encoder: one, id kEncoderId (the kit's app::kEncoder), a signed detent delta, + clockwise.

#include <cstdint>

namespace o3sdk::platform::mk66::input {

inline constexpr unsigned kPadCount = 48;
inline constexpr unsigned kButtonCount = 33;
inline constexpr unsigned kMatrixRows = 8;
inline constexpr unsigned kMatrixColumns = 10;
inline constexpr unsigned kKeyCount = kMatrixRows * kMatrixColumns;  // 80
inline constexpr std::uint8_t kNoKey = 0xFF;                         // "this matrix position has no switch"
inline constexpr std::uint8_t kEncoderId = 0;                        // the id of the encoder's events (the board has one)

// Pad index of matrix key `key`, or kNoKey.
constexpr std::uint8_t pad_of_key(unsigned key) noexcept {
  const unsigned row = key / kMatrixColumns;
  const unsigned col = key % kMatrixColumns;
  if (key >= kKeyCount) {
    return kNoKey;
  }
  if (row < 4u) {
    return static_cast<std::uint8_t>(row * 12u + col);  // pad columns 0..9
  }
  if (col >= 8u) {
    return static_cast<std::uint8_t>((row - 4u) * 12u + (col == 8u ? 11u : 10u));  // pad columns 11 and 10
  }
  return kNoKey;
}

// Button code of matrix key `key`, or kNoKey.
constexpr std::uint8_t button_of_key(unsigned key) noexcept {
  const unsigned row = key / kMatrixColumns;
  const unsigned col = key % kMatrixColumns;
  if (key >= kKeyCount) {
    return kNoKey;
  }
  if (row < 5u && col < 5u) {
    return static_cast<std::uint8_t>(8u + row * 5u + col);
  }
  if (row == 7u && col < 8u) {
    return static_cast<std::uint8_t>(7u - col);  // F8 at column 0 ... F1 at column 7
  }
  return kNoKey;
}

// The inverse, for the fakes and tests: the matrix key of a pad / a button.
constexpr unsigned key_of_pad(unsigned pad) noexcept {
  const unsigned row = pad / 12u;
  const unsigned col = pad % 12u;
  return col < 10u ? row * kMatrixColumns + col : (4u + row) * kMatrixColumns + (col == 10u ? 9u : 8u);
}
constexpr unsigned key_of_button(unsigned button) noexcept {
  if (button < 8u) {
    return 7u * kMatrixColumns + (7u - button);
  }
  const unsigned i = button - 8u;
  return (i / 5u) * kMatrixColumns + i % 5u;
}

// A raw key event of a scanner: matrix key and direction.
struct RawKey {
  std::uint8_t key;  // 0..79
  bool down;
};

// Counts per detent of the encoder: 96 pulses a turn in the four-states-a-cycle counting of the
// FTM quadrature decoder, over 24 detents: 4.
inline constexpr int kEncoderCountsPerDetent = 4;
// Which way the FTM counter counts for a clockwise turn. Counting up is taken as clockwise here; that is not confirmed, so the
// first hardware session decides (checklist INP-5). Flip this constant if the dial is found reversed.
inline constexpr bool kEncoderCountUpIsClockwise = true;

}  // namespace o3sdk::platform::mk66::input
