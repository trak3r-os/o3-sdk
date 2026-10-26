// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-POWER P2: the VLPR off (power/power_off.h, docs/specs/OS-POWER.md 4.4) over a recording fake of the platform: the order of the steps (the USB oscillator off, the one divider step in HSRUN, the HSRUN exit, only then the
// source to the crystal, the VLPR dividers, the MCG to BLPE, RUN before VLPR, the 39 pins before PTA24, PTA24 last), every step that fails or runs out of polls ends in a reset and nothing after it, the poll loop's edge semantics (a stuck-pressed
// switch and a glitch never wake, a stable press wakes once, kWakeAfterRelease (the default) waits for the release), the mark that survives the restart, the arithmetic of the VLPR clocks, and the
// Power switch's pin against the pins the off drives. The register encodings of Mk66OffPlatform are static_asserts in the image's regs.h and power_host_mk66.cpp (RF1-RF9, RF12). Host and
// Cortex-M4F (`qemu_mk66`): fixed arrays, no heap.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/power/power_off.h>
#include <o3sdk/platform/mk66/power/power_report.h>
#include <o3sdk/platform/mk66/sdramc_config.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace pw = o3sdk::platform::mk66::power;
namespace clock_cfg = o3sdk::platform::mk66::clock;

namespace {

enum class Op : std::uint8_t { Mark, Quiet, Watchdog, Irc, HsrunDiv, Hsrun, Crystal, Dividers, Blpe, Vlpr, Pins, Sdram, Press, Wait, Reset };

struct FakeOff final : pw::OffPlatform {
  struct Rec {
    Op op;
    std::uint32_t arg;
  } rec[320] = {};  // (a 130-poll wake records about 270 calls)
  unsigned n = 0;
  unsigned counts[static_cast<unsigned>(Op::Reset) + 1u] = {};  // per Op, not capped (the record above is)
  // knobs
  bool watchdog = false;
  bool hsrun_div_ok = true, hsrun_ok = true, crystal_ok = true, dividers_ok = true, blpe_ok = true, vlpr_ok = true;
  const bool* script = nullptr;  // the switch level per poll (pressed = true); past the end: the last value
  unsigned script_n = 0;
  unsigned polls = 0;
  // what was marked last
  pw::OffMark last_mark = pw::OffMark::None;
  pw::OffStep last_step = pw::OffStep::None;

  void note(Op op, std::uint32_t arg = 0) {
    if (n < sizeof rec / sizeof rec[0]) {
      rec[n++] = {op, arg};
    }
    ++counts[static_cast<unsigned>(op)];
  }
  unsigned count(Op op) const { return counts[static_cast<unsigned>(op)]; }
  // The index of the first record of `op`, or ~0u.
  unsigned at(Op op) const {
    for (unsigned i = 0; i < n; ++i) {
      if (rec[i].op == op) {
        return i;
      }
    }
    return ~0u;
  }
  bool before(Op a, Op b) const { return at(a) != ~0u && at(b) != ~0u && at(a) < at(b); }

  void quiet_cpu() noexcept override { note(Op::Quiet); }
  bool watchdog_running() noexcept override {
    note(Op::Watchdog);
    return watchdog;
  }
  void irc48m_off() noexcept override { note(Op::Irc); }
  bool hsrun_dividers() noexcept override {
    note(Op::HsrunDiv);
    return hsrun_div_ok;
  }
  bool leave_hsrun() noexcept override {
    note(Op::Hsrun);
    return hsrun_ok;
  }
  bool clock_to_crystal() noexcept override {
    note(Op::Crystal);
    return crystal_ok;
  }
  bool set_dividers() noexcept override {
    note(Op::Dividers);
    return dividers_ok;
  }
  bool mcg_to_blpe() noexcept override {
    note(Op::Blpe);
    return blpe_ok;
  }
  bool enter_vlpr() noexcept override {
    note(Op::Vlpr);
    return vlpr_ok;
  }
  void pins_quiet() noexcept override { note(Op::Pins); }
  void sdram_power_off() noexcept override { note(Op::Sdram); }
  bool power_pressed() noexcept override {
    note(Op::Press);
    const unsigned i = polls < script_n ? polls : (script_n != 0 ? script_n - 1 : 0);
    ++polls;
    return script_n != 0 && script[i];
  }
  void wait_us(std::uint32_t us) noexcept override { note(Op::Wait, us); }
  void mark(pw::OffMark mark, pw::OffStep step) noexcept override {
    note(Op::Mark, (static_cast<std::uint32_t>(mark) << 8) | static_cast<std::uint32_t>(step));
    last_mark = mark;
    last_step = step;
  }
  void reset() noexcept override { note(Op::Reset); }
};

// n polls at one level, appended to a script.
struct Script {
  bool lv[768] = {};  // (a Cortex-M4F QEMU run has a small stack: scripts stay short)
  unsigned n = 0;
  Script& run(unsigned polls, bool pressed) {
    for (unsigned i = 0; i < polls && n < sizeof lv / sizeof lv[0]; ++i) {
      lv[n++] = pressed;
    }
    return *this;
  }
};

// Feeds a script to a watch and returns the index of the poll that woke it, or ~0u.
unsigned wake_at(pw::WakeWatch& w, const Script& s) {
  for (unsigned i = 0; i < s.n; ++i) {
    if (w.feed(s.lv[i])) {
      return i;
    }
  }
  return ~0u;
}

}  // namespace

TEST_CASE("power off: the steps run in the order of the spec, and the poll loop is not started by the preparation") {
  FakeOff f;
  CHECK(pw::off_prepare(f) == pw::OffStep::Ready);
  CHECK(f.count(Op::Reset) == 0);
  CHECK(f.count(Op::Wait) == 0);   // no wait and no read of the switch: that is the loop's
  CHECK(f.count(Op::Press) == 0);
  for (const Op op : {Op::Quiet, Op::Watchdog, Op::Irc, Op::HsrunDiv, Op::Hsrun, Op::Crystal, Op::Dividers, Op::Blpe, Op::Vlpr, Op::Pins}) {
    CHECK(f.count(op) == 1);
  }
  CHECK(f.count(Op::Sdram) == (pw::kOffCutsSdram ? 1u : 0u));  // PTA24 is touched only when the constant says the supply goes
  // interrupts first; the USB oscillator off (RM 6.7.2: before VLPR); the ONE divider step in HSRUN (RM 16.4.3.3: at most a factor of 2 per step), then the HSRUN exit, and only THEN the switch of the source
  // to the crystal (x11, made in RUN), the VLPR dividers (while on the crystal), the MCG to BLPE, then VLPR
  CHECK(f.at(Op::Mark) == 0);
  CHECK(f.before(Op::Quiet, Op::Watchdog));
  CHECK(f.before(Op::Watchdog, Op::Irc));
  CHECK(f.before(Op::Irc, Op::HsrunDiv));
  CHECK(f.before(Op::HsrunDiv, Op::Hsrun));
  CHECK(f.before(Op::Hsrun, Op::Crystal));
  CHECK(f.before(Op::Crystal, Op::Dividers));
  CHECK(f.before(Op::Dividers, Op::Blpe));
  CHECK(f.before(Op::Blpe, Op::Vlpr));
  CHECK(f.before(Op::Irc, Op::Vlpr));
  // the pins only once the chip is in VLPR, and PTA24 (when it is cut) the last output
  CHECK(f.before(Op::Vlpr, Op::Pins));
  if (pw::kOffCutsSdram) {
    CHECK(f.before(Op::Pins, Op::Sdram));
    CHECK(f.rec[f.n - 1].op == Op::Sdram);
  } else {
    CHECK(f.rec[f.n - 1].op == Op::Pins);
  }
  CHECK(f.last_mark == pw::OffMark::Started);
}

TEST_CASE("power off: the defaults are the cautious ones until the unit test has run") {
  // QA-GLB-103: the bootloader may react to a button that is down at the reset, so the wake resets at the RELEASE of the press. Flip it only when the unit shows a held button is harmless.
  static_assert(pw::kWakeAfterRelease, "kWakeAfterRelease stays true until QA-GLB-103 has passed on the unit");
  // QA-GLB-117 / M3: if PTC3's external pull-up hangs on the rail PTA24 switches, PTC3 would read pressed for good and the unit never wake. PTA24 stays HIGH until a unit test shows the pull-up survives.
  static_assert(!pw::kOffCutsSdram, "kOffCutsSdram stays false until a unit test shows PTC3 still reads released with PTA24 LOW");
  // the watch that off_run() builds (the default constructor) follows the constant
  pw::WakeWatch w;
  Script s;
  s.run(pw::kReleasePolls, false).run(pw::kWakePolls + 50, true);
  CHECK(wake_at(w, s) == ~0u);  // pressed and still down: no wake yet
  CHECK(w.phase() == pw::WakeWatch::Phase::WaitFinalRelease);
  Script t;
  t.run(pw::kReleasePolls, false);
  CHECK(wake_at(w, t) == pw::kReleasePolls - 1u);  // the release is stable: now
  // nothing was driven on the cut path: with the constant false the preparation leaves PTA24 alone and does not call it
  FakeOff f;
  CHECK(pw::off_prepare(f) == pw::OffStep::Ready);
  CHECK(f.count(Op::Sdram) == 0);
}

TEST_CASE("power off: the Power switch is on no pin that the off drives") {
  namespace board = o3sdk::platform::mk66::board;
  namespace sdram = o3sdk::platform::mk66::sdram;
  using o3sdk::platform::mk66::Pin;
  const Pin sw = board::kPowerSwitch;
  CHECK(sw.port == o3sdk::platform::mk66::Port::C);
  CHECK(sw.bit == 3);
  for (const board::QuietPin& quiet : board::kQuietPins) {
    CHECK_FALSE(quiet.pin == sw);
  }
  for (const Pin& pin : sdram::kPins) {
    CHECK_FALSE(pin == sw);
  }
  CHECK_FALSE(board::kI2sMclk == sw);
  CHECK_FALSE(board::kSdramPower == sw);
}

TEST_CASE("power off: a step that fails or runs out of polls ends in a reset, marks the step and calls nothing later") {
  struct Case {
    pw::OffStep step;
    Op last;  // the last call made
  };
  const Case cases[] = {
      {pw::OffStep::Watchdog, Op::Watchdog},       {pw::OffStep::HsrunDividers, Op::HsrunDiv}, {pw::OffStep::LeaveHsrun, Op::Hsrun},
      {pw::OffStep::ClockToCrystal, Op::Crystal},  {pw::OffStep::Dividers, Op::Dividers},      {pw::OffStep::Blpe, Op::Blpe},
      {pw::OffStep::EnterVlpr, Op::Vlpr},
  };
  const Op sequence[] = {Op::Watchdog, Op::Irc, Op::HsrunDiv, Op::Hsrun, Op::Crystal, Op::Dividers, Op::Blpe, Op::Vlpr};  // the order of the calls of steps 2-9
  for (const Case& c : cases) {
    CAPTURE(pw::off_step_name(c.step));
    FakeOff f;
    f.watchdog = c.step == pw::OffStep::Watchdog;
    f.hsrun_div_ok = c.step != pw::OffStep::HsrunDividers;
    f.hsrun_ok = c.step != pw::OffStep::LeaveHsrun;
    f.crystal_ok = c.step != pw::OffStep::ClockToCrystal;
    f.dividers_ok = c.step != pw::OffStep::Dividers;
    f.blpe_ok = c.step != pw::OffStep::Blpe;
    f.vlpr_ok = c.step != pw::OffStep::EnterVlpr;
    CHECK(pw::off_prepare(f) == c.step);
    CHECK(f.count(Op::Reset) == 1);
    // the reset is the last thing, after the mark; the pins and the SDRAM key were never touched (the SDRAM is still powered, so the restart starts from a known state)
    NEED(f.n >= 2);
    CHECK(f.rec[f.n - 1].op == Op::Reset);
    CHECK(f.rec[f.n - 2].op == Op::Mark);
    CHECK(f.last_mark == pw::OffMark::Failed);
    CHECK(f.last_step == c.step);
    CHECK(f.rec[f.n - 3].op == c.last);
    // every call of the sequence up to the failing one was made once, none after it (in particular the source is never switched to the crystal before the HSRUN exit has been made and has worked)
    bool passed = false;
    for (const Op op : sequence) {
      CAPTURE(static_cast<unsigned>(op));
      CHECK(f.count(op) == (passed ? 0u : 1u));
      passed = passed || op == c.last;
    }
    CHECK(f.count(Op::Pins) == 0);
    CHECK(f.count(Op::Sdram) == 0);
    CHECK(f.count(Op::Wait) == 0);
    CHECK(f.count(Op::Press) == 0);
  }
}

TEST_CASE("power off: the wake needs a stable release, then a stable press; a stuck switch and a glitch never wake") {
  // released 100 ms, pressed 30 ms: the 30th pressed poll wakes
  {
    pw::WakeWatch w(false);
    Script s;
    s.run(pw::kReleasePolls, false).run(pw::kWakePolls, true).run(10, false);
    CHECK(wake_at(w, s) == pw::kReleasePolls + pw::kWakePolls - 1u);
  }
  // pressed from the start and for ever (a wrong polarity constant, a stuck switch): never
  {
    pw::WakeWatch w(false);
    Script s;
    s.run(700, true);
    CHECK(wake_at(w, s) == ~0u);
    CHECK(w.phase() == pw::WakeWatch::Phase::WaitRelease);
  }
  // the finger still on the switch from the shutdown, released after 50 ms: the 100 ms of release are counted from there
  {
    pw::WakeWatch w(false);
    Script s;
    s.run(50, true).run(pw::kReleasePolls - 1u, false).run(pw::kWakePolls, true);
    CHECK(wake_at(w, s) == ~0u);  // 99 released polls are not 100: the press that follows is still part of the arming
    pw::WakeWatch w2(false);
    Script s2;
    s2.run(50, true).run(pw::kReleasePolls, false).run(pw::kWakePolls, true);
    CHECK(wake_at(w2, s2) == 50u + pw::kReleasePolls + pw::kWakePolls - 1u);
  }
  // a glitch under the debounce never wakes, however often it comes
  {
    pw::WakeWatch w(false);
    Script s;
    s.run(pw::kReleasePolls, false);
    for (int i = 0; i < 20; ++i) {
      s.run(pw::kWakePolls - 1u, true).run(3, false);
    }
    CHECK(wake_at(w, s) == ~0u);
    CHECK(w.phase() == pw::WakeWatch::Phase::WaitPress);
  }
  // a bounce inside the press restarts the debounce
  {
    pw::WakeWatch w(false);
    Script s;
    s.run(pw::kReleasePolls, false).run(20, true).run(1, false).run(pw::kWakePolls, true);
    CHECK(wake_at(w, s) == pw::kReleasePolls + 20u + 1u + pw::kWakePolls - 1u);
  }
}

TEST_CASE("power off: kWakeAfterRelease resets only after the press has ended and the release is stable") {
  pw::WakeWatch w(true);
  Script s;
  s.run(pw::kReleasePolls, false).run(pw::kWakePolls + 400, true);  // a long press (the 10 s hold of QA-GLB-103 is longer still): no wake while it lasts
  CHECK(wake_at(w, s) == ~0u);
  CHECK(w.phase() == pw::WakeWatch::Phase::WaitFinalRelease);
  Script t;
  t.run(pw::kReleasePolls - 1u, false).run(1, true).run(pw::kReleasePolls, false);  // a bounce on the release starts the 100 ms again
  CHECK(wake_at(w, t) == pw::kReleasePolls - 1u + 1u + pw::kReleasePolls - 1u);
  // a glitch is no press even here
  pw::WakeWatch g(true);
  Script u;
  u.run(pw::kReleasePolls, false).run(pw::kWakePolls - 1u, true).run(1000, false);
  CHECK(wake_at(g, u) == ~0u);
}

TEST_CASE("power off: a poll waits kOffPollUs, reads the switch once, and a confirmed wake marks and resets exactly once") {
  FakeOff f;
  Script s;
  s.run(pw::kReleasePolls, false).run(pw::kWakePolls, true).run(50, true);
  f.script = s.lv;
  f.script_n = s.n;
  pw::WakeWatch w(false);
  unsigned polls = 0;
  bool woke = false;
  while (!woke && polls < s.n) {
    woke = pw::off_poll(f, w);
    ++polls;
  }
  CHECK(woke);
  CHECK(polls == pw::kReleasePolls + pw::kWakePolls);
  CHECK(f.count(Op::Wait) == polls);
  CHECK(f.count(Op::Press) == polls);
  for (unsigned i = 0; i < f.n; ++i) {
    if (f.rec[i].op == Op::Wait) {
      CHECK(f.rec[i].arg == pw::kOffPollUs);
    }
  }
  CHECK(f.count(Op::Reset) == 1);
  CHECK(f.last_mark == pw::OffMark::Woke);
  CHECK(f.before(Op::Mark, Op::Reset));
  // a stuck-pressed switch: a thousand polls, no reset at all
  FakeOff g;
  const bool down[1] = {true};
  g.script = down;
  g.script_n = 1;
  pw::WakeWatch v(false);
  for (int i = 0; i < 1000; ++i) {
    CHECK_FALSE(pw::off_poll(g, v));
  }
  CHECK(g.count(Op::Reset) == 0);
  CHECK(g.count(Op::Mark) == 0);
}

TEST_CASE("power off: `diag power` prints the record of the last shutdown and the result of the off, and nothing when there is none") {
  pw::PowerReport r;
  char buf[1536];
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "last shutdown") == nullptr);
  CHECK(std::strstr(buf, "last off") == nullptr);
  // a dry run that reached enter_off with a cancelled save
  r.rec_valid = true;
  r.rec.start(true);
  const pw::Stage stages[] = {pw::Stage::Quiet, pw::Stage::Mute, pw::Stage::Card, pw::Stage::Unmount, pw::Stage::Usb, pw::Stage::Lights, pw::Stage::Off, pw::Stage::Done};
  const std::uint32_t at[] = {0, 10, 12, 5030, 5031, 5140, 5300, 6000};
  for (unsigned i = 0; i < 8; ++i) {
    r.rec.enter(static_cast<std::uint8_t>(stages[i]), at[i]);
  }
  r.rec.note(pw::kRecSaveCancelled);
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "power: last shutdown dry, reached done\n") != nullptr);
  CHECK(std::strstr(buf, "power: ms from A: A quiet 0, A2 mute 10, B card 12, C unmount 5030, D usb 5031, E lights 5140, F off 5300, done 6000\n") != nullptr);
  CHECK(std::strstr(buf, "power: notes: save cancelled\n") != nullptr);
  CHECK(std::strstr(buf, "last off") == nullptr);
  // the result of the VLPR off
  r.rec.set_off(static_cast<std::uint8_t>(pw::OffMark::Failed), static_cast<std::uint8_t>(pw::OffStep::EnterVlpr));
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "power: last off RESTARTED at step: enter VLPR\n") != nullptr);
  r.rec.set_off(static_cast<std::uint8_t>(pw::OffMark::Woke), static_cast<std::uint8_t>(pw::OffStep::Ready));
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "power: last off ended by a press (the wake)\n") != nullptr);
  r.rec.set_off(static_cast<std::uint8_t>(pw::OffMark::Started), static_cast<std::uint8_t>(pw::OffStep::None));
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "power: last off began and left no result") != nullptr);
  // a refusal alone (no stage)
  pw::PowerReport q;
  q.rec_valid = true;
  q.rec.refused(2);
  (void)pw::format_power_report(q, buf, sizeof buf);
  CHECK(std::strstr(buf, "last shutdown") == nullptr);
  CHECK(std::strstr(buf, "power: last refusal: an export runs\n") != nullptr);
  // the worst case stays well inside the console's 1.5 KB buffer
  r.rec.note(0xFFu);
  r.stage = pw::Stage::Lights;
  r.dry = true;
  r.deadline_hit = true;
  r.save_cancelled = true;
  r.inject = pw::Inject::Pressed;
  r.rec.refusal = 3;
  r.rec.seal();
  const std::size_t n = pw::format_power_report(r, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(n < 1100);
}

TEST_CASE("power off: the VLPR clocks are inside their limits, and the dividers only ever divide further") {
  CHECK(clock_cfg::kVlprCoreHz == 2'000'000u);
  CHECK(clock_cfg::kVlprBusHz == 2'000'000u);
  CHECK(clock_cfg::kVlprFlexBusHz == 2'000'000u);
  CHECK(clock_cfg::kVlprFlashHz == 1'000'000u);  // flash /16 of the crystal: exactly the VLPR limit, the divider's maximum
  CHECK(clock_cfg::kVlprClkdiv1 == 0x777F0000u);
  CHECK(clock_cfg::kVlprCoreHz <= clock_cfg::kVlprMaxCoreHz);
  CHECK(clock_cfg::kVlprBusHz <= clock_cfg::kVlprMaxBusHz);
  CHECK(clock_cfg::kVlprFlexBusHz <= clock_cfg::kVlprMaxFlexBusHz);
  CHECK(clock_cfg::kVlprFlashHz <= clock_cfg::kVlprMaxFlashHz);
  // the dividers are written while MCGOUTCLK is the crystal, after HSRUN's /1 /2 /2 /7: each only grows, so no domain is ever faster than it was (the rule boot.cpp follows)
  CHECK(clock_cfg::kVlprOutdiv1 >= clock_cfg::kOutdiv1);
  CHECK(clock_cfg::kVlprOutdiv2 >= clock_cfg::kOutdiv2);
  CHECK(clock_cfg::kVlprOutdiv3 >= clock_cfg::kOutdiv3);
  CHECK(clock_cfg::kVlprOutdiv4 >= clock_cfg::kOutdiv4);
  // the intermediate state, PBE with HSRUN's dividers, is slower than HSRUN's in every domain
  CHECK(clock_cfg::kCrystalHz / (clock_cfg::kOutdiv1 + 1u) < clock_cfg::kCoreHz);
  CHECK(clock_cfg::kCrystalHz / (clock_cfg::kOutdiv4 + 1u) < clock_cfg::kFlashHz);
  // the HSRUN step (RM 16.4.3.3: a clock change in HSRUN is limited to a factor of 2 per step): no divider more than doubles, none shrinks, the fields fit, and it is the only change made in HSRUN
  CHECK(clock_cfg::kExitClkdiv1 == 0x133D0000u);
  CHECK(clock_cfg::kExitOutdiv1 + 1u <= 2u * (clock_cfg::kOutdiv1 + 1u));
  CHECK(clock_cfg::kExitOutdiv2 + 1u <= 2u * (clock_cfg::kOutdiv2 + 1u));
  CHECK(clock_cfg::kExitOutdiv3 + 1u <= 2u * (clock_cfg::kOutdiv3 + 1u));
  CHECK(clock_cfg::kExitOutdiv4 + 1u <= 2u * (clock_cfg::kOutdiv4 + 1u));
  CHECK(clock_cfg::kExitOutdiv1 >= clock_cfg::kOutdiv1);
  CHECK(clock_cfg::kExitOutdiv4 >= clock_cfg::kOutdiv4);
  CHECK(clock_cfg::kExitCoreHz * 2u >= clock_cfg::kCoreHz);          // 180 -> 90 MHz: exactly the factor
  CHECK(clock_cfg::kExitBusHz * 2u >= clock_cfg::kBusHz);
  CHECK(clock_cfg::kExitFlexBusHz * 2u >= clock_cfg::kFlexBusHz);
  CHECK(clock_cfg::kExitFlashHz * 2u + 2u >= clock_cfg::kFlashHz);   // (the integer Hz of /14 against /7)
  // the source switch after the exit (x11) is not made in HSRUN, and the VLPR dividers only divide further than the step's
  CHECK(clock_cfg::kCoreHz / clock_cfg::kCrystalHz >= 11u);
  CHECK(clock_cfg::kExitCrystalCoreHz == 8'000'000u);
  CHECK(clock_cfg::kVlprOutdiv1 >= clock_cfg::kExitOutdiv1);
  CHECK(clock_cfg::kVlprOutdiv2 >= clock_cfg::kExitOutdiv2);
  CHECK(clock_cfg::kVlprOutdiv3 >= clock_cfg::kExitOutdiv3);
  CHECK(clock_cfg::kVlprOutdiv4 >= clock_cfg::kExitOutdiv4);
  // the poll loop's counts are milliseconds
  CHECK(pw::kWakePolls == 30u);
  CHECK(pw::kReleasePolls == 100u);
}
