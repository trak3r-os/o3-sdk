// SPDX-License-Identifier: GPL-3.0-or-later
//
// The skeleton image's main (O3SDK_MK66_SKELETON, docs/M2-PLAN.md): the reset sequence (startup.cpp) has brought up the clocks, the
// interrupts, SysTick and the SDRAM, tested the SDRAM and cleared `.sdram_bss`; main reports what the last run left behind on the
// status LED, clears it, then idles in WFI with a heartbeat. No driver is linked: it proves the reset sequence and the image pipeline
// alone. The real image's main is main.cpp, the composition root.
//
// Its only output is the LED (blink.h; the codes are in README.md). A dark LED after power-on means the image did not get as far
// as `early_pins`, not that it is idle.
#include "internal.h"
#include "o3sdk_build_info.h"  // generated: tools/gen_build_info.cmake

#include <o3sdk/app/app.h>
#include <o3sdk/platform/mk66/blink.h>
#include <o3sdk/platform/mk66/crash_record.h>
#include <o3sdk/platform/mk66/platform.h>

#include <cstdint>

namespace mk66 = o3sdk::platform::mk66;

namespace {

// Seconds since boot, counted by the main loop; the loop is the only writer. Meant for a debugger.
volatile std::uint32_t g_heartbeat = 0;

// The app's entry point, referenced and never called: the app library and everything it reaches (its units, the kit's dsp and
// app layers, its O3SDK_BIG objects) are linked and placed as the real image will be, so the link, the memory map and the image
// checks see the app, while this main runs none of it.
o3sdk::app::App& (*volatile const g_app_entry)() noexcept = &o3sdk_app;

// Which tree the image was built from ("o3sdk 0123456789ab-dirty"), for `strings` on a HEX and for a debugger.
const char* volatile const g_build_label = "o3sdk " O3SDK_BUILD_LABEL;

}  // namespace

int main() {
  // One volatile read each keeps the two objects above (and what they point at) through the linker's garbage collection.
  (void)g_app_entry;
  (void)g_build_label;

  if (mk66::boot_crash_record() != nullptr) {
    mk66::boot::play_code(mk66::blink::kCrashSeen, 2);
    mk66::clear_crash_record();
  }

  std::uint32_t last_second = 0;
  for (;;) {
    asm volatile("wfi");  // SysTick wakes the core every millisecond
    const auto ms = static_cast<std::uint32_t>(mk66::system_clock().uptime_ms());
    mk66::boot::led_from_sram_l(mk66::blink::led_on(0, ms));
    if (ms / 1000u != last_second) {
      last_second = ms / 1000u;
      g_heartbeat = g_heartbeat + 1u;
    }
  }
}
