// SPDX-License-Identifier: GPL-3.0-or-later
//
// The composition root of an app's image (docs/M2-PLAN.md, "The image"). The reset sequence (startup.cpp) has brought up the
// clocks, the interrupts, SysTick, the eDMA's global configuration and the SDRAM, tested the SDRAM, cleared `.sdram_bss` and run the
// static constructors. main then:
//
//   crash record of the last run -> display bring-up -> input init -> app::Runtime over the app (assets, App::setup, engine start)
//   -> audio start (muted) -> loop { wfi; input; display; rt.poll(); audio beat; the outputs opened once after a clean run; LED }
//
// The order and the LED rules follow track3r-os's composition root (src/app_main.cpp there), which ran on the unit. With the console
// (O3SDK_CONSOLE, the default image: docs/M2B-PLAN.md section 5) the same sequence also brings up, in this order:
//
//   log -> crash record (kept for `crash`) -> ... -> audio start with the USB audio tap around the render -> SD card (init, mount,
//   watch) -> console::attach(rt) -> USB (the AUDIO set of track3r-os's default layout, unless the last run crashed in it)
//   -> loop { wfi; console::service(); input; display; rt.poll(); audio beat; the card slot; the outputs opened once after a clean run
//   and never during a firmware update; LED }
//
// Without it (-DO3SDK_MK66_CONSOLE=OFF) this is M2's image. The tracker's power-off is not part of either. The only output before the display is the
// status LED (blink.h; the codes are in README.md). A dark LED after power-on means the image did not get as far as `early_pins`, not that it is idle.
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

#if O3SDK_CONSOLE
#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/console/attach.h>
#include <o3sdk/platform/mk66/console/modules.h>
#include <o3sdk/platform/mk66/storage/device.h>
#include <o3sdk/platform/mk66/usb/boot_layout.h>
#include <o3sdk/platform/mk66/usb/console.h>
#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/platform/mk66/usb/usb0.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>
#endif

#include <cstdint>

#ifndef O3SDK_CONSOLE
// 1: the USB console, the SD card and the firmware update path are part of the image (platform/mk66/CMakeLists.txt O3SDK_MK66_CONSOLE).
#define O3SDK_CONSOLE 0
#endif

#ifndef O3SDK_AUDIO_TEST
// 1: the bring-up image of the audio path. The engine is not connected; the device plays audio_test_tone() (1 kHz at -20 dBFS,
// the left channel for about 6 s, then both), opened by the same gate as the app's sound.
#define O3SDK_AUDIO_TEST 0
#endif

namespace mk66 = o3sdk::platform::mk66;
namespace display = o3sdk::platform::mk66::display::device;
namespace input = o3sdk::platform::mk66::input::device;
#if O3SDK_CONSOLE
namespace console = o3sdk::platform::mk66::console;
namespace storage = o3sdk::platform::mk66::storage;
namespace usb0 = o3sdk::platform::mk66::usb0;
namespace usb_audio = o3sdk::platform::mk66::usb_audio;
#endif

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

// The boot log (`log`) and the start-up faults `stage` lists: nothing in an image without the console.
enum class Fault : std::uint8_t { Audio, Display, App, Sd };
void boot_log(const char* text) noexcept {
#if O3SDK_CONSOLE
  console::log(text);
#else
  (void)text;
#endif
}
void note_fault(Fault fault) noexcept {
#if O3SDK_CONSOLE
  switch (fault) {
    case Fault::Audio: console::note_boot_fault(console::kFaultAudio); break;
    case Fault::Display: console::note_boot_fault(console::kFaultDisplay); break;
    case Fault::App: console::note_boot_fault(console::kFaultApp); break;
    case Fault::Sd: console::note_boot_fault(console::kFaultSd); break;
  }
#else
  (void)fault;
#endif
}

// Which trees the image was built from ("o3sdk 0123456789ab-dirty"; in an app's image "ba9876543210 / o3sdk 0123456789ab", the app's
// identity first), for `strings` on the `.bin` or the `.elf` (the HEX stores hex digits) and for a debugger.
const char* volatile const g_build_label = O3SDK_IMAGE_LABEL;

// The app's engine: 23 KB, in SRAM_U's .bss (the render reads it every block). Built by the static constructors.
o3sdk::dsp::Engine g_engine;

// boot::delay_ms()'s idle hook, once a millisecond of a blocking wait (the codec's start and unmute, the blink codes below): the
// display keeps its swap and backlight ramp going (idle() keeps out of the driver's own waits and of a display that is down), and the
// audio guard gets its beat, so a wait never counts as a starved loop (track3r-os bench.cpp does the same).
void idle_hook() noexcept {
  display::idle();
  mk66::audio_main_pass();
}

#if O3SDK_CONSOLE
// ---- The card slot (OS-SDSWAP; track3r-os app_main.cpp poll_card, without the app's CARD REMOVED / READING CARD strip, which o3-sdk has no screen for yet) -------------------------------
// A card taken out is released the moment its detect level has been settled for 250 ms (storage::CardSwap, polled here, never from an interrupt): every card call answers NotMounted from
// then on. A card put in is mounted kCardMountDelayMs later (two UI frames in track3r-os, so that its announcement has been drawn before the mount blocks the main loop: the card's own
// initialisation, tens of ms, at most about 3 s for a card that does not answer; the display stays serviced from the idle hook). The mount waits while the firmware update path has the card
// (a pull ends it within a pass or two), and a card that does not mount is not tried again until it has been taken out and put back. Audio never touches the card. Main loop only.
constexpr std::uint32_t kCardMountDelayMs = 66;
bool g_card_mount_pending = false;
std::uint32_t g_card_mount_at = 0;

bool card_mount_pending() noexcept { return g_card_mount_pending; }

void poll_card(std::uint32_t now) noexcept {
  switch (storage::device::poll(now)) {
    case storage::CardSwap::Event::Removed:
      g_card_mount_pending = false;
      console::log("sd: card removed, volume released");
      break;
    case storage::CardSwap::Event::Inserted:
      g_card_mount_pending = true;
      g_card_mount_at = now;
      console::log("sd: card inserted");
      break;
    case storage::CardSwap::Event::None:
      break;
  }
  if (!g_card_mount_pending || now - g_card_mount_at < kCardMountDelayMs) {
    return;
  }
  g_card_mount_pending = false;  // (the pending mount is itself a claim: it must not count against its own gate)
  if (storage::device::card_busy()) {
    g_card_mount_pending = true;  // the update path holds the card: ask again next pass
    return;
  }
  if (storage::device::owner() == storage::CardOwner::Host) {
    return;  // OS-USBDISK F1: the computer has the card; nothing mounts here (device::mount() would refuse as well)
  }
  const std::uint64_t started = mk66::system_clock().uptime_ms();
  const o3sdk::hal::storage::Status status = storage::device::mount();
  const auto took_ms = static_cast<std::uint32_t>(mk66::system_clock().uptime_ms() - started);  // how long the main loop stood still
  char line[56];
  mk66::audio::diag::Text text(line, sizeof line);
  if (status == o3sdk::hal::storage::Status::Ok) {
    const o3sdk::hal::storage::VolumeInfo volume = storage::device::storage().volume();
    text.str("sd: card mounted, ").str(o3sdk::hal::storage::fs_kind_name(volume.kind)).str(", ").dec(volume.capacity_mb).str(" MB, ").dec(took_ms).str(" ms");
  } else {
    text.str("sd: card NOT mounted: ").str(o3sdk::hal::storage::status_name(status)).str(", ").dec(took_ms).str(" ms");
  }
  console::log(line);
}
#endif

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

#if O3SDK_CONSOLE
  // The log first: the SDRAM has been tested (the test overwrote all of it) and cleared, so its ring can be started now.
  console::init_log();
  console::log("boot: main reached");
  console::log_hex("boot: reset sources", mk66::boot_info().reset_sources);
#endif

  // 0: the heartbeat. A fault found at boot or while running (kAppFailure, kDisplayStalled, kAudioFailure) replaces it for good: the
  // image keeps running, the display is held dark, and the code is shown over and over so it cannot be taken for a quiet black screen.
  std::uint8_t fault_code = 0;
#if O3SDK_CONSOLE
  bool usb_crashed = false;  // the last run died in USB: this boot leaves it off (docs/M2B-PLAN.md section 6, mitigation c)
  std::uint32_t usb_code_until_ms = 0;  // usb0::start() failed: blink::kUsbFailure shows until this uptime (0: no)
#endif
  if (const mk66::crash::CrashRecord* const record = mk66::boot_crash_record()) {
#if O3SDK_CONSOLE
    console::note_boot_crash(record);  // `crash` keeps it for this run; the noinit copy is erased below
    console::log_hex("boot: previous run crashed, exception", record->exception);
    usb_crashed = console::crash_blames_usb(record);
#endif
    mk66::boot::play_code(mk66::blink::kCrashSeen, 2);
    mk66::clear_crash_record();
  }

  // ---- Display: SPI2 and its eDMA channels, the FT812 reset and brought up (bounded, retried), the DMA path self-tested, the
  // backlight off until the app has a frame on screen. A display that does not come up is a blink code and a headless run.
  mk66::set_module(kDisplayModule);
  const display::BringUpResult shown = display::bring_up();
  boot_log(shown.up ? "boot: display up" : "boot: display FAILED, headless");
  if (shown.up) {
    boot_log(shown.dma ? "boot: display dma ok" : "boot: display dma FAILED, polled path");
  } else {
    note_fault(Fault::Display);
  }
  mk66::boot::set_idle_hook(&idle_hook);  // installed for good, from here

  // ---- Input: the two key scanners and the pad LED driver, on I2C0 with the codec (main thread only, for both).
  mk66::set_module(kInputModule);
  const bool input_ok = input::init();

  // ---- The app: the Runtime uploads the UI's bitmaps, runs App::setup and starts the engine.
  mk66::set_module(kAppModule);
  g_engine.set_cycle_counter(&o3sdk::hal::cycles);  // the header's CPU %
#if O3SDK_CONSOLE
  // Every event the app polls is also offered to `input` (console::tap_input), and `key`, `pad` and `enc` inject into the scanner's own queue.
  static o3sdk::app::Platform platform{display::display(), console::tap_input(input::input()), &input::leds(), mk66::system_clock(), true};
#else
  static o3sdk::app::Platform platform{display::display(), input::input(), &input::leds(), mk66::system_clock(), true};
#endif
  static o3sdk::app::Runtime rt(o3sdk_app(), g_engine, platform);
  if (rt.start()) {
    // The first frame: the kit's boot screen (the logo the upload just put in RAM_G, the app's name and the build), on screen through
    // the blink codes and the audio start below, until the loop's first rt.poll() draws the app. Its swap starts the backlight ramp.
    (void)o3sdk::ui::show_boot_screen(display::display(), o3sdk_app().info().name, O3SDK_IMAGE_LABEL);
  } else if (shown.up) {
    // The safe state for an app that cannot start (its bitmaps did not go up): the display is held dark, never a lit black screen
    // that looks like a quiet success. The app and its sound still run.
    display::hold_dark();
    fault_code = mk66::blink::kAppFailure;
    note_fault(Fault::App);
    boot_log("boot: app FAILED, display held dark");
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
#elif O3SDK_CONSOLE
  // The USB audio tap after the app's block: the master the main output plays goes to the capture stream (usb_audio.h). Before start(): the callback points at static state.
  const o3sdk::hal::audio::BlockCallback render = usb_audio::wrap(rt.block_callback());
#else
  const o3sdk::hal::audio::BlockCallback render = rt.block_callback();
#endif
  const o3sdk::hal::audio::Device::Result audio_result = mk66::audio_device().start(render);
  const bool audio_up = audio_result == o3sdk::hal::audio::Device::Result::Ok;
  if (audio_up) {
    boot_log("boot: audio started, outputs muted");
  } else {
    fault_code = mk66::blink::kAudioFailure;  // mk66::audio_clock_fault() says which wait ran out, for a debugger
    note_fault(Fault::Audio);
#if O3SDK_CONSOLE
    console::log_dec("boot: audio FAILED, outputs muted, result", static_cast<std::uint32_t>(audio_result));
    console::log_audio_clock_fault("boot: audio", mk66::audio_clock_fault());  // which wait, MCR and MDR
#endif
  }
  mk66::set_module(0);

#if O3SDK_CONSOLE
  // ---- SD card: after the audio path runs and before USB (the card is initialised and its FAT volume mounted; every wait is bounded: a card that does not answer costs the init,
  // under 3 s, and the idle hook keeps the display serviced meanwhile). The firmware update path writes it (`fw put`), nothing else does yet. The slot is watched from here: a card taken
  // out is released, one put in is mounted (poll_card below).
  mk66::set_module(console::kStorageModule);
  storage::device::init();
  const o3sdk::hal::storage::Status sd_status = storage::device::mount();
  storage::device::watch_begin();
  {
    char line[56];
    mk66::audio::diag::Text text(line, sizeof line);
    if (sd_status == o3sdk::hal::storage::Status::Ok) {
      const o3sdk::hal::storage::VolumeInfo volume = storage::device::storage().volume();
      text.str("boot: sd mounted, ").str(o3sdk::hal::storage::fs_kind_name(volume.kind)).str(", ").dec(volume.capacity_mb).str(" MB");
    } else {
      text.str("boot: sd NOT mounted: ").str(o3sdk::hal::storage::status_name(sd_status));
    }
    console::log(line);
  }
  if (sd_status != o3sdk::hal::storage::Status::Ok && storage::device::storage().card_present()) {
    note_fault(Fault::Sd);  // a card is in and did not mount: a fault (`stage` lists it); an empty slot is not
  }
  // The card is in use from `fw put` to the end of `fw update`'s hand-over, and while a card put in waits for its mount (storage/card_claims.h): a hand-over to a computer, `sd mount`
  // and the like ask card_busy(). A refusal (table full) is logged, never dropped.
  if (!storage::device::claim(&mk66::appimg::firmware_update_active) || !storage::device::claim(&card_mount_pending)) {
    console::log("boot: sd claim table full");
  }
  mk66::set_module(0);

  // ---- The console and USB: the table (the app's commands included) and the shell first, then the port. USB starts last, after the audio path: enumeration happens at the USB priority,
  // below the audio DMA interrupt, and must never be able to delay its bring-up. The set of the first enumeration is track3r-os's default one, AUDIO with 8 channels out and the stereo
  // return in (docs/M2B-PLAN.md D5): the master is on channels 7 and 8. A failure (the table misaligned, the descriptors invalid) leaves the pull-up off and says why in the log; the
  // image runs without USB.
  console::attach(rt);
  if (usb_crashed) {
    console::log("boot: USB skipped, the last run crashed in it");
  } else {
    mk66::set_module(console::kUsbModule);
    namespace layout = usb0::boot_layout;
    if (usb0::start(layout::kMode, layout::kProfile, layout::kReturn)) {
      console::note_usb_started();
      // What the driver presents, which is not the boot layout when start() fell back to STANDARD (a layout it refused, descriptors that did not build).
      char line[56];
      char name[20];
      mk66::audio::diag::Text text(line, sizeof line);
      text.str("boot: USB started, ").str(console::usb_mode_name(usb0::mode())).chr(' ').str(console::usb_layout_name(usb0::profile(), usb0::return_enabled(), name));
      console::log(line);
    } else {
      console::log("boot: USB NOT started");
      // The log that says why cannot be read with USB down: the LED says that it is, for two periods of the code from the loop (no blocking at boot), then the heartbeat is back.
      usb_code_until_ms = static_cast<std::uint32_t>(mk66::system_clock().uptime_ms()) + 2u * mk66::blink::period_ms(mk66::blink::kUsbFailure);
    }
    mk66::set_module(0);
  }
#endif

  // ---- Watchdog re-enable point ----------------------------------------------------------------------------------
  // The reset sequence switched the watchdog off. This is where a later slice turns it back on, with the loop below as the place
  // that feeds it. Not done in M2 (as in track3r-os): the watchdog's clock source and timeout have not been checked on a unit, and a
  // wrong guess would be a reset loop on the very first boot.

  mk66::OutputGate output_gate;  // the outputs open after a clean run of the audio path
  mk66::OpenRetry open_retry;    // what a failed unmute costs: a backoff, at most three attempts
  // Whether the outputs are open. With the console the update path can close them behind the loop's back (`fw update`'s quiesce: appimg::audio_closed), so the flag is the image's.
#if O3SDK_CONSOLE
  const auto outputs_open = []() noexcept { return mk66::appimg::audio_outputs_open(); };
  const auto set_outputs_open = []() noexcept { mk66::appimg::audio_opened(); };
#else
  bool opened = false;
  const auto outputs_open = [&opened]() noexcept { return opened; };
  const auto set_outputs_open = [&opened]() noexcept { opened = true; };
#endif
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

#if O3SDK_CONSOLE
    console::service();  // the `diag` console: what the host typed, a stream's next lines, the output; the USB set's changes; the firmware hand-over's clock
#endif
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
#if O3SDK_CONSOLE
    poll_card(ms);  // a card taken out or put in
#endif
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
      // The outputs open once, after 400 clean blocks: the amplifier key's click is at power-on, never under a note. Never while the firmware update path holds the card or waits for the
      // bootloader (its quiesce muted them and cleared the flag; they open again when that ended without a hand-over). No app asks for them and nothing is loading: open_wanted's other arguments are false.
#if O3SDK_CONSOLE
      const bool update_busy = mk66::appimg::firmware_update_active();
#else
      const bool update_busy = false;
#endif
      if (mk66::open_wanted(audio_up, outputs_open(), /*asked=*/false, /*loading=*/false, update_busy, /*powering_off=*/false) && output_gate.ready(snap.boundaries) &&
          open_retry.ready(false, ms)) {
        if (mk66::audio_device().unmute()) {  // blocks about 0.2 s, the display serviced by the idle hook
          set_outputs_open();
          boot_log("audio: outputs open");
          open_retry.succeeded();
          if (unmute_fault && fault_code == mk66::blink::kAudioFailure) {
            fault_code = 0;
          }
#if O3SDK_CONSOLE
          if (unmute_fault) {
            console::clear_boot_fault(console::kFaultAudio);  // the retry worked: `stage` no longer lists it
          }
#endif
          unmute_fault = false;
        } else {
          open_retry.failed(ms);  // (a failure muted them again before it returned)
          unmute_fault = true;
          note_fault(Fault::Audio);
          boot_log("audio: unmute FAILED");
          if (fault_code == 0) {
            fault_code = mk66::blink::kAudioFailure;
          }
        }
      }
    }

    // One LED for every part: a hard audio fault first, then a display, app or audio-start safe-state code, then the audio verdict
    // (kAudioOverBudget or the heartbeat). Each is sticky; the status words tell the rest.
    std::uint8_t shown_code = pattern == mk66::blink::kAudioFault ? pattern : (fault_code != 0 ? fault_code : pattern);
#if O3SDK_CONSOLE
    if (pattern != mk66::blink::kAudioFault && fault_code == 0 && usb_code_until_ms != 0u && static_cast<std::int32_t>(usb_code_until_ms - ms) > 0) {
      shown_code = mk66::blink::kUsbFailure;  // under every fault code; over the heartbeat and the over-budget pattern, for its two periods
    }
#endif
    mk66::boot::led_from_sram_l(mk66::blink::led_on(shown_code, ms));
    if (ms / 1000u != last_second) {
      last_second = ms / 1000u;
      g_heartbeat = g_heartbeat + 1u;
    }
  }
}
