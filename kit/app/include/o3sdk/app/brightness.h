// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The device settings DISPLAY and PAD LIGHTS (Low, Medium, High; model/params.cpp kBrightness) as what the hardware is told (OS-CFGWIRE). The platform reads the settings and applies these; the
// simulator has neither a backlight nor LEDs and ignores them.
//
//   DISPLAY     the FT812's REG_PWM_DUTY (0..128): this OS sends 20, 50 and 128.
//   PAD LIGHTS  this OS scales every LED level by (setting + 1) / 3: Low a third, Medium two thirds, High as it is. A lit pad never drops below kPadLevelFloor, the driver's first
//               visible step (hal/leds.h: perceptual level, gamma 2: level 16 is PWM 1), so Low dims a pad and does not put it out.

#include <cstdint>

namespace o3sdk::app {

inline constexpr unsigned kBrightnessLevels = 3;
inline constexpr std::uint8_t kBacklightDuties[kBrightnessLevels] = {20, 50, 128};
inline constexpr std::uint8_t kPadLevelFloor = 16;

// A setting value (0..2; anything else is High) as a number 0..2.
constexpr unsigned brightness_index(float setting) noexcept { return setting >= 2.0f ? 2u : (setting >= 1.0f ? 1u : 0u); }

constexpr std::uint8_t backlight_duty(unsigned index) noexcept { return kBacklightDuties[index < kBrightnessLevels ? index : kBrightnessLevels - 1u]; }

// A pad LED level (0 stays 0) at the setting `index`: never above the level itself, so High changes nothing.
constexpr std::uint8_t scale_pad_level(std::uint8_t level, unsigned index) noexcept {
  const unsigned i = index < kBrightnessLevels ? index : kBrightnessLevels - 1u;
  const unsigned scaled = static_cast<unsigned>(level) * (i + 1u) / kBrightnessLevels;
  const unsigned floored = scaled < kPadLevelFloor ? kPadLevelFloor : scaled;
  return static_cast<std::uint8_t>(floored < level ? floored : level);
}

static_assert(scale_pad_level(255, 2) == 255 && scale_pad_level(255, 0) == 85 && scale_pad_level(0, 0) == 0 && scale_pad_level(40, 0) == kPadLevelFloor && scale_pad_level(5, 0) == 5 && scale_pad_level(200, 2) == 200,
              "thirds of the level, with a floor that never raises a level");

}  // namespace o3sdk::app
