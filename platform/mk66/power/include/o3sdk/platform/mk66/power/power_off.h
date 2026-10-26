// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER P2 (docs/specs/OS-POWER.md section 4.4, "P2 as built"): the real off. Stage F of the shutdown, after the stable release of Power, takes the chip out of HSRUN, into VLPR (very-low-power RUN),
// silences the pins, removes the SDRAM's supply and then POLLS PTC3 from a small loop in flash; a debounced press ends the loop with a software reset (a full boot). No LLWU, no VLLS, no wake source,
// no pad isolation, no change of the boot path. Our own design from the K66 reference manual (new Community OS behaviour); every register fact it rests on is cited where it is used (RM = K66 Sub-Family Reference Manual K66P144M180SF5RMV2 Rev 2,
// DS = data sheet K66P144M180SF5V2 Rev 4, AN4503) and listed with its page in the spec (section 4.7, RF1-RF13), because the repository holds no copy of the manual.
//
// The sequence is pure: it runs over an OffPlatform (the image's registers, a recording fake in the host test). Nothing here calls the millisecond clock (SysTick is stopped at step 1): every wait is
// a poll count in the platform and every time is the platform's wait_us() on the cycle counter.
//
//   1  quiet_cpu          interrupts masked, SysTick stopped. From here nothing may touch the SDRAM (the log, the sample arena, the app): the code and data of F live in flash and SRAM.
//   2  watchdog_running   [today never: startup.cpp switches the watchdog off and nothing re-enables it]. A running watchdog would reset the unit from VLPR (or need feeding there): until a later
//                         slice teaches the off to stop it, the off is a restart, visibly, instead of a half-state.
//   3  irc48m_off         the USB module's 48 MHz oscillator off (USB was detached at stage D and left it running): it "should be disabled by software prior to VLPR entry" (RM 6.7.2).
//   4  hsrun_dividers     ONE step of SIM_CLKDIV1 in HSRUN: every divider doubled (core /2, bus /4, FlexBus /4, flash /14), MCGOUTCLK still the PLL. In HSRUN a clock change is limited to a factor of 2 per
//                         step (RM 16.4.3.3), and the crystal is x11 slower than 180 MHz, so the source is not switched here: this one step is what puts every domain inside the RUN limits.
//   5  leave_hsrun        HSRUN -> RUN, wait PMSTAT. Neither VLPR nor a stop mode may be entered from HSRUN [RM 16.3.2 p342 note, Table 16-2 p347 (RF4)].
//   6  clock_to_crystal   in RUN: MCGOUTCLK from the PLL to the 16 MHz crystal (PEE -> PBE); the dividers of step 4 make that core 8 MHz.
//   7  set_dividers       SIM_CLKDIV1 to the VLPR limits (clock_config.h), while on the crystal, where the dividers only ever divide further (CLKDIV1 cannot be written in VLPR, RM 13.2.18).
//   8  mcg_to_blpe        PBE -> FBE -> BLPE (PLL off, the crystal stays the source): the MCG state VLPR needs [RM BLPE p648, Table 6-1 p120, Table 27-4 p646-647 (RF6, RF7)].
//   9  enter_vlpr         RUN -> VLPR, wait PMSTAT.
//   10 pins_quiet         the 39 SDRAM interface pins to GPIO outputs LOW (no pin drives a high level into a supply that is about to go), the board's other quiet pins, the amplifier key LOW, the LED off.
//   11 sdram_power_off    PTA24 LOW, the last of the outputs (only if kOffCutsSdram: it is false by default, PTA24 stays HIGH).
//   12 the poll loop      PTC3 every kOffPollUs. A stable release (core::power::kReleaseStableMs) first, then a press that stays down for kWakeDebounceMs, then (kWakeAfterRelease, the default) a stable
//                         release again, ends in reset(): a stuck-pressed switch (a wrong polarity constant) never wakes the unit and a glitch under the debounce never does either.
//
// ANY step 2-9 that fails or runs out of polls ends in reset() (a normal boot; the SDRAM is still powered, so it restarts from a known state), never in a half-entered mode. mark() leaves the step in the
// shutdown record (noinit SRAM), which `diag power` prints after the restart. Main loop only; the poll loop is the one deliberate unbounded loop of the design (it ends only in a reset).

#include <o3sdk/core/power_hold.h>

#include <cstdint>

namespace o3sdk::platform::mk66::power {

inline constexpr std::uint32_t kOffPollUs = 1000;      // the poll period of the off loop; the debounces below count polls, so they are milliseconds
inline constexpr std::uint32_t kWakeDebounceMs = 30;   // a press must stay down this long to wake the unit
// The bootloader runs first after any reset and what it does with a button held at that moment is open (spec section 6, R8). true [default, until QA-GLB-103 has passed on the unit]: the loop confirms
// the press (kWakeDebounceMs), then waits for a stable release (kReleasePolls) and resets then, so the bootloader never sees the button down: the wake comes at the RELEASE of a press, not at the press.
// false: the reset comes at the debounced press itself. The unit test of QA-GLB-103 (a .ptf in Firmware/, no card, held 1 s and 10 s) decides whether false is safe.
inline constexpr bool kWakeAfterRelease = true;
// Whether stage F removes the SDRAM's supply (PTA24 LOW, last of the outputs). false [default, until a unit test shows what the switched rail takes with it]: PTA24 stays HIGH, the SDRAM keeps its supply
// (its interface pins are silenced either way: the clock and CKE LOW, so it only loses its data). Why: if the external pull-up of PTC3 hangs on the rail PTA24 switches, PTC3 reads LOW (pressed) for good once
// the rail is down and the unit never wakes (the stuck-pressed rule waits for a release that cannot come). M3 compares the draw both ways and QA-GLB-117 checks the wake with true; the cost of false is the SDRAM's own
// idle current (CKE LOW, no clock: its power-down level; not measured, the part is not recorded in the tree).
inline constexpr bool kOffCutsSdram = false;

static_assert(kOffPollUs != 0u && 1000u % kOffPollUs == 0u, "the debounce counts are polls: a poll is a whole fraction of a millisecond");
inline constexpr std::uint32_t kPollsPerMs = 1000u / kOffPollUs;
inline constexpr std::uint32_t kWakePolls = kWakeDebounceMs * kPollsPerMs;
inline constexpr std::uint32_t kReleasePolls = core::power::kReleaseStableMs * kPollsPerMs;  // the released level this long before a press can wake (the 100 ms of F0)

// The step that failed (or None/Ready). The order of the enum is the order of the sequence.
enum class OffStep : std::uint8_t { None, Watchdog, HsrunDividers, LeaveHsrun, ClockToCrystal, Dividers, Blpe, EnterVlpr, Ready };
const char* off_step_name(OffStep s) noexcept;

// What the last off left in the shutdown record (power_record.h `off`, `diag power`): Started (no result: a hang, or the cable), Failed (restarted at a step), Woke (a press ended the poll loop).
enum class OffMark : std::uint8_t { None, Started, Failed, Woke };

// What the image does for the off, and the host test fakes. Main loop only, interrupts masked from quiet_cpu().
class OffPlatform {
 public:
  virtual void quiet_cpu() noexcept = 0;
  virtual bool watchdog_running() noexcept = 0;
  virtual void irc48m_off() noexcept = 0;        // the USB 48 MHz oscillator off (a write, nothing to wait for)
  virtual bool hsrun_dividers() noexcept = 0;    // false: SIM_CLKDIV1 does not read back what was written (an invalid ratio is ignored by the register)
  virtual bool leave_hsrun() noexcept = 0;       // false: PMSTAT never read RUN
  virtual bool clock_to_crystal() noexcept = 0;  // false: the wait ran out
  virtual bool set_dividers() noexcept = 0;      // false: the register does not read back what was written
  virtual bool mcg_to_blpe() noexcept = 0;       // false: the PLL did not leave
  virtual bool enter_vlpr() noexcept = 0;        // false: PMSTAT never read VLPR
  virtual void pins_quiet() noexcept = 0;
  virtual void sdram_power_off() noexcept = 0;
  virtual bool power_pressed() noexcept = 0;     // the raw switch level, through the polarity
  virtual void wait_us(std::uint32_t us) noexcept = 0;
  virtual void mark(OffMark mark, OffStep step) noexcept = 0;
  virtual void reset() noexcept = 0;             // boot::reset_chip(): never returns on the image

 protected:
  OffPlatform() = default;
  ~OffPlatform() = default;
  OffPlatform(const OffPlatform&) = delete;
  OffPlatform& operator=(const OffPlatform&) = delete;
};

// The poll loop's edge semantics as a pure object, one feed() per poll: the released level first (kReleasePolls in a row), then a press that stays down kWakePolls in a row. true: wake now. A press
// that is already down when the loop starts, or that never ends, never wakes; a glitch shorter than the debounce restarts the count. With `after_release` (the default, kWakeAfterRelease) the press is
// confirmed first and the wake comes when the level has then stayed released for kReleasePolls; without it the wake comes at the confirmed press.
class WakeWatch {
 public:
  explicit constexpr WakeWatch(bool after_release = kWakeAfterRelease) noexcept : after_release_(after_release) {}
  bool feed(bool pressed) noexcept;
  enum class Phase : std::uint8_t { WaitRelease, WaitPress, WaitFinalRelease };
  Phase phase() const noexcept { return phase_; }

 private:
  bool after_release_;
  Phase phase_ = Phase::WaitRelease;
  std::uint32_t run_ = 0;
};

// Steps 1-11. OffStep::Ready: the unit is in VLPR with its pins quiet, the poll loop may start. Any other value: that step failed, mark(Failed) was written and reset() has been called (the
// image never returns from it).
OffStep off_prepare(OffPlatform& p) noexcept;

// One poll of step 10: waits kOffPollUs, reads the switch, feeds the watch. On a confirmed wake it writes mark(Woke) and calls reset() and returns true.
bool off_poll(OffPlatform& p, WakeWatch& watch) noexcept;

// The whole off: off_prepare, then off_poll for ever. The image's enter_off() calls it; it does not return.
[[noreturn]] void off_run(OffPlatform& p) noexcept;

}  // namespace o3sdk::platform::mk66::power
