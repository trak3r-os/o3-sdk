// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/action.h>
#include <o3sdk/core/binding.h>
#include <o3sdk/core/gesture.h>
#include <o3sdk/core/input_event.h>

#include <cstdint>
#include <initializer_list>

using namespace o3sdk::core;
using Status = Resolution::Status;

namespace {

constexpr InputRef btn(std::uint8_t id) { return InputRef{InputKind::Button, id}; }
constexpr InputRef pad(std::uint8_t id) { return InputRef{InputKind::Pad, id}; }
constexpr InputRef enc(std::uint8_t id) { return InputRef{InputKind::Encoder, id}; }

constexpr std::uint8_t kLeft = 30;
constexpr std::uint8_t kShift = 29;
constexpr std::uint8_t kVol = 20;
constexpr std::uint8_t kFx = 21;

// Contexts of the tests, innermost to outermost in a real chain: overlay > mode > screen > global.
constexpr ContextId kGlobal = 0;
constexpr ContextId kScreen = 1;
constexpr ContextId kMode = 2;
constexpr ContextId kOverlay = 3;

constexpr ActionId act(const char* name) { return action_id(name); }

Gesture tap_of(InputRef in, InputRef mod = kNoInput, std::int16_t value = 0) {
  return Gesture{0, GestureKind::Tap, in, mod, value};
}

ContextChain chain_of(std::initializer_list<ContextId> ids, std::uint8_t capture = 0) {
  ContextChain c;
  for (ContextId id : ids) {
    CHECK(c.push(id));
  }
  c.capture = capture;
  return c;
}

BindingSet set_of(BindingView user, BindingView preset, BindingView builtin) {
  return BindingSet{{user, preset, builtin}};
}

template <std::size_t N>
BindingView view(const Binding (&b)[N]) {
  return BindingView{b, N};
}

const BindingView kEmpty{nullptr, 0};

}  // namespace

// The names hash to stable, distinct ids (and the hash is usable in constant expressions).
static_assert(act("nav.left") != act("nav.right"), "distinct names, distinct ids");
static_assert(act("nav.left") == action_id("nav.left", 8), "the two spellings agree");
static_assert(act("a") != ActionId::None, "no real name hashes to None here");
static_assert(fnv1a("", 0) == 2166136261u, "FNV-1a offset basis");
static_assert(fnv1a("a", 1) == 0xE40C292Cu, "FNV-1a known vector");
static_assert(fnv1a("foobar", 6) == 0xBF9CF968u, "FNV-1a known vector");

TEST_CASE("resolver: trigger builders put a chord in canonical order") {
  CHECK(chord(btn(kFx), btn(kVol)) == chord(btn(kVol), btn(kFx)));
  const Trigger t = chord(btn(kFx), btn(kVol));
  CHECK(t.gesture == GestureKind::Chord);
  CHECK(t.input == btn(kFx));     // the greater
  CHECK(t.modifier == btn(kVol));  // the lesser
  CHECK(tap(btn(kLeft)).modifier == kNoInput);
  CHECK(tap(btn(kLeft), btn(kShift)).modifier == btn(kShift));
  CHECK(hold_encoder(enc(0), btn(kFx)).gesture == GestureKind::HoldEncoder);
  CHECK(tap(btn(1)) != tap(btn(2)));
}

TEST_CASE("resolver: a gesture matches exactly, except that kAnyId stands for every id of a kind") {
  const Gesture left = tap_of(btn(kLeft));
  CHECK(trigger_matches(tap(btn(kLeft)), left));
  CHECK_FALSE(trigger_matches(tap(btn(kLeft + 1)), left));
  CHECK_FALSE(trigger_matches(tap(btn(kLeft), btn(kShift)), left));      // a modifier must match too
  CHECK_FALSE(trigger_matches(tap(btn(kLeft)), tap_of(btn(kLeft), btn(kShift))));
  CHECK_FALSE(trigger_matches(hold(btn(kLeft)), left));                  // and so must the gesture
  CHECK_FALSE(trigger_matches(tap(pad(kLeft)), left));                   // and the kind of input
  CHECK(trigger_matches(tap(pad(kAnyId)), tap_of(pad(0), kNoInput, 90)));
  CHECK(trigger_matches(tap(pad(kAnyId)), tap_of(pad(47), kNoInput, 90)));
  CHECK_FALSE(trigger_matches(tap(pad(kAnyId)), tap_of(btn(5))));
  CHECK(trigger_matches(chord(btn(kVol), btn(kFx)), Gesture{0, GestureKind::Chord, btn(kFx), btn(kVol), 0}));
}

TEST_CASE("resolver: the argument comes from the binding, the input's id or its value, plus an offset") {
  const Gesture g = tap_of(pad(7), kNoInput, 100);
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::Fixed, 5), g) == 5);
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::InputId), g) == 7);
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::InputId, 1), g) == 8);
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::Value), g) == 100);
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::Value, -1), g) == 99);
  const Gesture turn = tap_of(enc(0), kNoInput, -3);
  CHECK(binding_arg(bind(kGlobal, tap(enc(0)), act("x.y"), ArgFrom::Value), turn) == -3);
  // The sum wraps instead of overflowing: the offset comes from data.
  CHECK(binding_arg(bind(kGlobal, tap(pad(kAnyId)), act("x.y"), ArgFrom::InputId, 2147483647), g) ==
        static_cast<ActionArg>(2147483647u + 7u));
}

TEST_CASE("resolver: conflicts are two bindings of one context with overlapping triggers") {
  const ActionId a = act("a.a");
  const ActionId b = act("b.b");
  const Binding ok[] = {
      bind(kScreen, tap(btn(1)), a),
      bind(kScreen, tap(btn(2)), a),                       // other input
      bind(kGlobal, tap(btn(1)), b),                       // other context
      bind(kScreen, hold(btn(1)), b),                      // other gesture
      bind(kScreen, tap(btn(1), btn(kShift)), b),          // other modifier
      bind(kScreen, tap(pad(3)), a),
      bind(kScreen, tap(enc(0)), a),
      bind(kScreen, chord(btn(1), btn(2)), a),
      bind(kScreen, chord(btn(1), btn(3)), a),
  };
  CHECK_FALSE(find_conflict(ok, sizeof(ok) / sizeof(ok[0])).found);

  const Binding same[] = {bind(kScreen, tap(btn(1)), a), bind(kScreen, tap(btn(2)), a), bind(kScreen, tap(btn(1)), b)};
  const Conflict c = find_conflict(same, 3);
  CHECK(c.found);
  CHECK(c.first == 0u);
  CHECK(c.second == 2u);

  const Binding wildcard[] = {bind(kScreen, tap(pad(kAnyId)), a), bind(kScreen, tap(pad(5)), b)};
  CHECK(find_conflict(wildcard, 2).found);  // pad* covers pad 5: ambiguous, so a conflict
  const Binding wildcard_other_ctx[] = {bind(kScreen, tap(pad(kAnyId)), a), bind(kMode, tap(pad(5)), b)};
  CHECK_FALSE(find_conflict(wildcard_other_ctx, 2).found);

  const Binding chords[] = {bind(kGlobal, chord(btn(kVol), btn(kFx)), a), bind(kGlobal, chord(btn(kFx), btn(kVol)), b)};
  CHECK(find_conflict(chords, 2).found);  // the same pair, written the other way round
  CHECK_FALSE(find_conflict(nullptr, 0).found);
}

TEST_CASE("resolver: the layers resolve user over preset over built-in") {
  const Binding builtin[] = {bind(kScreen, tap(btn(1)), act("builtin.one")), bind(kScreen, tap(btn(2)), act("builtin.two")),
                             bind(kScreen, tap(btn(3)), act("builtin.three"))};
  const Binding preset[] = {bind(kScreen, tap(btn(2)), act("preset.two")), bind(kScreen, tap(btn(3)), act("preset.three"))};
  const Binding user[] = {bind(kScreen, tap(btn(3)), act("user.three"))};
  const BindingSet set = set_of(view(user), view(preset), view(builtin));
  const ContextChain chain = chain_of({kScreen, kGlobal});

  Resolution r = resolve(set, chain, tap_of(btn(1)));
  CHECK(r.status == Status::Bound);
  CHECK(r.action == act("builtin.one"));
  CHECK(r.layer == Layer::Builtin);

  r = resolve(set, chain, tap_of(btn(2)));
  CHECK(r.action == act("preset.two"));
  CHECK(r.layer == Layer::Preset);

  r = resolve(set, chain, tap_of(btn(3)));
  CHECK(r.action == act("user.three"));
  CHECK(r.layer == Layer::User);

  r = resolve(set, chain, tap_of(btn(4)));
  CHECK(r.status == Status::Unbound);
  CHECK(r.action == ActionId::None);
}

TEST_CASE("resolver: within a layer the innermost context wins") {
  const Binding builtin[] = {bind(kGlobal, tap(btn(1)), act("global.one")), bind(kScreen, tap(btn(1)), act("screen.one")),
                             bind(kMode, tap(btn(1)), act("mode.one")), bind(kGlobal, tap(btn(2)), act("global.two"))};
  const BindingSet set = set_of(kEmpty, kEmpty, view(builtin));

  CHECK(resolve(set, chain_of({kMode, kScreen, kGlobal}), tap_of(btn(1))).action == act("mode.one"));
  CHECK(resolve(set, chain_of({kScreen, kGlobal}), tap_of(btn(1))).action == act("screen.one"));
  CHECK(resolve(set, chain_of({kGlobal}), tap_of(btn(1))).action == act("global.one"));
  CHECK(resolve(set, chain_of({kMode, kScreen, kGlobal}), tap_of(btn(2))).action == act("global.two"));  // falls out to global
  CHECK(resolve(set, chain_of({kScreen}), tap_of(btn(2))).status == Status::Unbound);                    // global not in the chain
  CHECK(resolve(set, chain_of({kScreen, kGlobal}), tap_of(btn(1))).context == kScreen);
}

TEST_CASE("resolver: contexts come first, then layers: a user binding in an outer context never shadows an inner one") {
  const Binding builtin[] = {bind(kScreen, tap(btn(1)), act("builtin.screen")), bind(kGlobal, tap(btn(2)), act("builtin.global"))};
  const Binding preset[] = {bind(kGlobal, tap(btn(1)), act("preset.global"))};
  const Binding user[] = {bind(kGlobal, tap(btn(1)), act("user.global")), bind(kScreen, tap(btn(2)), act("user.screen")),
                          bind(kScreen, tap(btn(3)), act("user.screen.three"))};
  const BindingSet set = set_of(view(user), view(preset), view(builtin));
  const ContextChain chain = chain_of({kScreen, kGlobal});

  // Button 1: the screen's built-in binding is in the inner context, so it ends the walk: the global lines of the
  // user and preset layers are never reached.
  Resolution r = resolve(set, chain, tap_of(btn(1)));
  CHECK(r.action == act("builtin.screen"));
  CHECK(r.layer == Layer::Builtin);
  CHECK(r.context == kScreen);

  // Button 2: the user overrides per context, by binding it in the screen's own context.
  r = resolve(set, chain, tap_of(btn(2)));
  CHECK(r.action == act("user.screen"));
  CHECK(r.layer == Layer::User);

  // Button 3: only the user has it, in the screen.
  CHECK(resolve(set, chain, tap_of(btn(3))).action == act("user.screen.three"));

  // Where the screen is not in force the global lines apply, strongest layer first.
  r = resolve(set, chain_of({kGlobal}), tap_of(btn(1)));
  CHECK(r.action == act("user.global"));
  CHECK(r.layer == Layer::User);
  CHECK(resolve(set, chain_of({kGlobal}), tap_of(btn(2))).action == act("builtin.global"));
}

TEST_CASE("resolver: action None in a higher layer switches a lower binding off") {
  const Binding builtin[] = {bind(kScreen, tap(btn(1)), act("builtin.one"))};
  const Binding user[] = {bind(kScreen, tap(btn(1)), ActionId::None)};
  const Resolution r = resolve(set_of(view(user), kEmpty, view(builtin)), chain_of({kScreen}), tap_of(btn(1)));
  CHECK(r.status == Status::Bound);  // bound, to nothing: it does not fall through to the built-in binding
  CHECK(r.action == ActionId::None);
  CHECK(r.layer == Layer::User);

  // `none` in an outer context switches off only that context's lower layers, not an inner context's binding.
  const Binding inner[] = {bind(kScreen, tap(btn(1)), act("builtin.screen")), bind(kGlobal, tap(btn(2)), act("builtin.global"))};
  const Binding global_none[] = {bind(kGlobal, tap(btn(1)), ActionId::None), bind(kGlobal, tap(btn(2)), ActionId::None)};
  const BindingSet set = set_of(view(global_none), kEmpty, view(inner));
  CHECK(resolve(set, chain_of({kScreen, kGlobal}), tap_of(btn(1))).action == act("builtin.screen"));
  const Resolution off = resolve(set, chain_of({kScreen, kGlobal}), tap_of(btn(2)));
  CHECK((off.status == Status::Bound && off.action == ActionId::None));
}

TEST_CASE("resolver: an overlay that captures swallows what it does not bind, and hides everything outside it") {
  const Binding builtin[] = {bind(kOverlay, tap(btn(1)), act("overlay.one")), bind(kScreen, tap(btn(2)), act("screen.two")),
                             bind(kGlobal, tap(btn(3)), act("global.three"))};
  const Binding user[] = {bind(kGlobal, tap(btn(4)), act("user.global.four")), bind(kOverlay, tap(btn(5)), act("user.overlay.five"))};
  const BindingSet set = set_of(view(user), kEmpty, view(builtin));
  const ContextChain open = chain_of({kOverlay, kScreen, kGlobal});                  // an overlay that lets things through
  const ContextChain capturing = chain_of({kOverlay, kScreen, kGlobal}, 1u << 0);   // one that does not

  CHECK(resolve(set, capturing, tap_of(btn(1))).action == act("overlay.one"));       // its own bindings work
  CHECK(resolve(set, capturing, tap_of(btn(5))).action == act("user.overlay.five"));  // in every layer
  for (int id : {2, 3, 4}) {
    const Resolution r = resolve(set, capturing, tap_of(btn(static_cast<std::uint8_t>(id))));
    CHECK(r.status == Status::Captured);  // swallowed: neither the screen's nor the global nor the user's binding
    CHECK(r.action == ActionId::None);
  }
  CHECK(resolve(set, open, tap_of(btn(2))).action == act("screen.two"));
  CHECK(resolve(set, open, tap_of(btn(3))).action == act("global.three"));
  CHECK(resolve(set, open, tap_of(btn(4))).action == act("user.global.four"));
  CHECK(resolve(set, open, tap_of(btn(9))).status == Status::Unbound);

  // The middle of a chain may capture too: the screen below a mode that captures is out of reach.
  const ContextChain mode_captures = chain_of({kMode, kScreen, kGlobal}, 1u << 0);
  CHECK(resolve(set, mode_captures, tap_of(btn(2))).status == Status::Captured);
  const ContextChain screen_captures = chain_of({kMode, kScreen, kGlobal}, 1u << 1);
  CHECK(resolve(set, screen_captures, tap_of(btn(2))).action == act("screen.two"));
  CHECK(resolve(set, screen_captures, tap_of(btn(3))).status == Status::Captured);
  // The last context capturing is a capture as well.
  CHECK(resolve(set, chain_of({kGlobal}, 1u << 0), tap_of(btn(9))).status == Status::Captured);
  // An empty chain binds nothing and captures nothing.
  CHECK(resolve(set, ContextChain{}, tap_of(btn(1))).status == Status::Unbound);
}

TEST_CASE("resolver: chain bookkeeping") {
  ContextChain c;
  CHECK(c.reach() == 0u);
  CHECK(c == ContextChain{});
  for (unsigned i = 0; i < ContextChain::kMax; ++i) {
    CHECK(c.push(static_cast<ContextId>(i), i == 2));
  }
  CHECK_FALSE(c.push(9));  // full
  CHECK(c.count == ContextChain::kMax);
  CHECK(c.reach() == 3u);  // up to and including the first capturing entry

  // Equality is what tells the app that a screen or an overlay changed.
  CHECK(chain_of({kScreen, kGlobal}) == chain_of({kScreen, kGlobal}));
  CHECK(chain_of({kScreen, kGlobal}) != chain_of({kGlobal, kScreen}));
  CHECK(chain_of({kScreen}) != chain_of({kScreen, kGlobal}));
  CHECK(chain_of({kOverlay, kScreen}, 1u << 0) != chain_of({kOverlay, kScreen}));
  CHECK(chain_of({kScreen, kGlobal}) != chain_of({kMode, kGlobal}));
}

TEST_CASE("resolver: a gesture's context chain decides what a button is waited on for") {
  const Binding builtin[] = {
      bind(kScreen, hold(btn(1)), act("a.hold")),
      bind(kScreen, double_tap(btn(2)), act("a.double")),
      bind(kGlobal, chord(btn(kVol), btn(kFx)), act("a.chord")),
      bind(kMode, hold_encoder(enc(0), btn(3)), act("a.holdenc")),
      bind(kScreen, tap(btn(4)), act("a.tap")),
      bind(kScreen, hold(btn(5), btn(kShift)), act("a.shifthold")),
  };
  const BindingSet set = set_of(kEmpty, kEmpty, view(builtin));
  const ContextChain full = chain_of({kMode, kScreen, kGlobal});

  CHECK(gesture_wants(set, full, btn(1)) == kWantHold);
  CHECK(gesture_wants(set, full, btn(2)) == kWantDoubleTap);
  CHECK(gesture_wants(set, full, btn(kVol)) == kWantChord);
  CHECK(gesture_wants(set, full, btn(kFx)) == kWantChord);
  CHECK(gesture_wants(set, full, btn(3)) == kWantHoldEncoder);  // a hold+encoder binding holds the tap back to the release (OS-009: Fx + encoder)
  CHECK(gesture_wants(set, full, enc(0)) == 0);
  CHECK(gesture_wants(set, full, btn(4)) == 0);  // a plain tap needs no waiting
  CHECK(gesture_wants(set, full, btn(5)) == kWantHold);
  CHECK(gesture_wants(set, full, btn(6)) == 0);

  // Only contexts that are in force count.
  CHECK(gesture_wants(set, chain_of({kScreen}), btn(kVol)) == 0);
  CHECK(gesture_wants(set, chain_of({kScreen, kGlobal}), btn(1)) == kWantHold);
  // Behind a capturing overlay nothing outside it is waited for.
  CHECK(gesture_wants(set, chain_of({kMode, kScreen, kGlobal}, 1u << 0), btn(1)) == 0);

  // Wants add up across bindings and layers.
  const Binding user[] = {bind(kScreen, double_tap(btn(1)), act("u.double"))};
  CHECK(gesture_wants(set_of(view(user), kEmpty, view(builtin)), full, btn(1)) == (kWantHold | kWantDoubleTap));

  CHECK(chord_bound(set, full, btn(kVol), btn(kFx)));
  CHECK(chord_bound(set, full, btn(kFx), btn(kVol)));
  CHECK_FALSE(chord_bound(set, full, btn(kVol), btn(1)));
  CHECK_FALSE(chord_bound(set, chain_of({kScreen}), btn(kVol), btn(kFx)));  // global is not in force

  // Hold + encoder is asked per button and per encoder.
  CHECK(hold_encoder_bound(set, full, btn(3), enc(0)));
  CHECK_FALSE(hold_encoder_bound(set, full, btn(3), enc(1)));  // an encoder nobody bound
  CHECK_FALSE(hold_encoder_bound(set, full, btn(4), enc(0)));
  CHECK_FALSE(hold_encoder_bound(set, chain_of({kScreen, kGlobal}), btn(3), enc(0)));  // mode not in force
}

TEST_CASE("resolver: a button named as the modifier of a Tap, Hold or DoubleTap binding is a held modifier (kWantModifier)") {
  constexpr std::uint8_t kNote = 7;
  const Binding builtin[] = {
      bind(kScreen, hold(btn(kNote)), act("a.hold")),
      bind(kScreen, tap(btn(1), btn(kNote)), act("a.note1")),
      bind(kScreen, tap(btn(2), btn(kShift)), act("a.shift2")),
      bind(kMode, double_tap(btn(3), btn(8)), act("a.dbl")),
      bind(kScreen, chord(btn(9), btn(10)), act("a.chord")),            // a chord's lesser member is not a modifier
      bind(kScreen, hold_encoder(enc(0), btn(11)), act("a.holdenc")),   // nor is the button a HoldEncoder holds
  };
  const BindingSet set = set_of(kEmpty, kEmpty, view(builtin));
  const ContextChain full = chain_of({kMode, kScreen, kGlobal});
  CHECK(gesture_wants(set, full, btn(kNote)) == (kWantHold | kWantModifier));
  CHECK(gesture_wants(set, full, btn(8)) == kWantModifier);  // only its Tap waits for the release
  CHECK((gesture_wants(set, full, btn(kShift)) & kWantModifier) != 0);  // (the recogniser ignores it for the modifiers it was constructed with)
  CHECK(gesture_wants(set, full, btn(1)) == 0);
  CHECK(gesture_wants(set, full, btn(9)) == kWantChord);
  CHECK(gesture_wants(set, full, btn(10)) == kWantChord);
  CHECK(gesture_wants(set, full, btn(11)) == kWantHoldEncoder);
  // Context scoped: outside the chain that holds the binding nothing changes; behind a capturing overlay neither.
  CHECK(gesture_wants(set, chain_of({kMode, kGlobal}), btn(kNote)) == 0);
  CHECK(gesture_wants(set, chain_of({kOverlay, kScreen, kGlobal}, 1u << 0), btn(kNote)) == 0);
  // A binding switched off by `none` does not count.
  const Binding user[] = {bind(kScreen, tap(btn(1), btn(kNote)), ActionId::None), bind(kScreen, hold(btn(kNote)), ActionId::None)};
  CHECK(gesture_wants(set_of(view(user), kEmpty, view(builtin)), full, btn(kNote)) == 0);
}

TEST_CASE("resolver: what is shadowed by `none` is not waited for") {
  const Binding builtin[] = {
      bind(kGlobal, chord(btn(kVol), btn(kFx)), act("a.chord")),
      bind(kScreen, hold(btn(1)), act("a.hold")),
      bind(kScreen, double_tap(btn(2)), act("a.double")),
      bind(kScreen, hold_encoder(enc(0), btn(3)), act("a.holdenc")),
  };
  const Binding user[] = {
      bind(kGlobal, chord(btn(kFx), btn(kVol)), ActionId::None),
      bind(kScreen, hold(btn(1)), ActionId::None),
      bind(kScreen, hold_encoder(enc(0), btn(3)), ActionId::None),
  };
  const ContextChain chain = chain_of({kScreen, kGlobal});
  const BindingSet plain = set_of(kEmpty, kEmpty, view(builtin));
  const BindingSet shadowed = set_of(view(user), kEmpty, view(builtin));

  CHECK(chord_bound(plain, chain, btn(kVol), btn(kFx)));
  CHECK(gesture_wants(plain, chain, btn(kVol)) == kWantChord);
  CHECK(gesture_wants(plain, chain, btn(1)) == kWantHold);
  CHECK(hold_encoder_bound(plain, chain, btn(3), enc(0)));

  CHECK_FALSE(chord_bound(shadowed, chain, btn(kVol), btn(kFx)));
  CHECK(gesture_wants(shadowed, chain, btn(kVol)) == 0);
  CHECK(gesture_wants(shadowed, chain, btn(kFx)) == 0);
  CHECK(gesture_wants(shadowed, chain, btn(1)) == 0);
  CHECK_FALSE(hold_encoder_bound(shadowed, chain, btn(3), enc(0)));
  CHECK(gesture_wants(shadowed, chain, btn(2)) == kWantDoubleTap);  // untouched
}

TEST_CASE("resolver: a modifier that matches nothing falls back to the plain binding, once") {
  const Binding builtin[] = {bind(kScreen, tap(btn(1)), act("plain.one")), bind(kScreen, hold(btn(1)), act("plain.hold")),
                             bind(kScreen, tap(btn(2)), act("plain.two")), bind(kScreen, tap(btn(2), btn(kShift)), act("shift.two")),
                             bind(kScreen, tap(btn(3)), act("plain.three")), bind(kScreen, chord(btn(4), btn(5)), act("a.chord")),
                             bind(kGlobal, tap(btn(6), btn(kShift)), act("shift.global.six")),
                             bind(kScreen, tap(btn(6)), act("plain.six"))};
  const Binding user[] = {bind(kScreen, tap(btn(3), btn(kShift)), ActionId::None)};
  const BindingSet set = set_of(view(user), kEmpty, view(builtin));
  const ContextChain chain = chain_of({kScreen, kGlobal});
  const auto with_shift = [](GestureKind kind, InputRef in) { return Gesture{0, kind, in, btn(kShift), 0}; };

  // Shift held, nothing bound with Shift: the plain binding runs.
  Resolution r = resolve(set, chain, with_shift(GestureKind::Tap, btn(1)));
  CHECK(r.status == Status::Bound);
  CHECK(r.action == act("plain.one"));
  CHECK(resolve(set, chain, with_shift(GestureKind::Hold, btn(1))).action == act("plain.hold"));
  // A binding that names the modifier wins over the plain one.
  CHECK(resolve(set, chain, with_shift(GestureKind::Tap, btn(2))).action == act("shift.two"));
  CHECK(resolve(set, chain, tap_of(btn(2))).action == act("plain.two"));
  // Shift+3 bound to `none` is an explicit "nothing under Shift": no fallback past it.
  r = resolve(set, chain, with_shift(GestureKind::Tap, btn(3)));
  CHECK((r.status == Status::Bound && r.action == ActionId::None));
  CHECK(resolve(set, chain, tap_of(btn(3))).action == act("plain.three"));
  // The exact match is tried over the whole chain before the fallback: Shift+6 in Global beats plain 6 in the screen.
  CHECK(resolve(set, chain, with_shift(GestureKind::Tap, btn(6))).action == act("shift.global.six"));
  CHECK(resolve(set, chain_of({kScreen}), with_shift(GestureKind::Tap, btn(6))).action == act("plain.six"));
  // Nothing at all: unbound, as before.
  CHECK(resolve(set, chain, with_shift(GestureKind::Tap, btn(9))).status == Status::Unbound);
  // No fallback for a chord or hold+encoder: their second button is what they are.
  const Gesture proper_chord{0, GestureKind::Chord, btn(5), btn(4), 0};
  CHECK(resolve(set, chain, proper_chord).action == act("a.chord"));
  const Gesture odd_chord{0, GestureKind::Chord, btn(5), btn(kShift), 0};
  CHECK(resolve(set, chain, odd_chord).status == Status::Unbound);
  // A capturing context swallows what the fallback does not find either.
  CHECK(resolve(set, chain_of({kMode, kScreen}, 1u << 0), with_shift(GestureKind::Tap, btn(1))).status == Status::Captured);
}

TEST_CASE("resolver: HoldEnd is a gesture of its own, matched like a hold, with the modifier fallback") {
  const Binding builtin[] = {bind(kOverlay, hold_end(btn(1)), act("end.plain")),
                             bind(kOverlay, hold_end(btn(2), btn(kShift)), act("end.shift")),
                             bind(kScreen, hold(btn(1), btn(kShift)), act("start")),
                             bind(kScreen, tap(btn(1)), act("tap"))};
  const BindingSet set = set_of(kEmpty, kEmpty, view(builtin));
  const auto end_of = [](InputRef in, InputRef mod) { return Gesture{0, GestureKind::HoldEnd, in, mod, 0}; };
  const ContextChain chain = chain_of({kOverlay, kScreen, kGlobal}, 1u << 0);  // the overlay captures

  CHECK(hold_end(btn(1)).gesture == GestureKind::HoldEnd);
  CHECK(hold_end(btn(1)) != hold(btn(1)));  // not the hold it ends
  CHECK(resolve(set, chain, end_of(btn(1), kNoInput)).action == act("end.plain"));
  // The release of a hold made with Shift down is a HoldEnd with Shift: no binding names it, the plain one runs.
  CHECK(resolve(set, chain, end_of(btn(1), btn(kShift))).action == act("end.plain"));
  CHECK(resolve(set, chain, end_of(btn(2), btn(kShift))).action == act("end.shift"));  // one that names it wins
  CHECK(resolve(set, chain, end_of(btn(2), kNoInput)).status == Status::Captured);     // the overlay swallows the rest
  // The end of a hold is only seen if the button waits for the hold: a HoldEnd binding makes its button wait, with or
  // without a Hold binding of its own (the hold itself then runs nothing). Switched off by `none`, it does not.
  const Binding only_end[] = {bind(kScreen, hold_end(btn(5)), act("end"))};
  CHECK(gesture_wants(set_of(kEmpty, kEmpty, view(only_end)), chain_of({kScreen}), btn(5)) == kWantHold);
  CHECK(gesture_wants(set_of(kEmpty, kEmpty, view(only_end)), chain_of({kScreen}), btn(6)) == 0);  // another button
  const Binding shadowed_end[] = {bind(kScreen, hold_end(btn(5)), ActionId::None)};
  CHECK(gesture_wants(set_of(view(shadowed_end), kEmpty, view(only_end)), chain_of({kScreen}), btn(5)) == 0);
  const Binding shifted_end[] = {bind(kScreen, hold_end(btn(5), btn(kShift)), act("end"))};
  CHECK(gesture_wants(set_of(kEmpty, kEmpty, view(shifted_end)), chain_of({kScreen}), btn(5)) == kWantHold);
  CHECK(gesture_wants(set, chain_of({kScreen, kGlobal}), btn(1)) == kWantHold);  // the Shift hold does
  // Two HoldEnd bindings with one trigger in one context conflict, like any other gesture.
  const Binding twice[] = {bind(kScreen, hold_end(btn(1)), act("a")), bind(kScreen, hold_end(btn(1)), act("b"))};
  CHECK(find_conflict(twice, 2).found);
  const Binding apart[] = {bind(kScreen, hold_end(btn(1)), act("a")), bind(kScreen, hold(btn(1)), act("b"))};
  CHECK_FALSE(find_conflict(apart, 2).found);
}
