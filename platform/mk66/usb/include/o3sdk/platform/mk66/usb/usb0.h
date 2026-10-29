// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's USB0 device driver (OS-006b): the register policy, the objects of the pure usb library wired to the chip, the interrupt
// handler and the bring-up. Implemented in the MK66 image only (src/usb0.cpp); the declarations compile everywhere so the header
// check passes on every platform. The logic (control pipe, endpoints, rings) is usb and is tested on the host against a fake
// controller; what is here is the part that touches registers: README.md, "USB" lists every register sequence with its source.
//
// Two descriptor sets (the maintainer's identity 16D0:0E9F "Track3r" in both, told apart by bcdDevice): STANDARD (MIDI + CDC `diag`) and, since OS-006d, AUDIO
// (UAC2 stems IN + stereo return OUT with explicit feedback, plus MIDI and CDC; ADR 0012). The set of the first enumeration is the caller's (start(): since OS-USBAUDIO the device
// setting USB AUDIO decides it, default AUDIO; before it every boot was STANDARD), and a later change is request_mode() from the main loop (the `diag usb mode` command, which is
// runtime only; Config's USB AUDIO and a project's USB part): the device detaches, waits, and attaches with the other set. The firmware-update path (OS-006c) and `diag` run over
// the CDC function, present in both.
//
// Contexts: start() runs once in the main thread; the interrupt handler (USB0_IRQHandler, priority 64, ADR 0013 D1) runs the
// usb::UsbDevice and touches the buffer descriptors and the endpoints alone; everything the main loop and the engine share with it is
// a lock-free ring (usb/device.h).

#include <o3sdk/platform/mk66/midi/in.h>
#include <o3sdk/platform/mk66/midi/time_ref.h>
#include <o3sdk/platform/mk66/midi/usb_bind.h>
#include <o3sdk/platform/mk66/midi/usb_out.h>
#include <o3sdk/usb/cdc.h>
#include <o3sdk/usb/control.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/device.h>
#include <o3sdk/usb/iso_audio.h>

#include <cstdint>

namespace o3sdk::platform::mk66::usb0 {

// The module is on only between an attach and the next detach (step 4 of the sequence in usb0.cpp is shared by start() and every attach, so a runtime attach starts from the state of a cold start).
// Brings the module up and, last, switches the D+ pull-up on so that the host sees the device: IRC48M clock and its recovery, the
// system MPU grant, the buffer descriptor table, the endpoint 0 receive descriptors, the interrupt. Returns false, with the pull-up
// OFF and the reason in the log, if a precondition fails (the descriptor table is not 512-byte aligned, the descriptors did not build or
// are inconsistent). Call once, in the main thread, after the clock tree, the SDRAM, the interrupt priorities and the log.
// The descriptor set of that first enumeration is `mode` with `profile` and `usb_return` (OS-USBAUDIO: chosen before the pull-up, so the host sees one device and no STANDARD-then-AUDIO
// switch). A profile the engine cannot feed, the DISK set (not in o3-sdk), or descriptors that do not build, fall back to STANDARD (logged). In STANDARD the profile and the return are only remembered for a later AUDIO attach.
bool start(o3sdk::usb::Mode mode, o3sdk::usb::Profile profile, bool usb_return) noexcept;

// True once start() switched the pull-up on.
bool attached() noexcept;

// Read-only views for `diag usb` and the MIDI binding (OS-005). The MIDI rings are the engine's ends of the two MIDI streams: the
// engine writes midi_to_host() and reads midi_from_host(), each from one context. Since OS-005c2 that one context is the USB interrupt itself
// (bind_midi below): it is the only writer of midi_to_host() (the frame's events from the MIDI timing core) and the only reader of
// midi_from_host() (the host's bytes, stamped), so nothing else may touch either ring.
const o3sdk::usb::DeviceCounters& device_counters() noexcept;
const o3sdk::usb::ControlPipe& control() noexcept;
const o3sdk::usb::Counters& stream_counters() noexcept;
o3sdk::usb::CdcTxWriter& cdc_writer() noexcept;
o3sdk::usb::CdcRxRing& cdc_rx() noexcept;
o3sdk::usb::MidiRing& midi_to_host() noexcept;
o3sdk::usb::MidiRing& midi_from_host() noexcept;
const char* serial() noexcept;

// OS-005c2: binds the two MIDI rings to the MIDI timing core (midi/usb_bind.h). From now on the USB interrupt (priority 64) is the ONE consumer of `out` (the frame's events
// go into midi_to_host() before the device logic arms the bulk-IN buffer, see usb0.cpp) and the ONE producer of `in` (the host's bytes, stamped at the interrupt's
// entry), and it reads `ref` for the time. The three objects must outlive the USB interrupt (statics); call once, main thread, before or after start() (o3-sdk's main.cpp binds first: the interrupt then has it from its first pass): the binding is
// published with interrupts masked, so the interrupt sees all of it or none. Until then the from-host ring is simply overwritten and nothing is sent to the host.
void bind_midi(o3sdk::platform::mk66::midi::UsbMidiOut& out, o3sdk::platform::mk66::midi::RtInRing& in, const o3sdk::platform::mk66::midi::TimeRef& ref) noexcept;
const o3sdk::platform::mk66::midi::UsbBindStats& midi_bind_stats() noexcept;

// ---- AUDIO mode (OS-006d, ADR 0012) --------------------------------------------------------------------------------------------------------
// The descriptor set presented now, and the profile it carries (the channel layout of the stems stream), and whether it carries the return stream
// (USB RETURN, OS-USBRET: false is capture only, no OUT interface and no feedback endpoint). The layout of a boot is profile 8 with the return on.
o3sdk::usb::Mode mode() noexcept;
o3sdk::usb::Profile profile() noexcept;
bool return_enabled() noexcept;

// Asks for another descriptor set. Non-blocking: the change is carried out by service() from the main loop, in three steps: wait a moment for the
// console's reply to leave over the CDC port, detach (pull-up off, streams stopped, the stem tap off), wait for the host to notice, then rebuild the
// descriptors and attach again. The host re-enumerates (CDC `diag` terminal sessions end; MIDI and the firmware-update path come back with the new set).
// False, nothing changed, when a change is already in progress, the profile is one the engine cannot feed (P4S), or the driver is not running.
// Asking for the set that is presented now is a successful no-op. The DISK set (Mode::Disk) is refused.
bool request_mode(o3sdk::usb::Mode mode, o3sdk::usb::Profile profile) noexcept;  // keeps the current USB RETURN
// The same with USB RETURN chosen too. In AUDIO a different mode, profile or return re-enumerates; in STANDARD (whose descriptors depend on none of them) the
// choice is only remembered for the next AUDIO attach.
bool request_layout(o3sdk::usb::Mode mode, o3sdk::usb::Profile profile, bool usb_return) noexcept;

// 0: nothing in progress, 1: waiting for the reply to leave, 2: detached. The target of a change in progress.
std::uint8_t change_phase() noexcept;
o3sdk::usb::Mode pending_mode() noexcept;
o3sdk::usb::Profile pending_profile() noexcept;
bool pending_return() noexcept;

// The state machine of request_mode(). Main loop, every pass (console::service() calls it). No waits beyond a few cycles.
void service() noexcept;

// OS-POWER P1 (stage D of the shutdown, docs/specs/OS-POWER.md 4.1): the detach of request_mode() without the attach that follows it. D+ released (the computer sees the device go), the interrupt
// masked, every stream stopped (the stem tap and the return pull follow). Never undone: a mode change in progress is dropped, no later request_layout() is taken and service() does nothing from here;
// the run ends in a restart, which enumerates again. Main loop. Idempotent.
void detach_for_power() noexcept;

// OS-POWER P2 (the VLPR off, docs/specs/OS-POWER.md 4.4 step 3): the 48 MHz oscillator that clocks the module (IRC48M, started by start()) off, because "it should be disabled by software prior to VLPR entry"
// (RM 6.7.2). Clock recovery off first, then the oscillator and its regulator (CLK_RECOVER_IRC_EN cleared). Only after detach_for_power(): the module must be detached, with its interrupt off. A module that was never
// clocked (SIM_SCGC4 gate off) has nothing running and its registers are not touched. Main loop, interrupts masked; no wait, never returns an error. Never undone: the run ends in a reset.
void irc48m_off() noexcept;

// The streams, for `diag usb audio`: null before start(). The getters are plain words, torn at most across fields.
const o3sdk::usb::IsoAudio* iso() noexcept;
// Zeroes the stream counters (`diag usb audio reset`), with the USB interrupt masked for the few words.
void reset_stream_counters() noexcept;

}  // namespace o3sdk::platform::mk66::usb0
