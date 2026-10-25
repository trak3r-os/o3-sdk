// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The safety net under the USB set (docs/M2B-PLAN.md section 6, mitigation e; decision D5): the console must not be lost to a set the host does not take. The image boots in the
// AUDIO set and `usb mode` can change it. When the device was started in, or asked for, a set with audio, the host has reset the bus (it is there and enumerating) and 10 seconds
// later it has still not configured the device, the guard says "go back to STANDARD", once. Pure: the glue (console::service) feeds it the time and what it reads from the driver,
// and does the request and the log line.
//
//   host_seen   the host has reset the bus since the set was attached. The driver zeroes its counters every time it attaches (UsbDevice::prepare), at the start and after each change
//               of set, so DeviceCounters::bus_resets counts from zero from the attach on: any reset in it is the host's answer to the set now presented, and the guard needs no
//               base value of its own (a base taken when the change was asked for goes stale at the attach: with one reset before the request and one after, "1 != 1" would hide the
//               host). While the change is on its way (usb0::change_phase() != 0) the counter still holds the old set's figure and the guard ignores it. Without a host the unit is not
//               stranded and the guard waits for one: a cable plugged in after the boot enumerates the boot set and is watched from its reset.
//   configured  the host selected the configuration (usb0::control().configured()). A reset puts the device back to the default state, so a `configured` seen after one is the host's
//               answer to the new set, never the lingering configuration of the old one.
//   changing    a change of set is in progress (usb0::change_phase() != 0): drain, detach, wait, attach.

#include <cstdint>

namespace o3sdk::platform::mk66::console {

class UsbModeGuard {
 public:
  static constexpr std::uint32_t kConfigureMs = 10'000;

  enum class Layout : std::uint8_t { Standard, Audio };  // the console-only set, and any set that carries USB audio
  enum class Action : std::uint8_t { None, RequestStandard };

  // The device was started in `layout`, or asked for it (the glue calls it when change_phase() != 0 after the request). Audio arms the guard (again: a new request starts it over);
  // Standard disarms it.
  void layout_set(Layout layout) noexcept {
    armed_ = layout == Layout::Audio;
    timing_ = false;
  }

  // Once per pass of the main loop, `now_ms` the uptime in milliseconds (wrap-safe), `bus_resets` the driver's count of resets since its last attach. RequestStandard comes once;
  // the guard is disarmed by then (the glue's request is a Standard layout_set).
  Action tick(std::uint32_t now_ms, bool configured, std::uint32_t bus_resets, bool changing) noexcept {
    if (!armed_ || changing || bus_resets == 0u) {
      return Action::None;
    }
    if (configured) {
      armed_ = false;  // the host took the set
      ++settled_;
      return Action::None;
    }
    if (!timing_) {
      timing_ = true;  // the host is there and has not configured: its 10 seconds start here
      since_ = now_ms;
      return Action::None;
    }
    if (static_cast<std::uint32_t>(now_ms - since_) < kConfigureMs) {
      return Action::None;
    }
    armed_ = false;
    ++fallbacks_;
    return Action::RequestStandard;
  }

  bool armed() const noexcept { return armed_; }
  std::uint32_t fallbacks() const noexcept { return fallbacks_; }  // times it asked for STANDARD
  std::uint32_t settled() const noexcept { return settled_; }      // times the host configured in time

 private:
  bool armed_ = false;
  bool timing_ = false;
  std::uint32_t since_ = 0;
  std::uint32_t fallbacks_ = 0;
  std::uint32_t settled_ = 0;
};

}  // namespace o3sdk::platform::mk66::console
