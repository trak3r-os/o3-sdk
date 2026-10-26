// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-POWER P1: the shutdown state machine (power/power_service.h, docs/specs/OS-POWER.md section 4) over a recording fake host: the order of the calls, every stage's deadline with a job that
// never ends, the refusals (no call at all), the second hold, card_release() never while card_busy() before the hard cap, the global deadline with every query stuck, and F0 (Power held
// forever makes no restart, a release makes exactly one after 100 ms, a bounce starts the 100 ms again). Host and Cortex-M4F (`qemu_mk66`): fixed arrays, no heap.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3sdk/core/power_hold.h>
#include <o3sdk/platform/mk66/power/power_report.h>
#include <o3sdk/platform/mk66/power/power_service.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace pw = o3sdk::platform::mk66::power;
using o3sdk::core::PowerHold;
using o3sdk::core::PowerText;
namespace hold_cfg = o3sdk::core::power;

namespace {

enum class Call : std::uint8_t { Quiesce, AudioDown, SettingsFlush, SaveCancel, CardRelease, UsbDrain, UsbDown, LightsOut, AmpKeyLow, EnterOff, ExportCancel };
constexpr unsigned kCallKinds = 11;

// A fake host that records what is called, with the time (a clock the rig shares), and answers the queries from knobs. `cost` is how long a call takes in fake milliseconds (a call that blocks).
struct FakeHost final : pw::PowerHost {
  std::uint32_t* clock = nullptr;
  std::uint32_t cost = 0;
  struct Rec {
    Call call;
    std::uint32_t at;
  } rec[96] = {};
  unsigned n = 0;
  unsigned counts[kCallKinds] = {};
  char logs[24][60] = {};
  unsigned log_n = 0;

  // knobs
  bool firmware = false;
  bool exporting = false;
  bool disk = false;
  std::uint32_t save_busy_until = 0;  // absolute fake time; ~0u: never ends
  std::uint32_t flush_busy_ms = 0;    // the settings flush the shutdown starts: quiesce_engine() makes the save layer busy for this long
  bool committing = false;
  bool cancel_ends_save = true;       // a cancelled job is gone a moment later
  std::uint32_t card_busy_until = 0;
  std::uint32_t lights_done_at = 0;   // absolute; ~0u: never
  bool card_busy_seen_at_release = false;
  bool save_cancelled = false;
  // What the shutdown record held at the instant enter_off was called (the image's enter_off does not return, so this is the only moment the order "Done is written BEFORE the call" can be seen).
  const pw::ShutdownRecord* record = nullptr;
  bool rec_seen_at_off = false;
  bool rec_valid_at_off = false;
  std::uint8_t rec_stage_at_off = 0;
  std::uint16_t rec_t_done_at_off = 0;
  std::uint16_t rec_t_off_at_off = 0;

  void note(Call c) {
    if (n < sizeof rec / sizeof rec[0]) {
      rec[n++] = {c, *clock};
    }
    ++counts[static_cast<unsigned>(c)];
    *clock += cost;
  }
  unsigned count(Call c) const { return counts[static_cast<unsigned>(c)]; }
  std::uint32_t when(Call c) const {
    for (unsigned i = 0; i < n; ++i) {
      if (rec[i].call == c) {
        return rec[i].at;
      }
    }
    return ~0u;
  }
  bool before(Call a, Call b) const { return count(a) != 0 && count(b) != 0 && when(a) <= when(b); }
  bool has_log(const char* text) const {
    for (unsigned i = 0; i < log_n; ++i) {
      if (std::strstr(logs[i], text) != nullptr) {
        return true;
      }
    }
    return false;
  }
  unsigned total() const { return n; }

  bool firmware_active() noexcept override { return firmware; }
  bool export_running() noexcept override { return exporting; }
  bool disk_writing() noexcept override { return disk; }
  void export_cancel() noexcept override {
    note(Call::ExportCancel);
    exporting = false;
  }
  void quiesce_engine() noexcept override {
    note(Call::Quiesce);
    if (flush_busy_ms != 0u && save_busy_until == 0u) {
      save_busy_until = *clock + flush_busy_ms;
    }
  }
  void audio_down() noexcept override { note(Call::AudioDown); }
  void settings_flush() noexcept override { note(Call::SettingsFlush); }
  bool save_busy() noexcept override { return !save_cancelled && (save_busy_until == ~0u || *clock < save_busy_until); }
  bool save_committing() noexcept override { return committing && save_busy(); }
  void save_cancel() noexcept override {
    note(Call::SaveCancel);
    if (cancel_ends_save) {
      save_cancelled = true;
    }
  }
  bool card_busy() noexcept override { return save_busy() || card_busy_until == ~0u || *clock < card_busy_until; }
  void card_release() noexcept override {
    card_busy_seen_at_release = card_busy();
    note(Call::CardRelease);
  }
  void usb_drain() noexcept override { note(Call::UsbDrain); }
  void usb_down() noexcept override { note(Call::UsbDown); }
  void lights_out() noexcept override { note(Call::LightsOut); }
  bool lights_done() noexcept override { return lights_done_at != ~0u && *clock >= lights_done_at; }
  void amp_key_low() noexcept override { note(Call::AmpKeyLow); }
  void enter_off(bool) noexcept override {
    if (record != nullptr && !rec_seen_at_off) {
      rec_seen_at_off = true;
      rec_valid_at_off = record->valid();
      rec_stage_at_off = record->stage;
      rec_t_done_at_off = record->t[7];
      rec_t_off_at_off = record->t[6];
    }
    note(Call::EnterOff);
  }
  void log(const char* text) noexcept override {
    if (log_n < 24) {
      std::strncpy(logs[log_n], text, 59);
      logs[log_n][59] = '\0';
      ++log_n;
    }
  }
};

struct Rig {
  Rig() : svc(host, hold, &rec) {
    host.clock = &t;
    host.record = &rec;
  }
  std::uint32_t t = 0;
  FakeHost host;
  PowerHold hold{true};
  pw::ShutdownRecord rec = {};  // (the image's lives in the noinit SRAM)
  pw::PowerService svc;
  PowerHold::Result hr;
  bool pressed = false;
  bool shift = false;

  // One pass at the current fake time (a host call may have moved it on), then one fake millisecond.
  void pass() {
    hr = hold.update(t, !pressed /* active low */, shift, true);
    svc.step(t, hr);
    ++t;
  }
  void run(std::uint32_t ms, bool down) {
    pressed = down;
    const std::uint32_t end = t + ms;
    while (t < end) {
      pass();
    }
  }
  void arm() { run(hold_cfg::kArmReleaseMs + hold_cfg::kDebounceMs, false); }
  // A full hold: down until the commit has been seen and a little more.
  void hold_to_commit() {
    run(hold_cfg::kHoldMs + 5, true);
  }
};

}  // namespace

TEST_CASE("power service: a hold runs the stages in order, then waits for the release before leaving") {
  Rig r;
  r.arm();
  r.hold_to_commit();
  CHECK(r.svc.locked());
  CHECK(r.svc.stage() != pw::Stage::Idle);
  r.run(1500, true);  // the finger stays down through the whole shutdown
  CHECK(r.host.count(Call::EnterOff) == 0);  // F0: never while Power is down
  CHECK(r.svc.stage() == pw::Stage::Off);
  // the order of the calls
  CHECK(r.host.before(Call::Quiesce, Call::AudioDown));
  CHECK(r.host.before(Call::AudioDown, Call::SettingsFlush));
  CHECK(r.host.before(Call::SettingsFlush, Call::CardRelease));
  CHECK(r.host.before(Call::CardRelease, Call::UsbDrain));
  CHECK(r.host.before(Call::UsbDrain, Call::UsbDown));
  CHECK(r.host.before(Call::UsbDown, Call::LightsOut));
  CHECK(r.host.before(Call::LightsOut, Call::AmpKeyLow));
  // each exactly once
  for (const Call c : {Call::Quiesce, Call::AudioDown, Call::SettingsFlush, Call::CardRelease, Call::UsbDrain, Call::UsbDown, Call::LightsOut, Call::AmpKeyLow}) {
    CHECK(r.host.count(c) == 1);
  }
  CHECK(r.host.count(Call::SaveCancel) == 0);
  CHECK(r.host.count(Call::ExportCancel) == 0);
  CHECK(r.host.has_log("power: commit"));
  // the Stop reaches the render before the codec is muted
  CHECK(r.host.when(Call::AudioDown) - r.host.when(Call::Quiesce) >= pw::kQuietMs);
  // the USB drain gets its time
  CHECK(r.host.when(Call::UsbDown) - r.host.when(Call::UsbDrain) >= pw::kUsbDrainMs);
  // and the unit is nowhere near done: no restart while the finger is down, even 30 s later
  r.run(30000, true);
  CHECK(r.host.count(Call::EnterOff) == 0);
  CHECK(r.host.has_log("waiting for Power to be released"));
  // the release: the restart comes about 100 ms later, once
  const std::uint32_t released_at = r.t;
  r.run(400, false);
  NEED(r.host.count(Call::EnterOff) == 1);
  const std::uint32_t delay = r.host.when(Call::EnterOff) - released_at;
  CHECK(delay >= hold_cfg::kReleaseStableMs);
  CHECK(delay <= hold_cfg::kReleaseStableMs + 10);
  CHECK(r.svc.stage() == pw::Stage::Done);
  // nothing after enter_off
  const unsigned total = r.host.total();
  r.run(20000, false);
  CHECK(r.host.total() == total);
}

TEST_CASE("power service: the stage lines of the log carry the stamps, and the strip text follows the stages") {
  Rig r;
  r.arm();
  r.hold_to_commit();
  CHECK(r.svc.show(r.t, r.hr).fill == 1000);
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::PoweringOff);
  r.run(1000, true);
  CHECK(r.svc.stage() == pw::Stage::Off);
  // one line a stage, "power: <stage> +<ms from A> ms": the unit session reads the timings there (`diag log`)
  CHECK(r.host.has_log("power: A quiet +0 ms"));
  for (const char* const line : {"power: A2 mute +", "power: B card +", "power: C unmount +", "power: D usb +", "power: E lights +", "power: F off +"}) {
    CHECK(r.host.has_log(line));
  }
}

TEST_CASE("power service: the settings flush that the shutdown itself starts does not turn the strip to SAVING") {
  Rig r;
  r.host.flush_busy_ms = 300;  // not busy at the commit; the flush starts at stage A and runs for a while, as the settings job does
  r.arm();
  r.hold_to_commit();
  r.run(pw::kQuietMs + 5, true);
  NEED(r.svc.stage() == pw::Stage::Card);
  CHECK(r.host.save_busy());
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::PoweringOff);
  r.run(1000, true);
  CHECK(r.host.count(Call::SaveCancel) == 0);
}

TEST_CASE("power service: a refusal makes no call at all, says why for 3 s, and the switch is armed again after the release") {
  Rig r;
  r.host.firmware = true;
  r.arm();
  r.hold_to_commit();
  CHECK_FALSE(r.svc.locked());
  CHECK(r.host.total() == 0);
  CHECK(r.hold.refusals() == 1);
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::Firmware);
  CHECK(r.svc.show(r.t, r.hr).fill == 0);
  r.run(2000, true);  // still down: nothing more, the hold is spent
  CHECK(r.host.total() == 0);
  CHECK(r.hold.state() == PowerHold::State::Spent);
  r.run(100, false);
  CHECK(r.hold.state() == PowerHold::State::Armed);
  r.run(3000, false);
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::None);  // the 3 s are over
  // the update ended: the next hold goes through
  r.host.firmware = false;
  r.hold_to_commit();
  CHECK(r.svc.locked());
  CHECK(r.host.count(Call::Quiesce) == 1);
}

TEST_CASE("power service: an export is refused once, a second hold within 10 s stops it and runs the stages") {
  Rig r;
  r.host.exporting = true;
  r.arm();
  r.hold_to_commit();
  CHECK_FALSE(r.svc.locked());
  CHECK(r.host.total() == 0);
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::Export);
  r.run(200, false);
  r.run(3000, false);
  CHECK(r.host.total() == 0);
  r.hold_to_commit();  // the second hold, about 6 s after the first
  CHECK(r.svc.locked());
  CHECK(r.host.count(Call::ExportCancel) == 1);
  CHECK(r.host.before(Call::ExportCancel, Call::Quiesce));
  CHECK(r.host.count(Call::Quiesce) == 1);
}

TEST_CASE("power service: a second hold after 10 s is refused again") {
  Rig r;
  r.host.exporting = true;
  r.arm();
  r.hold_to_commit();
  r.run(200, false);
  r.run(pw::kSecondHoldMs + 1000, false);
  r.hold_to_commit();
  CHECK_FALSE(r.svc.locked());
  CHECK(r.host.total() == 0);
  CHECK(r.hold.refusals() == 2);
}

TEST_CASE("power service: a writable boot disk is refused once too") {
  Rig r;
  r.host.disk = true;
  r.arm();
  r.hold_to_commit();
  CHECK_FALSE(r.svc.locked());
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::DiskWriting);
  r.run(200, false);
  r.hold_to_commit();
  CHECK(r.svc.locked());
  CHECK(r.host.count(Call::ExportCancel) == 0);
  CHECK(r.host.count(Call::Quiesce) == 1);
}

TEST_CASE("power service: a save in flight is waited for, the strip says so, and nothing is cancelled when it ends in time") {
  Rig r;
  r.host.save_busy_until = 5000;
  r.arm();
  r.hold_to_commit();
  r.run(1000, true);
  CHECK(r.svc.stage() == pw::Stage::Card);
  CHECK(r.svc.show(r.t, r.hr).text == PowerText::Saving);
  CHECK(r.host.count(Call::CardRelease) == 0);
  r.run(2500, true);
  CHECK(r.host.count(Call::SaveCancel) == 0);
  CHECK(r.host.count(Call::CardRelease) == 1);
  CHECK(r.host.when(Call::CardRelease) >= 5000);
  CHECK_FALSE(r.host.card_busy_seen_at_release);
}

TEST_CASE("power service: a save over 5 s is cancelled, once, and the strip says so") {
  Rig r;
  r.host.save_busy_until = ~0u;
  r.arm();
  r.hold_to_commit();
  const std::uint32_t b_start = r.t;  // (about)
  r.run(pw::kSaveWaitMs + 500, true);
  CHECK(r.host.count(Call::SaveCancel) == 1);
  CHECK(r.host.when(Call::SaveCancel) >= b_start + pw::kSaveWaitMs - 100);
  CHECK(r.svc.cancelled_save());
  CHECK(r.host.count(Call::CardRelease) == 1);  // the cancelled job was gone at once in this fake
  CHECK_FALSE(r.host.card_busy_seen_at_release);
}

TEST_CASE("power service: a job that is committing is not cancelled; the hard cap releases the card anyway") {
  Rig r;
  r.host.save_busy_until = ~0u;
  r.host.committing = true;
  r.arm();
  r.hold_to_commit();
  r.run(pw::kCardHardCapMs + 1000, true);
  CHECK(r.host.count(Call::SaveCancel) == 0);
  CHECK(r.host.count(Call::CardRelease) == 1);
  CHECK(r.host.card_busy_seen_at_release);  // a cut like a pulled card, after the cap
  CHECK(r.host.when(Call::CardRelease) >= r.host.when(Call::Quiesce) + pw::kCardHardCapMs - 100);
  CHECK(r.host.has_log("card still busy"));
  CHECK(r.svc.stage() == pw::Stage::Off);
}

TEST_CASE("power service: the card is never released while card_busy() before the hard cap") {
  Rig r;
  r.host.card_busy_until = 4000;  // the app's job runner claim (a listing, an audition, an import)
  r.arm();
  r.hold_to_commit();
  for (int i = 0; i < 6000; ++i) {
    r.run(1, true);
    if (r.host.count(Call::CardRelease) != 0) {
      break;
    }
    CHECK(r.host.count(Call::CardRelease) == 0);
  }
  CHECK(r.host.count(Call::CardRelease) == 1);
  CHECK_FALSE(r.host.card_busy_seen_at_release);
  CHECK(r.host.when(Call::CardRelease) >= 4000);
}

TEST_CASE("power service: every query stuck (a save, the card, the lights never finish): the off is reached inside the global deadline") {
  Rig r;
  r.host.save_busy_until = ~0u;
  r.host.committing = true;
  r.host.card_busy_until = ~0u;
  r.host.lights_done_at = ~0u;
  r.arm();
  r.pressed = false;
  const std::uint32_t t0 = r.t;
  CHECK(r.svc.request(r.t, false) == pw::Refusal::None);  // the console's request: no finger, so F0 passes at once
  while (r.t < t0 + pw::kGlobalCapMs + 2000 && r.host.count(Call::EnterOff) == 0) {
    r.pass();
  }
  NEED(r.host.count(Call::EnterOff) == 1);
  CHECK(r.host.when(Call::EnterOff) <= t0 + pw::kGlobalCapMs + 2);
  for (unsigned k = 0; k < kCallKinds; ++k) {
    CHECK(r.host.counts[k] <= 1);
  }
  // nothing after enter_off
  const unsigned total = r.host.total();
  r.run(5000, false);
  CHECK(r.host.total() == total);
}

TEST_CASE("power service: calls that take seconds hit the global deadline, which jumps to F and leaves the busy card alone") {
  Rig r;
  r.host.cost = 4000;  // every call with a side effect takes 4 s of fake time
  r.host.save_busy_until = ~0u;
  r.host.card_busy_until = ~0u;
  r.arm();
  const std::uint32_t t0 = r.t;
  CHECK(r.svc.request(r.t, false) == pw::Refusal::None);
  unsigned passes_after_cap = 0;
  unsigned passes_to_f = 0;  // the passes after the cap before F was entered (the amplifier key LOW is F's first act)
  while (r.t < t0 + 60000 && r.host.count(Call::EnterOff) == 0) {
    if (r.t >= t0 + pw::kGlobalCapMs) {
      ++passes_after_cap;
      passes_to_f += r.host.count(Call::AmpKeyLow) == 0 ? 1u : 0u;
    }
    r.pass();
  }
  NEED(r.host.count(Call::EnterOff) == 1);
  CHECK(r.svc.deadline_hit());
  CHECK(passes_to_f == 1);  // the cap fires on the first pass after it and enters F in that same pass
  CHECK(passes_after_cap <= passes_to_f + hold_cfg::kReleaseStableMs + 5);  // F0 then needs the 100 ms of released samples, which the 4 s calls had restarted
  CHECK(r.host.count(Call::CardRelease) == 0);  // busy: left as a pulled card, logged
  CHECK(r.host.has_log("card left busy"));
  CHECK(r.host.count(Call::AmpKeyLow) == 1);
  for (unsigned k = 0; k < kCallKinds; ++k) {
    CHECK(r.host.counts[k] <= 1);
  }
  CHECK(r.host.rec[r.host.n - 1].call == Call::EnterOff);
}

TEST_CASE("power service: F0 waits for a stable release, and a bounce starts the 100 ms again") {
  Rig r;
  r.arm();
  r.hold_to_commit();
  r.run(2000, true);
  NEED(r.svc.stage() == pw::Stage::Off);
  r.run(80, false);
  r.run(1, true);  // a bounce on the release
  r.run(80, false);
  CHECK(r.host.count(Call::EnterOff) == 0);
  r.run(30, false);
  CHECK(r.host.count(Call::EnterOff) == 1);
}

TEST_CASE("power service: the console's request has the refusals and no second hold") {
  {
    Rig r;
    r.host.firmware = true;
    r.arm();
    CHECK(r.svc.request(r.t, false) == pw::Refusal::Firmware);
    CHECK(r.host.total() == 0);
    CHECK_FALSE(r.svc.locked());
  }
  {
    Rig r;
    r.host.exporting = true;
    r.arm();
    CHECK(r.svc.request(r.t, false) == pw::Refusal::Export);
    CHECK(r.svc.request(r.t, false) == pw::Refusal::Export);  // no second-hold rule on the console
    CHECK(r.host.total() == 0);
  }
  {
    Rig r;
    r.host.disk = true;
    r.arm();
    CHECK(r.svc.request(r.t, true) == pw::Refusal::Disk);
    CHECK(r.host.total() == 0);
  }
  {
    Rig r;
    r.arm();
    CHECK(r.svc.request(r.t, true) == pw::Refusal::None);
    CHECK(r.svc.dry());
    CHECK(r.svc.request(r.t, true) == pw::Refusal::Running);
    r.run(2000, false);
    CHECK(r.host.count(Call::EnterOff) == 1);
    CHECK(r.host.count(Call::Quiesce) == 1);
  }
}

TEST_CASE("power service: the console's off is refused while Power is not stably released (a finger, or a polarity that reads pressed)") {
  Rig r;
  r.arm();
  r.pressed = true;
  r.run(300, true);
  CHECK(r.svc.request(r.t, false) == pw::Refusal::Held);  // a finger on it (or a switch that reads pressed for ever): F0 would wait for ever
  CHECK(r.host.total() == 0);
  CHECK_FALSE(r.svc.locked());
  r.run(50, false);
  CHECK(r.svc.request(r.t, false) == pw::Refusal::Held);  // released, but not for 100 ms yet
  r.run(100, false);
  CHECK(r.svc.request(r.t, false) == pw::Refusal::None);
  CHECK(r.host.count(Call::Quiesce) == 1);
}

TEST_CASE("power service: a commit while the stages run changes nothing (the state machine runs once)") {
  Rig r;
  r.arm();
  r.hold_to_commit();
  r.run(100, true);
  const unsigned total = r.host.total();
  PowerHold::Result again;
  again.commit = true;  // a second commit event, as a bug or a race would make it
  r.svc.step(r.t, again);
  CHECK(r.host.total() == total);
  CHECK(r.host.count(Call::Quiesce) == 1);
}

TEST_CASE("power service: the strip text names are all different words the app can show") {
  CHECK(std::strcmp(pw::stage_name(pw::Stage::Card), "B card") == 0);
  CHECK(std::strcmp(pw::stage_name(pw::Stage::Off), "F off") == 0);
  static_assert(pw::kGlobalCapMs > pw::kCardHardCapMs, "the stage caps are all below the global deadline");
  static_assert(pw::kSaveWaitMs < pw::kCardHardCapMs, "the save's soft cap comes before the hard one");
  static_assert(pw::kCardHardCapMs + pw::kUsbDrainMs + pw::kLightsCapMs < pw::kGlobalCapMs, "a worst case with every stage at its cap still ends before the deadline");
}

// ---- `diag power`'s text (power_report.h) ------------------------------------------------------------------------------------------------
TEST_CASE("power report: the idle report names the pin in the three pulls, the hold and the counters") {
  pw::PowerReport r;
  r.active_low = true;
  r.pull = 0;
  r.level_high = true;  // released on an active-low board
  r.read_high[0] = true;
  r.read_high[1] = true;
  r.read_high[2] = false;
  r.state = PowerHold::State::Armed;
  r.presses = 3;
  r.holds = 2;
  r.taps = 1;
  char buf[1024];
  const std::size_t n = pw::format_power_report(r, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strstr(buf, "power: PTC3 high (pull none), the switch reads released\n") != nullptr);
  CHECK(std::strstr(buf, "power: read now: no pull high, pull-up high, pull-down low\n") != nullptr);
  CHECK(std::strstr(buf, "power: board: pressed is low, internal pull none\n") != nullptr);
  CHECK(std::strstr(buf, "power: hold Armed, debounced up, line 0/1000\n") != nullptr);
  CHECK(std::strstr(buf, "power: presses 3 holds 2 taps 1 commits 0 refusals 0\n") != nullptr);
  CHECK(std::strstr(buf, "power: shutdown idle\n") != nullptr);
  CHECK(n < 700);  // (the console's buffer is 1.5 KB)
}

TEST_CASE("power report: an injected switch and a running shutdown say so") {
  pw::PowerReport r;
  r.active_low = false;
  r.pull = 2;
  r.level_high = true;  // pressed on an active-high board
  r.inject = pw::Inject::Pressed;
  r.state = PowerHold::State::Committed;
  r.pressed = true;
  r.fill = 1000;
  r.commits = 1;
  r.stage = pw::Stage::Lights;
  r.dry = true;
  r.deadline_hit = true;
  r.save_cancelled = true;
  char buf[1024];
  (void)pw::format_power_report(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "power: PTC3 high (pull down), the switch reads pressed, INJECTED pressed\n") != nullptr);
  CHECK(std::strstr(buf, "power: board: pressed is high, internal pull down\n") != nullptr);
  CHECK(std::strstr(buf, "power: hold Committed, debounced down, line 1000/1000\n") != nullptr);
  CHECK(std::strstr(buf, "power: shutdown stage E lights, dry, GLOBAL DEADLINE HIT, save cancelled\n") != nullptr);
  CHECK(std::strstr(buf, "stages (ms") == nullptr);  // (the stages' timings are the log lines, see "the stage lines of the log")
  // a small buffer cuts, never overruns
  char tiny[16];
  const std::size_t m = pw::format_power_report(r, tiny, sizeof tiny);
  CHECK(m == std::strlen(tiny));
  CHECK(m < sizeof tiny);
}

// ---- The record of the last shutdown (power_record.h) -------------------------------------------------------------------------------------
TEST_CASE("power record: a run writes the stages in order, whole after every step, and Done before enter_off returns") {
  Rig r;
  CHECK_FALSE(r.rec.valid());  // (zeros are not a record)
  r.arm();
  r.hold_to_commit();
  CHECK(r.rec.valid());
  CHECK(r.rec.stage == static_cast<std::uint8_t>(pw::Stage::Quiet));
  CHECK((r.rec.flags & pw::kRecDry) == 0u);
  r.run(2000, true);  // the finger stays down: the unit waits at F0
  NEED(r.svc.stage() == pw::Stage::Off);
  CHECK(r.rec.valid());
  CHECK(r.rec.stage == static_cast<std::uint8_t>(pw::Stage::Off));
  CHECK(r.rec.t[0] == 0u);  // A is the origin
  for (unsigned i = 1; i < 7; ++i) {
    CHECK(r.rec.t[i] >= r.rec.t[i - 1]);
  }
  CHECK(r.rec.t[1] >= pw::kQuietMs);  // A2 comes after the Stop reached the render
  CHECK(r.rec.t[6] > 0u);             // F was entered
  CHECK(r.rec.t[7] == 0u);            // not yet left
  CHECK(r.rec.flags == 0u);
  CHECK(r.rec.refusal == 0u);
  CHECK(r.rec.off == 0u);
  CHECK_FALSE(r.host.rec_seen_at_off);
  r.run(300, false);
  CHECK(r.host.count(Call::EnterOff) == 1);
  CHECK(r.rec.valid());
  CHECK(r.rec.stage == static_cast<std::uint8_t>(pw::Stage::Done));
  CHECK(r.rec.t[7] >= r.rec.t[6]);
  // The order itself: at the moment enter_off was entered (it does not return on the image) the record already read Done, whole (valid), with the time of F kept and the time of Done set.
  NEED(r.host.rec_seen_at_off);
  CHECK(r.host.rec_valid_at_off);
  CHECK(r.host.rec_stage_at_off == static_cast<std::uint8_t>(pw::Stage::Done));
  CHECK(r.host.rec_t_off_at_off == r.rec.t[6]);
  CHECK(r.host.rec_t_done_at_off == r.rec.t[7]);
  CHECK(r.host.rec_t_done_at_off >= r.host.rec_t_off_at_off);
  // the image's off platform adds its result to the same record
  r.rec.set_off(static_cast<std::uint8_t>(pw::OffMark::Failed), static_cast<std::uint8_t>(pw::OffStep::EnterVlpr));
  CHECK(r.rec.valid());
  CHECK((r.rec.off >> 4) == static_cast<std::uint8_t>(pw::OffMark::Failed));
  CHECK((r.rec.off & 0xFu) == static_cast<std::uint8_t>(pw::OffStep::EnterVlpr));
}

TEST_CASE("power record: the time saturates, a flipped bit is not a record, and garbage is not either") {
  pw::ShutdownRecord c = {};
  c.start(false);
  c.enter(3, 1'000'000u);
  CHECK(c.valid());
  CHECK(c.t[2] == 0xFFFFu);
  c.enter(9, 5u);  // a stage number out of range sets no time (and never indexes out of the array)
  CHECK(c.valid());
  for (unsigned byte = 0; byte < sizeof c; ++byte) {
    pw::ShutdownRecord bad = c;
    reinterpret_cast<unsigned char*>(&bad)[byte] ^= 0x10u;
    CHECK_FALSE(bad.valid());
  }
  pw::ShutdownRecord ones;
  std::memset(&ones, 0xFF, sizeof ones);
  CHECK_FALSE(ones.valid());
}

TEST_CASE("power record: the notes name what the run met: a dry run, a cancelled save, the caps, the lights, the deadline") {
  {
    Rig r;
    r.arm();
    CHECK(r.svc.request(r.t, true) == pw::Refusal::None);
    CHECK((r.rec.flags & pw::kRecDry) != 0u);
  }
  {
    Rig r;
    r.host.save_busy_until = ~0u;
    r.arm();
    r.hold_to_commit();
    r.run(pw::kSaveWaitMs + 500, true);
    CHECK((r.rec.flags & pw::kRecSaveCancelled) != 0u);
    CHECK((r.rec.flags & pw::kRecSaveCap) == 0u);
    CHECK((r.rec.flags & pw::kRecDeadline) == 0u);
  }
  {
    Rig r;
    r.host.save_busy_until = ~0u;
    r.host.committing = true;
    r.arm();
    r.hold_to_commit();
    r.run(pw::kCardHardCapMs + 1000, true);
    CHECK((r.rec.flags & pw::kRecSaveCap) != 0u);
    CHECK((r.rec.flags & pw::kRecCardBusy) != 0u);
    CHECK((r.rec.flags & pw::kRecSaveCancelled) == 0u);
  }
  {
    Rig r;
    r.host.lights_done_at = ~0u;
    r.arm();
    r.hold_to_commit();
    r.run(2000, true);
    CHECK((r.rec.flags & pw::kRecLightsCap) != 0u);
  }
  {
    Rig r;
    r.host.cost = 4000;
    r.host.save_busy_until = ~0u;
    r.host.card_busy_until = ~0u;
    r.arm();
    CHECK(r.svc.request(r.t, false) == pw::Refusal::None);
    for (int i = 0; i < 40000 && r.host.count(Call::EnterOff) == 0; ++i) {
      r.pass();
    }
    CHECK((r.rec.flags & pw::kRecDeadline) != 0u);
    CHECK((r.rec.flags & pw::kRecCardBusy) != 0u);
    CHECK(r.rec.valid());
  }
}

TEST_CASE("power record: a refusal is the last event and a new run clears it; the second hold of an export is noted") {
  Rig r;
  r.host.firmware = true;
  r.arm();
  r.hold_to_commit();
  CHECK(r.rec.valid());
  CHECK(r.rec.stage == 0u);
  CHECK(r.rec.refusal == static_cast<std::uint8_t>(pw::Refusal::Firmware));
  r.run(200, false);
  r.host.firmware = false;
  r.host.exporting = true;
  r.hold_to_commit();  // the export is refused once
  CHECK(r.rec.refusal == static_cast<std::uint8_t>(pw::Refusal::Export));
  CHECK(r.rec.stage == 0u);
  r.run(200, false);
  r.hold_to_commit();  // the second hold stops it and runs the stages
  CHECK(r.svc.locked());
  CHECK(r.rec.refusal == 0u);  // the new run cleared the refusal
  CHECK((r.rec.flags & pw::kRecExportCancelled) != 0u);
  CHECK(r.rec.stage == static_cast<std::uint8_t>(pw::Stage::Quiet));
  CHECK(r.rec.valid());
}

TEST_CASE("power record: no record given, the machine works as before (the host tests of P1)") {
  FakeHost host;
  std::uint32_t t = 0;
  host.clock = &t;
  PowerHold hold{true};
  pw::PowerService svc(host, hold);  // nullptr record
  for (int i = 0; i < 3000; ++i) {
    PowerHold::Result hr = hold.update(t, true, false, true);  // (active low: true is released)
    svc.step(t, hr);
    if (i == 300) {
      CHECK(svc.request(t, true) == pw::Refusal::None);
    }
    ++t;
  }
  CHECK(host.count(Call::EnterOff) == 1);
}
