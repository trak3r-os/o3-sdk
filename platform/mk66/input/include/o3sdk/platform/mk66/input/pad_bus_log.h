// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-BUG-PADS: how much of the pad bus's story goes to `diag log`. The log is a ring of 1024 records for the whole boot: a pad scanner that flaps
// (online, offline, online, about once a second: the very case under debug) must not push the boot lines out of it. So the log gets
//
//   * a full entry (the headline, the chips' results, the bus clears and the levels) at the first result of each chip, and for the first
//     kFullFlips changes of the pad scanner's online state;
//   * after those, one summary line (the number of changes so far and the state now) per kSummaryEvery changes, and nothing in between.
//
// At the scanner's retry rate (a second; at worst a few changes a second after a loss) that is about one line a minute at the most. Pure logic,
// host-tested; the main loop owns the instance and does the logging (platform/mk66/src/app_main.cpp, watch_pad_bus).

#include <cstdint>

namespace o3sdk::platform::mk66::input {

class PadBusLogPolicy {
 public:
  static constexpr std::uint32_t kFullFlips = 4;
  static constexpr std::uint32_t kSummaryEvery = 64;

  enum class Entry : std::uint8_t {
    None,     // nothing to write
    Full,     // the headline and the bus record (take the levels now)
    Summary,  // one line: flips() changes, state online() now
  };

  // One look at the pad bus (every pass is fine: it reads nothing). `online`: the pad scanner answers; `scanner_tried` / `leds_tried`: the chip has had
  // its first transaction (answered or refused).
  Entry update(bool online, bool scanner_tried, bool leds_tried) noexcept {
    const bool first_result = (scanner_tried && !scanner_tried_) || (leds_tried && !leds_tried_);
    scanner_tried_ = scanner_tried_ || scanner_tried;
    leds_tried_ = leds_tried_ || leds_tried;
    const bool flipped = online != online_;
    online_ = online;
    if (flipped) {
      ++flips_;
    }
    if (first_result) {
      return Entry::Full;
    }
    if (!flipped) {
      return Entry::None;
    }
    if (flips_ <= kFullFlips) {
      return Entry::Full;
    }
    return flips_ % kSummaryEvery == 0u ? Entry::Summary : Entry::None;
  }

  std::uint32_t flips() const noexcept { return flips_; }  // changes of the online state so far (the first answer counts as one)
  bool online() const noexcept { return online_; }

 private:
  std::uint32_t flips_ = 0;
  bool online_ = false;
  bool scanner_tried_ = false;
  bool leds_tried_ = false;
};

}  // namespace o3sdk::platform::mk66::input
