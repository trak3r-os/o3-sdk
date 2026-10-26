// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/power/power_report.h>

#include <o3sdk/platform/mk66/audio/diag.h>

namespace o3sdk::platform::mk66::power {

namespace {

using Out = audio::diag::Text;

const char* level(bool high) noexcept { return high ? "high" : "low"; }

const char* state_name(core::PowerHold::State s) noexcept {
  switch (s) {
    case core::PowerHold::State::Booting: return "Booting";
    case core::PowerHold::State::Armed: return "Armed";
    case core::PowerHold::State::Holding: return "Holding";
    case core::PowerHold::State::Spent: return "Spent";
    case core::PowerHold::State::Committed: return "Committed";
  }
  return "?";
}

const char* pull_name(std::uint8_t pull) noexcept { return pull == 1u ? "up" : (pull == 2u ? "down" : "none"); }

}  // namespace

std::size_t format_power_report(const PowerReport& r, char* out, std::size_t cap) noexcept {
  Out o(out, cap);
  const bool pressed_level = r.level_high != r.active_low;  // the level that reads "pressed"
  o.str("power: PTC3 ").str(level(r.level_high)).str(" (pull ").str(pull_name(r.pull)).str("), the switch reads ").str(pressed_level ? "pressed" : "released");
  if (r.inject != Inject::Real) {
    o.str(", INJECTED ").str(r.inject == Inject::Pressed ? "pressed" : "released");
  }
  o.str("\n");
  o.str("power: read now: no pull ").str(level(r.read_high[0])).str(", pull-up ").str(level(r.read_high[1])).str(", pull-down ").str(level(r.read_high[2])).str("\n");
  o.str("power: board: pressed is ").str(r.active_low ? "low" : "high").str(", internal pull ").str(pull_name(r.pull)).str("\n");
  o.str("power: hold ").str(state_name(r.state)).str(", debounced ").str(r.pressed ? "down" : "up").str(", line ").dec(r.fill).str("/1000\n");
  o.str("power: presses ").dec(r.presses).str(" holds ").dec(r.holds).str(" taps ").dec(r.taps).str(" commits ").dec(r.commits).str(" refusals ").dec(r.refusals).str("\n");
  if (r.rec_valid) {  // the record of the last shutdown (power_record.h): it survives the reset that ended it, the diag log and the console do not
    const ShutdownRecord& c = r.rec;
    if (c.stage != 0u) {
      o.str("power: last shutdown ").str((c.flags & kRecDry) != 0u ? "dry" : "off").str(", reached ").str(stage_name(static_cast<Stage>(c.stage))).str("\n");
      o.str("power: ms from A:");
      for (unsigned i = 0; i < c.stage && i < 8u; ++i) {
        if (i == 0u || c.t[i] != 0u) {
          o.str(i == 0u ? " " : ", ").str(stage_name(static_cast<Stage>(i + 1u))).str(" ").dec(c.t[i]);
        }
      }
      o.str("\n");
      static const char* const kNotes[] = {"dry", "GLOBAL DEADLINE", "save cancelled", "save at the cap", "card busy at release", "lights cap", "export cancelled"};
      bool any = false;
      for (unsigned i = 1; i < 7u; ++i) {  // (bit 0, dry, is in the first line)
        if ((c.flags >> i) & 1u) {
          o.str(any ? ", " : "power: notes: ").str(kNotes[i]);
          any = true;
        }
      }
      o.str(any ? "\n" : "");
    }
    if (c.refusal != 0u) {
      o.str("power: last refusal: ").str(c.refusal == 1u ? "a firmware update runs" : (c.refusal == 2u ? "an export runs" : "the USB disk may be written")).str("\n");
    }
    const unsigned mark = c.off >> 4;  // OffMark
    if (mark != 0u) {                  // the VLPR off of P2 (the unit session reads here why a restart came instead of an off)
      o.str("power: last off ");
      if (mark == static_cast<unsigned>(OffMark::Failed)) {
        o.str("RESTARTED at step: ").str(off_step_name(static_cast<OffStep>(c.off & 0xFu)));
      } else if (mark == static_cast<unsigned>(OffMark::Woke)) {
        o.str("ended by a press (the wake)");
      } else {
        o.str("began and left no result (a hang, or the cable)");
      }
      o.str("\n");
    }
  }
  if (r.stage == Stage::Idle) {
    o.str("power: shutdown idle\n");
    return o.size();
  }
  o.str("power: shutdown stage ").str(stage_name(r.stage)).str(r.dry ? ", dry" : ", off").str(r.deadline_hit ? ", GLOBAL DEADLINE HIT" : "").str(r.save_cancelled ? ", save cancelled" : "").str("\n");
  return o.size();  // (the stages' timings are the `power: <stage> +N ms` lines of the diag log)
}

}  // namespace o3sdk::platform::mk66::power
