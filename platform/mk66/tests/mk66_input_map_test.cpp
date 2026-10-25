// SPDX-License-Identifier: GPL-3.0-or-later
//
// The key matrices of the Tracker OG's two TCA8418 scanners (input_map.h): every key of each matrix is a pad, a button or unwired, the
// 48 pads and the 33 buttons are each reached exactly once, the inverse functions are inverses, and the corners are the board's. Runs on the host.
#include <doctest/doctest.h>

#include <o3sdk/app/panel.h>
#include <o3sdk/platform/mk66/input/input_map.h>

#include <cstdint>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)               \
  do {                           \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);             \
    if (!must_ok_) {             \
      return;                    \
    }                            \
  } while (0)

namespace input = o3sdk::platform::mk66::input;
namespace kit = o3sdk::app;

TEST_CASE("input map: the pads are reached exactly once and the rest of the pad matrix is unwired") {
  unsigned seen[input::kPadCount] = {};
  unsigned unwired = 0;
  for (unsigned key = 0; key < input::kKeyCount; ++key) {
    const std::uint8_t pad = input::pad_of_key(key);
    if (pad == input::kNoKey) {
      ++unwired;
    } else {
      MUST(pad < input::kPadCount);
      ++seen[pad];
    }
  }
  for (unsigned pad = 0; pad < input::kPadCount; ++pad) {
    CHECK_MESSAGE(seen[pad] == 1u, "pad " << pad);
  }
  CHECK(unwired == input::kKeyCount - input::kPadCount);  // 80 positions, 48 pads
}

TEST_CASE("input map: the buttons are reached exactly once and the rest of the button matrix is unwired") {
  unsigned seen[input::kButtonCount] = {};
  unsigned unwired = 0;
  for (unsigned key = 0; key < input::kKeyCount; ++key) {
    const std::uint8_t button = input::button_of_key(key);
    if (button == input::kNoKey) {
      ++unwired;
    } else {
      MUST(button < input::kButtonCount);
      ++seen[button];
    }
  }
  for (unsigned button = 0; button < input::kButtonCount; ++button) {
    CHECK_MESSAGE(seen[button] == 1u, "button " << button);
  }
  CHECK(unwired == input::kKeyCount - input::kButtonCount);  // 80 positions, 33 buttons
}

TEST_CASE("input map: the inverse functions are inverses") {
  for (unsigned pad = 0; pad < input::kPadCount; ++pad) {
    CHECK(input::pad_of_key(input::key_of_pad(pad)) == pad);
  }
  for (unsigned button = 0; button < input::kButtonCount; ++button) {
    CHECK(input::button_of_key(input::key_of_button(button)) == button);
  }
  // a key out of the matrix is nothing
  CHECK(input::pad_of_key(input::kKeyCount) == input::kNoKey);
  CHECK(input::button_of_key(input::kKeyCount) == input::kNoKey);
  CHECK(input::pad_of_key(255) == input::kNoKey);
}

TEST_CASE("input map: the corners are the board's") {
  // Key = row * 10 + column. Pad matrix: pad row 0 on matrix row 0, pad columns 10 and 11 on matrix rows 4..7, columns 9 and 8.
  CHECK(input::pad_of_key(0) == 0);    // row 0, column 0
  CHECK(input::pad_of_key(9) == 9);    // row 0, column 9
  CHECK(input::pad_of_key(10) == 12);  // row 1, column 0: the second pad row starts at 12
  CHECK(input::pad_of_key(39) == 45);  // row 3, column 9
  CHECK(input::pad_of_key(48) == 11);  // row 4, column 8: pad 11 (the last of the first row)
  CHECK(input::pad_of_key(49) == 10);  // row 4, column 9: pad 10
  CHECK(input::pad_of_key(78) == 47);  // row 7, column 8
  CHECK(input::pad_of_key(79) == 46);  // row 7, column 9
  CHECK(input::pad_of_key(40) == input::kNoKey);  // row 4, column 0: no pad
  CHECK(input::pad_of_key(47) == input::kNoKey);  // row 4, column 7
  // Control matrix: F1..F8 are row 7, columns 7..0; the screen buttons run row by row from code 8.
  CHECK(input::button_of_key(77) == 0);   // F1 (row 7, column 7)
  CHECK(input::button_of_key(70) == 7);   // F8 (row 7, column 0)
  CHECK(input::button_of_key(0) == 8);    // Params
  CHECK(input::button_of_key(4) == 12);   // Master
  CHECK(input::button_of_key(10) == 13);  // SampleLoad
  CHECK(input::button_of_key(44) == 32);  // Right (row 4, column 4)
  CHECK(input::button_of_key(5) == input::kNoKey);   // row 0, column 5: nothing
  CHECK(input::button_of_key(78) == input::kNoKey);  // row 7, column 8
}

// ---- The kit's codes ---------------------------------------------------------------------------------------------------------
// What the app sees is the kit's numbering: o3sdk::app::Button (kit/app/include/o3sdk/app/panel.h: F1 = 0 ... Right = 32), pads
// 0..47 row by row from the top left (kit::pad_at(row, col) = row * 12 + col), the encoder id kit::kEncoder. This test includes
// panel.h and checks the scanner's map against it (the platform libraries themselves do not depend on kit/app): renumbering
// panel.h, or changing a count, fails here. Each button is tied to the matrix position the board wires it to (track3r-os
// platform/mk66/README.md, "The input hardware as found").

namespace {
struct KitButton {
  kit::Button button;  // panel.h's enumerator
  unsigned row;        // the control scanner's matrix row and column
  unsigned column;
};
constexpr KitButton kKitButtons[] = {
    {kit::Button::F1, 7, 7},          {kit::Button::F2, 7, 6},          {kit::Button::F3, 7, 5},         {kit::Button::F4, 7, 4},
    {kit::Button::F5, 7, 3},          {kit::Button::F6, 7, 2},          {kit::Button::F7, 7, 1},         {kit::Button::F8, 7, 0},
    {kit::Button::Params, 0, 0},      {kit::Button::Performance, 0, 1}, {kit::Button::File, 0, 2},       {kit::Button::Config, 0, 3},
    {kit::Button::Master, 0, 4},      {kit::Button::SampleLoad, 1, 0},  {kit::Button::SamplePlay, 1, 1}, {kit::Button::SampleEdit, 1, 2},
    {kit::Button::SampleRec, 1, 3},   {kit::Button::Song, 1, 4},        {kit::Button::Note, 2, 0},       {kit::Button::Instr, 2, 1},
    {kit::Button::Vol, 2, 2},         {kit::Button::Fx, 2, 3},          {kit::Button::Pattern, 2, 4},    {kit::Button::Play, 3, 0},
    {kit::Button::Copy, 3, 1},        {kit::Button::Insert, 3, 2},      {kit::Button::Up, 3, 3},         {kit::Button::Delete, 3, 4},
    {kit::Button::Rec, 4, 0},         {kit::Button::Shift, 4, 1},       {kit::Button::Left, 4, 2},       {kit::Button::Down, 4, 3},
    {kit::Button::Right, 4, 4},
};
static_assert(sizeof(kKitButtons) / sizeof(kKitButtons[0]) == kit::kButtonCount, "one row per kit button");
static_assert(static_cast<unsigned>(kit::Button::F1) == 0u, "the kit's codes start at 0");
static_assert(static_cast<unsigned>(kit::Button::Right) + 1u == kit::kButtonCount, "the kit's codes are 0..kButtonCount-1");
static_assert(input::kButtonCount == kit::kButtonCount, "the platform's button count is the kit's");
static_assert(input::kPadCount == kit::kPadCount, "the platform's pad count is the kit's");
static_assert(kit::kPadCount == kit::kPadCols * kit::kPadRows, "the kit's pad grid");
static_assert(input::kEncoderId == kit::kEncoder, "the encoder's id is the kit's");
}  // namespace

TEST_CASE("input map: every button press carries the kit's code (app::Button, panel.h)") {
  for (unsigned i = 0; i < kit::kButtonCount; ++i) {
    const KitButton& b = kKitButtons[i];
    const unsigned code = static_cast<unsigned>(b.button);
    CHECK_MESSAGE(code == i, kit::button_name(b.button) << ": panel.h numbers the buttons 0..32 in the table's order");
    const unsigned key = b.row * input::kMatrixColumns + b.column;
    CHECK_MESSAGE(input::button_of_key(key) == code, kit::button_name(b.button) << " at row " << b.row << ", column " << b.column);
    CHECK_MESSAGE(input::key_of_button(code) == key, kit::button_name(b.button));
  }
}

TEST_CASE("input map: pads are 0..47 row by row from the top left, as the kit numbers them") {
  for (unsigned row = 0; row < kit::kPadRows; ++row) {
    for (unsigned col = 0; col < kit::kPadCols; ++col) {
      const unsigned pad = kit::pad_at(row, col);
      // the first ten columns are matrix row `row`, columns 0..9; columns 10 and 11 are matrix rows 4..7, columns 9 and 8
      const unsigned key = col < 10u ? row * input::kMatrixColumns + col : (4u + row) * input::kMatrixColumns + (col == 10u ? 9u : 8u);
      CHECK_MESSAGE(input::pad_of_key(key) == pad, "pad row " << row << ", column " << col);
    }
  }
  CHECK(input::pad_of_key(input::key_of_pad(kit::pad_at(0, 0))) == 0);    // top left
  CHECK(input::pad_of_key(input::key_of_pad(kit::pad_at(0, 11))) == 11);  // top right
  CHECK(input::pad_of_key(input::key_of_pad(kit::pad_at(3, 0))) == 36);   // bottom left
  CHECK(input::pad_of_key(input::key_of_pad(kit::pad_at(3, 11))) == 47);  // bottom right
}

TEST_CASE("input map: the encoder has four counts a detent (96 per turn, 24 detents)") {
  CHECK(input::kEncoderCountsPerDetent * 24 == 96);
}
