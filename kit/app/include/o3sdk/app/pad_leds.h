// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pads' LEDs as one frame of 48 brightness levels. A screen writes a whole frame every UI frame (Screen::leds) from its state,
// so the LEDs never drift from what the screen shows; the runtime sends only what changed (hal/leds.h). Monochrome: a level is
// all there is, so meaning comes from a few distinct levels and from blinking.

#include <o3sdk/app/panel.h>
#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::app {

// The levels apps share, so that "dim" means the same on every screen (perceptual: the driver applies the gamma curve).
inline constexpr std::uint8_t kLedOff = 0;
inline constexpr std::uint8_t kLedDim = 24;    // there is something here (a note, a loaded slot, a white key)
inline constexpr std::uint8_t kLedLow = 70;    // a second kind of something (an accent, a selected page)
inline constexpr std::uint8_t kLedMid = 140;   // selected
inline constexpr std::uint8_t kLedOn = 255;    // now: the playhead, a held pad, a playing slot

class PadLeds {
 public:
  void clear() noexcept {
    for (auto& l : level_) {
      l = kLedOff;
    }
  }
  void set(unsigned pad, std::uint8_t level) noexcept {
    if (pad < kPadCount) {
      level_[pad] = level;
    }
  }
  void set(unsigned row, unsigned col, std::uint8_t level) noexcept {
    if (row < kPadRows && col < kPadCols) {
      level_[pad_at(row, col)] = level;
    }
  }
  // Raises a pad to at least `level` (a held pad on top of what the layer drew).
  void raise(unsigned pad, std::uint8_t level) noexcept {
    if (pad < kPadCount && level_[pad] < level) {
      level_[pad] = level;
    }
  }
  std::uint8_t get(unsigned pad) const noexcept { return pad < kPadCount ? level_[pad] : kLedOff; }
  const std::uint8_t* levels() const noexcept { return level_; }

  // The phase of a blink of `period_ms` at `now`: true in the first half of each period.
  static bool blink(core::Tick now, std::uint32_t period_ms) noexcept {
    const std::uint32_t ms = now / core::kTicksPerMs;
    return period_ms == 0u || (ms % period_ms) < period_ms / 2u;
  }

 private:
  std::uint8_t level_[kPadCount] = {};
};

}  // namespace o3sdk::app
