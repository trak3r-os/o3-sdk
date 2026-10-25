// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::core {

// How long the boot screen stays up (OS-003f, docs/specs/OS-003-ui-toolkit.md): it is the first frame the display shows, the
// backlight comes up behind it, and the first screen of the app replaces it only when it has been lit for kBootMinMs. Boot work
// that outlasts that (the audio bring-up takes about 0.6 s) is not delayed by it: when the main loop starts and the minimum has
// passed, the app draws at once. Pure timekeeping on the 64-bit uptime in milliseconds (it never wraps), driven by the platform.
//
//   hold.start(now);                          // the boot frame is committed
//   each pass: if (backlight is on) hold.lit(now);  if (hold.holding(now)) skip the app's frame; else hold.release();
//
// The minimum counts from the first light, not from the commit: the backlight ramps in over about 130 ms, which would otherwise
// be taken out of the 800 ms. A display that never lights (held dark, or a ramp that never starts) does not hold the app for
// ever: the hold ends kBootLightGraceMs after the minimum would have, counted from the commit. Main thread only.
inline constexpr std::uint32_t kBootMinMs = 800;
inline constexpr std::uint32_t kBootLightGraceMs = 2500;

class BootHold {
 public:
  // The boot frame was committed at `now_ms`. Starts a hold, forgetting any earlier one.
  void start(std::uint64_t now_ms) noexcept {
    active_ = true;
    lit_ = false;
    shown_ms_ = now_ms;
    lit_ms_ = 0;
  }

  // The backlight is seen on at `now_ms`. Only the first call of a hold counts.
  void lit(std::uint64_t now_ms) noexcept {
    if (active_ && !lit_) {
      lit_ = true;
      lit_ms_ = now_ms;
    }
  }

  // True while the boot screen must stay: from start() until kBootMinMs after the first light, or, if the light never comes,
  // until kBootLightGraceMs after kBootMinMs past the commit. False when there is no hold (never started, or released).
  bool holding(std::uint64_t now_ms) const noexcept {
    if (!active_) {
      return false;
    }
    const std::uint64_t deadline = lit_ ? lit_ms_ + kBootMinMs : shown_ms_ + kBootMinMs + kBootLightGraceMs;
    return now_ms < deadline;
  }

  // The app takes the screen: the hold is over for good (until the next start()).
  void release() noexcept { active_ = false; }
  bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
  bool lit_ = false;
  std::uint64_t shown_ms_ = 0;
  std::uint64_t lit_ms_ = 0;
};

}  // namespace o3sdk::core
