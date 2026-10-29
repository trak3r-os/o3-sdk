// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `diag` console of the o3-sdk image (docs/M2B-PLAN.md, slice C): the shell of kit/usb on the CDC port, and the commands whose data is the platform's. Everything here runs in the
// main loop; the shell never blocks (its output is a ring that drops its oldest bytes when the host does not read) and a command that keeps talking (`input`, `shot`, `log`, `ls`) is a
// stream that says a bounded amount per pass.
//
// The composition is track3r-os's (platform/mk66/usb/src/console.cpp, OS-006b and its successors), cut to the commands o3-sdk has (console/commands.h). The handler bodies below are
// that file's, copied line for line with their comments, and say so; what is rewritten is only the binding to things o3-sdk does not have: `cpu` and `mem` (no module voices, no
// app tap, no tap memory: the figures that exist), `usb` (the AUDIO view and `mode`: no profile, return, map or ep0 views), `sd` (no project loader), `enc` (no acceleration),
// `cache check` (no bench), `state` (the runtime's, console/shell_bind.h) and `key` / `pad` / `enc` (console/inject_commands.h, the same code over a port). The table itself is
// console/core.h and console/shell_bind.h; this file binds the kit's handlers to it at attach().
//
// What each command reads (nothing here writes hardware but `reset`, `poke`, `rot` and `cache`):
//   help       the table (console/core.h)
//   version    the build identity (CMake writes it per build), the USB identity and serial
//   stage      the crash module's stage and module words, uptime, reset sources
//   cpu        audio::AudioStatus through audio::diag::format_cpu; `cpu reset` zeroes the per-run figures (AudioStatus::reset_peaks), the one thing this command writes
//   guard      the main-loop guard's window (audio/pipeline.h); `guard n <1..255>` sets it
//   mem        the memory map and budgets through audio::diag::format_mem, with the image's own hot-code, SRAM_U and SDRAM figures (console/mem_report.h)
//   crash      the record the previous run left (kept before main erased it)
//   usb        the driver's counters and the control pipe's; `usb audio`, `usb mode [standard|audio]`
//   log        the ring boot code and drivers write to
//   input      the events the app polls, live (and `input stats`)
//   peek       aligned 32-bit reads of the modules the image has clocked on (usb/mem_access.h): a read, nothing written
//   poke       one 32-bit write, I2S0's registers only
//   rot        REG_ROTATE written through the display driver, the display idle (not saved)
//   sd         the SD card (slot, mount, identity, speed, counters); `sd mount` initialises and mounts it again. Reads the card.
//   ls         a directory of the card, a pager. A read.
//   reset      mutes the outputs (OS-OUTON: the amplifier key, then the codec), then SYSRESETREQ, after the output has had time to leave. Nothing else.
//   fw         the firmware update path (update/fw_service.h): `fw` lists Firmware/ and the write state, `fw put` receives a .ptf, `fw rm` removes a dev image, `fw arm` / `fw update`
//              hand over to the bootloader. The only command that writes the card.
//   key, pad, enc  input injection into the scanner's own event queue (usb/hil_inject.h)
//   state      the runtime's state line (app::Runtime::state_line)
//   shot       the screen as framed base64 lines (usb/shot_stream.h): CMD_SNAPSHOT2 into the spare RAM_G, read back over the SPI a few lines a pass of the main loop
//   leds       the pad LED levels the driver holds, one line. A read.
//   cache      the LMEM code cache: `cache` shows the state, `cache on|off` switches it (PCCCR only), `cache inv` invalidates it.
//   files      slice FA (o3-sdk's own): the app file API's state and cost (console/files_command.h), and `files crc <path>`, the CRC-32 of a file of the card. A read.
//   radio      OS-RAD1 (slice AI of M2c): the FM radio driver's state, step, last failure (step/cause), last I2C result, registers as last read and pin levels; `radio retry` runs the power-up again from the
//              reset while the radio is the selected input. A read apart from the retry.
//   rec        slice RC of M2c: the recorder's state (idle, waiting, listening, recording, done), the take's frames and their length, the frames that did not fit (overrun), the tap and the pattern
//              length (Context::recorder()). A read: the app arms a take, its own keys or hooks.
//   mon        slice AI of M2c: the audio-in monitor's level (`mon`, `mon off`, `mon on`, `mon 0..100`), Context::audio_in().monitor(). The one command that changes the sound.
//   midi       slice MD (o3-sdk's own): the MIDI service's snapshot and the DIN and USB drivers' counters (console/midi_report.h says each word). A read.
//   power      OS-POWER P1 (docs/M2B2-PLAN.md, slice P): the Power switch (PTC3): `power` the pin in the three pulls, the hold's state and counters, the shutdown's stages; `power press|release|pin` feed the hold
//              in place of the pin (a scripted hold needs no finger); `power off [dry]` runs the shutdown (stages 0 to F: USB off, the lights out, then, after the release of Power, the VLPR off of OS-POWER P2:
//              dark, PTC3 polled, a press boots the unit; `dry` ends in a restart instead, for the first run on the unit). Once the shutdown has begun the shell refuses every command but the reads (help,
//              version, stage, log, crash, power, state, cpu, mem): `ERR powering off`. (track3r-os's header lines, verbatim.)
#include <o3sdk/platform/mk66/usb/console.h>

#include "../../src/internal.h"
#include "o3sdk_build_info.h"  // generated: tools/gen_build_info.cmake

#include <o3sdk/app/runtime.h>
#include <o3sdk/core/input_event.h>
#include <o3sdk/core/placement.h>
#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/capture.h>
#include <o3sdk/platform/mk66/console/attach.h>
#include <o3sdk/platform/mk66/console/commands.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/platform/mk66/console/files_command.h>
#include <o3sdk/platform/mk66/console/inject_commands.h>
#include <o3sdk/platform/mk66/console/mem_report.h>
#include <o3sdk/platform/mk66/console/midi_report.h>
#include <o3sdk/platform/mk66/console/shell_bind.h>
#include <o3sdk/platform/mk66/console/usb_mode_guard.h>
#include <o3sdk/platform/mk66/display/device.h>
#include <o3sdk/platform/mk66/input/device.h>
#include <o3sdk/platform/mk66/mem_map.h>
#include <o3sdk/platform/mk66/midi.h>
#include <o3sdk/platform/mk66/midi/midi_service.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/power/power_device.h>
#include <o3sdk/platform/mk66/storage/device.h>
#include <o3sdk/platform/mk66/storage/ls_command.h>
#include <o3sdk/platform/mk66/storage/sd_text.h>
#include <o3sdk/platform/mk66/update/fw_device.h>
#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/platform/mk66/usb/hil_inject.h>
#include <o3sdk/platform/mk66/usb/input_hold.h>
#include <o3sdk/platform/mk66/usb/log_command.h>
#include <o3sdk/platform/mk66/usb/mem_access.h>
#include <o3sdk/platform/mk66/usb/shot_stream.h>
#include <o3sdk/platform/mk66/usb/soc_facts.h>
#include <o3sdk/platform/mk66/usb/usb0.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>
#include <o3sdk/ui/plot/ram_g_map.h>
#include <o3sdk/usb/diag_log.h>
#include <o3sdk/usb/diag_pump.h>
#include <o3sdk/usb/diag_shell.h>
#include <o3sdk/usb/identity.h>

#include <cstdint>
#include <cstring>
#include <new>

// Linker symbols (mk66.ld): the hot code, the SRAM_U and the SDRAM the image uses.
extern "C" {
extern std::uint32_t __ramfunc_l_start[];
extern std::uint32_t __ramfunc_l_end[];
extern std::uint32_t __noinit_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __sdram_bss_start[];
extern std::uint32_t __sdram_bss_end[];
}

namespace o3sdk::platform::mk66::console {
namespace {

namespace shell = o3sdk::usb::diag;

// ---- The log ---------------------------------------------------------------------------------------------------------------
// 1,024 records of 64 bytes, session only (the SDRAM does not survive power-off): an O3SDK_BIG object, so it is in `.sdram_bss`, which the reset sequence zeroes before the constructors.
// init_log() runs once in the main thread right after (the SDRAM test overwrote everything before the reset sequence cleared it).
shell::LogRing g_log;
constexpr std::uint32_t kLogRecords = 1024;
O3SDK_BIG shell::LogRecord g_log_slots[kLogRecords];
static_assert(sizeof(shell::LogRecord) == 64 && (kLogRecords & (kLogRecords - 1u)) == 0u, "the log ring holds a power-of-two number of 64-byte records");
static_assert(kLogMax == kLogRecords, "`log` can show the whole ring");

// ---- The banner ---------------------------------------------------------------------------------------------------------------
// The image label already says "o3sdk" ("o3sdk <kit>" for a kit app, "<app> / o3sdk <kit>" for another project's), so the banner does not.
constexpr char kBanner[] = "Track3r diag - " O3SDK_IMAGE_LABEL " - 'help' lists the commands";

// ---- Input tap ----------------------------------------------------------------------------------------------------------------
// Both ends run in the main loop (the app's poll and the console's stream), so the ring's single-producer, single-consumer rule holds.
// (track3r-os's TeeInput, line for line.)
class TeeInput final : public hal::Input {
 public:
  explicit TeeInput(hal::Input& source) noexcept : source_(source) {}
  bool poll(core::InputEvent& out) noexcept override {
    if (locked_) {
      while (source_.poll(out)) {  // OS-POWER P1 (D2): the shutdown has begun: the keys, pads and the encoder are read and dropped, the app is given nothing
      }
      return false;
    }
    if (!source_.poll(out)) {
      return false;
    }
    (void)ring_.push(out);  // a full ring drops the newest: nobody is reading
    return true;
  }
  hal::InputStats stats() const noexcept override { return source_.stats(); }
  bool pop(core::InputEvent& out) noexcept { return ring_.pop(out); }
  void lock() noexcept {  // never undone: a restart ends the run
    source_.pass(false);  // (a hold ends first: the locked poll reads the kept releases and the queue dry)
    locked_ = true;
  }
  void hold(bool held) noexcept { source_.pass(held); }  // OS-SCREENSHOT (spec Q4), once a pass

 private:
  InputHold source_;  // between the key scan and the app: the screenshot's capture takes the presses (usb/input_hold.h)
  core::InputQueue ring_;
  bool locked_ = false;  // main loop only
};
alignas(TeeInput) unsigned char g_tee_storage[sizeof(TeeInput)];
TeeInput* g_tee = nullptr;

// ---- Crash record of the previous run -------------------------------------------------------------------------------------------
crash::CrashRecord g_boot_crash;
bool g_have_boot_crash = false;

// Start-up faults main went on after (boot_faults.h): a mask, written in the main thread only.
std::uint32_t g_boot_faults = 0;

// ---- Output ----------------------------------------------------------------------------------------------------------------------
void sink_write(void*, const char* data, std::size_t n) { usb0::cdc_writer().write(data, n); }
void sink_flush(void*) { usb0::cdc_writer().flush(); }
std::size_t sink_room(void*) { return usb0::cdc_writer().room_bytes(); }

constexpr std::size_t kTextBuffer = 1536;  // the longest reply is `usb`, about 1.2 KB
static_assert(kTextBuffer >= kMidiReportRoom, "`midi`'s worst-case report fits the buffer (console/midi_report.h)");

void send(shell::Shell& sh, std::size_t n, const char* buffer) { sh.out().text(buffer, n); }

bool same(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

std::uint32_t uptime_ms32() { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

// ---- The USB set's guard (usb_mode_guard.h) ---------------------------------------------------------------------------------------
UsbModeGuard g_guard;

void guard_layout(UsbModeGuard::Layout layout) { g_guard.layout_set(layout); }

// ---- Commands ------------------------------------------------------------------------------------------------------------------------
// (`help` and `state` are console/shell_bind.h's; the app's rows run through its dispatcher.)

void cmd_version(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  send(sh, format_version(O3SDK_IMAGE_LABEL, o3sdk::usb::kVendorId, o3sdk::usb::kProductId, o3sdk::usb::kDeviceBcdStandard, usb0::serial(), buf, sizeof buf), buf);
}

void cmd_stage(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  const BootInfo& boot = boot_info();
  send(sh, format_stage(boot::load_context(), static_cast<std::uint32_t>(system_clock().uptime_ms()), boot.reset_sources, boot.crash_record_found, g_boot_faults, buf, sizeof buf), buf);
}

// `cpu [reset]` (track3r-os cmd_cpu without the module voices, the app tap and `perf off`, which o3-sdk does not have).
void cmd_cpu(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 2 && same(a.argv[1], "reset")) {
    audio_cpu_reset();  // OS-PERF3: the maxima and the fault counters back to zero, for a measurement of one run
    sh.out().text("cpu: peaks and fault counters reset\n");
    return;
  }
  if (a.argc != 1) {
    sh.out().text("usage: cpu [reset]\n");
    return;
  }
  char buf[kTextBuffer];
  send(sh, audio::diag::format_cpu(audio_status().snapshot(), hal::audio::kCyclesPerBlock, buf, sizeof buf), buf);
  const audio::Pipeline& pipe = audio_pipeline();
  send(sh, audio::diag::format_shed(pipe.shed_blocks(), pipe.shed_late(), pipe.shed_runs(), pipe.shed_longest(), pipe.starve_blocks(), buf, sizeof buf), buf);  // OS-MAINGUARD: since boot or `cpu reset` (the render zeroes them)
}

// `guard [n <1..255>]` (OS-MAINGUARD tuning): the main-loop guard's starvation window N, the over-budget renders in a row before the render sheds. With no argument it prints the window;
// `guard n 12` sets it (the render reads it at its next block; it is not saved, a reset goes back to the default). `cpu` shows it at the end of its guard line.
void cmd_guard(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t n = 0;
  if (a.argc == 3 && same(a.argv[1], "n") && parse_dec(a.argv[2], audio::kStarveBlocksMax, n) && n != 0u) {
    audio_set_starve_blocks(n);
  } else if (a.argc != 1) {
    sh.out().text("usage: guard [n <1..255>]\n");
    return;
  }
  char buf[kTextBuffer];
  const audio::Pipeline& pipe = audio_pipeline();
  send(sh, audio::diag::format_shed(pipe.shed_blocks(), pipe.shed_late(), pipe.shed_runs(), pipe.shed_longest(), pipe.starve_blocks(), buf, sizeof buf), buf);
}

// `mem`: track3r-os fills the report from its SDRAM regions (fill_mem_tables); here from the image's linker symbols (console/mem_report.h).
void cmd_mem(shell::Shell& sh, const shell::Args&) {
  MemTables tables;
  MemFacts facts;
  facts.hot_code_bytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__ramfunc_l_end) - reinterpret_cast<std::uintptr_t>(__ramfunc_l_start));
  facts.sram_u_image_bytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__bss_end) - reinterpret_cast<std::uintptr_t>(__noinit_start));
  facts.sdram_bss_bytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__sdram_bss_end) - reinterpret_cast<std::uintptr_t>(__sdram_bss_start));
  fill_mem_tables(tables, facts);
  char buf[kTextBuffer];
  send(sh, audio::diag::format_mem(tables.report, buf, sizeof buf), buf);
}

void cmd_crash(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  send(sh, format_crash(g_have_boot_crash ? &g_boot_crash : nullptr, buf, sizeof buf), buf);
}

// ---- `usb audio | mode` (OS-006d): the AUDIO mode's view and controls. The mode is RUNTIME state here: a boot comes up in the AUDIO set of track3r-os's default layout (P8, return on;
// docs/M2B-PLAN.md D5), and `usb mode` is runtime only. (track3r-os's `usb profile`, `return`, `map` and `ep0` are cut: o3-sdk has one layout and no stem map yet.)
void usb_audio_view(shell::Shell& sh) {
  const o3sdk::usb::IsoAudio* iso = usb0::iso();
  if (iso == nullptr) {
    sh.out().text("usb audio: the driver is not running\n");
    return;
  }
  const o3sdk::usb::ControlPipe& pipe = usb0::control();
  UsbAudioReport r{usb0::mode(),
                   usb0::profile(),
                   usb0::change_phase(),
                   usb0::pending_mode(),
                   usb0::pending_profile(),
                   pipe.configured(),
                   iso->in_active(),
                   iso->out_active(),
                   iso->in_channels(),
                   &usb0::stream_counters(),
                   &usb0::device_counters(),
                   iso->rate().locked(),
                   iso->rate().period_cycles_q8(),
                   iso->rate().resyncs(),
                   iso->rate().frames_per_sof_q16(),
                   iso->feedback_rate_q16(),
                   iso->feedback().trim_q16(),
                   iso->stem_fill_frames(),
                   iso->return_fill_frames(),
                   usb0::return_enabled(),
                   usb0::pending_return()};
  char buf[kTextBuffer];
  send(sh, format_usb_audio(r, buf, sizeof buf), buf);
}

void usb_request(shell::Shell& sh, o3sdk::usb::Mode mode, o3sdk::usb::Profile profile, bool usb_return) {
  if (!usb0::request_layout(mode, profile, usb_return)) {
    sh.out().text(usb0::change_phase() != 0u ? "a mode change is already in progress\n"
                  : usb0::mode() == o3sdk::usb::Mode::Disk ? "refused: the USB DISK mode ends with a restart\n"
                                                        : "refused: profile not supported yet (4s needs post-pan stems) or driver down\n");
    return;
  }
  char layout[20];
  (void)usb_layout_name(profile, usb_return, layout);
  if (usb0::change_phase() == 0u) {
    if (usb0::mode() == o3sdk::usb::Mode::Disk) {  // (accepted only because it is the layout DISK already presents)
      sh.out().text("usb: DISK mode is held by the boot disk until a restart, nothing changed: ").text(layout).nl();
      return;
    }
    sh.out().text(mode == o3sdk::usb::Mode::Audio ? "usb: already presenting AUDIO with that layout: " : "usb: STANDARD, layout set (used by the next AUDIO attach): ").text(layout).nl();
    return;
  }
  // o3-sdk: a change is on its way; the guard watches the set it ends in (usb_mode_guard.h).
  guard_layout(mode == o3sdk::usb::Mode::Audio ? UsbModeGuard::Layout::Audio : UsbModeGuard::Layout::Standard);
  sh.out().text("usb: re-enumerating as ").text(usb_mode_name(mode)).text(" (").text(layout).text(
      "); this terminal drops, reconnect in about a second\n");
}

void usb_map_command(shell::Shell& sh, const shell::Args& a) {
  char buf[kTextBuffer];
  audio::StemMap& map = usb0::stem_map();
  if (a.argc == 3 && same(a.argv[2], "reset")) {
    map.reset(map.profile());
    usb0::apply_stem_map();
  } else if (a.argc == 4) {
    std::uint32_t slot = 0;
    std::uint32_t sources = 0;
    if (!parse_dec(a.argv[2], o3sdk::usb::kMaxChannels, slot) || slot == 0u || !audio::parse_sources(a.argv[3], sources) || !map.set_slot(slot - 1u, sources)) {
      sh.out().text("usage: usb map [reset | <slot> <sources>]   slot 1..profile channels, sources like T1+T9, ML, MR, SA, SB or -\n");
      return;
    }
    usb0::apply_stem_map();
  } else if (a.argc != 2) {
    sh.out().text("usage: usb map [reset | <slot> <sources>]\n");
    return;
  }
  send(sh, format_stem_map(map, buf, sizeof buf), buf);
}

// OS-USBDISK U2a: the first events of endpoint 0 in the last DISK attach (usb/ep0_trace.h), one line each; the ring survives the rollback to the previous layout, so this is read once the console is back.
void usb_ep0_view(shell::Shell& sh) {
  const o3sdk::usb::Ep0Trace& trace = usb0::ep0_trace();
  const unsigned n = trace.count();
  sh.out().text("usb ep0: ").text(n == 0u ? "nothing recorded (no DISK attach since the boot)\n" : "the first events of the last DISK attach\n");
  char line[96];
  for (unsigned i = 0; i < n; ++i) {
    send(sh, format_usb_ep0_entry(i, trace.entry(i), line, sizeof line), line);
  }
  if (n != 0u) {
    send(sh, format_usb_ep0_retries(trace.stall_retries(), line, sizeof line), line);
  }
}

void usb_subcommand(shell::Shell& sh, const shell::Args& a) {
  const char* sub = a.argv[1];
  if (same(sub, "audio")) {
    if (a.argc == 3 && same(a.argv[2], "reset")) {
      usb0::reset_stream_counters();
    }
    usb_audio_view(sh);
  } else if (same(sub, "mode")) {
    if (a.argc == 2) {
      char layout[20];
      sh.out().text("usb mode: ").text(usb_mode_name(usb0::mode())).text(", profile ").text(usb_profile_name(usb0::profile())).text(", ").text(
          usb_layout_name(usb0::profile(), usb0::return_enabled(), layout)).nl();
    } else if (a.argc == 3 && same(a.argv[2], "audio")) {
      usb_request(sh, o3sdk::usb::Mode::Audio, usb0::profile(), usb0::return_enabled());
    } else if (a.argc == 3 && same(a.argv[2], "standard")) {
      usb_request(sh, o3sdk::usb::Mode::Standard, usb0::profile(), usb0::return_enabled());
    } else {
      sh.out().text("usage: usb mode [standard|audio]\n");
    }
  } else if (same(sub, "profile")) {
    std::uint32_t channels = 0;
    o3sdk::usb::Profile profile = o3sdk::usb::Profile::P10;
    // The third word, when there is one, chooses USB RETURN with the profile: `out` is capture only ("10 OUT"), `duplex` adds the stereo return ("8 OUT / 2 IN").
    bool usb_return = usb0::return_enabled();
    const bool word = a.argc == 4 && (same(a.argv[3], "out") || same(a.argv[3], "duplex"));
    if (word) {
      usb_return = same(a.argv[3], "duplex");
    }
    if ((a.argc == 3 || word) && parse_dec(a.argv[2], 11u, channels) && (channels == 2u || channels == 8u || channels == 10u || channels == 11u)) {
      profile = channels == 2u ? o3sdk::usb::Profile::P2 : (channels == 8u ? o3sdk::usb::Profile::P8 : (channels == 11u ? o3sdk::usb::Profile::P11 : o3sdk::usb::Profile::P10));
      usb_request(sh, usb0::mode(), profile, usb_return);  // in AUDIO mode this re-enumerates; in STANDARD it only sets what the next AUDIO attach uses
    } else {
      sh.out().text("usage: usb profile <2|8|10|11> [out|duplex]   (10 = T1..T8 + master, 8 = T1..T6 + master; out = capture only: `profile 10 out` is \"10 OUT\",\n"
                    "       duplex adds the stereo return: `profile 8 duplex` is \"8 OUT / 2 IN\", the layout that opens both ways behind a hub)\n");
    }
  } else if (same(sub, "return")) {
    if (a.argc == 2) {
      char layout[20];
      sh.out().text("usb return: ").text(usb0::return_enabled() ? "on" : "off").text(" (").text(usb_layout_name(usb0::profile(), usb0::return_enabled(), layout)).text(")\n");
    } else if (a.argc == 3 && (same(a.argv[2], "on") || same(a.argv[2], "off"))) {
      usb_request(sh, usb0::mode(), usb0::profile(), same(a.argv[2], "on"));
    } else {
      sh.out().text("usage: usb return [on|off]   (off = no playback stream: the host cannot open a return the bus has no room for)\n");
    }
  } else if (same(sub, "map")) {
    usb_map_command(sh, a);
  } else if (same(sub, "ep0")) {
    usb_ep0_view(sh);
  } else {
    sh.out().text("usage: usb [audio [reset] | mode [standard|audio] | profile <2|8|10|11> [out|duplex] | return [on|off] | map [reset | <slot> <sources>] | ep0]\n");
  }
}

void cmd_usb(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 1) {
    usb_subcommand(sh, a);
    return;
  }
  const o3sdk::usb::ControlPipe& pipe = usb0::control();
  UsbReport r{&usb0::device_counters(),
              &pipe.counters(),
              &usb0::stream_counters(),
              pipe.state(),
              pipe.address(),
              pipe.suspended(),
              pipe.cdc().dtr(),
              pipe.cdc().rts(),
              pipe.cdc().coding,
              pipe.cdc().coding_sets,
              pipe.cdc().control_sets,
              usb0::cdc_writer().bytes_written(),
              usb0::cdc_writer().dropped_chunks(),
              usb0::cdc_rx().size(),
              usb0::attached() ? 1u : 0u,
              usb0::soc::kUsbClockHz};
  char buf[kTextBuffer];
  send(sh, format_usb(r, buf, sizeof buf), buf);
}

// `disk`, `disk share [ro]`, `disk eject` (OS-USBDISK U2a, the running mode): the unit gives the card to the computer while it goes on playing, and takes it back. `share` waits for a due settings write (2 s),
// switches the USB set to DISK (the console's port re-enumerates: open it again), and hands the card over once the computer has configured the disk; `ro` exports it write-protected. `eject` is the unit's side
// of an eject (the computer's next command is UNIT ATTENTION, a copy in flight ends at its next block): it asks no confirmation, the console command is the confirmation. The Tracker takes the card back only through
// an eject, never on its own. `disk` alone prints the state.
void cmd_disk(shell::Shell& sh, const shell::Args& a) {
  const char* refusal = nullptr;
  if (a.argc >= 2 && same(a.argv[1], "share") && a.argc <= 3 && (a.argc == 2 || same(a.argv[2], "ro"))) {
    refusal = usb_audio::disk_request(a.argc == 3);
    if (refusal == nullptr) {
      sh.out().text(a.argc == 3 ? "disk: sharing read-only; the console port re-enumerates, open it again and `disk` shows the state\n" : "disk: sharing; the console port re-enumerates, open it again and `disk` shows the state\n");
      log("disk: share asked from the console");
      return;
    }
  } else if (a.argc == 2 && same(a.argv[1], "eject")) {
    refusal = usb_audio::disk_eject_request();
    if (refusal == nullptr) {
      sh.out().text("disk: ejecting; the card is remounted, the USB set comes back\n");
      log("disk: eject asked from the console");
      return;
    }
  } else if (a.argc != 1) {
    sh.out().text("usage: disk [share [ro]|eject]\n");
    return;
  }
  if (refusal != nullptr) {
    sh.out().text("ERR disk: ").text(refusal).nl();
    return;
  }
  const usb0::DiskState d = usb0::disk_state();
  sh.out().text("disk: ").text(usb_audio::disk_state_name()).text(", owner ").text(storage::device::owner() == storage::CardOwner::Host ? "computer" : "tracker").text(d.configured ? ", configured" : ", not configured");
  sh.out().text(d.writable ? ", writable" : ", read-only").text(usb0::disk_throttled() ? ", throttled" : "").nl();
}

void cmd_log(shell::Shell& sh, const shell::Args& a) { run_log_command(sh, g_log, a); }

constexpr unsigned kInputEventsPerTick = 8;

bool input_stream(shell::Shell& sh, void*) {
  core::InputEvent e;
  char buf[96];
  for (unsigned i = 0; i < kInputEventsPerTick && g_tee != nullptr && g_tee->pop(e); ++i) {
    send(sh, format_input_event(e, buf, sizeof buf), buf);
  }
  return true;  // runs until a key is pressed
}

void cmd_input(shell::Shell& sh, const shell::Args& a) {
  char buf[kTextBuffer];
  if (a.argc > 1) {
    if (same(a.argv[1], "stats")) {
      send(sh, format_input_stats(input::device::scanner().stats(), buf, sizeof buf), buf);
    } else {
      sh.out().text("usage: input [stats]\n");
    }
    return;
  }
  if (g_tee == nullptr) {
    sh.out().text("no input tap: the app has not started\n");
    return;
  }
  core::InputEvent stale;
  while (g_tee->pop(stale)) {  // what happened before the command is not live
  }
  sh.out().text("input events, live (any key stops):\n");
  sh.start_stream(&input_stream, nullptr);
}

// ---- OS-HW1 bring-up diagnostics ------------------------------------------------------------------------------------------------------
// `peek <hexaddr> [count 1..16]`: aligned 32-bit reads, only inside the modules this image has clocked on (mem_access.h): a read of a gated module
// is a bus fault. A volatile word read of a listed address, nothing else.
void cmd_peek(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t addr = 0;
  std::uint32_t count = 1;
  if (a.argc < 2 || a.argc > 3 || !parse_hex32(a.argv[1], addr) || (a.argc == 3 && (!parse_dec(a.argv[2], kPeekMaxWords, count) || count == 0u))) {
    sh.out().text("usage: peek <hexaddr> [count 1..16]\n");
    return;
  }
  const Access ok = check_peek(addr, count);
  if (ok == Access::Unaligned) {
    sh.out().text("peek: the address must be 4-byte aligned\n");
    return;
  }
  if (ok != Access::Ok) {
    sh.out().text("peek: refused, outside the modules clocked on in this image (a read of a gated module is a bus fault)\n");
    return;
  }
  char buf[kTextBuffer];
  audio::diag::Text t(buf, sizeof buf);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t word_addr = addr + 4u * i;
    t.hex32(word_addr).str(": ").hex32(*reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(word_addr))).nl();
  }
  send(sh, t.size(), buf);
}

// `poke <hexaddr> <hexvalue>`: one aligned 32-bit write, inside I2S0 only; prints the value read back.
void cmd_poke(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t addr = 0;
  std::uint32_t value = 0;
  if (a.argc != 3 || !parse_hex32(a.argv[1], addr) || !parse_hex32(a.argv[2], value)) {
    sh.out().text("usage: poke <hexaddr> <hexvalue>\n");
    return;
  }
  const Access ok = check_poke(addr);
  if (ok == Access::Unaligned) {
    sh.out().text("poke: the address must be 4-byte aligned\n");
    return;
  }
  if (ok != Access::Ok) {
    sh.out().text("poke: refused, only I2S0 (0x4002F000..0x4002F1FF) may be written\n");
    return;
  }
  volatile std::uint32_t& word = *reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(addr));
  word = value;
  char buf[96];
  audio::diag::Text t(buf, sizeof buf);
  t.str("poke ").hex32(addr).str(" <- ").hex32(value).str(", read back ").hex32(word).nl();
  send(sh, t.size(), buf);
}

// `rot <0..7>`: REG_ROTATE through the chip, at once (ft812_regs.h), not saved. Only with the display idle (a transfer in flight owns the SPI):
// busy says so and a second try works. The scan-out re-reads the display list every frame, so there is nothing to redraw.
void cmd_rot(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t value = 0;
  if (a.argc != 2 || !parse_dec(a.argv[1], 7u, value)) {
    sh.out().text("usage: rot <0..7>\n");
    return;
  }
  display::Ft812Display& screen = display::device::display();
  if (!screen.ready() || screen.failed()) {
    sh.out().text("rot: the display is not running\n");
    return;
  }
  if (!screen.idle()) {
    sh.out().text("rot: the display is busy with a frame, try again\n");
    return;
  }
  display::device::chip().wr32(display::ft812::reg::kRotate, value);
  sh.out().text("rot: REG_ROTATE = ").dec(value).text(" (not saved)\n");
}

// `sd`: the slot, the mount, the card, the counters (storage/sd_text.h). `sd mount` runs the
// card's initialisation and the mount again (a card put back in, a retry after a failure): it blocks for the card's own time (the display stays
// serviced), and not while the card is in use (a project loading, a save, a firmware upload, a listing: OS-SDSWAP, OS-USBDISK F3) or on the computer (`ERR card on host`).
void cmd_sd(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 1) {
    if (!same(a.argv[1], "mount") || a.argc > 2) {
      sh.out().text("usage: sd [mount]\n");
      return;
    }
    if (storage::device::owner() == storage::CardOwner::Host) {  // OS-USBDISK F1: a mount under the computer would be a second writer on its FAT
      sh.out().text("ERR card on host\n");
      return;
    }
    if (storage::device::card_busy()) {  // OS-SDSWAP: a save is as much "in use" as a load: the mount closes every handle, which would end it
      sh.out().text("sd: the card is in use (a load, a save, an upload, a listing), try again in a moment\n");
      return;
    }
    ls_release(storage::device::storage());  // a listing that a key press ended
    files_release(storage::device::storage());  // and a `files crc` that one ended: its handle would be stale after the remount, and could close a slot the service reused
    appimg::service_while_blocked(true);
    const hal::storage::Status s = storage::device::mount();
    appimg::service_while_blocked(false);
    sh.out().text("sd mount: ").text(hal::storage::status_name(s)).nl();
    log(s == hal::storage::Status::Ok ? "sd: mounted again from the console" : "sd: mount from the console FAILED");
  }
  char buf[kTextBuffer];
  const storage::SdReport report = storage::device::report();  // (no project loader here: the report's load and sample summaries stay empty)
  send(sh, storage::format_sd(report, buf, sizeof buf), buf);
}

// `ls [path]`: the card's directory, a few lines a pass (storage/ls_command.h). Never writes: hal/storage.h has no write call.
void cmd_ls(shell::Shell& sh, const shell::Args& a) { run_ls_command(sh, storage::device::storage(), a); }

// `files [crc <path>]` (slice FA, o3-sdk's own): the app file API's state and cost, and the CRC-32 of a card file (console/files_command.h). A read.
const app::Files* g_files = nullptr;
void cmd_files(shell::Shell& sh, const shell::Args& a) {
  if (g_files == nullptr) {
    sh.out().text("files: not in this image\n");
    return;
  }
  run_files_command(sh, *g_files, storage::device::storage(), a);
}

// `radio [retry]` (OS-RAD1; slice AI of M2c, track3r-os's handler with the recorder's conditions replaced by the kit's): the FM radio driver's state, step, last failure, last bus result, the registers as last
// read and the lines' levels; `radio retry` runs the power-up again from the reset (main loop, like a source change does), but only while the radio is the selected input (Context::audio_in(): the reset
// would otherwise power a receiver nobody listens to; then the state only, and why). A read apart from the retry, which writes the radio's registers.
hal::capture::CaptureHw* g_capture = nullptr;
app::AudioIn* g_audio_in = nullptr;
void cmd_radio(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 2 || (a.argc == 2 && !same(a.argv[1], "retry"))) {
    sh.out().text("usage: radio [retry]\n");
    return;
  }
  if (g_capture == nullptr) {
    sh.out().text("radio: this image has no radio\n");
    return;
  }
  bool retried = false;
  if (a.argc == 2) {
    if (g_audio_in != nullptr && g_audio_in->source() == hal::capture::Source::Radio) {
      capture_radio_restart(*g_capture);
      retried = true;
    } else {
      sh.out().text("radio: retry skipped, the radio is not the selected input (select INPUT RADIO in the Config's AUDIO section first). State only:\n");
    }
  }
  char buf[kTextBuffer];
  const std::size_t n = capture_radio_report(*g_capture, buf, sizeof buf);
  if (n == 0u) {
    sh.out().text("radio: this image has no radio\n");
    return;
  }
  if (retried) {
    sh.out().text("radio: power-up started again from the reset; `radio` shows how it went\n");
  }
  send(sh, n, buf);
}

// `mon [off|on|0..100]` (slice AI of M2c; track3r-os's line-in monitor level on the kit's audio in): shows the monitor's level; `mon off`, `mon on` (70) or `mon <0..100>` sets it
// (Context::audio_in().monitor(): the input joins the master bus before the master inserts, ramped across a block, 0 is off and free). Not with the main out patched to the line in.
void cmd_mon(shell::Shell& sh, const shell::Args& a) {
  if (g_audio_in == nullptr) {
    sh.out().text("mon: not in this image\n");
    return;
  }
  if (a.argc > 2) {
    sh.out().text("usage: mon [off|on|0..100]\n");
    return;
  }
  if (a.argc == 2) {
    std::uint32_t level = 0;
    if (same(a.argv[1], "off")) {
      level = 0;
    } else if (same(a.argv[1], "on")) {
      level = app::AudioIn::kMonitorOn;
    } else if (!parse_dec(a.argv[1], 100u, level)) {
      sh.out().text("usage: mon [off|on|0..100]\n");
      return;
    }
    g_audio_in->monitor(level);
  }
  char buf[160];
  audio::diag::Text t(buf, sizeof buf);
  t.str("monitor ").dec(g_audio_in->monitor()).str(" %, input ").dec(static_cast<std::uint32_t>(g_audio_in->source())).str(" (0 line .. 5 radio), gain ").dec(g_audio_in->gain());
  t.str(" %, the gate is ").str(g_audio_in->settling() ? "shut (settling)" : "open").str("; mon on = ").dec(app::AudioIn::kMonitorOn).str(", mon off = 0").nl();
  send(sh, t.size(), buf);
}

// `rec` (slice RC of M2c; track3r-os had none): what the app's recorder is doing, for a take made from the app's keys: its state, the frames written (and as seconds), the frames that did not
// fit (0 for a good take), the tap and the pattern length. A Level take counts its pre-roll ring in the frames. A read.
app::Recorder* g_recorder = nullptr;

void cmd_rec(shell::Shell& sh, const shell::Args& a) {
  if (g_recorder == nullptr || !g_recorder->available()) {
    sh.out().text("rec: this image has no recorder\n");
    return;
  }
  if (a.argc > 1) {
    sh.out().text("usage: rec\n");
    return;
  }
  const char* state = "idle";
  switch (g_recorder->state()) {
    case hal::record::State::Idle: break;
    case hal::record::State::Waiting: state = "waiting"; break;
    case hal::record::State::Listening: state = "listening"; break;
    case hal::record::State::Recording: state = "recording"; break;
    case hal::record::State::Done: state = "done"; break;
  }
  const std::uint32_t frames = g_recorder->frames();
  const std::uint32_t rate = (hal::audio::sample_rate_mhz() + 500u) / 1000u;
  const std::uint32_t ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(frames) * 1000u / rate);
  char buf[160];
  audio::diag::Text t(buf, sizeof buf);
  t.str("rec ").str(state).str(", frames ").dec(frames).str(" (").dec(ms / 1000u).chr('.').dec(ms % 1000u / 100u).str(" s), overrun ").dec(g_recorder->overrun());
  t.str(", tap ").str(g_recorder->tap() == hal::record::Tap::Master ? "master" : "input").nl();
  send(sh, t.size(), buf);
}

// ---- OS-HIL: input injection (console/inject_commands.h), the screen, the pad LEDs ------------------------------------------------------------
// The scanner's queue is the one the key chips feed, both ends in the main loop, and so is the console: the injector's pushes never race the scan.

// No injection while the firmware update path is mid-flight: a token is armed, a screen's check waits for its INSTALL question (OS-FWCONFIG: only a pressed key answers it), or a
// put / arm / hand-over is running (the arm token is bound to what the unit shows, and a stray key must not be what cancels or confirms anything).
class BoardInjectPort final : public InjectPort {
 public:
  bool input_up() noexcept override { return g_tee != nullptr; }
  bool allowed() noexcept override { return !update::device::service().blocks_injection(); }
  bool push(core::InputKind kind, std::uint8_t id, std::int16_t value) noexcept override { return input::device::scanner().inject(kind, id, value); }
  std::uint32_t now_ms() noexcept override { return uptime_ms32(); }
  void log(const char* text) noexcept override { console::log(text); }
};
BoardInjectPort g_inject_port;
InjectCommands g_inject(g_inject_port);

constexpr std::size_t kStateBuffer = 384;

void cmd_leds(shell::Shell& sh, const shell::Args&) {
  const input::PadLeds& leds = input::device::pad_leds();
  char buf[kStateBuffer];
  audio::diag::Text t(buf, sizeof buf);
  unsigned lit = 0;
  for (unsigned i = 0; i < hal::kPadLedCount; ++i) {
    lit += leds.level(i) != 0u ? 1u : 0u;
  }
  t.str("leds online=").chr(leds.online() ? '1' : '0').str(" errors=").dec(leds.errors()).str(" reinits=").dec(leds.reinits());
  t.str(" lit=").dec(lit).str(" sync=").chr(leds.in_sync() ? '1' : '0').str(" levels=");
  for (unsigned i = 0; i < hal::kPadLedCount; ++i) {  // pad 0 first, two hex digits a pad: the staged level (the driver applies its gamma)
    t.chr("0123456789ABCDEF"[leds.level(i) >> 4]).chr("0123456789ABCDEF"[leds.level(i) & 0xFu]);
  }
  sh.out().text(buf, t.size()).nl();
}

// The chip as `shot` uses it. Everything here is the main loop's, and so is the display driver: a transaction of ours never overlaps one of its own.
class ChipShotPort final : public ShotPort {
 public:
  bool ready() noexcept override {
    const display::Ft812Display& d = display::device::display();
    return d.ready() && !d.failed() && !d.needs_assets();
  }
  bool bus_free() noexcept override { return display::device::display().bus_idle(); }
  bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t width_param, std::uint32_t rows) noexcept override {
    const std::uint32_t words[5] = {display::ft812::kCmdSnapshot2, format, addr, y << 16, (rows << 16) | (width_param & 0xFFFFu)};
    display::Chip& chip = display::device::chip();
    // OS-SIMKEY2, `shot ... pclk0`: the Guide's way, the panel clock off while the band is taken (the default leaves it running, H-2). Restored by every way out of a band below.
    if (stop_clock_ && !clock_stopped_) {
      saved_pclk_ = chip.rd8(display::ft812::reg::kPclk);
      chip.wr8(display::ft812::reg::kPclk, 0);
      clock_stopped_ = saved_pclk_ != 0u;
    }
    if (!chip.cmd_write(words, 5, 20)) {
      restore_clock();
      return false;
    }
    return true;
  }
  Capture poll_capture() noexcept override {
    display::Chip& chip = display::device::chip();
    if (chip.cmd_check()) {
      restore_clock();
      return Capture::Fault;  // (recovered: the coprocessor is running again)
    }
    const std::uint32_t read = chip.rd16(display::ft812::reg::kCmdRead) & 0xFFFu;  // (the 12-bit ring pointers, read as the driver's own cmd_check does)
    const std::uint32_t write = chip.rd16(display::ft812::reg::kCmdWrite) & 0xFFFu;
    if (read != write) {
      return Capture::Pending;
    }
    restore_clock();
    return Capture::Done;
  }
  void abort_capture() noexcept override {
    display::device::chip().cmd_recover();  // the driver's own recovery (CPURESET, pointers zeroed); blocks 10 ms, on a failure path only
    restore_clock();
  }
  void stop_clock(bool on) noexcept override { stop_clock_ = on; }
  std::uint32_t reinits() noexcept override { return display::device::display().reinit_count(); }
  void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept override { display::device::chip().read_block(addr, out, n); }
  std::uint32_t frames() noexcept override { return display::device::display().stats().frames; }
  std::uint32_t now_ms() noexcept override { return uptime_ms32(); }

 private:
  void restore_clock() noexcept {
    if (clock_stopped_) {
      display::device::chip().wr8(display::ft812::reg::kPclk, saved_pclk_);
      clock_stopped_ = false;
    }
  }
  bool stop_clock_ = false;     // `shot ... pclk0`: REG_PCLK 0 for the time of a band (main loop only)
  bool clock_stopped_ = false;  // REG_PCLK is 0 now because of us, saved_pclk_ is what it was
  std::uint8_t saved_pclk_ = 0;
};
ChipShotPort g_shot_port;
ShotStream g_shot;
bool (*g_screenshot_active)() noexcept = nullptr;  // the image's screenshot service says it is running (bind_screenshot)

bool shot_stream(shell::Shell& sh, void*) { return g_shot.step(sh.out(), g_shot_port); }

}  // namespace

bool shot_active() noexcept { return g_shot.active(); }

void bind_screenshot(bool (*active)() noexcept) noexcept { g_screenshot_active = active; }

ShotPort& screenshot_port() noexcept { return g_shot_port; }  // (a getter: the screenshot service, when there is one, clears `pclk0` itself when it starts a capture)

namespace {

// `shot [565|8888 [w]]`: 565 (default) takes two bands of 240 rows, 8888 (32-bit pixels) six of 80; `w` is what CMD_SNAPSHOT2 is given as its width
// (default default_width_param: 800 for 565, 1600 for 8888; the chip counts w in 16-bit units), for a hardware session that finds the picture sheared: a larger w
// makes the bands shorter (ShotStream::start sizes them by what the chip may write, so the region is never overrun). A key press ends the stream and drops a snapshot that is still queued.
// (The region is ui::plot's kRamGShot, the spare RAM_G up to the app's; the image's screenshot service (Shift + Power) shares it, so `shot` is refused while one is being saved.)
void cmd_shot(shell::Shell& sh, const shell::Args& a) {
  ShotParams params;
  std::uint32_t w = 0;
  // A trailing `pclk0` (OS-SIMKEY2) stops the panel clock while a band is taken, the Guide's recipe; rects were 0.545 of their colour in captures without it.
  std::size_t argc = a.argc;
  const bool pclk0 = argc >= 2 && same(a.argv[argc - 1], "pclk0");
  if (pclk0) {
    --argc;
  }
  if (argc > 3 || (argc >= 2 && !same(a.argv[1], "565") && !same(a.argv[1], "8888")) || (argc == 3 && (!parse_dec(a.argv[2], 4095u, w) || w == 0u))) {
    sh.out().text("usage: shot [565|8888 [w]] [pclk0]\n");
    return;
  }
  if (g_screenshot_active != nullptr && g_screenshot_active()) {  // OS-SCREENSHOT: the same RAM_G region
    sh.out().text("ERR shot: a screenshot is being saved\n");
    return;
  }
  g_shot_port.stop_clock(pclk0);
  if (argc >= 2 && same(a.argv[1], "8888")) {
    params.format = ShotFormat::Argb8;
  }
  params.width_param = argc == 3 ? w : default_width_param(params.format);
  if (!g_shot.start(params, ui::plot::kRamGShot, ui::plot::kRamGShotBytes)) {
    sh.out().text("ERR shot: the RAM_G region cannot hold bands of that width (try a smaller w)\n");
    return;
  }
  log("shot: started by the console");
  sh.start_stream(&shot_stream, nullptr, true);
}

constexpr std::uint32_t kResetDrainMs = 40;

void cmd_reset(shell::Shell& sh, const shell::Args&) {
  sh.out().text("resetting (SYSRESETREQ)\n");
  sh.out().flush();
  // OS-OUTON: the outputs are open from boot, so the way out is quiet: the amplifier key first, then the codec's outputs and its DAC (two register writes, no ramp to wait for: the DAC
  // ramps down by itself in the codec while the packets below leave), then the reset (reset_chip drops the key again, a no-op here). Idempotent, and plain when the audio never started.
  audio_device().mute();
  // Give the packets time to leave: a bounded wait of at most kResetDrainMs, then the reset whatever the host did.
  const std::uint64_t start = system_clock().uptime_ms();
  while (system_clock().uptime_ms() - start < kResetDrainMs) {
    asm volatile("wfi");
  }
  boot::reset_chip();
}

// `fw ...` (OS-006c): everything is in update/fw_service.cpp; this binds the image's service (the store on the SD volume, the board's hand-over) to the shell.
void cmd_fw(shell::Shell& sh, const shell::Args& a) { update::device::service().run(sh, a); }

// `cache [on|off|inv]` (OS-PERF3, src/cache.cpp, lmem.h). (`cache check` is the bench image's in track3r-os; o3-sdk has no bench.)
void cmd_cache(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 2) {
    bool ok = true;
    if (same(a.argv[1], "on")) {
      ok = cache::enable();
    } else if (same(a.argv[1], "off")) {
      ok = cache::disable();
    } else if (same(a.argv[1], "inv")) {
      ok = cache::invalidate_all();
    } else if (same(a.argv[1], "check")) {
      sh.out().text("ERR cache check is not in this image\n");
      return;
    } else {
      sh.out().text("usage: cache [on|off|inv|check]\n");
      return;
    }
    if (!ok) {
      sh.out().text("ERR the cache command did not finish\n");
    }
  } else if (a.argc != 1) {
    sh.out().text("usage: cache [on|off|inv|check]\n");
    return;
  }
  char buf[160];
  audio::diag::Text t(buf, sizeof buf);
  t.str("code cache ").str(cache::enabled() ? "on" : "off").str("  PCCCR ").hex32(cache::control()).str("  PCCRMR ").hex32(cache::region_modes());
  t.str("  (8 KiB, 2 ways, 16 B lines; flash and the 0x0800_0000 SDRAM alias only)\n");
  send(sh, t.size(), buf);
}

// `midi` (slice MD, o3-sdk's own): the MIDI service's snapshot and the drivers' counters, as midi_report.h words. A read: nothing is written. The report is 1.2 KB at its worst (console/midi_report.h pins that it fits the buffer).
const midi::MidiService* g_midi = nullptr;

void cmd_midi(shell::Shell& sh, const shell::Args& a) {
  if (a.argc != 1) {
    sh.out().text("usage: midi\n");
    return;
  }
  MidiFigures figures;
  midi::MidiService::Diag diag;  // (about 0.4 KB on this stack: the main loop's)
  if (g_midi != nullptr) {
    g_midi->diag(diag);
    figures.service = &diag;
    figures.din_rx = &midi_io::rx_stats();
    figures.din_tx = &midi_io::tx_stats();
    figures.usb = &usb0::midi_bind_stats();
  }
  char buf[kTextBuffer];
  send(sh, format_midi_report(figures, buf, sizeof buf), buf);
}

// `power ...` (OS-POWER P1, power/src/power_host_mk66.cpp): the Power switch and the shutdown. The stages are in the diag log as `power: <stage> +N ms` (`log` right after `power off`: the CDC ends at USB off).
// (track3r-os's handler, line for line.)
const char* power_refusal_text(power::Refusal why) {
  switch (why) {
    case power::Refusal::Firmware: return "a firmware update is running";
    case power::Refusal::Export: return "an export is running (stop it first)";
    case power::Refusal::Disk: return "the USB disk may be written by the computer (eject it first)";
    case power::Refusal::Running: return "already powering off";
    case power::Refusal::Held: return "Power is down (or reads pressed: check the polarity with power)";
    case power::Refusal::None: break;
  }
  return "";
}

void cmd_power(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 1) {
    char buf[kTextBuffer];
    send(sh, power::format_power_report(power::device::report(), buf, sizeof buf), buf);
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "press")) {
    power::device::inject(power::Inject::Pressed);
    log("power: switch held by the console");
    sh.out().text("power: the console holds the switch down (`power release` lets go, `power pin` reads the pin again)\n");
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "release")) {
    power::device::inject(power::Inject::Released);
    log("power: switch released by the console");
    sh.out().text("power: the console holds the switch up (`power pin` reads the pin again)\n");
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "pin")) {
    power::device::inject(power::Inject::Real);
    sh.out().text("power: the hold reads the pin\n");
    return;
  }
  if ((a.argc == 2 || a.argc == 3) && same(a.argv[1], "off") && (a.argc == 2 || same(a.argv[2], "dry"))) {
    const bool dry = a.argc == 3;
    const power::Refusal why = power::device::request_off(uptime_ms32(), dry);
    if (why != power::Refusal::None) {
      sh.out().text("ERR power: ").text(power_refusal_text(why)).nl();
      return;
    }
    sh.out().text(dry ? "power: off dry: the stages 0 to E, USB off, the lights out, then a restart after Power is released\n"
                      : (power::device::vlpr_off() ? "power: off: the stages 0 to F, USB off, the lights out, then the VLPR off after Power is released (a press boots the unit)\n"
                                                   : "power: off: the stages 0 to F, USB off, the lights out, then a restart after Power is released (built without the VLPR off)\n"));
    return;
  }
  sh.out().text("usage: power [press|release|pin|off [dry]]\n");
}

// Once the one-way commit has happened (D2) the shell answers every command that could start anything with `ERR powering off`; the reads that diagnose the shutdown stay (commands.h kPowerReads,
// pinned by mk66_console_core_test). The decision is console/core.h's power_gate_text, over the command's name (an app row cannot carry a kit name, so it is refused).
const char* power_gate(const shell::Command& cmd) { return power_gate_text(power::device::locked(), cmd.name); }

// ---- The shell ---------------------------------------------------------------------------------------------------------------------
// Built by attach(): the table has the app's rows, and the shell takes its table at construction. Placement-new into static storage, once.
CommandTable g_commands;
ShellTable g_table;
alignas(shell::Shell) unsigned char g_shell_storage[sizeof(shell::Shell)];
alignas(shell::ConsolePump) unsigned char g_pump_storage[sizeof(shell::ConsolePump)];
shell::Shell* g_shell = nullptr;
shell::ConsolePump* g_pump = nullptr;
bool g_dtr_seen = false;

}  // namespace

// ---- The log -------------------------------------------------------------------------------------------------------------------
void init_log() noexcept { g_log.init(g_log_slots, kLogRecords); }

void log(const char* text) noexcept { g_log.write(boot::uptime_ms_low(), text); }

void log_dec(const char* text, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(text).chr(' ').dec(value);
  log(buf);
}

void log_hex(const char* text, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(text).chr(' ').hex32(value);
  log(buf);
}

// The clock fault detail of a failed audio start, as two log lines (a line is cut at 55 characters): where it stopped, then MCR and MDR.
void log_audio_clock_fault(const char* prefix, const AudioClockFault& fault) noexcept {
  if (fault.where == AudioClockFault::None) {
    return;
  }
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(prefix).str(" clock fault at ").str(audio_fault_where_name(fault.where));
  log(buf);
  audio::diag::Text u(buf, sizeof buf);
  u.str(prefix).str(" MCR ").hex32(fault.mcr).str(" MDR ").hex32(fault.mdr);
  log(buf);
}

// ---- The console ---------------------------------------------------------------------------------------------------------------
void note_boot_fault(BootFault fault) noexcept { g_boot_faults |= static_cast<std::uint32_t>(fault); }
void clear_boot_fault(BootFault fault) noexcept { g_boot_faults &= ~static_cast<std::uint32_t>(fault); }

void note_boot_crash(const crash::CrashRecord* record) noexcept {
  if (record != nullptr) {
    g_boot_crash = *record;
    g_have_boot_crash = true;
  }
}

hal::Input& tap_input(hal::Input& source) noexcept {
  if (g_tee == nullptr) {
    g_tee = new (g_tee_storage) TeeInput(source);
  }
  return *g_tee;
}

void lock_input() noexcept {
  if (g_tee != nullptr) {
    g_tee->lock();
  }
}

void hold_input(bool held) noexcept {
  if (g_tee != nullptr) {
    g_tee->hold(held);
  }
}

void attach(app::Runtime& runtime) noexcept {
  if (g_shell != nullptr) {
    return;  // once
  }
  appimg::bind_engine(runtime.context().engine());
  appimg::bind_files(runtime.context().files());
  g_files = &runtime.context().files();
  g_audio_in = &runtime.context().audio_in();
  g_recorder = &runtime.context().recorder();
  g_commands.assemble(runtime.context().app(), &log);
  shell::CommandFn kit[kKitCommandCount] = {};
  kit[static_cast<unsigned>(Cmd::Help)] = &help_command;
  kit[static_cast<unsigned>(Cmd::Version)] = &cmd_version;
  kit[static_cast<unsigned>(Cmd::Stage)] = &cmd_stage;
  kit[static_cast<unsigned>(Cmd::Cpu)] = &cmd_cpu;
  kit[static_cast<unsigned>(Cmd::Mem)] = &cmd_mem;
  kit[static_cast<unsigned>(Cmd::Crash)] = &cmd_crash;
  kit[static_cast<unsigned>(Cmd::Usb)] = &cmd_usb;
  kit[static_cast<unsigned>(Cmd::Log)] = &cmd_log;
  kit[static_cast<unsigned>(Cmd::Input)] = &cmd_input;
  kit[static_cast<unsigned>(Cmd::Peek)] = &cmd_peek;
  kit[static_cast<unsigned>(Cmd::Poke)] = &cmd_poke;
  kit[static_cast<unsigned>(Cmd::Rot)] = &cmd_rot;
  kit[static_cast<unsigned>(Cmd::Sd)] = &cmd_sd;
  kit[static_cast<unsigned>(Cmd::Ls)] = &cmd_ls;
  kit[static_cast<unsigned>(Cmd::Reset)] = &cmd_reset;
  kit[static_cast<unsigned>(Cmd::Fw)] = &cmd_fw;
  kit[static_cast<unsigned>(Cmd::Key)] = &key_command;
  kit[static_cast<unsigned>(Cmd::Pad)] = &pad_command;
  kit[static_cast<unsigned>(Cmd::Enc)] = &enc_command;
  kit[static_cast<unsigned>(Cmd::State)] = &state_command;
  kit[static_cast<unsigned>(Cmd::Shot)] = &cmd_shot;
  kit[static_cast<unsigned>(Cmd::Leds)] = &cmd_leds;
  kit[static_cast<unsigned>(Cmd::Cache)] = &cmd_cache;
  kit[static_cast<unsigned>(Cmd::Power)] = &cmd_power;
  kit[static_cast<unsigned>(Cmd::Guard)] = &cmd_guard;
  kit[static_cast<unsigned>(Cmd::Midi)] = &cmd_midi;
  kit[static_cast<unsigned>(Cmd::Files)] = &cmd_files;
  kit[static_cast<unsigned>(Cmd::Radio)] = &cmd_radio;
  kit[static_cast<unsigned>(Cmd::Mon)] = &cmd_mon;
  kit[static_cast<unsigned>(Cmd::Rec)] = &cmd_rec;
  kit[static_cast<unsigned>(Cmd::Disk)] = &cmd_disk;
  g_table.build(g_commands, runtime, kit);
  bind_inject(&g_inject);
  g_shell = new (g_shell_storage) shell::Shell(g_table.rows(), g_table.size(), shell::Output(shell::Sink{&sink_write, &sink_flush, nullptr, &sink_room}));
  g_shell->set_gate(&power_gate);  // OS-POWER P1 (D2): after the one-way commit the shell refuses what could start anything
  g_pump = new (g_pump_storage) shell::ConsolePump(*g_shell, usb0::cdc_rx());
}

void attach_midi(const midi::MidiService& service) noexcept { g_midi = &service; }

void attach_capture(hal::capture::CaptureHw& capture) noexcept { g_capture = &capture; }

void note_usb_started() noexcept {
  guard_layout(usb0::mode() == o3sdk::usb::Mode::Audio ? UsbModeGuard::Layout::Audio : UsbModeGuard::Layout::Standard);
}

void note_usb_layout_requested() noexcept {
  const o3sdk::usb::Mode mode = usb0::change_phase() != 0u ? usb0::pending_mode() : usb0::mode();
  guard_layout(mode == o3sdk::usb::Mode::Audio ? UsbModeGuard::Layout::Audio : UsbModeGuard::Layout::Standard);
}

void service() noexcept {
  if (g_shell == nullptr) {
    return;  // attach() has not run
  }
  g_inject.service();  // OS-HIL: the releases of taps and holds, whether or not a terminal is open
  if (g_shot.active() && !g_shell->streaming()) {
    g_shot.cancel(g_shot_port);  // a key press (or the terminal closing) ended the stream
  }
  update::device::service().poll(*g_shell);  // OS-006c: a hand-over that has started ends by its own clock, with or without a terminal
  usb0::service();  // OS-006d: a STANDARD / AUDIO switch in progress (drain, detach, attach)
  if (!usb0::attached()) {
    if (g_dtr_seen) {  // the device detached for a mode change: the terminal that was open is gone, as if DTR had dropped
      g_shell->stop_stream();
      update::device::service().terminal_closed();
      g_dtr_seen = false;
    }
    return;
  }
  // What the host typed, with back-pressure (usb/diag_pump.h): input waits while the output has no room for a reply or a pager is running.
  g_pump->service();
  const bool dtr = usb0::control().cdc().dtr();
  if (dtr && !g_dtr_seen) {
    update::device::service().terminal_closed();  // a stale upload or token of a terminal that vanished without DTR dropping (OS-006c)
    g_shell->begin(kBanner);  // a terminal opened the port
  } else if (!dtr && g_dtr_seen) {
    g_shell->stop_stream();   // it closed: a stream (and a raw receiver) has no one to talk to
    update::device::service().terminal_closed();  // OS-006c: an upload in progress is abandoned and its temporary file deleted
  }
  g_dtr_seen = dtr;
  g_shell->tick();
  usb0::cdc_writer().flush();
  // The safety net under the AUDIO set (usb_mode_guard.h): a host that is there and does not configure it gets the console-only set back. The layout keeps the profile and the return,
  // so a later `usb mode audio` comes back as the boot layout (P8, return on).
  if (g_guard.tick(uptime_ms32(), usb0::control().configured(), usb0::device_counters().bus_resets, usb0::change_phase() != 0u) == UsbModeGuard::Action::RequestStandard) {
    if (usb0::request_layout(o3sdk::usb::Mode::Standard, usb0::profile(), usb0::return_enabled())) {
      guard_layout(UsbModeGuard::Layout::Standard);
      log("usb: AUDIO not configured in 10 s, back to STANDARD");
    } else {
      log("usb: AUDIO not configured in 10 s, STANDARD refused");
    }
  }
}

}  // namespace o3sdk::platform::mk66::console
