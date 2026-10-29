// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The text of the console's `midi` command (docs/M2B2-PLAN.md section 5, slice MD): one line per thing a hardware session asks about MIDI, each starting `midi:` and made of
// `name value` words, so that o3console's expect patterns and a person read the same. Pure: the image gathers the figures (console.cpp), this formats them, and the host test pins the
// words (mk66_console_midi_test.cpp).
//
//   midi: following yes|no locked yes|no tempo 120.00 bpm (loop 120.00 bpm, clocks N, error N us)
//   midi: config channel omni|N out N ports din+usb|din|usb|none clock-out on|off transport-out on|off transport-in on|off follow off|din|usb
//   midi: rx N tx N dropped N messages N notes N held N polled-dropped N sent N bad-out N
//   midi: clock-out N start-stop-out N transport-in N ignored N requests N attached N released N
//   midi: din rx isrs N bytes N errors N overruns N full N multi N tx irqs N bytes N rt N late-max N us no-time N not-ready N
//   midi: usb frames N events-out N events-in N bytes-in N discarded N no-time N in-full N
//   midi: loop clocks N reacquires N dropouts N missing N
//   midi: last rx D:90 D:3C U:64        (the last non-timing bytes, D = DIN, U = USB, oldest first)
//   midi: last tx 3:90:3C:64            (the last messages sent: ports mask:status:data1:data2 in hex, the clock left out)
//
// The `loop` tempo is the clock-follow loop's whether or not the engine follows it; `tempo` is the master's as the engine gets it (0.00 while not following).

#include <o3sdk/platform/mk66/midi/din_io.h>
#include <o3sdk/platform/mk66/midi/midi_service.h>
#include <o3sdk/platform/mk66/midi/usb_bind.h>

#include <cstddef>

namespace o3sdk::platform::mk66::console {

// The room the console gives the report (console.cpp's text buffer, which it asserts is at least this); the worst case, every counter at its maximum and every line full, is pinned to fit it.
inline constexpr std::size_t kMidiReportRoom = 1536;

// What the report is made of: the service's snapshot and the drivers' counters (single words the interrupts write, read as they are).
struct MidiFigures {
  const midi::MidiService::Diag* service = nullptr;
  const midi::DinRxStats* din_rx = nullptr;
  const midi::DinTxStats* din_tx = nullptr;
  const midi::UsbBindStats* usb = nullptr;
};

// The report into `buf` (at most `cap` bytes with the terminating zero, cut off where it does not fit); returns the length. The worst case is 1,206 bytes.
std::size_t format_midi_report(const MidiFigures& f, char* buf, std::size_t cap) noexcept;

}  // namespace o3sdk::platform::mk66::console
