// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The gesture layer (ADR 0009 rev 1): between HAL input events and bindings. A stateful recogniser turns
// timestamped InputEvents into Gestures: tap, hold, double tap, chord, and encoder-with-a-button-held.
//
// Time. The recogniser never reads a clock. Time arrives as arguments: the timestamp of each event (feed) and the
// current tick (advance), so a run is a pure function of what it is given: the same events give the same
// gestures however often advance() is called in between, and a gesture that comes from a timer carries the
// deadline of that timer, not the moment someone looked. All three timings are in one table (GestureTimings).
// Timestamps compare wrap-safely (core/tick.h); a timestamp older than the newest one seen is treated as that one.
//
// Latency. Waiting costs responsiveness, so a button is only held back when something is bound that needs the
// wait; the caller says what through GestureHints (the binding layer answers it, binding.h). A button nobody
// wants hold / double tap / chord of gives its Tap at the press itself. Otherwise:
//   hold wanted         Tap at the release, Hold once the button has been down hold_ms (then no Tap)
//   double tap wanted   after a release, Tap only once double_tap_ms has passed without a second press;
//                       a second press within it gives DoubleTap (no Tap)
//   chord wanted        a second button pressed within chord_ms (and bound as a chord with the first) gives one
//                       Chord and neither Tap; otherwise Tap at the release or after chord_ms, whichever is first
//   hold-encoder wanted the button has a "held while the encoder turns" binding (OS-009: Fx + encoder is the FX type): its Tap waits
//                       for the release, so that a turn can still consume it (neither Tap nor Hold) and the button that is held to
//                       be a modifier does not also act as itself the moment it is pressed
//   modifier wanted     (OS-ZOOM) a Tap, Hold or DoubleTap binding of the context names the button as its `modifier` (Note + Instr on Pattern): the button is a held modifier like Shift,
//                       and its own Tap waits for the release, like the hold-encoder buttons. See "Held modifiers" below.
// Any other press, pad hit or encoder turn ends these waits: the older button's Tap comes out first, in order.
//
// Meaning of the fields (Gesture):
//   Tap          a single actuation. Button: the press (or the release, if it had to wait); Pad: the hit, value =
//                velocity; Encoder: a turn, value = signed detents. `modifier` is the declared modifier button (Shift)
//                held at the time, else kNoInput.
//   Hold         a button held for hold_ms.  DoubleTap  two taps in double_tap_ms.
//   HoldEncoder  an encoder turn while a button is held that is bound with that encoder (hints.hold_encoder):
//                input = the encoder, modifier = that button, value = detents. A button still waiting to decide
//                is consumed by the turn: it will give neither Tap nor Hold. (A button that tapped at its press
//                has done so already.) Its release is silent, unless its Hold had fired: that release is HoldEnd.
//   HoldEnd      the release of a button whose Hold has fired (OS-009: a held F1 on Pattern keeps the pattern chooser
//                open, letting go of it closes it): input = the button, modifier = what was held at
//                its press. Any other release is silent. It is what makes "hold to peek, release to pick" a binding.
//                A binding on HoldEnd makes its button wait for the hold (kWantHold), as a Hold binding does.
//   Chord        two buttons pressed together: input = the greater, modifier = the lesser (input_less), so a
//                binding is written once whichever was pressed first.
// Held modifiers (OS-ZOOM, NEW OS BEHAVIOUR). Shift is a modifier by construction (the constructor's mask). A button with the kWantModifier bit (the binding layer sets it from the bindings of the current context chain,
// never for a button of the mask) is one too, while it is down: the Tap, Hold or DoubleTap of another button made under it carries it as `modifier`, unless Shift is held (a declared modifier wins; the
// binding layer then tries the plain row). The press of any other button, Shift apart, while such a button is still undecided (Down) consumes it: it gives neither Tap nor Hold, its release is
// silent. A pad hit or an encoder turn does not consume it (a Tap or a Hold it waits for still comes). A modifier already decided (consumed, or its Hold out) stays a modifier until its release.
//
// Pads support Tap only, encoders Tap and HoldEncoder: no hold or chord on pads yet. Reserved InputKinds are ignored.
//
// Auto-repeat (OS-009b, NEW OS BEHAVIOUR). A button that is in set_repeat_buttons() (the arrows: the app names them) and that tapped at its press
// goes on tapping while it is held: one more Tap (same input, same modifier as at the press, `timestamp` the moment it was due) at each time of the
// schedule below, and nothing at the release. The repeats are Taps flagged `repeat` (OS-DELREPEAT), so every binding of the button resolves as at the press and no binding changes; whether the action then runs is the app's rule (dispatch: only a kActionRepeatable action takes a repeat). A button that
// had to wait (hold, double tap, chord wanted: its Tap is at the release) does not repeat. A call that finds several repeats due gives one: a stalled
// loop does not queue a burst of cursor moves.
//
// Main loop only; no heap; every call is bounded by kMaxHeld.

#include <o3sdk/core/fixed_vector.h>
#include <o3sdk/core/input_event.h>
#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::core {

enum class GestureKind : std::uint8_t { Tap, Hold, DoubleTap, HoldEncoder, Chord, HoldEnd };

struct Gesture {
  Tick timestamp;  // when it was recognised: the event's time, or the deadline of the timer that decided it
  GestureKind kind;
  InputRef input;
  InputRef modifier;
  std::int16_t value;
  bool repeat = false;  // a Tap that is an auto-repeat of a held button (OS-DELREPEAT), not the press itself: the app lets only a repeatable action take it
};

// All gesture timings, in one place. PROVISIONAL except hold_ms (400 ms, our choice);
// double tap and chord windows are to be tuned on the device.
struct GestureTimings {
  std::uint32_t hold_ms;        // a button down this long is a Hold
  std::uint32_t double_tap_ms;  // second press within this long after the first release: DoubleTap
  std::uint32_t chord_ms;       // second button within this long after the first press: Chord
};
inline constexpr GestureTimings kGestureTimings{400, 300, 40};

// The auto-repeat schedule, in one table: milliseconds from the press to the first repeat, then the gap before each next one; the last gap holds for
// as long as the button is. This is our own schedule, a choice of this OS (the first repeat at 400 ms is hold_ms, also our choice): a
// held key fires its first repeat at 400 ms, then at 700, 900, 1050, 1150, 1230, 1310 ... (the gaps are 400, 300, 200, 150, 100, then 80 ms from there). That the
// pattern arrows repeat is the maintainer's request for this OS. Tune HERE.
inline constexpr unsigned kRepeatSteps = 6;
inline constexpr std::uint32_t kRepeatGapMs[kRepeatSteps] = {400, 300, 200, 150, 100, 80};

// What a button's bindings need, as bits.
inline constexpr std::uint8_t kWantHold = 1u << 0;
inline constexpr std::uint8_t kWantDoubleTap = 1u << 1;
inline constexpr std::uint8_t kWantChord = 1u << 2;
inline constexpr std::uint8_t kWantHoldEncoder = 1u << 3;
inline constexpr std::uint8_t kWantModifier = 1u << 4;  // a Tap / Hold / DoubleTap binding names this button as its modifier (never set for the constructor's modifiers)

// Questions the recogniser asks the binding layer; any pointer may be null (nothing is wanted: every button taps
// at the press and every turn is a plain tap). The answers hold for the current context chain, evaluated at the
// time of the press or the turn.
struct GestureHints {
  const void* ctx;
  std::uint8_t (*wants)(const void* ctx, InputRef input) noexcept;       // kWant* bits for this button
  bool (*chord)(const void* ctx, InputRef a, InputRef b) noexcept;       // is the pair bound as a chord?
  // Is "this encoder turned while this button is held" bound? Asked per encoder: a turn of an encoder nobody
  // bound is a plain tap, whatever is held.
  bool (*hold_encoder)(const void* ctx, InputRef button, InputRef encoder) noexcept;
};
inline constexpr GestureHints kNoHints{nullptr, nullptr, nullptr, nullptr};

// One call emits at most one gesture per held button plus the new one: kMaxHeld + 1 <= capacity.
inline constexpr unsigned kGestureListCapacity = 8;
using GestureList = FixedVector<Gesture, kGestureListCapacity>;

class GestureRecognizer {
 public:
  static constexpr unsigned kMaxHeld = 6;  // buttons tracked at once; a seventh press forgets the oldest

  // `modifier_buttons`: bit n set = button id n (< 64) is a modifier (Shift).
  explicit GestureRecognizer(std::uint64_t modifier_buttons = 0, const GestureTimings& timings = kGestureTimings) noexcept
      : timings_(timings), modifiers_(modifier_buttons) {}

  // The buttons that auto-repeat while held (bit n = button id n < 64; none at first). The arrows, for the app. Takes effect for the next press.
  void set_repeat_buttons(std::uint64_t buttons) noexcept { repeats_ = buttons; }

  // Handles one event: first lets time run up to its timestamp, then applies it. Appends to `out`.
  void feed(const InputEvent& event, const GestureHints& hints, GestureList& out) noexcept;

  // Lets time run up to `now`: fires the holds, double-tap and chord waits that have come due, in deadline order.
  void advance(Tick now, GestureList& out) noexcept;

  // Forgets every button believed held, and every pending wait, without emitting anything. For when input may
  // have been lost, or its meaning changes under the user's fingers (a new screen, mode or overlay).
  // `keep_held` spares what is still physically down and must survive such a change: the modifier buttons (Shift
  // held across a screen change still modifies the next key, and a Shift press that is waiting for its own hold goes
  // on waiting), and the buttons whose Hold has fired (a Hold that opens an overlay changes the context, and the
  // release that ends it, HoldEnd, must still be seen). Everything else is forgotten, a button waiting for a second
  // tap included. The price: a modifier whose release was lost stays held until it is pressed or released again, or
  // until a full reset(): App::reset_input() is that.
  void reset(bool keep_held = false) noexcept;

  unsigned held() const noexcept { return count_; }  // buttons currently tracked (down, or waiting for a second tap)

  // OS-PATTEMPO: is this button physically down and tracked (not one that is only waiting for a second tap)? And `consume`: a button still undecided (Down) gives neither Tap nor Hold, as after
  // an encoder turn (HoldEncoder); its release is silent. The app uses them for a pad hit under a held button, which it takes ahead of the bindings.
  bool down(std::uint8_t id) const noexcept {
    const int i = find(id);
    return i >= 0 && entries_[static_cast<unsigned>(i)].state != State::WaitDouble;
  }
  void consume(std::uint8_t id) noexcept {
    const int i = find(id);
    if (i >= 0 && entries_[static_cast<unsigned>(i)].state == State::Down) {
      entries_[static_cast<unsigned>(i)].state = State::Decided;
    }
  }

 private:
  enum class State : std::uint8_t { Down, Decided, WaitDouble, Held };
  // One tracked button. Down: pressed, nothing emitted yet. Decided: pressed, its gesture is out, the release
  // will be silent. WaitDouble: released as a tap candidate, waiting for a second press. Held: its Hold is out,
  // the release will give HoldEnd.
  struct Entry {
    Tick t0;  // press time
    Tick t1;  // release time (WaitDouble)
    std::uint8_t id;
    State state;
    std::uint8_t wants;
    std::uint8_t mod;  // modifier button held at the press, or 0xFF
    bool repeat = false;       // Decided at its press and in repeats_: it taps again on the schedule
    std::uint32_t reps = 0;    // repeats given so far (or skipped: see fire())
  };

  void press(std::uint8_t id, Tick t, const GestureHints& hints, GestureList& out) noexcept;
  void release(std::uint8_t id, Tick t, bool allow_double, GestureList& out) noexcept;
  void hit_pad(std::uint8_t id, std::int16_t velocity, Tick t, GestureList& out) noexcept;
  void turn(std::uint8_t id, std::int16_t delta, Tick t, const GestureHints& hints, GestureList& out) noexcept;

  int find(std::uint8_t id) const noexcept;
  void add(std::uint8_t id, State state, std::uint8_t wants, std::uint8_t mod, Tick t) noexcept;
  void remove(unsigned index) noexcept;
  std::uint8_t held_modifier() const noexcept;
  bool is_modifier(std::uint8_t id) const noexcept { return id < 64 && ((modifiers_ >> id) & 1u) != 0; }
  void consume_modifiers(std::uint8_t pressed) noexcept;
  bool deadline_of(const Entry& e, Tick& deadline) const noexcept;
  void fire(unsigned index, Tick deadline, Tick now, GestureList& out) noexcept;
  void settle_waiting(Tick t, GestureList& out, std::uint8_t except) noexcept;
  void settle_chords(Tick t, GestureList& out, std::uint8_t except) noexcept;

  GestureTimings timings_;
  std::uint64_t modifiers_;
  std::uint64_t repeats_ = 0;
  Entry entries_[kMaxHeld] = {};
  unsigned count_ = 0;
  Tick now_ = 0;
  bool started_ = false;
};

static_assert(GestureRecognizer::kMaxHeld + 1 <= kGestureListCapacity, "one call must fit the gesture list");

}  // namespace o3sdk::core
