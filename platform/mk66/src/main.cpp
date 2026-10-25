// SPDX-License-Identifier: GPL-3.0-or-later
//
// The composition root of an app's image (docs/M2-PLAN.md, "The image"). The reset sequence (startup.cpp) has brought up the
// clocks, the interrupts, SysTick, the eDMA's global configuration and the SDRAM, tested the SDRAM, cleared `.sdram_bss` and run the
// static constructors. main then:
//
//   crash record of the last run -> display bring-up -> input init -> app::Runtime over the app (assets, App::setup, engine start)
//   -> audio start (muted) -> loop { wfi; input; display; rt.poll(); audio beat; the outputs opened once after a clean run; LED }
//
// The order and the LED rules follow track3r-os's composition root (src/app_main.cpp there), which ran on the unit; the tracker's
// console, card, USB and power-off are not part of M2. The only output before the display is the status LED (blink.h; the codes
// are in README.md). A dark LED after power-on means the image did not get as far as `early_pins`, not that it is idle.
#include "internal.h"
#include "o3sdk_build_info.h"  // generated: tools/gen_build_info.cmake

#include <o3sdk/app/app.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/cycles.h>
#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/blink.h>
#include <o3sdk/platform/mk66/crash_record.h>
#include <o3sdk/platform/mk66/display/device.h>
#include <o3sdk/platform/mk66/input/device.h>
#include <o3sdk/platform/mk66/output_gate.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/boot_screen.h>

#include <cstdint>

#ifndef O3SDK_AUDIO_TEST
// 1: the bring-up image of the audio path. The engine is not connected; the device plays audio_test_tone() (1 kHz at -20 dBFS,
// the left channel for about 6 s, then both), opened by the same gate as the app's sound.
#define O3SDK_AUDIO_TEST 0
#endif

namespace mk66 = o3sdk::platform::mk66;
namespace display = o3sdk::platform::mk66::display::device;
namespace input = o3sdk::platform::mk66::input::device;

namespace {

// What a crash record says the boot was doing (set_module): 0 is the main loop.
constexpr std::uint16_t kDisplayModule = 1;
constexpr std::uint16_t kInputModule = 2;
constexpr std::uint16_t kAppModule = 3;
constexpr std::uint16_t kAudioModule = 4;

constexpr std::uint32_t kAudioPollMs = 100;   // the status snapshot, the output gate and the verdict
constexpr std::uint32_t kAudioStallMs = 300;  // a boundary count standing still this long is a stopped block clock

// Seconds since boot, counted by the main loop; the loop is the only writer. Meant for a debugger.
volatile std::uint32_t g_heartbeat = 0;

// Which tree the image was built from ("o3sdk 0123456789ab-dirty"), for `strings` on a HEX and for a debugger.
const char* volatile const g_build_label = "o3sdk " O3SDK_BUILD_LABEL;

// The app's engine: 23 KB, in SRAM_U's .bss (the render reads it every block). Built by the static constructors.
o3sdk::dsp::Engine g_engine;

// boot::delay_ms()'s idle hook, once a millisecond of a blocking wait (the codec's start and unmute, the blink codes below): the
// display keeps its swap and backlight ramp going (idle() keeps out of the driver's own waits and of a display that is down), and the
// audio guard gets its beat, so a wait never counts as a starved loop (track3r-os bench.cpp does the same).
void idle_hook() noexcept {
  display::idle();
  mk66::audio_main_pass();
}

// Which blink pattern the audio counters call for: the heartbeat (0) while everything is clean, else a sticky code.
std::uint8_t audio_pattern(const o3sdk::platform::mk66::audio::StatusSnapshot& s) noexcept {
  if (s.fault_seen != 0u) {
    return mk66::blink::kAudioFault;
  }
  if (s.governor_over != 0u) {
    return mk66::blink::kAudioOverBudget;
  }
  return 0;
}

}  // namespace

int main() {
  (void)g_build_label;  // one volatile read keeps the label through the linker's garbage collection

  // 0: the heartbeat. A fault found at boot or while running (kAppFailure, kDisplayStalled, kAudioFailure) replaces it for good: the
  // image keeps running, the display is held dark, and the code is shown over and over so it cannot be taken for a quiet black screen.
  std::uint8_t fault_code = 0;
  if (mk66::boot_crash_record() != nullptr) {
    mk66::boot::play_code(mk66::blink::kCrashSeen, 2);
    mk66::clear_crash_record();
  }

  // ---- Display: SPI2 and its eDMA channels, the FT812 reset and brought up (bounded, retried), the DMA path self-tested, the
  // backlight off until the app has a frame on screen. A display that does not come up is a blink code and a headless run.
  mk66::set_module(kDisplayModule);
  const display::BringUpResult shown = display::bring_up();
  mk66::boot::set_idle_hook(&idle_hook);  // installed for good, from here

  // ---- Input: the two key scanners and the pad LED driver, on I2C0 with the codec (main thread only, for both).
  mk66::set_module(kInputModule);
  const bool input_ok = input::init();

  // ---- The app: the Runtime uploads the UI's bitmaps, runs App::setup and starts the engine.
  mk66::set_module(kAppModule);
  g_engine.set_cycle_counter(&o3sdk::hal::cycles);  // the header's CPU %
  static o3sdk::app::Platform platform{display::display(), input::input(), &input::leds(), mk66::system_clock(), true};
  static o3sdk::app::Runtime rt(o3sdk_app(), g_engine, platform);
  if (rt.start()) {
    // The first frame: the kit's boot screen (the logo the upload just put in RAM_G, the app's name and the build), on screen through
    // the blink codes and the audio start below, until the loop's first rt.poll() draws the app. Its swap starts the backlight ramp.
    (void)o3sdk::ui::show_boot_screen(display::display(), o3sdk_app().info().name, O3SDK_BUILD_LABEL);
  } else if (shown.up) {
    // The safe state for an app that cannot start (its bitmaps did not go up): the display is held dark, never a lit black screen
    // that looks like a quiet success. The app and its sound still run.
    display::hold_dark();
    fault_code = mk66::blink::kAppFailure;
  }
  if (shown.up && !shown.dma) {
    // Up, but the DMA write did not check out: the polled path carries it. Played after the boot screen is committed, so the
    // backlight's 2 s fallback never lights an undrawn screen; the idle hook keeps the display serviced while the code plays.
    mk66::boot::play_code(mk66::blink::kDisplayPolled, 1);
  } else if (!shown.up) {
    mk66::boot::play_code(mk66::blink::kDisplayFailure, 2);
  }
  if (!input_ok) {
    mk66::boot::play_code(mk66::blink::kInputDegraded, 1);  // the app runs with the scanner that answered
  }

  // ---- Audio: the codec brought up muted and the I2S/DMA path running; the render line calls the Runtime once per block. A
  // failure leaves the outputs muted and the image running, with the code latched for the LED.
  mk66::set_module(kAudioModule);
#if O3SDK_AUDIO_TEST
  const o3sdk::hal::audio::BlockCallback render = mk66::audio_test_tone();
#else
  const o3sdk::hal::audio::BlockCallback render = rt.block_callback();
#endif
  const bool audio_up = mk66::audio_device().start(render) == o3sdk::hal::audio::Device::Result::Ok;
  if (!audio_up) {
    fault_code = mk66::blink::kAudioFailure;  // mk66::audio_clock_fault() says which wait ran out, for a debugger
  }
  mk66::set_module(0);

  // ---- Watchdog re-enable point ----------------------------------------------------------------------------------
  // The reset sequence switched the watchdog off. This is where a later slice turns it back on, with the loop below as the place
  // that feeds it. Not done in M2 (as in track3r-os): the watchdog's clock source and timeout have not been checked on a unit, and a
  // wrong guess would be a reset loop on the very first boot.

  mk66::OutputGate output_gate;  // the outputs open after a clean run of the audio path
  mk66::OpenRetry open_retry;    // what a failed unmute costs: a backoff, at most three attempts
  bool opened = false;
  bool unmute_fault = false;     // kAudioFailure is this open's (an attempt that works takes it back)
  bool assets_failed = false;    // the bitmaps did not go back up after a re-initialisation: given up, held dark
  std::uint8_t pattern = 0;      // the audio verdict, sticky: a fault never un-happens
  const mk66::SysTickClock& clock = mk66::system_clock();
  std::uint32_t last_second = 0;
  std::uint32_t last_audio_poll = 0;
  std::uint32_t last_boundaries = 0;
  std::uint32_t last_progress_ms = static_cast<std::uint32_t>(clock.uptime_ms());

  // From here the loop beats once per pass (audio_main_pass), so a render that runs back to back and starves it sheds blocks until a
  // pass completes (audio/pipeline.h).
  mk66::audio_arm_starve_guard();
  for (;;) {
    asm volatile("wfi");  // SysTick wakes the core every millisecond; a DMA completion wakes it sooner

    input::service();  // the scanners' events into the queue rt.poll() reads
    switch (display::service()) {
      case display::Status::NeedsAssets:
        // The chip was re-initialised after a stalled swap: RAM_G is presumed lost, frames are refused until the bitmaps are back.
        // One attempt: a failed upload (each chunk may wait up to 70 ms) is not repeated on every pass, the display stays dark.
        if (!assets_failed) {
          if (o3sdk::ui::upload_assets(display::display()) == o3sdk::hal::UploadResult::Ok) {
            display::display().assets_loaded();
          } else {
            assets_failed = true;
            display::hold_dark();
            fault_code = mk66::blink::kAppFailure;
          }
        }
        break;
      case display::Status::Failed:
        fault_code = mk66::blink::kDisplayStalled;  // held dark by the driver, for good
        break;
      case display::Status::Ok:
      case display::Status::Down:
        break;
    }
    rt.poll();                // input -> app -> screen, the pad LEDs
    mk66::audio_main_pass();  // the beat: a whole pass has run

    const auto ms = static_cast<std::uint32_t>(clock.uptime_ms());
    if (audio_up && ms - last_audio_poll >= kAudioPollMs) {
      last_audio_poll = ms;
      const mk66::audio::StatusSnapshot snap = mk66::audio_status().snapshot();
      output_gate.observe(snap.boundaries, snap.underruns, snap.hw_faults);
      if (snap.boundaries != last_boundaries) {
        last_boundaries = snap.boundaries;
        last_progress_ms = ms;
      }
      // The block clock stopping (the DMA halted on an error, the clocks died) raises no interrupt and counts nothing: the boundary
      // count standing still is how it shows.
      const bool stalled = ms - last_progress_ms >= kAudioStallMs;
      const std::uint8_t verdict = stalled ? mk66::blink::kAudioFault : audio_pattern(snap);
      if (verdict != 0 && pattern != mk66::blink::kAudioFault) {
        pattern = verdict;  // a fault is final; "over budget" stays until a fault replaces it
      }
      // The outputs open once, after 400 clean blocks: the amplifier key's click is at power-on, never under a note.
      if (!opened && output_gate.ready(snap.boundaries) && open_retry.ready(false, ms)) {
        if (mk66::audio_device().unmute()) {  // blocks about 0.2 s, the display serviced by the idle hook
          opened = true;
          open_retry.succeeded();
          if (unmute_fault && fault_code == mk66::blink::kAudioFailure) {
            fault_code = 0;
          }
          unmute_fault = false;
        } else {
          open_retry.failed(ms);  // (a failure muted them again before it returned)
          unmute_fault = true;
          if (fault_code == 0) {
            fault_code = mk66::blink::kAudioFailure;
          }
        }
      }
    }

    // One LED for every part: a hard audio fault first, then a display, app or audio-start safe-state code, then the audio verdict
    // (kAudioOverBudget or the heartbeat). Each is sticky; the status words tell the rest.
    const std::uint8_t shown_code = pattern == mk66::blink::kAudioFault ? pattern : (fault_code != 0 ? fault_code : pattern);
    mk66::boot::led_from_sram_l(mk66::blink::led_on(shown_code, ms));
    if (ms / 1000u != last_second) {
      last_second = ms / 1000u;
      g_heartbeat = g_heartbeat + 1u;
    }
  }
}
