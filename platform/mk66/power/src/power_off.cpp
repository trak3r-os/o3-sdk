// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/power/power_off.h>

namespace o3sdk::platform::mk66::power {

namespace {

OffStep fail(OffPlatform& p, OffStep step) noexcept {
  p.mark(OffMark::Failed, step);
  p.reset();  // a normal boot: the SDRAM is still powered at every failing step, so the restart starts from a known state
  return step;
}

}  // namespace

const char* off_step_name(OffStep s) noexcept {
  switch (s) {
    case OffStep::None: return "none";
    case OffStep::Watchdog: return "watchdog running";
    case OffStep::HsrunDividers: return "HSRUN dividers";
    case OffStep::LeaveHsrun: return "leave HSRUN";
    case OffStep::ClockToCrystal: return "clock to the crystal";
    case OffStep::Dividers: return "VLPR dividers";
    case OffStep::Blpe: return "MCG to BLPE";
    case OffStep::EnterVlpr: return "enter VLPR";
    case OffStep::Ready: return "ready";
  }
  return "?";
}

bool WakeWatch::feed(bool pressed) noexcept {
  switch (phase_) {
    case Phase::WaitRelease:
      run_ = pressed ? 0u : run_ + 1u;
      if (run_ >= kReleasePolls) {
        phase_ = Phase::WaitPress;
        run_ = 0;
      }
      return false;
    case Phase::WaitPress:
      run_ = pressed ? run_ + 1u : 0u;
      if (run_ >= kWakePolls) {
        if (!after_release_) {
          return true;
        }
        phase_ = Phase::WaitFinalRelease;
        run_ = 0;
      }
      return false;
    case Phase::WaitFinalRelease:
      run_ = pressed ? 0u : run_ + 1u;
      return run_ >= kReleasePolls;
  }
  return false;
}

OffStep off_prepare(OffPlatform& p) noexcept {
  p.mark(OffMark::Started, OffStep::None);
  p.quiet_cpu();
  if (p.watchdog_running()) {
    return fail(p, OffStep::Watchdog);
  }
  p.irc48m_off();
  if (!p.hsrun_dividers()) {
    return fail(p, OffStep::HsrunDividers);
  }
  if (!p.leave_hsrun()) {
    return fail(p, OffStep::LeaveHsrun);
  }
  if (!p.clock_to_crystal()) {
    return fail(p, OffStep::ClockToCrystal);
  }
  if (!p.set_dividers()) {
    return fail(p, OffStep::Dividers);
  }
  if (!p.mcg_to_blpe()) {
    return fail(p, OffStep::Blpe);
  }
  if (!p.enter_vlpr()) {
    return fail(p, OffStep::EnterVlpr);
  }
  p.pins_quiet();
  if (kOffCutsSdram) {
    p.sdram_power_off();  // last of the outputs; from here the SDRAM has no supply
  }
  return OffStep::Ready;
}

bool off_poll(OffPlatform& p, WakeWatch& watch) noexcept {
  p.wait_us(kOffPollUs);
  if (!watch.feed(p.power_pressed())) {
    return false;
  }
  p.mark(OffMark::Woke, OffStep::Ready);
  p.reset();
  return true;
}

void off_run(OffPlatform& p) noexcept {
  if (off_prepare(p) == OffStep::Ready) {
    WakeWatch watch;
    for (;;) {
      (void)off_poll(p, watch);
    }
  }
  for (;;) {
    p.reset();  // (off_prepare called it already and the image does not return from it; this keeps the loop from being empty)
  }
}

}  // namespace o3sdk::platform::mk66::power
