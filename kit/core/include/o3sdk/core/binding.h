// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bindings (ADR 0009 rev 1, mechanism 2): which action a gesture runs. A Binding is
//   (context, trigger = gesture + input + modifier) -> action [+ how its argument is made].
//
// Layers. Three binding layers, strongest first: user > preset > built-in. The built-in layer is a constexpr
// table; user and preset are loaded from text (core/keymap.h) into fixed pools.
//
// Contexts. A ContextChain lists the active contexts from the innermost out, e.g. overlay, mode, screen, global.
// An entry may be marked as capturing (an overlay): a gesture not bound in it, nor in anything inside it, goes
// no further out; it is swallowed (Resolution::Status::Captured). Context numbers are the app's: core only
// compares them.
//
// Resolution order. Contexts first, then layers: the capture-truncated chain is walked from the innermost
// context out, and in each context the strongest layer that has a binding for the gesture wins (user, preset,
// built-in). The first context with any binding for it, `none` included, ends the walk. So what is bound in a
// context is what happens there, a user or preset line in Global never shadows a screen's own binding, and a
// user who wants to change a screen's key binds it in that screen's context. Action None means "nothing": it
// shadows the lower layers of its own context and runs nothing (an unbind).
//
// Modifiers. A Tap, Hold, DoubleTap or HoldEnd made with a modifier held (Shift) is matched exactly first; if nothing
// matches, once more as if no modifier were held, so holding Shift never kills a plain binding. A binding that
// names the modifier always wins over one that does not, and Shift+X bound to `none` does switch X off under Shift.
// HoldEncoder and Chord carry their second button as part of what they are: no fallback.
//
// Conflicts. Two bindings in one layer and context whose triggers overlap (equal, or one has kAnyId where the other
// has an id) are a conflict: it is reported, never resolved silently. Across layers an overlap is an override.
// Matching of a gesture is exact, modifier included (kAnyId only on the input), apart from the fallback above.
//
// Everything here is plain data and bounded loops, usable at compile time where it says constexpr.

#include <o3sdk/core/action.h>
#include <o3sdk/core/gesture.h>
#include <o3sdk/core/input_event.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::core {

using ContextId = std::uint8_t;

// How the action's argument is made: source + `arg`, where the source is nothing (a fixed argument), the id of
// the input (pad index), or its value (encoder detents, pad velocity).
enum class ArgFrom : std::uint8_t { Fixed, InputId, Value };

struct Trigger {
  GestureKind gesture;
  InputRef input;     // kAnyId as id: any input of this kind
  InputRef modifier;  // Tap/Hold/DoubleTap: the held modifier; HoldEncoder: the held button; Chord: the lesser; else kNoInput
};

struct Binding {
  ContextId context;
  ArgFrom arg_from;
  Trigger trigger;
  ActionId action;
  ActionArg arg;
};
static_assert(sizeof(Binding) == 16, "bindings sit in fixed pools: keep them small");

constexpr bool operator==(const Trigger& a, const Trigger& b) noexcept {
  return a.gesture == b.gesture && a.input == b.input && a.modifier == b.modifier;
}
constexpr bool operator!=(const Trigger& a, const Trigger& b) noexcept { return !(a == b); }
constexpr bool operator==(const Binding& a, const Binding& b) noexcept {
  return a.context == b.context && a.arg_from == b.arg_from && a.trigger == b.trigger && a.action == b.action &&
         a.arg == b.arg;
}
constexpr bool operator!=(const Binding& a, const Binding& b) noexcept { return !(a == b); }

constexpr Trigger tap(InputRef input, InputRef modifier = kNoInput) noexcept {
  return Trigger{GestureKind::Tap, input, modifier};
}
constexpr Trigger hold(InputRef input, InputRef modifier = kNoInput) noexcept {
  return Trigger{GestureKind::Hold, input, modifier};
}
constexpr Trigger double_tap(InputRef input, InputRef modifier = kNoInput) noexcept {
  return Trigger{GestureKind::DoubleTap, input, modifier};
}
// The release of a button whose Hold fired (GestureKind::HoldEnd): "hold to open, let go to pick".
constexpr Trigger hold_end(InputRef input, InputRef modifier = kNoInput) noexcept {
  return Trigger{GestureKind::HoldEnd, input, modifier};
}
constexpr Trigger hold_encoder(InputRef encoder, InputRef held) noexcept {
  return Trigger{GestureKind::HoldEncoder, encoder, held};
}
// The pair in canonical order, so it is the same whichever button is named first (and whichever was pressed first).
constexpr Trigger chord(InputRef a, InputRef b) noexcept {
  return input_less(a, b) ? Trigger{GestureKind::Chord, b, a} : Trigger{GestureKind::Chord, a, b};
}

constexpr Binding bind(ContextId context, const Trigger& trigger, ActionId action, ArgFrom arg_from = ArgFrom::Fixed,
                       ActionArg arg = 0) noexcept {
  return Binding{context, arg_from, trigger, action, arg};
}

constexpr bool input_matches(InputRef pattern, InputRef input) noexcept {
  return pattern.kind == input.kind && (pattern.id == kAnyId || pattern.id == input.id);
}

constexpr bool trigger_matches(const Trigger& t, const Gesture& g) noexcept {
  return t.gesture == g.kind && input_matches(t.input, g.input) && t.modifier == g.modifier;
}

constexpr bool triggers_overlap(const Trigger& a, const Trigger& b) noexcept {
  return a.gesture == b.gesture && a.input.kind == b.input.kind &&
         (a.input.id == kAnyId || b.input.id == kAnyId || a.input.id == b.input.id) && a.modifier == b.modifier;
}

// The argument a gesture gives an action through this binding. The sum wraps (unsigned arithmetic) rather than
// overflow: `arg` comes from data.
constexpr ActionArg binding_arg(const Binding& b, const Gesture& g) noexcept {
  std::int32_t source = 0;
  switch (b.arg_from) {
    case ArgFrom::InputId:
      source = g.input.id;
      break;
    case ArgFrom::Value:
      source = g.value;
      break;
    case ArgFrom::Fixed:
      break;
  }
  return static_cast<ActionArg>(static_cast<std::uint32_t>(source) + static_cast<std::uint32_t>(b.arg));
}

// First pair of bindings of one layer that conflict (same context, overlapping triggers).
struct Conflict {
  bool found;
  std::size_t first;   // indices into the layer, first < second
  std::size_t second;
};

constexpr Conflict find_conflict(const Binding* items, std::size_t count) noexcept {
  for (std::size_t j = 1; j < count; ++j) {
    for (std::size_t i = 0; i < j; ++i) {
      if (items[i].context == items[j].context && triggers_overlap(items[i].trigger, items[j].trigger)) {
        return Conflict{true, i, j};
      }
    }
  }
  return Conflict{false, 0, 0};
}

struct BindingView {
  const Binding* items;
  std::size_t count;
};

enum class Layer : std::uint8_t { User = 0, Preset = 1, Builtin = 2 };
inline constexpr unsigned kLayerCount = 3;

// The three layers, indexed by Layer (strongest first).
struct BindingSet {
  BindingView layers[kLayerCount];
};

// The active contexts, innermost first.
struct ContextChain {
  static constexpr unsigned kMax = 5;  // overlay, mode, field, screen, global
  ContextId ids[kMax] = {};
  std::uint8_t count = 0;
  std::uint8_t capture = 0;  // bit i set: ids[i] captures

  constexpr bool push(ContextId id, bool captures = false) noexcept {
    if (count == kMax) {
      return false;
    }
    ids[count] = id;
    if (captures) {
      capture = static_cast<std::uint8_t>(capture | (1u << count));
    }
    ++count;
    return true;
  }

  // How many entries are consulted: up to and including the first one that captures.
  constexpr unsigned reach() const noexcept {
    for (unsigned i = 0; i < count; ++i) {
      if ((capture & (1u << i)) != 0) {
        return i + 1;
      }
    }
    return count;
  }
};

// Chains are equal when they list the same contexts with the same capture flags: how the app notices that the
// screen or an overlay changed.
constexpr bool operator==(const ContextChain& a, const ContextChain& b) noexcept {
  if (a.count != b.count || a.capture != b.capture) {
    return false;
  }
  for (unsigned i = 0; i < a.count; ++i) {
    if (a.ids[i] != b.ids[i]) {
      return false;
    }
  }
  return true;
}
constexpr bool operator!=(const ContextChain& a, const ContextChain& b) noexcept { return !(a == b); }

struct Resolution {
  enum class Status : std::uint8_t {
    Unbound,   // nothing is bound and nothing captured: the gesture is for nobody
    Captured,  // not bound, but a capturing context swallowed it
    Bound,     // `action` (possibly None, an explicit "nothing") with `arg`
  };
  Status status = Status::Unbound;
  ActionId action = ActionId::None;
  ActionArg arg = 0;
  Layer layer = Layer::Builtin;  // where the winning binding came from (Bound only)
  ContextId context = 0;
};

Resolution resolve(const BindingSet& set, const ContextChain& chain, const Gesture& gesture) noexcept;

// What the bindings that would actually run need of a button (kWant* bits): the answer to GestureHints::wants.
// A binding shadowed by `none` (or by a stronger one) does not count: the button is not held back for a gesture
// nothing will answer.
std::uint8_t gesture_wants(const BindingSet& set, const ContextChain& chain, InputRef input) noexcept;

// Is {a, b} bound as a chord (to a real action) in `chain`? Order does not matter.
bool chord_bound(const BindingSet& set, const ContextChain& chain, InputRef a, InputRef b) noexcept;

// Is `encoder` turned while `button` is held bound (to a real action) in `chain`?
bool hold_encoder_bound(const BindingSet& set, const ContextChain& chain, InputRef button, InputRef encoder) noexcept;

}  // namespace o3sdk::core
