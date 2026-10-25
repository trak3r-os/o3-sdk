// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The vocabulary of actions (ADR 0009 rev 1, mechanism 1): what every input, menu, script and later MIDI message
// asks for. The types live here because bindings (core/binding.h) name actions; the registry that maps an
// action to its handler lives in the app layer, which knows the model.

#include <cstddef>
#include <cstdint>

namespace o3sdk::core {

// Stable identity of an action: the 32-bit FNV-1a hash of its lower-case name ("nav.left"), so it is the same in
// every build, can be written in a keymap as text, and needs no central enum when a module adds an action.
// None is "no action": as a binding's action it means "explicitly nothing here", which lets a higher layer
// switch off a lower layer's binding.
enum class ActionId : std::uint32_t { None = 0 };

// One 32-bit argument: a pad index, an encoder delta, a column number, an instrument.
using ActionArg = std::int32_t;

// Facts about an action that the machinery around it needs.
using ActionFlags = std::uint8_t;
inline constexpr ActionFlags kActionUndoable = 1u << 0;    // changes the model: the undo journal (OS-002c) records it
inline constexpr ActionFlags kActionRepeatable = 1u << 1;  // may be re-issued while its input is held
inline constexpr ActionFlags kActionNeedsFocus = 1u << 2;  // only runs while the base screen owns the input
// Undoable when a document is open, but not Disabled without one (OS-002d): the action edits the project through the editor it is
// handed (ActionContext::edit, its edits are one undo unit) or, when there is no document, only what needs none (a device setting, a
// device Learn binding). The handler must cope with a null `edit`.
inline constexpr ActionFlags kActionUndoableIfOpen = 1u << 3;
// An undoable action that says so when there is no document to edit (OS-009): with kActionUndoable it is still Disabled, and the dispatcher
// also posts the info notification "NO PROJECT", so that a key that does nothing is not a mystery (the Pattern screen's demo is read-only).
inline constexpr ActionFlags kActionSaysNoProject = 1u << 4;
// An action that records into the live take (OS-LIVEREC): while the Pattern screen records, the dispatcher keeps the take's undo group open round it (the group of the whole take is the
// unit); any other undoable action closes the take first, so that it is a unit of its own.
inline constexpr ActionFlags kActionRecords = 1u << 5;

constexpr std::uint32_t fnv1a(const char* s, std::size_t n) noexcept {
  std::uint32_t h = 2166136261u;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint8_t>(s[i]);
    h *= 16777619u;
  }
  return h;
}

constexpr ActionId action_id(const char* name, std::size_t length) noexcept {
  return static_cast<ActionId>(fnv1a(name, length));
}

// For a string literal or any NUL-terminated name.
constexpr ActionId action_id(const char* name) noexcept {
  std::size_t n = 0;
  while (name[n] != '\0') {
    ++n;
  }
  return action_id(name, n);
}

}  // namespace o3sdk::core
