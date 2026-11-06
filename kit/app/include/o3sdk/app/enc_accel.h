// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-AUDIT-28 (E-GLOB-11, E-LOAD-09): encoder acceleration in LISTS only. The scanner reports detents (several in one report when they arrive between two passes); a list moved a row
// a detent. A fast spin now moves more rows a detent: a screen that wants it asks Context::enc_scale() (the kit's accessor over this) for its lists, and for no other
// context (a value being edited, the pattern's cursor and every Shift + encoder coarse step are untouched).
//
// The curve: a RUN is the detents that follow each other with less than `gap_ms` between two reports in one direction. A report moves its detents by the multiplier of the run
// BEFORE it: x1 until the run has `t1` detents, x4 from `t1`, x8 from `t2`. So a lone report (one click, a batch from the scanner, the console's `enc 20`) is always x1, a slow turn
// (a report every >= gap_ms) is always x1, and a spin gets faster as it goes on. The decision is from the detent counts and the main loop's time alone: no timer, no ISR, no heap;
// the time is injected (Context::now), so a host test is deterministic. The state is the main loop's only (a screen's input() runs there).
//
// The numbers are runtime-tunable from the console (`enc accel [off | <t1> <t2> [gap_ms]]`, the ENCODER row of Config picks OFF, NORMAL or FAST and is saved; the console numbers are not saved) and are to be tuned on the unit.

#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::app {

struct EncAccel {
  static constexpr int kMult1 = 4;  // rows a detent from t1 detents of a run
  static constexpr int kMult2 = 8;  // rows a detent from t2 detents of a run
  static constexpr int kMaxReport = 1 << 15;
  static constexpr std::uint8_t kDefaultT1 = 6;
  static constexpr std::uint8_t kDefaultT2 = 16;
  static constexpr std::uint8_t kDefaultGapMs = 60;
  static constexpr std::uint8_t kMinGapMs = 5;
  static constexpr std::uint8_t kMaxGapMs = 250;

  struct Config {
    bool on = true;  // false: a row a detent, as before
    std::uint8_t t1 = kDefaultT1;
    std::uint8_t t2 = kDefaultT2;
    std::uint8_t gap_ms = kDefaultGapMs;
  };
  // The console's check: 1 <= t1 <= t2, gap_ms in kMinGapMs..kMaxGapMs.
  static constexpr bool valid(const Config& c) noexcept {
    return c.t1 >= 1u && c.t1 <= c.t2 && c.gap_ms >= kMinGapMs && c.gap_ms <= kMaxGapMs;
  }

  Config cfg;

  void reset() noexcept {
    run_ = 0;
    dir_ = 0;
  }

  // The rows to move for a report of `detents` (signed) at `now`: detents x the multiplier of the run so far. 0 stays 0.
  int scale(core::Tick now, int detents) noexcept {
    if (detents == 0) {
      return 0;
    }
    detents = detents < -kMaxReport ? -kMaxReport : (detents > kMaxReport ? kMaxReport : detents);  // the input is an int16; this keeps a value fed from data in range
    const int dir = detents < 0 ? -1 : 1;
    if (!cfg.on || dir != dir_ || core::tick_elapsed(now, last_) > core::ticks_from_ms(cfg.gap_ms)) {
      run_ = 0;  // off, a reversal or a pause: a new run
    }
    dir_ = static_cast<std::int8_t>(dir);
    last_ = now;
    const int mult = !cfg.on ? 1 : (run_ >= cfg.t2 ? kMult2 : (run_ >= cfg.t1 ? kMult1 : 1));
    const int n = detents * dir;  // |detents|
    run_ = n > 0xFFFF - run_ ? std::uint16_t{0xFFFF} : static_cast<std::uint16_t>(run_ + n);
    return detents * mult;
  }

 private:
  core::Tick last_ = 0;
  std::uint16_t run_ = 0;  // the detents of the run so far (saturating)
  std::int8_t dir_ = 0;
};

}  // namespace o3sdk::app
