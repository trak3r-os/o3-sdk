// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The `diag` console on the CDC port, and the log it reads (ADR 0011 item 1, OS-006b). Implemented in the MK66 image (src/console.cpp,
// src/log_store.cpp); the declarations compile everywhere. The line shell and the log ring are the usb library's (usb/diag_shell.h,
// usb/diag_log.h); the commands that need the platform's data live here, and their text is in usb/diag_text.h.
//
//   help  version  stage  cpu  mem  crash  usb  log  input  mix  mon  peek  poke  audio  rot  sd  ls  load  reset
//
// Out of scope and maintainer gated (ADR 0011 item 3): `update`, the bootloader pin and anything that writes `_fwinfo`. `reset` mutes the
// outputs (OS-OUTON) and is a SYSRESETREQ and nothing else.

#include <o3sdk/hal/input.h>
#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/crash_record.h>
#include <o3sdk/platform/mk66/usb/boot_faults.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

// ---- The log: any context may write ---------------------------------------------------------------------------------------
// The ring is 1,024 records of 64 bytes in the `diag_log` region of the SDRAM map (ADR 0013 D7: session only, SDRAM does not
// survive power-off). init_log() runs once in the main thread right after the SDRAM has been tested (the test overwrites everything);
// until then log() does nothing. Text is cut at 55 characters. A write is one atomic increment, a 64-byte copy and two stores: safe from
// an ISR, never blocks. Every line is stamped with the SysTick millisecond count.
void init_log() noexcept;
void log(const char* text) noexcept;
void log_dec(const char* text, std::uint32_t value) noexcept;  // "text 1234"
void log_hex(const char* text, std::uint32_t value) noexcept;  // "text 0x000004D2"

// The detail of a ClockFault (audio.h) as two lines, "<prefix> clock fault at mdr" and "<prefix> MCR 0x... MDR 0x...". Nothing when `where` is None.
void log_audio_clock_fault(const char* prefix, const AudioClockFault& fault) noexcept;

// ---- The console ---------------------------------------------------------------------------------------------------------------
// The record that the previous run left, kept for `diag crash` (main erases the noinit copy once the LED code has been played, so the
// console keeps its own copy for this run). nullptr when there was none.
void note_boot_crash(const crash::CrashRecord* record) noexcept;

// Records a fault the image went on after (boot_faults.h); `diag stage` lists them. Main thread; a fault is never cleared in a run.
void note_boot_fault(BootFault fault) noexcept;
// OS-HW1: a fault the image went on after was cleared by a later success (`diag audio start`).
void clear_boot_fault(BootFault fault) noexcept;

// Wraps the app's input source: every event the app polls is also offered to `diag input` (a small ring that drops what nobody reads,
// so it costs one push per event). Returns the wrapper to give the app; call once with the real source (a second call returns the
// same wrapper).
hal::Input& tap_input(hal::Input& source) noexcept;

// OS-POWER P1 (D2): the shutdown has begun. Every event of the key scan is read and dropped from here (the app is given nothing: no key, pad or encoder turn does anything); never undone. The shell's own
// refusal after the commit is power_gate in console.cpp (`ERR powering off`). A no-op before tap_input() was called.
void lock_input() noexcept;

// One pass of the main loop: feeds what the host typed to the shell, announces the banner when a terminal opens the port (DTR), runs a
// stream and publishes the output. Bounded: it reads at most the receive ring's depth and a stream says a bounded amount per call.
void service() noexcept;
// A change of the USB set was asked for from outside the console (the app's Config screen, the card's hand-over: usb_audio_device.cpp) and is on its way: the mode guard watches the set it ends in
// (usb_mode_guard.h), as it does for the console's own `usb mode`. Main thread.
void note_usb_layout_requested() noexcept;

// OS-SCREENSHOT: the console's `shot` stream is running (the screenshot service refuses meanwhile: the two share the RAM_G region), and the chip port `shot` reads through (a getter: it
// changes nothing; the service clears a `pclk0` the console left set when it starts a capture). Main loop only.
class ShotPort;
bool shot_active() noexcept;
ShotPort& screenshot_port() noexcept;

// OS-SCREENSHOT, spec Q4: once a pass, `held` = a capture holds the app. While it does, the presses of keys and pads and the encoder turns are dropped, and the releases of keys that were down
// before are kept and handed to the app after (usb/input_hold.h). A no-op before tap_input() was called.
void hold_input(bool held) noexcept;

}  // namespace o3sdk::platform::mk66::console
