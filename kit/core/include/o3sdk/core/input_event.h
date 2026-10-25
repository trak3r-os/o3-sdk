// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/ring.h>
#include <o3sdk/core/tick.h>

#include <cstdint>
#include <type_traits>

namespace o3sdk::core {

enum class InputKind : std::uint8_t {
  Button = 0,   // front-panel button; id = hardware button code
  Pad = 1,      // pad; id = pad index 0..47
  Encoder = 2,  // rotary encoder; id = encoder index
  // Reserved (ADR 0009 rev 1): host keyboard (the OctaMED keymap comes first), USB HID, MIDI. No HAL produces
  // them yet, the gesture layer ignores them and the keymap text has no word for them.
  PcKey = 3,
  Hid = 4,
  MidiNote = 5,
  MidiCc = 6,
  None = 0xFF,  // not an event kind: "no input" in an InputRef (an absent modifier)
};

// One user-input event with the tick at which the hardware saw it.
//   Button:  value = 1 pressed, 0 released.
//   Pad:     value = 0 released, 1..127 pressed with that velocity (127 where the pad has no velocity).
//            Provisional until the pad HAL is specified.
//   Encoder: value = signed detent delta since the previous event (+ clockwise).
// Button and pad codes are plain numbers here; their names belong to the app layer.
struct InputEvent {
  Tick timestamp;
  InputKind kind;
  std::uint8_t id;
  std::int16_t value;
};

static_assert(sizeof(InputEvent) == 8, "InputEvent is copied inside ISRs; keep it two words");
static_assert(std::is_trivially_copyable<InputEvent>::value, "InputEvent must be ring-buffer safe");

// What an input is, without its value: the key of bindings and gestures.
struct InputRef {
  InputKind kind;
  std::uint8_t id;
};

inline constexpr InputRef kNoInput{InputKind::None, 0};
inline constexpr std::uint8_t kAnyId = 0xFF;  // in a binding's input: every input of that kind (all 48 pads)

constexpr bool operator==(InputRef a, InputRef b) noexcept { return a.kind == b.kind && a.id == b.id; }
constexpr bool operator!=(InputRef a, InputRef b) noexcept { return !(a == b); }
constexpr bool is_none(InputRef r) noexcept { return r.kind == InputKind::None; }
// Total order (kind, then id): puts the two members of a chord in one canonical order.
constexpr bool input_less(InputRef a, InputRef b) noexcept {
  return a.kind != b.kind ? static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind) : a.id < b.id;
}
constexpr InputRef input_of(const InputEvent& e) noexcept { return InputRef{e.kind, e.id}; }

constexpr bool operator==(const InputEvent& a, const InputEvent& b) noexcept {
  return a.timestamp == b.timestamp && a.kind == b.kind && a.id == b.id && a.value == b.value;
}
constexpr bool operator!=(const InputEvent& a, const InputEvent& b) noexcept { return !(a == b); }

constexpr InputEvent make_button_event(Tick timestamp, std::uint8_t id, bool pressed) noexcept {
  return InputEvent{timestamp, InputKind::Button, id, static_cast<std::int16_t>(pressed ? 1 : 0)};
}
constexpr InputEvent make_pad_event(Tick timestamp, std::uint8_t index, std::int16_t velocity) noexcept {
  return InputEvent{timestamp, InputKind::Pad, index, velocity};
}
constexpr InputEvent make_encoder_event(Tick timestamp, std::uint8_t id, std::int16_t delta) noexcept {
  return InputEvent{timestamp, InputKind::Encoder, id, delta};
}

// Hand-over queue for the input scanner: the scanner (ISR or timer tick) is the single producer, the
// main loop the single consumer. 64 events is a conservative bound for a main-loop pass at 30 Hz.
inline constexpr std::uint32_t kInputQueueCapacity = 64;
using InputQueue = SpscRing<InputEvent, kInputQueueCapacity>;

}  // namespace o3sdk::core
