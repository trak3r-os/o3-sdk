// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/power/power_service.h>

#include <o3sdk/platform/mk66/audio/diag.h>

namespace o3sdk::platform::mk66::power {

// ---- The record of the last shutdown (power_record.h) ------------------------------------------------------------------------------------
std::uint32_t ShutdownRecord::sum() const noexcept {
  std::uint32_t x = magic ^ (static_cast<std::uint32_t>(stage) | (static_cast<std::uint32_t>(flags) << 8) | (static_cast<std::uint32_t>(refusal) << 16) | (static_cast<std::uint32_t>(off) << 24));
  for (unsigned i = 0; i < 8; i += 2) {
    x = ((x << 5) | (x >> 27)) ^ (static_cast<std::uint32_t>(t[i]) | (static_cast<std::uint32_t>(t[i + 1]) << 16));
  }
  return ~x;
}
void ShutdownRecord::seal() noexcept { check = sum(); }
bool ShutdownRecord::valid() const noexcept { return magic == kRecordMagic && check == sum(); }
void ShutdownRecord::refused(std::uint8_t why) noexcept {
  start(false);
  refusal = why;
  seal();
}
void ShutdownRecord::start(bool dry) noexcept {
  magic = kRecordMagic;
  stage = 0;
  flags = dry ? kRecDry : 0u;
  refusal = 0;
  off = 0;
  for (std::uint16_t& ms : t) {
    ms = 0;
  }
  seal();
}
void ShutdownRecord::enter(std::uint8_t stage_no, std::uint32_t ms_from_a) noexcept {
  stage = stage_no;
  if (stage_no >= 1u && stage_no <= 8u) {
    t[stage_no - 1u] = static_cast<std::uint16_t>(ms_from_a > 0xFFFFu ? 0xFFFFu : ms_from_a);
  }
  seal();
}
void ShutdownRecord::note(std::uint8_t bits) noexcept {
  flags = static_cast<std::uint8_t>(flags | bits);
  seal();
}
void ShutdownRecord::set_off(std::uint8_t mark, std::uint8_t step) noexcept {
  off = static_cast<std::uint8_t>((mark << 4) | (step & 0xFu));
  seal();
}

const char* stage_name(Stage s) noexcept {
  switch (s) {
    case Stage::Idle: return "idle";
    case Stage::Quiet: return "A quiet";
    case Stage::Mute: return "A2 mute";
    case Stage::Card: return "B card";
    case Stage::Unmount: return "C unmount";
    case Stage::Usb: return "D usb";
    case Stage::Lights: return "E lights";
    case Stage::Off: return "F off";
    case Stage::Done: return "done";
  }
  return "?";
}

void PowerService::refuse(core::PowerText text, std::uint32_t now_ms) noexcept {
  refusal_text_ = text;
  refusal_until_ = now_ms + kRefusalShowMs;
  hold_.refuse();
  host_.log("power: refused, the strip says why");
}

// Stage 0. The questions are asked in the order of the spec (4.2) and change nothing: a refusal leaves everything as it was. From the console (`diag power off`) a refusal is final and
// the caller prints it; from a hold, an export or a writable disk is refused once and a second completed hold within kSecondHoldMs goes ahead.
Refusal PowerService::check(std::uint32_t now_ms, bool console) noexcept {
  if (host_.firmware_active()) {
    if (!console) {
      refuse(core::PowerText::Firmware, now_ms);
    }
    note_refusal(Refusal::Firmware);
    return Refusal::Firmware;
  }
  const bool exporting = host_.export_running();
  const bool disk = !exporting && host_.disk_writing();
  if (exporting || disk) {
    const core::PowerText text = exporting ? core::PowerText::Export : core::PowerText::DiskWriting;
    const Refusal why = exporting ? Refusal::Export : Refusal::Disk;
    if (console) {
      note_refusal(why);
      return why;
    }
    if (armed_text_ == text && now_ms - armed_at_ <= kSecondHoldMs) {
      armed_text_ = core::PowerText::None;
      if (exporting) {
        host_.export_cancel();  // its normal cancel: the files finished stay, no project.mt for the OG set, no file under a final name for a WAV
        export_cancelled_ = true;
      }
      return Refusal::None;
    }
    armed_text_ = text;
    armed_at_ = now_ms;
    refuse(text, now_ms);
    note_refusal(why);
    return why;
  }
  return Refusal::None;
}

void PowerService::on_commit(std::uint32_t now_ms) noexcept {
  if (check(now_ms, false) == Refusal::None) {
    begin(now_ms, false);
  }
}

Refusal PowerService::request(std::uint32_t now_ms, bool dry) noexcept {
  if (stage_ != Stage::Idle) {
    return Refusal::Running;
  }
  if (!hold_.released_stable()) {
    return Refusal::Held;
  }
  const Refusal why = check(now_ms, true);
  if (why == Refusal::None) {
    begin(now_ms, dry);
  }
  return why;
}

void PowerService::begin(std::uint32_t now_ms, bool dry) noexcept {
  dry_ = dry;
  t_a_ = now_ms;
  saving_ = host_.save_busy();  // a save (not the settings flush, which starts after this, at stage A) in flight at the commit: the strip says SAVING in B
  refusal_text_ = core::PowerText::None;
  armed_text_ = core::PowerText::None;
  if (rec_ != nullptr) {
    rec_->start(dry);
    if (export_cancelled_) {
      rec_->note(kRecExportCancelled);
    }
  }
  export_cancelled_ = false;
  host_.log(dry ? "power: commit, dry" : "power: commit");
  enter(Stage::Quiet, now_ms);
}

// Enters a stage and does its entry action (the calls with a side effect: each is made here and nowhere else, so at most once).
void PowerService::enter(Stage s, std::uint32_t now_ms) noexcept {
  stage_ = s;
  t_stage_ = now_ms;
  if (rec_ != nullptr) {
    rec_->enter(static_cast<std::uint8_t>(s), now_ms - t_a_);
  }
  char line[56];
  audio::diag::Text text(line, sizeof line);
  text.str("power: ").str(stage_name(s)).str(" +").dec(now_ms - t_a_).str(" ms");
  host_.log(line);
  switch (s) {
    case Stage::Quiet:
      host_.quiesce_engine();
      break;
    case Stage::Mute:
      host_.audio_down();
      break;
    case Stage::Card:
      t_b_ = now_ms;
      host_.settings_flush();
      break;
    case Stage::Usb:
      host_.usb_drain();
      break;
    case Stage::Lights:
      host_.lights_out();
      break;
    case Stage::Off:
      host_.amp_key_low();
      break;
    default:
      break;
  }
}

void PowerService::release_card(std::uint32_t) noexcept {
  if (!card_released_) {
    card_released_ = true;
    host_.card_release();
  }
}

// The global deadline: every stage that has not finished is abandoned. The card is released only if nothing holds it; otherwise it is left as a pulled card and the log says so.
void PowerService::abandon(std::uint32_t now_ms) noexcept {
  deadline_hit_ = true;
  note(kRecDeadline);
  host_.log("power: global deadline, on to F");
  if (!card_released_) {
    if (!host_.card_busy()) {
      release_card(now_ms);
    } else {
      note(kRecCardBusy);
      host_.log("power: card left busy at the deadline");
    }
  }
  enter(Stage::Off, now_ms);
}

void PowerService::step(std::uint32_t now_ms, const core::PowerHold::Result& hr) noexcept {
  if (stage_ == Stage::Idle) {
    if (hr.commit) {
      on_commit(now_ms);
    }
    return;  // (the first stage's entry action was the pass's work)
  }
  if (stage_ == Stage::Done) {
    return;
  }
  if (stage_ != Stage::Off && now_ms - t_a_ >= kGlobalCapMs) {
    abandon(now_ms);
    return;
  }
  switch (stage_) {
    case Stage::Quiet:
      if (now_ms - t_stage_ >= kQuietMs) {
        enter(Stage::Mute, now_ms);
      }
      break;
    case Stage::Mute:
      enter(Stage::Card, now_ms);  // audio_down() returned in the pass that entered Mute
      break;
    case Stage::Card: {
      if (!host_.save_busy()) {
        enter(Stage::Unmount, now_ms);
        break;
      }
      const std::uint32_t elapsed = now_ms - t_b_;
      if (elapsed >= kCardHardCapMs) {
        note(kRecSaveCap);
        host_.log("power: save still running at the cap");
        enter(Stage::Unmount, now_ms);
        break;
      }
      if (elapsed >= kSaveWaitMs && !save_cancelled_ && !host_.save_committing()) {
        save_cancelled_ = true;
        note(kRecSaveCancelled);  // the Temp is removed and the old file stays whole; the job needs a few passes to say so
        host_.save_cancel();
      }
      break;
    }
    case Stage::Unmount: {
      const bool busy = host_.card_busy();
      if (!busy || now_ms - t_b_ >= kCardHardCapMs) {
        if (busy) {
          note(kRecCardBusy);
          host_.log("power: card still busy");  // a cut like a pulled card
        }
        release_card(now_ms);
        enter(Stage::Usb, now_ms);
      }
      break;
    }
    case Stage::Usb:
      if (now_ms - t_stage_ >= kUsbDrainMs) {
        host_.usb_down();
        enter(Stage::Lights, now_ms);
      }
      break;
    case Stage::Lights:
      if (host_.lights_done()) {
        enter(Stage::Off, now_ms);
      } else if (now_ms - t_stage_ >= kLightsCapMs) {
        note(kRecLightsCap);
        host_.log("power: lights cap");
        enter(Stage::Off, now_ms);
      }
      break;
    case Stage::Off:
      // F0: the unit is dark and silent; the user is usually still holding Power. Neither the restart nor the off happens until the level has been released for kReleaseStableMs.
      if (!off_done_) {
        if (hold_.released_stable()) {
          off_done_ = true;
          host_.log("power: Power released, leaving");
          if (rec_ != nullptr) {
            rec_->enter(static_cast<std::uint8_t>(Stage::Done), now_ms - t_a_);  // BEFORE the call: enter_off does not return on the image
          }
          host_.enter_off(dry_);
          stage_ = Stage::Done;
        } else if (!wait_logged_ && now_ms - t_stage_ >= kWaitLogMs) {
          wait_logged_ = true;
          host_.log("power: waiting for Power to be released");
        }
      }
      break;
    default:
      break;
  }
}

core::PowerShow PowerService::show(std::uint32_t now_ms, const core::PowerHold::Result& hr) const noexcept {
  core::PowerShow s;
  if (stage_ != Stage::Idle) {
    s.fill = 1000;
    if (save_cancelled_ && stage_ <= Stage::Unmount) {
      s.text = core::PowerText::SaveCancelled;
    } else if (saving_ && stage_ == Stage::Card) {
      s.text = core::PowerText::Saving;
    } else {
      s.text = core::PowerText::PoweringOff;
    }
    return s;
  }
  if (refusal_text_ != core::PowerText::None && static_cast<std::int32_t>(refusal_until_ - now_ms) > 0) {
    s.text = refusal_text_;
    return s;
  }
  return hr.show;
}

}  // namespace o3sdk::platform::mk66::power
