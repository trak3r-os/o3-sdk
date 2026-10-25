// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The idle hook of boot::delay_ms() (OS-003f), as a pure object so that its rule is tested on the host (mk66_idle_hook_test.cpp): a function that
// the busy-wait calls once for every millisecond it waits, so that the display keeps being serviced while a long blocking bring-up
// (today: the audio start, about 0.6 s of codec waits) runs. Main thread only, never from an interrupt.
//
// The rule: the hook is never entered from itself. A hook that services the display may find the driver re-initialising the chip,
// which waits with boot::delay_ms(), which would call the hook again in the middle of the init; run() does nothing then.
// (Whoever installs a hook that services a driver also keeps the hook out while the main loop is inside that driver: in track3r-os
// that was the composition root's flag, boot_gate.h, which the kit's composition root replaces.)

namespace o3sdk::platform::mk66 {

class IdleHook {
 public:
  using Function = void (*)() noexcept;

  // nullptr removes the hook (the default, and what every wait before the display exists sees).
  void set(Function hook) noexcept { hook_ = hook; }
  bool installed() const noexcept { return hook_ != nullptr; }

  // Calls the hook unless there is none or it is already running (called from inside itself).
  void run() noexcept {
    if (hook_ != nullptr && !running_) {
      running_ = true;
      hook_();
      running_ = false;
    }
  }

 private:
  Function hook_ = nullptr;
  bool running_ = false;
};

}  // namespace o3sdk::platform::mk66
