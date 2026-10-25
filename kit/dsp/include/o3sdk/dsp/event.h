// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What reaches a unit inside a block (docs/FRAMEWORK.md, "Units"): notes and parameter changes, each at a frame of the block.
// A unit sees its events sorted by frame, so it can render up to an event, apply it and go on (unit.h: for_each_run).
//
// Events come from two places: the main loop (EngineLink: a pad, the encoder, a preset), which lands at frame 0 of the next
// block, and the clocked hooks of the render context (a sequencer, an arpeggiator), which place them at any frame.

#include <cstdint>
#include <type_traits>

namespace o3sdk::dsp {

enum class EventKind : std::uint8_t {
  NoteOn,       // note, velocity 1..127, flags
  NoteOff,      // note
  Param,        // param (the unit's parameter id), value (in the descriptor's units)
  AllNotesOff,  // every sounding note ends (stop, panic)
};

// NoteOn flags. A unit that has no use for one ignores it.
inline constexpr std::uint8_t kNoteAccent = 1u << 0;  // an accented note (the 303's accent; a drum's accent)
inline constexpr std::uint8_t kNoteSlide = 1u << 1;   // legato: glide from the sounding note into this one, without a new envelope

struct Event {
  std::uint16_t frame;  // 0 .. frames - 1 of the block it is delivered in
  EventKind kind;
  std::uint8_t note;      // NoteOn / NoteOff: MIDI note number 0..127
  std::uint8_t velocity;  // NoteOn: 1..127
  std::uint8_t flags;     // NoteOn: kNote*
  std::uint16_t param;    // Param: the unit's parameter id (the index in its descriptor table)
  float value;            // Param: the new value, in the descriptor's units
};
static_assert(sizeof(Event) == 12 && std::is_trivially_copyable<Event>::value, "an Event is three words, copied through rings");

constexpr Event note_on(std::uint8_t note, std::uint8_t velocity, std::uint8_t flags = 0, std::uint16_t frame = 0) noexcept {
  return Event{frame, EventKind::NoteOn, note, velocity, flags, 0u, 0.0f};
}
constexpr Event note_off(std::uint8_t note, std::uint16_t frame = 0) noexcept {
  return Event{frame, EventKind::NoteOff, note, 0u, 0u, 0u, 0.0f};
}
constexpr Event param_change(std::uint16_t param, float value, std::uint16_t frame = 0) noexcept {
  return Event{frame, EventKind::Param, 0u, 0u, 0u, param, value};
}
constexpr Event all_notes_off(std::uint16_t frame = 0) noexcept {
  return Event{frame, EventKind::AllNotesOff, 0u, 0u, 0u, 0u, 0.0f};
}

}  // namespace o3sdk::dsp
