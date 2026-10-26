// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The firmware switcher's flow (OS-FWCONFIG, docs/M2B2-PLAN.md section 4): the steps of FirmwareView, from the list to the hand-over.
// They see the update path through hal::firmware::FirmwareSink and nothing else (no card access, no pin, no flash), and ask for its steps
// in order, under the spec's rules: two questions, each answered by F1 alone, nothing written before INSTALL, cancel() on every way out.
// Ported from track3r-os app/src/firmware_actions.cpp (class N, firmware_flow.cpp says which bodies are verbatim). The system Config
// screen (system_config.h) is the one caller; the free functions are public so that the flow has its own tests.
//
// `blocker` is App::update_blocker(): what stops an update right now, as the strip says it, or nullptr. The caller asks it when the list
// opens and at INSTALL. Main loop only.

#include <o3sdk/app/firmware_view.h>
#include <o3sdk/app/input.h>
#include <o3sdk/core/tick.h>
#include <o3sdk/hal/firmware.h>

namespace o3sdk::app {

// Reads the list in this one pass (the card has one directory handle) and enters Phase::List. false: it did not open, and `said` is why
// when the caller should tell the user (an app's blocker, or the update path's refusal); nullptr when there is nothing to tell (no
// sink, the sink has no path, or the list is already open).
bool firmware_open(FirmwareView& v, hal::firmware::FirmwareSink* sink, const char* blocker, const char*& said) noexcept;
// F1: UPDATE on the list asks question 1, YES starts the check, INSTALL starts the hand-over. Nothing is written before the last.
bool firmware_go(FirmwareView& v, hal::firmware::FirmwareSink* sink, const char* blocker) noexcept;
// F8: BACK on the list closes it, CANCEL takes a question or a running check back to the list. Once INSTALL has been taken nothing cancels it.
bool firmware_back(FirmwareView& v, hal::firmware::FirmwareSink* sink) noexcept;
// Does this input take the open question back? Any control but F1 alone (and F8, whose binding does it): another button, Shift + F1, a
// pad hit or a turn of the encoder.
bool firmware_question_taken_back(const FirmwareView& v, const Input& in) noexcept;
void firmware_question_drop(FirmwareView& v, hal::firmware::FirmwareSink* sink) noexcept;
// The pass of the main loop, while the list is open: the check and the install are sliced by poll_check(), one slice a call (2 KiB of the
// file). `shown`: the page of the list is on screen. Returns true when the picture changed. Past INSTALL a page that is not shown is
// not closed: the caller puts it back.
bool firmware_poll(FirmwareView& v, hal::firmware::FirmwareSink& sink, core::Tick now, bool shown) noexcept;
// The card was taken out or put in, the page was left, or the unit is switching off: a list that is not past the point of no return
// closes and the sink is cancelled. An install in the hand-over is not touched. true: it closed.
bool firmware_abandon(FirmwareView& v, hal::firmware::FirmwareSink* sink) noexcept;

// "closed", "list", "ask_update", "checking", "ask_install", "installing": the `fw=` of the console's `state`.
const char* firmware_phase_name(FirmwareView::Phase phase) noexcept;

}  // namespace o3sdk::app
