// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER P1: what `diag power` prints (docs/specs/OS-POWER.md section 3 and QA-GLB-113), as a pure function of the numbers, so the host tests read it and the image's command only fills the struct.
// Several lines, at most 1.2 KB; every line starts with `power:`. The M4 measurement is the first two: the raw level of PTC3 with no pull, the pull-up and the pull-down, idle and pressed, which
// settles the one constant kPowerActiveLow and the pull (board.h).

#include <o3sdk/core/power_hold.h>
#include <o3sdk/platform/mk66/power/power_off.h>
#include <o3sdk/platform/mk66/power/power_record.h>
#include <o3sdk/platform/mk66/power/power_service.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::power {

enum class Inject : std::uint8_t { Real, Pressed, Released };  // `diag power press|release|pin`: what feeds the hold in place of the pin

struct PowerReport {
  bool active_low = true;          // board::kPowerActiveLow
  std::uint8_t pull = 0;           // board::kPowerPull: 0 none, 1 up, 2 down
  bool level_high = false;         // the raw level the hold is fed now (the pin with the configured pull, or the injected one)
  bool read_high[3] = {};          // the pin read just now with no pull, with the pull-up and with the pull-down
  Inject inject = Inject::Real;
  core::PowerHold::State state = core::PowerHold::State::Booting;
  bool pressed = false;            // the debounced level
  std::uint16_t fill = 0;          // the line, thousandths
  std::uint32_t presses = 0;
  std::uint32_t holds = 0;
  std::uint32_t taps = 0;
  std::uint32_t commits = 0;
  std::uint32_t refusals = 0;
  Stage stage = Stage::Idle;
  bool dry = false;
  bool deadline_hit = false;
  bool save_cancelled = false;
  bool rec_valid = false;            // OS-POWER P2: the record of the last shutdown (power_record.h) is whole; false: none since the power-up (or garbage)
  ShutdownRecord rec = {};
};

// Writes the report into out[0..cap) (NUL terminated, cut to fit); returns the length.
std::size_t format_power_report(const PowerReport& r, char* out, std::size_t cap) noexcept;

}  // namespace o3sdk::platform::mk66::power
