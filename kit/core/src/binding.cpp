// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/core/binding.h>

namespace o3sdk::core {

namespace {

// One walk over the chain: contexts from the innermost out, in each the strongest layer first. Fills `out` and
// returns true at the first binding that matches.
bool find_binding(const BindingSet& set, const ContextChain& chain, const Gesture& gesture, Resolution& out) noexcept {
  const unsigned reach = chain.reach();
  for (unsigned c = 0; c < reach; ++c) {
    for (unsigned layer = 0; layer < kLayerCount; ++layer) {
      const BindingView& view = set.layers[layer];
      for (std::size_t i = 0; i < view.count; ++i) {
        const Binding& b = view.items[i];
        if (b.context == chain.ids[c] && trigger_matches(b.trigger, gesture)) {
          out.status = Resolution::Status::Bound;
          out.action = b.action;
          out.arg = binding_arg(b, gesture);
          out.layer = static_cast<Layer>(layer);
          out.context = b.context;
          return true;
        }
      }
    }
  }
  return false;
}

// Would this gesture run a real action? (Not unbound, not captured, not an explicit `none`.)
bool runs(const BindingSet& set, const ContextChain& chain, const Gesture& gesture) noexcept {
  const Resolution r = resolve(set, chain, gesture);
  return r.status == Resolution::Status::Bound && r.action != ActionId::None;
}

// Calls `visit(binding)` for every binding of every layer whose context is reachable from `chain`.
template <typename Visit>
void for_each_reachable(const BindingSet& set, const ContextChain& chain, Visit visit) noexcept {
  const unsigned reach = chain.reach();
  for (const BindingView& view : set.layers) {
    for (std::size_t i = 0; i < view.count; ++i) {
      for (unsigned c = 0; c < reach; ++c) {
        if (view.items[i].context == chain.ids[c]) {
          visit(view.items[i]);
          break;
        }
      }
    }
  }
}

}  // namespace

Resolution resolve(const BindingSet& set, const ContextChain& chain, const Gesture& gesture) noexcept {
  Resolution r;
  if (find_binding(set, chain, gesture, r)) {
    return r;
  }
  // Holding a modifier must not kill a plain binding: try once more as if it were not held. HoldEncoder and
  // Chord have no such fallback, their second button is part of what they are.
  const bool plain_gesture = gesture.kind == GestureKind::Tap || gesture.kind == GestureKind::Hold ||
                             gesture.kind == GestureKind::DoubleTap || gesture.kind == GestureKind::HoldEnd;
  if (plain_gesture && !is_none(gesture.modifier)) {
    Gesture unmodified = gesture;
    unmodified.modifier = kNoInput;
    if (find_binding(set, chain, unmodified, r)) {
      return r;
    }
  }
  r = Resolution{};
  const unsigned reach = chain.reach();
  if (reach > 0 && (chain.capture & (1u << (reach - 1))) != 0) {
    r.status = Resolution::Status::Captured;  // the chain ends at a capturing context (reach stops at the first one)
  }
  return r;
}

std::uint8_t gesture_wants(const BindingSet& set, const ContextChain& chain, InputRef input) noexcept {
  std::uint8_t wants = 0;
  for_each_reachable(set, chain, [&](const Binding& b) {
    const Trigger& t = b.trigger;
    std::uint8_t bit = 0;
    Gesture probe{0, t.gesture, input, t.modifier, 0};  // the gesture this binding answers, as `input` would make it
    switch (t.gesture) {
      case GestureKind::Hold:
      case GestureKind::HoldEnd:  // the end of a hold is only seen if the button waits for the hold: it counts as one
        bit = input_matches(t.input, input) ? kWantHold : std::uint8_t{0};
        break;
      case GestureKind::DoubleTap:
        bit = input_matches(t.input, input) ? kWantDoubleTap : std::uint8_t{0};
        break;
      case GestureKind::Chord:
        bit = (t.input == input || t.modifier == input) ? kWantChord : std::uint8_t{0};
        probe.input = t.input;
        break;
      case GestureKind::HoldEncoder:  // the held button is `t.modifier`: it waits for the release, so that a turn can consume its Tap
        bit = t.modifier == input ? kWantHoldEncoder : std::uint8_t{0};
        probe.input = t.input;  // the encoder of the binding
        probe.modifier = input;
        break;
      case GestureKind::Tap:
        break;
    }
    if (bit != 0 && (wants & bit) == 0 && runs(set, chain, probe)) {
      wants = static_cast<std::uint8_t>(wants | bit);
    }
    // OS-ZOOM: a Tap, Hold or DoubleTap made with this button held (Note + Instr). Chord and HoldEncoder use the field for their own meaning and do not count.
    if (t.modifier == input && (t.gesture == GestureKind::Tap || t.gesture == GestureKind::Hold || t.gesture == GestureKind::DoubleTap) &&
        (wants & kWantModifier) == 0 && runs(set, chain, Gesture{0, t.gesture, t.input, t.modifier, 0})) {
      wants = static_cast<std::uint8_t>(wants | kWantModifier);
    }
  });
  return wants;
}

bool chord_bound(const BindingSet& set, const ContextChain& chain, InputRef a, InputRef b) noexcept {
  const Trigger pair = chord(a, b);
  return runs(set, chain, Gesture{0, GestureKind::Chord, pair.input, pair.modifier, 0});
}

bool hold_encoder_bound(const BindingSet& set, const ContextChain& chain, InputRef button, InputRef encoder) noexcept {
  return runs(set, chain, Gesture{0, GestureKind::HoldEncoder, encoder, button, 0});
}

}  // namespace o3sdk::core
