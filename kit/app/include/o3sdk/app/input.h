// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One input as an app and its screens receive it: a press, a release or an encoder turn, with Shift's state at that moment.
// Raw on purpose: a press arrives at the press (no waiting to tell a tap from a hold), so pads and keys play at once. An app that
// wants holds or chords looks at Panel (what is held) or uses core/gesture.h itself.

#include <o3sdk/app/panel.h>
#include <o3sdk/core/input_event.h>
#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::app {

struct Input {
  enum class Type : std::uint8_t { Press, Release, Turn };

  Type type;
  core::InputKind kind;  // Button, Pad or Encoder
  std::uint8_t id;       // button code, pad 0..47, encoder 0
  std::int16_t value;    // pad: velocity (127: the pads have none); encoder: signed detents
  bool shift;            // Shift was held (for Shift itself: false)
  core::Tick time;

  bool press(Button b) const noexcept { return type == Type::Press && is(b); }
  bool release(Button b) const noexcept { return type == Type::Release && is(b); }
  bool is(Button b) const noexcept { return kind == core::InputKind::Button && id == static_cast<std::uint8_t>(b); }
  bool button() const noexcept { return kind == core::InputKind::Button; }
  Button which() const noexcept { return static_cast<Button>(id); }

  bool pad_press() const noexcept { return type == Type::Press && kind == core::InputKind::Pad; }
  bool pad_release() const noexcept { return type == Type::Release && kind == core::InputKind::Pad; }
  unsigned pad() const noexcept { return id; }

  bool turn() const noexcept { return type == Type::Turn; }
  int delta() const noexcept { return value; }

  // An F key's press: true and its index (F1 = 0) in `index`.
  bool fkey_press(unsigned& index) const noexcept {
    if (type == Type::Press && kind == core::InputKind::Button && is_fkey(which())) {
      index = fkey_index(which());
      return true;
    }
    return false;
  }
};

// From a HAL event (core/input_event.h): pads with value 0 and buttons with value 0 are releases.
Input make_input(const core::InputEvent& e, bool shift) noexcept;

}  // namespace o3sdk::app
