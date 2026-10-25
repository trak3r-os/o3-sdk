// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-POWER P1: core::PowerHold, the table of docs/specs/OS-POWER.md section 3, for both polarities of the pin (M4 is open: the board constant is the only unknown). Time is driven by hand, one
// millisecond a sample unless a case feeds a gap on purpose.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/core/power_hold.h>

#include <cstdint>

using o3sdk::core::PowerHold;
using o3sdk::core::PowerText;
namespace power = o3sdk::core::power;

namespace {

struct Rig {
  explicit Rig(bool active_low) : hold(active_low), low(active_low) {}

  PowerHold hold;
  bool low;
  std::uint32_t t = 0;
  PowerHold::Result last;
  unsigned commits = 0;
  unsigned stuck = 0;
  unsigned shots = 0;  // OS-SCREENSHOT: Result::screenshot, counted
  bool ready = true;
  bool shift = false;

  // One sample at the current time, then the time moves on by `step` ms.
  PowerHold::Result sample(bool pressed, std::uint32_t step = 1) {
    last = hold.update(t, pressed != low, shift, ready);
    commits += last.commit ? 1u : 0u;
    stuck += last.stuck ? 1u : 0u;
    shots += last.screenshot ? 1u : 0u;
    t += step;
    return last;
  }
  // `ms` samples of one ms with the switch in one state.
  void run(std::uint32_t ms, bool pressed) {
    for (std::uint32_t i = 0; i < ms; ++i) {
      sample(pressed);
    }
  }
  // A released stretch long enough to arm the switch.
  void arm() { run(power::kArmReleaseMs + power::kDebounceMs, false); }
};

// Every case below runs for both polarities.
template <typename F>
void both(F body) {
  body(true);
  body(false);
}

}  // namespace

TEST_CASE("power hold: the constants are the spec's") {
  CHECK(power::kHoldMs == 2000);
  CHECK(power::kDebounceMs == 30);
  CHECK(power::kArmReleaseMs == 100);
  CHECK(power::kMaxGapMs == 250);
  CHECK(power::kShowAfterMs == 150);
  CHECK(power::kHintMinMs == 150);
  CHECK(power::kHintShowMs == 1500);
  CHECK(power::kStuckLogMs == 5000);
  CHECK(power::kReleaseStableMs == 100);
}

TEST_CASE("power hold: Booting needs the app ready and a released level for 100 ms") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.ready = false;
    r.run(500, false);
    CHECK(r.last.state == PowerHold::State::Booting);  // released for ever, but the app has not the screen
    r.ready = true;
    r.sample(false);
    CHECK(r.last.state == PowerHold::State::Armed);  // ready and long released: armed at once
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.run(99, false);
    CHECK(r.last.state == PowerHold::State::Booting);
    r.sample(false);
    CHECK(r.last.state == PowerHold::State::Booting);  // 99 ms of release (the first sample starts the count)
    r.sample(false);
    CHECK(r.last.state == PowerHold::State::Armed);
  });
}

TEST_CASE("power hold: a Power held through the boot never counts, and logs once at 5 s") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.run(power::kStuckLogMs + 4000, true);
    CHECK(r.commits == 0);
    CHECK(r.last.state == PowerHold::State::Booting);
    CHECK_FALSE(r.last.show.shown());
    CHECK(r.stuck == 1);
    // the release, then a real hold works
    r.arm();
    CHECK(r.last.state == PowerHold::State::Armed);
    r.run(power::kHoldMs + 100, true);
    CHECK(r.commits == 1);
  });
}

TEST_CASE("power hold: a switch that always reads pressed (wrong polarity) never arms") {
  Rig r(true);
  for (std::uint32_t i = 0; i < 20000; ++i) {
    r.last = r.hold.update(r.t++, false /* pin low = pressed for an active-low board */, false, true);
    r.commits += r.last.commit ? 1u : 0u;
  }
  CHECK(r.commits == 0);
  CHECK(r.hold.state() == PowerHold::State::Booting);
}

TEST_CASE("power hold: the press needs its debounce, both edges") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(power::kDebounceMs - 1, true);  // 29 ms: a brush
    r.run(40, false);
    CHECK(r.hold.presses() == 0);
    CHECK(r.last.state == PowerHold::State::Armed);
    // contact bounce on the way down: 10 on, 10 off, 10 on, 10 off, then down for good
    r.run(10, true);
    r.run(10, false);
    r.run(10, true);
    r.run(10, false);
    r.run(100, true);
    CHECK(r.hold.presses() == 1);
    CHECK(r.last.state == PowerHold::State::Holding);
    // a 10 ms bounce on the way up does not release the hold
    r.run(500, true);
    r.run(10, false);
    r.run(200, true);
    CHECK(r.last.state == PowerHold::State::Holding);
    CHECK(r.hold.presses() == 1);
  });
}

TEST_CASE("power hold: the line fills at the true fraction from 150 ms and shows HOLD") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    const std::uint32_t pressed_at = r.t;
    while (r.t - pressed_at < 1000) {
      r.sample(true);
      const std::uint32_t held = r.t - 1 - pressed_at;
      if (held < power::kShowAfterMs) {
        CHECK(r.last.show.fill == 0);
        CHECK(r.last.show.text == PowerText::None);
      }
    }
    // the last sample was 999 ms after the first of the press
    CHECK(r.last.state == PowerHold::State::Holding);
    CHECK(r.last.show.text == PowerText::Hold);
    CHECK(r.last.show.fill == 999 * 1000 / 2000);
    r.run(700, true);
    CHECK(r.last.show.fill == 1699 * 1000 / 2000);
    CHECK(r.commits == 0);
  });
}

TEST_CASE("power hold: Commit comes once, at 2000 ms from the press, and a release after it does not cancel") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    const std::uint32_t pressed_at = r.t;
    std::uint32_t commit_at = 0;
    for (int i = 0; i < 4000; ++i) {
      const std::uint32_t at = r.t;
      r.sample(true);
      if (r.last.commit) {
        commit_at = at;
      }
    }
    CHECK(r.commits == 1);
    CHECK(commit_at - pressed_at == power::kHoldMs);
    CHECK(r.last.state == PowerHold::State::Committed);
    CHECK(r.last.show.fill == 1000);
    r.run(300, false);  // let go
    CHECK(r.last.state == PowerHold::State::Committed);
    CHECK(r.commits == 1);
    CHECK(r.hold.commits() == 1);
  });
}

TEST_CASE("power hold: letting go before the end cancels, and a tap over 150 ms shows the hint for 1.5 s") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(1900, true);
    r.run(100, false);
    CHECK(r.commits == 0);
    CHECK(r.last.state == PowerHold::State::Armed);
    CHECK(r.last.show.fill == 0);
    CHECK(r.last.show.text == PowerText::Hint);  // 1.9 s is a long tap: the hint says how long to hold
  });
}

TEST_CASE("power hold: the hint after a tap") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(500, true);
    r.run(power::kDebounceMs + 5, false);
    CHECK(r.last.state == PowerHold::State::Armed);
    CHECK(r.last.show.text == PowerText::Hint);
    CHECK(r.last.show.fill == 0);
    CHECK(r.hold.taps() == 1);
    r.run(power::kHintShowMs - 200, false);
    CHECK(r.last.show.text == PowerText::Hint);
    r.run(300, false);
    CHECK(r.last.show.text == PowerText::None);
  });
}

TEST_CASE("power hold: a tap under 150 ms shows nothing at all") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(120, true);
    r.run(power::kDebounceMs + 5, false);
    CHECK(r.hold.presses() == 1);
    CHECK(r.last.show.text == PowerText::None);
    CHECK(r.last.show.fill == 0);
    CHECK(r.hold.taps() == 0);
    r.run(2000, false);
    CHECK(r.last.show.text == PowerText::None);
  });
}

TEST_CASE("power hold: Shift down when Power goes down is reserved, nothing at all") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.shift = true;
    for (int i = 0; i < 3000; ++i) {
      r.sample(true);
      CHECK_FALSE(r.last.show.shown());
    }
    CHECK(r.commits == 0);
    CHECK(r.shots == 1);  // OS-SCREENSHOT: the press with Shift down is the screenshot, once, however long Power stays down
    CHECK(r.hold.screenshots() == 1);
    CHECK(r.last.state == PowerHold::State::Spent);
    // Power then Shift released: armed again; a plain hold works
    r.run(100, false);
    r.shift = false;
    r.run(100, false);
    CHECK(r.last.state == PowerHold::State::Armed);
    r.run(power::kHoldMs + 50, true);
    CHECK(r.commits == 1);
    CHECK(r.shots == 1);  // the plain hold added none
  });
}

TEST_CASE("power hold: the screenshot event is Shift-then-Power only (OS-SCREENSHOT)") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    // a plain press and release, a tap and a full hold: no event
    r.run(300, true);
    r.run(100, false);
    r.run(power::kHoldMs + 50, true);
    CHECK(r.commits == 1);
    CHECK(r.shots == 0);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    // Power first, Shift during the hold: the cancel, not a screenshot
    r.run(500, true);
    r.shift = true;
    r.run(3000, true);
    CHECK(r.shots == 0);
    CHECK(r.commits == 0);
    r.run(100, false);
    r.shift = false;
    // Shift down, Power down and up, twice: two events, no hold, no line, no commit
    r.arm();
    for (unsigned n = 1; n <= 2; ++n) {
      r.shift = true;
      r.run(300, true);
      CHECK(r.shots == n);
      CHECK_FALSE(r.last.show.shown());
      r.run(100, false);
      r.shift = false;
      r.run(100, false);
      CHECK(r.last.state == PowerHold::State::Armed);
    }
    CHECK(r.commits == 0);
    CHECK(r.hold.holds() == 1);  // the one hold Shift cancelled; the two Shift-first presses started none
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // not armed yet (Booting): Shift + Power through the boot is nothing, and a bounce under kDebounceMs is nothing
    r.shift = true;
    r.run(500, true);
    CHECK(r.shots == 0);
    r.run(200, false);
    r.arm();
    r.run(power::kDebounceMs - 5, true);
    r.run(50, false);
    CHECK(r.shots == 0);
    // the event comes with the debounce, on the sample that completes it
    r.run(power::kDebounceMs, true);
    CHECK(r.shots == 0);
    r.sample(true);  // 30 ms after the first sample of the press
    CHECK(r.shots == 1);
  });
}

TEST_CASE("power hold: Shift pressed during a hold cancels it until Power is released, even after Shift is let go") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(1000, true);
    CHECK(r.last.show.fill > 0);
    r.shift = true;
    r.sample(true);
    CHECK(r.last.state == PowerHold::State::Spent);
    CHECK(r.last.show.fill == 0);
    r.run(500, true);
    r.shift = false;  // Shift let go while Power is still down: the hold stays spent
    r.run(3000, true);
    CHECK(r.commits == 0);
    CHECK(r.last.state == PowerHold::State::Spent);
    CHECK_FALSE(r.last.show.shown());
    r.run(power::kDebounceMs + 5, false);
    CHECK(r.last.state == PowerHold::State::Armed);
    r.run(power::kHoldMs + 50, true);
    CHECK(r.commits == 1);
  });
}

TEST_CASE("power hold: the sample gap makes a new start") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // two low samples 2 s and 3 s apart never add up to one hold
    r.arm();
    r.sample(true, 2000);
    r.sample(true, 3000);
    r.sample(true, 3000);
    CHECK(r.commits == 0);
    CHECK(r.hold.presses() == 0);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // Holding, then a block (a card mount) of 300 ms: back to Armed, the press is seen again and the whole hold is needed
    r.arm();
    r.run(1500, true);
    CHECK(r.last.state == PowerHold::State::Holding);
    r.t += 300 - 1;
    r.sample(true);
    CHECK(r.last.state == PowerHold::State::Armed);
    CHECK(r.last.show.fill == 0);
    r.run(power::kDebounceMs + 5, true);
    CHECK(r.last.state == PowerHold::State::Holding);
    r.run(power::kHoldMs - 400, true);
    CHECK(r.commits == 0);
    r.run(500, true);
    CHECK(r.commits == 1);
    CHECK(r.hold.presses() == 2);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // a gap of exactly kMaxGapMs is a continuation
    r.arm();
    r.run(1000, true);
    r.t += power::kMaxGapMs - 1;
    r.sample(true);
    CHECK(r.last.state == PowerHold::State::Holding);
    r.run(power::kHoldMs, true);
    CHECK(r.commits == 1);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // Booting: a gap restarts the 100 ms release
    r.run(60, false);
    r.t += 400;
    r.run(60, false);
    CHECK(r.last.state == PowerHold::State::Booting);
    r.run(60, false);
    CHECK(r.last.state == PowerHold::State::Armed);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // Armed: a gap restarts the debounce, so a press that began before it does not count at once
    r.arm();
    r.run(10, true);
    r.t += 400;
    r.sample(true);
    CHECK(r.hold.presses() == 0);
    r.run(power::kDebounceMs, true);
    CHECK(r.hold.presses() == 1);
  });
}

TEST_CASE("power hold: a gap in Spent keeps it spent") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.shift = true;
    r.run(200, true);
    CHECK(r.last.state == PowerHold::State::Spent);
    r.shift = false;
    r.t += 3000;  // a card mount
    r.run(300, true);
    CHECK(r.last.state == PowerHold::State::Spent);
    CHECK(r.commits == 0);
  });
}

TEST_CASE("power hold: refuse() makes a committed hold spent until Power is released") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(power::kHoldMs + 10, true);
    NEED(r.commits == 1);
    r.hold.refuse();
    CHECK(r.hold.refusals() == 1);
    r.run(3000, true);
    CHECK(r.commits == 1);
    CHECK(r.last.state == PowerHold::State::Spent);
    CHECK_FALSE(r.last.show.shown());
    r.run(power::kDebounceMs + 5, false);
    CHECK(r.last.state == PowerHold::State::Armed);
    r.run(power::kHoldMs + 50, true);
    CHECK(r.commits == 2);
  });
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    // refused after the finger was already off: armed again at once
    r.arm();
    r.run(power::kHoldMs + 10, true);
    r.run(200, false);
    r.hold.refuse();
    r.sample(false);
    CHECK(r.last.state == PowerHold::State::Armed);
  });
}

TEST_CASE("power hold: the stable release of stage F0 needs 100 ms and a bounce restarts it") {
  both([](bool low) {
    Rig r(low);
    INFO("active low: " << low);
    r.arm();
    r.run(power::kHoldMs + 10, true);
    NEED(r.hold.state() == PowerHold::State::Committed);
    CHECK_FALSE(r.hold.released_stable());
    r.run(5000, true);  // held for ever: never
    CHECK_FALSE(r.hold.released_stable());
    r.run(99, false);
    CHECK_FALSE(r.hold.released_stable());
    r.sample(true);  // a bounce on the release
    r.run(99, false);
    CHECK_FALSE(r.hold.released_stable());
    r.run(2, false);
    CHECK(r.hold.released_stable());
    r.sample(true);
    CHECK_FALSE(r.hold.released_stable());
  });
}
