// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::hal {

// The backlight of the pads (docs/design/product-ux.md section 3: "per-pad PWM LED"). One LED a pad, numbered like the pad
// events (InputKind::Pad ids 0..47). The LEDs are MONOCHROME: the board has one PWM channel a pad (an IS31FL3731 matrix
// driver), so there is a brightness and no colour. The front-panel buttons have no LED at all (none is wired). A platform whose pads had colour would add a second contract; the app writes brightness only.
inline constexpr unsigned kPadLedCount = 48;

// Frame-based, main loop only, never blocking: the app stages a brightness per LED with set() and flush() sends what changed
// since the last flush. flush() does at most one bounded bus transaction per call, so a whole frame goes out over a few
// calls (the caller flushes on every pass of its loop); it says when nothing is left to send.
class Leds {
 public:
  virtual unsigned count() const noexcept = 0;
  // Stages `level` (0 = off, 255 = the brightest the board allows; perceptual: the driver applies the gamma curve) for LED
  // `index`. An index at or past count() is ignored. Nothing is sent until flush().
  virtual void set(unsigned index, std::uint8_t level) noexcept = 0;
  // Stages every LED off.
  virtual void clear() noexcept = 0;
  // Sends the next piece of what changed. true: nothing is left to send (the chip shows what was staged, or the chip is
  // unreachable and the driver is waiting to retry); false: call again.
  virtual bool flush() noexcept = 0;
  // The chip answered its last transaction.
  virtual bool online() const noexcept = 0;

 protected:
  Leds() = default;
  ~Leds() = default;  // never deleted through the interface: no heap
  Leds(const Leds&) = delete;
  Leds& operator=(const Leds&) = delete;
};

}  // namespace o3sdk::hal
