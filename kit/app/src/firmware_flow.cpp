// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware switcher's flow (firmware_flow.h). Class N of docs/M2B2-PLAN.md: the bodies of track3r-os's app/src/firmware_actions.cpp
// (OS-FWCONFIG slice 2a, lines 22-300) are copied verbatim inside this file, with these changes and no others:
//   * `ActionContext` is gone: the functions take the FirmwareView and the sink they act on (the kit's system Config screen owns both);
//   * `blocked(ctx)` is App::update_blocker(): its text arrives as the `blocker` argument;
//   * the dialog's overlay (ViewState::overlay) is the page being on screen: the `shown` argument of firmware_poll, and nothing to clear
//     in leave();
//   * the refusal that firmware_open() said on the strip is returned in `said`;
//   * `core::InputEvent` is the kit's Input; `view.shift` is Input::shift.
// Named verbatim: refusal_text, to_list, step, to_failure, firmware_go and firmware_back (the phase logic), firmware_question_taken_back
// (the cases), firmware_question_drop, firmware_poll and firmware_abandon (the card-change close).
#include <o3sdk/app/firmware_flow.h>

#include "sys_text.h"

#include <cstdint>

namespace o3sdk::app {

namespace {

using hal::firmware::CheckStatus;
using hal::firmware::FirmwareSink;
using hal::firmware::Refusal;

// Every text is a refusal or a failure of one step and fits the dialog's line (FirmwareView::kMsgChars) and the strip (48 characters). NoPath is never asked: open() returns before it when available() is false.
// The texts that two cases share are one string.
constexpr const char* kCheckAgain = "CHECK AGAIN";
constexpr const char* kCheckExpired = "THE CHECK EXPIRED: CHECK AGAIN";

const char* refusal_text(Refusal r) noexcept {
  switch (r) {
    case Refusal::None:
    case Refusal::NoPath: break;
    case Refusal::OnComputer: return "THE CARD IS ON THE COMPUTER";
    case Refusal::StorageBusy: return "THE CARD IS BUSY: TRY AGAIN";
    case Refusal::Busy: return "ANOTHER UPDATE STEP IS RUNNING";
    case Refusal::NoCard: return "NO CARD";
    case Refusal::NotWritable: return "THE CARD CANNOT BE USED FOR AN UPDATE";
    case Refusal::NoFolder: return "NO FIRMWARE FOLDER ON THE CARD";
    case Refusal::NotFound: return "THE FILE IS NOT ON THE CARD ANY MORE";
    case Refusal::BadName: return "THE UPDATE CANNOT TAKE THIS FILE NAME";
    case Refusal::NotChecked: return kCheckAgain;
    case Refusal::Expired: return kCheckExpired;
    case Refusal::Io: return "CARD READ ERROR";
  }
  return "";
}

// The list again, with `text` on the dialog's line ("" for none). The sink's job is over or was never started: whoever calls this has already told it to cancel() when it must.
void to_list(FirmwareView& v, const char* text) noexcept {
  v.phase = FirmwareView::Phase::List;
  v.bad = text[0] != '\0';
  v.msg.assign(text);
  v.done = v.total = 0;
}

// A step of the walk (a question, the check, the hand-over): nothing is left on the line or the bar.
void step(FirmwareView& v, FirmwareView::Phase phase) noexcept {
  to_list(v, "");
  v.phase = phase;
}

// The list again with a failure of the sink on the line: "<head><FAULT>", and " AT <offset>" for a fault found in the file. The fault is the sink's static text, upper-cased with the export's own loop.
void to_failure(FirmwareView& v, const char* head, const char* fault, const std::uint32_t* offset) noexcept {
  char upper[FirmwareView::kMsgChars + 1];
  upper_copy(upper, sizeof upper, fault != nullptr ? fault : "");
  to_list(v, head);
  v.bad = true;
  v.msg.append(upper);
  if (offset != nullptr) {
    v.msg.append(" AT ");
    append_uint(v.msg, *offset);
  }
}

}  // namespace

// The list (track3r-os firmware_open, F8 UPDATE on the INFO page of Config). The refusals are said by the caller: the app's blocker, and whatever the update path itself refuses (the
// card is on a computer, storage is busy, a console `fw` operation runs, no card, no Firmware/ folder). The list is copied in this one pass: the card has one directory handle.
bool firmware_open(FirmwareView& v, FirmwareSink* sink, const char* blocker, const char*& said) noexcept {
  said = nullptr;
  if (sink == nullptr || !sink->available() || v.open()) {
    return false;
  }
  if (blocker != nullptr) {
    said = blocker;
    return false;
  }
  const Refusal why = sink->list_begin();
  if (why != Refusal::None) {
    said = refusal_text(why);
    return false;
  }
  v.close();
  while (v.n < FirmwareView::kMax && sink->list_next(v.files[v.n])) {
    ++v.n;
  }
  hal::firmware::File beyond;
  v.more = v.n == FirmwareView::kMax && sink->list_next(beyond);
  sink->list_end();
  v.phase = FirmwareView::Phase::List;
  return true;
}

// F1 in the dialog: UPDATE on the list asks question 1, YES starts the check, INSTALL starts the hand-over. Nothing is written before the last.
bool firmware_go(FirmwareView& v, FirmwareSink* sink, const char* blocker) noexcept {
  if (sink == nullptr) {
    return false;
  }
  switch (v.phase) {
    case FirmwareView::Phase::List:
      if (v.n == 0) {
        return false;
      }
      step(v, FirmwareView::Phase::AskUpdate);
      return true;
    case FirmwareView::Phase::AskUpdate: {
      const Refusal why = sink->begin_check(v.chosen());
      if (why != Refusal::None) {
        to_list(v, refusal_text(why));  // (a refused check started nothing)
        return true;
      }
      step(v, FirmwareView::Phase::Checking);
      return true;
    }
    case FirmwareView::Phase::AskInstall: {
      if (blocker != nullptr) {  // (the dialog is modal, but a recording or an edit can still come from outside, and 30 s is long: the rules of the opening hold at INSTALL too)
        sink->cancel();
        to_list(v, blocker);
        return true;
      }
      const Refusal why = sink->install();
      if (why != Refusal::None) {
        sink->cancel();  // (nothing was touched; the good check, if it is still there, is let go)
        to_list(v, refusal_text(why));
        return true;
      }
      step(v, FirmwareView::Phase::Installing);  // (the hand-over stops the transport and mutes the outputs itself: FwHost::quiesce)
      return true;
    }
    case FirmwareView::Phase::Closed:
    case FirmwareView::Phase::Checking:
    case FirmwareView::Phase::Installing: break;
  }
  return false;
}

// F8 in the dialog: BACK closes the list, CANCEL takes a question or a running check back to the list. Once INSTALL has been taken nothing cancels it.
bool firmware_back(FirmwareView& v, FirmwareSink* sink) noexcept {
  switch (v.phase) {
    case FirmwareView::Phase::List:
      if (sink != nullptr) {
        sink->cancel();  // (nothing may be left running or waiting behind a closed dialog)
      }
      v.close();
      return true;
    case FirmwareView::Phase::AskUpdate:
      to_list(v, "");
      return true;
    case FirmwareView::Phase::Checking:
    case FirmwareView::Phase::AskInstall:
      if (sink != nullptr) {
        sink->cancel();
      }
      to_list(v, "");
      return true;
    case FirmwareView::Phase::Closed:
    case FirmwareView::Phase::Installing: break;
  }
  return false;
}

// Does this input take the open question back? Any control but F1 alone (and F8, whose binding does it): another button, Shift + F1 (bound to nothing in the dialog), a pad hit or a turn of the
// encoder. The input then acts as usual in the list (OS-KEYROWDLG: a question is answered by its key and taken back by any other). Shift + F8 is F8: it also steps back.
bool firmware_question_taken_back(const FirmwareView& v, const Input& in) noexcept {
  if (!v.asking()) {
    return false;
  }
  switch (in.kind) {
    case core::InputKind::Encoder: return true;
    case core::InputKind::Pad: return in.type == Input::Type::Press;
    case core::InputKind::Button: {
      if (in.type != Input::Type::Press || in.id == static_cast<std::uint8_t>(Button::Shift) || in.id == static_cast<std::uint8_t>(Button::F8)) {
        return false;
      }
      return in.id != static_cast<std::uint8_t>(Button::F1) || in.shift;
    }
    default: return false;
  }
}

void firmware_question_drop(FirmwareView& v, FirmwareSink* sink) noexcept {
  if (v.phase == FirmwareView::Phase::AskInstall && sink != nullptr) {
    sink->cancel();
  }
  to_list(v, "");
}

// The pass, once, while the dialog is open: the check and the install are sliced by poll_check(), one slice a call (2 KiB of the file). Returns true when the picture changed.
bool firmware_poll(FirmwareView& v, FirmwareSink& sink, core::Tick now, bool shown) noexcept {
  if (!v.open()) {
    return false;
  }
  if (!shown) {  // the page was left by a path that is not the dialog's own keys (the dialog is modal, so none does today)
    if (v.phase == FirmwareView::Phase::Installing) {  // past INSTALL nothing cancels or closes it, and "DO NOT POWER OFF" stays on the screen (the caller puts the page back)
      return true;
    }
    sink.cancel();  // before INSTALL nothing may be left running or waiting behind a closed dialog (a power switch-off is firmware_abandon's)
    v.close();
    return true;
  }
  if (v.phase != FirmwareView::Phase::Checking && v.phase != FirmwareView::Phase::AskInstall && v.phase != FirmwareView::Phase::Installing) {
    return false;
  }
  const CheckStatus s = sink.poll_check();
  using State = CheckStatus::State;
  if (v.phase == FirmwareView::Phase::AskInstall) {
    if (s.state == State::Checked && core::tick_elapsed(now, v.asked_at) < core::ticks_from_ms(hal::firmware::kCheckLifetimeMs)) {
      return false;
    }
    sink.cancel();  // the 30 s are over, or the service let the check go (it lapsed, a console command took the card, the card went to a computer): the good check is no longer good
    to_list(v, s.state == State::Checked ? kCheckExpired : kCheckAgain);
    return true;
  }
  const bool installing = v.phase == FirmwareView::Phase::Installing;
  switch (s.state) {
    case State::Running: {
      const bool changed = s.done != v.done || s.total != v.total;
      v.done = s.done;
      v.total = s.total;
      return changed;
    }
    case State::Checked:
      if (!installing) {
        v.image = s.image;
        v.crc = s.crc;
        v.asked_at = now;
        v.phase = FirmwareView::Phase::AskInstall;
        return true;
      }
      break;  // (never in an install: it does not report Checked again)
    case State::Failed:
      if (installing) {
        to_failure(v, "UPDATE FAILED: ", s.fault, nullptr);
      } else {
        sink.cancel();
        to_failure(v, "NOT A VALID IMAGE: ", s.fault, &s.offset);
      }
      return true;
    case State::DidNotStart:
      if (installing) {
        to_list(v, "UPDATE DID NOT START");
        return true;
      }
      break;
    case State::Idle: break;
  }
  to_list(v, "THE STEP WAS DROPPED");  // (the service has no job: nothing to follow)
  return true;
}

// The card was taken out or put in, or the unit is switching off: a dialog that is not past the point of no return closes, and the sink is cancelled (a check read a card that is gone, or
// holds the card the shutdown's save needs). An install in the hand-over is not touched.
bool firmware_abandon(FirmwareView& v, FirmwareSink* sink) noexcept {
  if (!v.open() || v.phase == FirmwareView::Phase::Installing) {
    return false;
  }
  if (sink != nullptr) {
    sink->cancel();
  }
  v.close();
  return true;
}

const char* firmware_phase_name(FirmwareView::Phase phase) noexcept {
  switch (phase) {
    case FirmwareView::Phase::Closed: return "closed";
    case FirmwareView::Phase::List: return "list";
    case FirmwareView::Phase::AskUpdate: return "ask_update";
    case FirmwareView::Phase::Checking: return "checking";
    case FirmwareView::Phase::AskInstall: return "ask_install";
    case FirmwareView::Phase::Installing: return "installing";
  }
  return "closed";
}

}  // namespace o3sdk::app
