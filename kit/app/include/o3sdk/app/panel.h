// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Tracker's front panel as an app sees it: 33 buttons (no LED on any of them), 48 pads in 4 rows of 12 (one monochrome LED
// each, no velocity, no pressure), one encoder (no push). Facts from track3r-os platform/mk66/README.md, "Input".
//
// Button codes are the hardware's (what core::InputEvent carries as `id`); pads are numbered 0..47 from the top left, row by row
// (pad 0 top left, 11 top right, 36 bottom left). The printed legends are the original firmware's: an app is free to give any
// button any meaning, and says what it does on the screen (the soft-key row, the help line).

#include <o3sdk/core/input_event.h>

#include <cstdint>

namespace o3sdk::app {

enum class Button : std::uint8_t {
  F1 = 0, F2, F3, F4, F5, F6, F7, F8,                       // the soft keys under the screen
  Params = 8, Performance, File, Config, Master,            // the screen buttons
  SampleLoad = 13, SamplePlay, SampleEdit, SampleRec, Song,
  Note = 18, Instr, Vol, Fx,                                // the four column buttons
  Pattern = 22,
  Play = 23, Copy, Insert, Up, Delete,
  Rec = 28, Shift, Left, Down, Right,
};
inline constexpr unsigned kButtonCount = 33;
inline constexpr unsigned kFKeyCount = 8;

inline constexpr unsigned kPadCols = 12;
inline constexpr unsigned kPadRows = 4;
inline constexpr unsigned kPadCount = kPadCols * kPadRows;
inline constexpr std::uint8_t kEncoder = 0;  // the encoder's id

constexpr unsigned pad_at(unsigned row, unsigned col) noexcept { return row * kPadCols + col; }
constexpr unsigned pad_row(unsigned pad) noexcept { return pad / kPadCols; }
constexpr unsigned pad_col(unsigned pad) noexcept { return pad % kPadCols; }

constexpr bool is_fkey(Button b) noexcept { return static_cast<unsigned>(b) < kFKeyCount; }
constexpr unsigned fkey_index(Button b) noexcept { return static_cast<unsigned>(b); }  // F1 = 0 .. F8 = 7
constexpr Button fkey(unsigned index) noexcept { return static_cast<Button>(index); }

// Lower-case names ("f1", "play", "sampleload"): the simulator's scenarios and debug output.
const char* button_name(Button b) noexcept;
// By name, case-insensitive. false (out untouched) for an unknown name.
bool find_button(const char* name, Button& out) noexcept;

// What is held right now, kept by the runtime from the input events.
class Panel {
 public:
  bool held(Button b) const noexcept { return (buttons_ >> static_cast<unsigned>(b) & 1u) != 0u; }
  bool pad_held(unsigned pad) const noexcept { return pad < kPadCount && (pads_ >> pad & 1u) != 0u; }
  bool shift() const noexcept { return held(Button::Shift); }
  unsigned pads_held() const noexcept;
  unsigned fkeys_held() const noexcept { return static_cast<unsigned>(buttons_ & 0xFFu); }  // bit n = F(n+1)

  // The runtime's: one input event.
  void apply(const core::InputEvent& e) noexcept;
  void clear() noexcept {
    buttons_ = 0;
    pads_ = 0;
  }

 private:
  std::uint64_t buttons_ = 0;
  std::uint64_t pads_ = 0;
};

}  // namespace o3sdk::app
