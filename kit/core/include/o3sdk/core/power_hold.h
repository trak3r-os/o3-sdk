// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER (docs/specs/OS-POWER.md section 3): the hold of the Power switch as a pure object. The platform reads the pin once per pass of the main loop and calls update() with the
// millisecond clock; the simulator drives the same object from its scenario commands. No clock of its own, no heap, no I/O.
//
//   Booting    the switch is not read as a hold until the app has the screen (`ready`) and the level has been released for kArmReleaseMs. A Power held through the boot never counts, and a
//              switch that always reads "pressed" (a wrong polarity constant) never arms.
//   Armed      a debounced press (kDebounceMs, both edges) starts the hold; Shift down at that moment is Spent and reports Result::screenshot once (D5; OS-SCREENSHOT: the platform
//              takes the picture; the hold shows nothing).
//   Holding    the line fills; Shift pressed: Spent. A debounced release goes back to Armed (a tap of kHintMinMs or more shows the hint). kHoldMs of hold: Committed, once.
//   Spent      waits for the release, then Armed.
//   Committed  the one-way state (D2): a release does not cancel. The owner of the shutdown (platform/mk66/power) may refuse() it, which makes it Spent again.
//
// The sample gap. A sample more than kMaxGapMs after the previous one is a new start, not a continuation (the main loop blocks for a card mount or a codec unmute): the debounce starts
// again from that sample, Holding goes back to Armed (the press is seen again and needs its debounce and the whole kHoldMs), the release run of Booting starts again. Two low samples
// seconds apart never add up to one hold.
//
// Polarity is the one board unknown (M4): the constructor takes `active_low`, and update() takes the pin level, so a host test runs the whole table for both.

#include <cstdint>

namespace o3sdk::core {

namespace power {
inline constexpr std::uint32_t kHoldMs = 2000;          // maintainer: the line reaches its end at 2 s
inline constexpr std::uint32_t kDebounceMs = 30;        // both edges, by the millisecond clock, not by passes
inline constexpr std::uint32_t kArmReleaseMs = 100;     // Booting: released this long before the first hold can count
inline constexpr std::uint32_t kMaxGapMs = 250;         // a sample later than this after the one before is a new start
inline constexpr std::uint32_t kShowAfterMs = 150;      // the line shows from this much hold (a brush leaves no flicker)
inline constexpr std::uint32_t kHintMinMs = 150;        // a tap of at least this long (and short of kHoldMs) shows the hint
inline constexpr std::uint32_t kHintShowMs = 1500;      // ... for this long
inline constexpr std::uint32_t kStuckLogMs = 5000;      // Booting with the switch down this long: one log line
inline constexpr std::uint32_t kReleaseStableMs = 100;  // the stable release the shutdown's stage F waits for
}  // namespace power

// What the strip says (the app owns the words, app/power_line.h). Hold and Hint come from PowerHold, the rest from the shutdown (platform/mk66/power/power_service.h).
enum class PowerText : std::uint8_t {
  None,
  Hold,           // HOLD TO SWITCH OFF (UNSAVED WORK - LET GO TO KEEP IT when the app has unsaved work)
  Hint,           // HOLD POWER 2 S TO SWITCH OFF
  Firmware,       // FIRMWARE UPDATE RUNNING
  Export,         // EXPORT RUNNING - HOLD POWER AGAIN TO STOP IT
  Saving,         // SAVING... THEN POWER OFF
  SaveCancelled,  // SAVE CANCELLED - POWERING OFF
  PoweringOff,    // POWERING OFF
  DiskWriting,    // COMPUTER MAY BE WRITING - HOLD AGAIN
};

// The line and the strip, as the app is told them: the fill in thousandths (0 = no line) and the text.
struct PowerShow {
  std::uint16_t fill = 0;
  PowerText text = PowerText::None;
  constexpr bool shown() const noexcept { return fill != 0u || text != PowerText::None; }
  constexpr bool operator==(const PowerShow& o) const noexcept { return fill == o.fill && text == o.text; }
  constexpr bool operator!=(const PowerShow& o) const noexcept { return !(*this == o); }
};

class PowerHold {
 public:
  enum class State : std::uint8_t { Booting, Armed, Holding, Spent, Committed };

  struct Result {
    State state = State::Booting;
    PowerShow show;      // the hold's own line and text (Hold while it fills, Hint after a tap)
    bool commit = false; // true on exactly one call: the hold reached its end
    bool stuck = false;  // true on exactly one call: the switch has been down kStuckLogMs since boot
    bool screenshot = false;  // true on exactly one call: Power went down (debounced) with Shift already down (OS-SCREENSHOT); no hold starts
  };

  // `active_low`: the pin reads LOW while the switch is pressed (the default assumption, M4 settles it on the unit).
  explicit constexpr PowerHold(bool active_low = true) noexcept : active_low_(active_low) {}

  // One sample. `pin_high` is the raw level of the pin; `shift_down` the Shift button; `ready` the app has the screen (the boot gate has let go, no load phase).
  Result update(std::uint32_t now_ms, bool pin_high, bool shift_down, bool ready) noexcept {
    const bool raw = pin_high != active_low_;
    const bool gap = !have_ || now_ms - last_ms_ > power::kMaxGapMs;
    have_ = true;
    last_ms_ = now_ms;
    if (gap || raw != run_pressed_) {
      run_start_ = now_ms;
      run_pressed_ = raw;
    }
    Result r;
    if (gap) {
      cand_ = raw;
      cand_since_ = now_ms;
      if (state_ == State::Holding) {
        state_ = State::Armed;  // the press is seen again, with its debounce and the whole hold
        deb_ = false;
      }
    } else if (raw != deb_) {
      if (cand_ != raw) {
        cand_ = raw;
        cand_since_ = now_ms;
      }
      if (now_ms - cand_since_ >= power::kDebounceMs) {
        deb_ = raw;
        if (raw) {
          on_press(shift_down);
        } else {
          on_release(now_ms);
        }
      }
    } else {
      cand_ = raw;
      cand_since_ = now_ms;
    }
    r.screenshot = shot_pending_;
    shot_pending_ = false;
    if (state_ == State::Spent && !deb_) {
      state_ = State::Armed;  // the release the cancel waited for (or a refusal after a release that was already seen)
    }
    switch (state_) {
      case State::Booting:
        if (ready && !run_pressed_ && now_ms - run_start_ >= power::kArmReleaseMs && !deb_) {
          state_ = State::Armed;
        } else if (run_pressed_ && now_ms - run_start_ >= power::kStuckLogMs && !stuck_logged_) {
          stuck_logged_ = true;
          r.stuck = true;
        }
        break;
      case State::Holding:
        if (shift_down) {
          state_ = State::Spent;  // D5: Shift during a hold cancels it until Power is released
        } else if (now_ms - press_at_ >= power::kHoldMs) {
          state_ = State::Committed;
          ++commits_;
          r.commit = true;
        }
        break;
      default:
        break;
    }
    r.state = state_;
    r.show = show_for(now_ms);
    return r;
  }

  // The shutdown refused the commit (a firmware update, a running export): the line is spent until Power is released, then the switch is armed again.
  void refuse() noexcept {
    if (state_ == State::Committed) {
      state_ = State::Spent;
      ++refusals_;
    }
  }

  State state() const noexcept { return state_; }
  bool pressed() const noexcept { return deb_; }  // the debounced level
  // The raw level has been released for kReleaseStableMs, by samples no more than kMaxGapMs apart, as of the last sample (the shutdown's stage F0). A bounce restarts the count.
  bool released_stable() const noexcept { return have_ && !run_pressed_ && last_ms_ - run_start_ >= power::kReleaseStableMs; }
  // For `diag power`.
  std::uint32_t presses() const noexcept { return presses_; }  // (16 bits each: a diagnostic, not a count that matters at 65,536)
  std::uint32_t holds() const noexcept { return holds_; }
  std::uint32_t taps() const noexcept { return taps_; }
  std::uint32_t commits() const noexcept { return commits_; }
  std::uint32_t refusals() const noexcept { return refusals_; }
  std::uint32_t screenshots() const noexcept { return shots_; }
  bool active_low() const noexcept { return active_low_; }

 private:
  void on_press(bool shift_down) noexcept {
    ++presses_;
    press_at_ = cand_since_;  // the physical press: the first sample of the run that was debounced
    hint_on_ = false;
    if (state_ == State::Armed) {
      if (shift_down) {
        state_ = State::Spent;  // D5: nothing is shown; the one-shot event is the screenshot's (OS-SCREENSHOT)
        shot_pending_ = true;
        ++shots_;
      } else {
        state_ = State::Holding;
        ++holds_;
      }
    }
  }

  void on_release(std::uint32_t now_ms) noexcept {
    if (state_ == State::Holding) {
      state_ = State::Armed;
      const std::uint32_t held = cand_since_ - press_at_;
      if (held >= power::kHintMinMs && held < power::kHoldMs) {
        ++taps_;
        hint_on_ = true;
        hint_until_ = now_ms + power::kHintShowMs;
      }
    }
  }

  PowerShow show_for(std::uint32_t now_ms) noexcept {
    PowerShow s;
    if (state_ == State::Holding) {
      const std::uint32_t held = now_ms - press_at_;
      if (held >= power::kShowAfterMs) {
        s.fill = static_cast<std::uint16_t>(held >= power::kHoldMs ? 1000u : held * 1000u / power::kHoldMs);
        s.text = PowerText::Hold;
      }
    } else if (state_ == State::Committed) {
      s.fill = 1000;
      s.text = PowerText::Hold;
    } else if (hint_on_) {
      if (static_cast<std::int32_t>(hint_until_ - now_ms) > 0) {
        s.text = PowerText::Hint;
      } else {
        hint_on_ = false;
      }
    }
    return s;
  }

  bool active_low_;
  State state_ = State::Booting;
  bool have_ = false;
  std::uint32_t last_ms_ = 0;
  bool run_pressed_ = false;        // the run of equal raw samples: its level and when it began
  std::uint32_t run_start_ = 0;
  bool deb_ = false;                // the debounced level (pressed)
  bool cand_ = false;               // the level that is trying to become the debounced one, and since when
  std::uint32_t cand_since_ = 0;
  std::uint32_t press_at_ = 0;
  bool hint_on_ = false;
  std::uint32_t hint_until_ = 0;
  bool stuck_logged_ = false;
  bool shot_pending_ = false;       // on_press saw Shift down: reported by the update() that debounced the press
  std::uint16_t shots_ = 0;
  std::uint16_t presses_ = 0;
  std::uint16_t holds_ = 0;
  std::uint16_t taps_ = 0;
  std::uint16_t commits_ = 0;
  std::uint16_t refusals_ = 0;
};

}  // namespace o3sdk::core
