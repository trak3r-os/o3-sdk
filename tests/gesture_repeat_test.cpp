// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-009b: auto-repeat of a held arrow in the gesture layer (core/gesture.h, kRepeatGapMs: our own schedule). New OS behaviour,
// so the schedule is pinned here, in event times.
#include <doctest/doctest.h>

#include <o3sdk/core/gesture.h>
#include <o3sdk/core/input_event.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace o3sdk::core;

namespace {

constexpr std::uint8_t kArrow = 26;
constexpr std::uint8_t kOther = 5;
constexpr std::uint8_t kShift = 29;
constexpr std::uint64_t kArrows = std::uint64_t{1} << kArrow;
constexpr std::uint64_t kShiftMask = std::uint64_t{1} << kShift;

constexpr Tick ms(std::uint32_t n) { return ticks_from_ms(n); }

struct Rig {
  GestureRecognizer rec{kShiftMask};
  std::vector<Gesture> taps;

  Rig() { rec.set_repeat_buttons(kArrows); }
  void collect(const GestureList& out) {
    for (const Gesture& g : out) {
      taps.push_back(g);
    }
  }
  void feed(const InputEvent& e) {
    GestureList out;
    rec.feed(e, kNoHints, out);
    collect(out);
  }
  void advance(std::uint32_t at_ms) {
    GestureList out;
    rec.advance(ms(at_ms), out);
    collect(out);
  }
  void press(std::uint32_t at_ms, std::uint8_t id) { feed(make_button_event(ms(at_ms), id, true)); }
  void release(std::uint32_t at_ms, std::uint8_t id) { feed(make_button_event(ms(at_ms), id, false)); }
  // The main loop's pass: one advance every 10 ms up to `to_ms`.
  void run(std::uint32_t from_ms, std::uint32_t to_ms) {
    for (std::uint32_t t = from_ms; t <= to_ms; t += 10) {
      advance(t);
    }
  }
  std::vector<std::uint32_t> times_ms() const {
    std::vector<std::uint32_t> v;
    for (const Gesture& g : taps) {
      v.push_back(g.timestamp / kTicksPerMs);
    }
    return v;
  }
};

}  // namespace

TEST_CASE("repeat: a held arrow taps at the press, at 400 ms, then at gaps of 300, 200, 150, 100 and 80 ms for as long as it is held") {
  Rig r;
  r.press(0, kArrow);
  r.run(10, 2000);
  const std::vector<std::uint32_t> expected = {0,    400,  700,  900,  1050, 1150, 1230, 1310,
                                               1390, 1470, 1550, 1630, 1710, 1790, 1870, 1950};
  CHECK(r.times_ms() == expected);
  for (std::size_t i = 0; i < r.taps.size(); ++i) {
    const Gesture& g = r.taps[i];
    CHECK(g.kind == GestureKind::Tap);
    CHECK(g.input == (InputRef{InputKind::Button, kArrow}));
    CHECK_MESSAGE(g.repeat == (i != 0u), "tap " << i);  // OS-DELREPEAT: the press is a fresh Tap, every later one is flagged a repeat
  }
  r.release(2010, kArrow);
  r.run(2020, 3000);
  CHECK(r.taps.size() == expected.size());  // the release is silent and nothing follows it
}

TEST_CASE("repeat: only the buttons named repeat, a stall gives one tap, and the modifier the press was made under keeps it going") {
  Rig r;
  r.press(0, kOther);
  r.run(10, 1500);
  CHECK(r.taps.size() == 1);  // not an arrow: one Tap at the press
  r.release(1510, kOther);

  Rig s;
  s.press(0, kArrow);
  s.advance(5000);  // the loop was away for five seconds: one repeat, due at 400 ms, not a burst
  CHECK(s.taps.size() == 2);
  s.advance(5010);
  CHECK(s.taps.size() == 2);
  s.advance(5100);  // and the next is the first one still ahead of the clock
  CHECK(s.taps.size() == 3);

  Rig m;
  m.press(0, kShift);
  m.press(100, kArrow);
  m.run(110, 780);
  CHECK(m.taps.size() == 3);  // Shift's tap, the arrow's, one repeat at 500 ms (400 after the press at 100)
  CHECK(m.taps[2].modifier == (InputRef{InputKind::Button, kShift}));
  m.release(790, kShift);  // Shift let go while the arrow is held (its next repeat is due at 800): the repeats end rather than turn into plain arrows
  m.run(800, 2500);
  CHECK(m.taps.size() == 3);
}

// OS-ZOOM: Note + Right repeats the track move for as long as both are held, under the Note modifier, and stops when Note is let go.
namespace {
constexpr std::uint8_t kNoteKey = 4;
std::uint8_t note_wants(const void*, InputRef in) noexcept {
  return in.kind == InputKind::Button && in.id == kNoteKey ? static_cast<std::uint8_t>(kWantHold | kWantModifier) : std::uint8_t{0};
}
}  // namespace

TEST_CASE("repeat: Note + an arrow repeats under Note and stops when Note is let go") {
  Rig r;
  const GestureHints hints{nullptr, note_wants, nullptr, nullptr};
  const auto feed = [&](const InputEvent& e) {
    GestureList out;
    r.rec.feed(e, hints, out);
    r.collect(out);
  };
  feed(make_button_event(ms(0), kNoteKey, true));
  feed(make_button_event(ms(100), kArrow, true));
  r.run(110, 940);
  const std::vector<std::uint32_t> before = r.times_ms();
  CHECK(before == std::vector<std::uint32_t>{100, 500, 800});  // the press and the repeats of the schedule, 400 and 300 ms later
  for (const Gesture& g : r.taps) {
    CHECK(g.kind == GestureKind::Tap);
    CHECK(g.input == (InputRef{InputKind::Button, kArrow}));
    CHECK(g.modifier == (InputRef{InputKind::Button, kNoteKey}));
  }
  feed(make_button_event(ms(950), kNoteKey, false));  // Note let go (the next repeat is due at 1000), the arrow still down: the repeats end, they do not turn into another gesture
  r.run(960, 3000);
  CHECK(r.taps.size() == before.size());
  feed(make_button_event(ms(3000), kArrow, false));
  CHECK(r.taps.size() == before.size());
}
