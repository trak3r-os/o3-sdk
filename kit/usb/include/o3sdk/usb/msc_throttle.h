// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U2a (docs/specs/OS-USBDISK.md section 8, decision Q11): the load throttle of the MSC pump. The USB interrupt preempts the render, so the transfer must never take more of a busy render's
// time than it has to: above kOnPercent of the render's block budget (a smoothed figure, so one long block does not flip it) the pump runs at most once every kGapMs, which halves the rate of the
// transfer and the work of the card I/O between two UI frames; it is released below kOffPercent (hysteresis). The threshold is the spec's default and is to be set from the unit's paired
// measurement (F8). Pure logic, main loop only, no clock of its own, no allocation.

#include <cstdint>

namespace o3sdk::usb {

class MscThrottle {
 public:
  static constexpr unsigned kOnPercent = 85;   // above this the pump is held to every kGapMs
  static constexpr unsigned kOffPercent = 80;  // below this it runs every pass again
  static constexpr std::uint32_t kGapMs = 2;
  static constexpr unsigned kMaxPercent = 200;  // a figure past it (a block that ran over) counts as this

  // The render's load in percent of the block budget, once a pass. An average over about eight passes (fixed point, x16).
  void feed(unsigned load_percent) noexcept {
    const unsigned x = (load_percent > kMaxPercent ? kMaxPercent : load_percent) * 16u;
    avg16_ = avg16_ - (avg16_ >> 3) + (x >> 3);
    if (avg16_ > kOnPercent * 16u) {
      throttled_ = true;
    } else if (avg16_ < kOffPercent * 16u) {
      throttled_ = false;
    }
  }
  // Whether the pump may run in this pass. Not throttled: always. Throttled: once per kGapMs.
  bool allow(std::uint32_t now_ms) noexcept {
    if (!throttled_) {
      return true;
    }
    if (now_ms - last_ms_ < kGapMs) {
      return false;
    }
    last_ms_ = now_ms;
    return true;
  }
  bool throttled() const noexcept { return throttled_; }
  unsigned load_percent() const noexcept { return avg16_ / 16u; }

 private:
  unsigned avg16_ = 0;
  bool throttled_ = false;
  std::uint32_t last_ms_ = 0;
};

}  // namespace o3sdk::usb
