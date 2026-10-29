// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's side of OS-POWER P1 (docs/specs/OS-POWER.md): the Power switch PTC3 and what the shutdown does to the board. THE ONLY SOURCE THAT READS PTC3 (board::kPowerSwitch).
//
// The switch. PTC3 (Teensy pin 9) is a GPIO input with the board's pull (board.h kPowerPull: the internal pull-up, so that a pin nothing pulls cannot float low).
// Nothing is ever driven on it. It is read ONCE per pass of the main loop, by pass() below, called from appimg::pass(): not from an interrupt and not from the idle hook (which runs only inside
// boot::delay_ms()), so two samples can be seconds apart (a card mount, a codec unmute): core::PowerHold's gap rule is what keeps such a block from completing a hold. The pin is also read
// in the three pulls by `diag power` (report()), which is how the unit session settles the polarity and the pull (M4).
//
// The shutdown (PowerService over Mk66PowerHost, in the order of the spec's 4.1):
//   A   appimg::power_quiesce: the power-off latch (main.cpp: no audio stall verdict, no reopening of the outputs), the input locked, the transport and the live notes stopped, the app's power mode
//   A2  audio_stop(): the codec's click-free mute (sgtl5000.h run_stop: the DAC's soft ramp and its wait FIRST, then the amplifier key LOW, the outputs muted), the I2S and the two eDMA channels stopped
//   B   the device settings written at once, a save in flight finished (5 s) or cancelled; C the card released once nothing holds it
//   D   the `power: off` line, the CDC flushed, USB detached (usb0::detach_for_power)
//   E   the pad LEDs dark and in software shutdown, the backlight faded to 0 and held, the FT812 in power-down (PD# LOW, CS HIGH); the display is not touched after it
//   F   the amplifier key LOW once more, F0 the stable release of Power, then: `dry`, and a build with O3SDK_POWER_VLPR_OFF=OFF, restart (boot::reset_chip(), the P1 behaviour); otherwise (P2) the VLPR off:
//       Mk66OffPlatform below over the pure sequence of power_off.h (HSRUN to RUN to VLPR, the pins quiet, PTA24 LOW, then PTC3 polled; a debounced press is a software reset).
#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/display/device.h>
#include <o3sdk/platform/mk66/input/device.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/power/power_device.h>
#include <o3sdk/platform/mk66/power/power_off.h>
#include <o3sdk/platform/mk66/sdramc_config.h>
#include <o3sdk/platform/mk66/sections.h>
#include <o3sdk/platform/mk66/storage/device.h>
#include <o3sdk/platform/mk66/usb/console.h>
#include <o3sdk/platform/mk66/usb/usb0.h>

#include <cstdint>

#ifndef O3SDK_POWER_VLPR_OFF
#error "O3SDK_POWER_VLPR_OFF is the CMake option of that name (platform/mk66/CMakeLists.txt, the one place of its default)"
#endif

namespace o3sdk::platform::mk66::power::device {
namespace {

constexpr std::uint32_t kPcrPullEnable = 1u << 1;  // PORTx_PCRn[PE]
constexpr std::uint32_t kPcrPullUp = 1u << 0;      // PORTx_PCRn[PS]: 1 up, 0 down (with PE)
constexpr std::uint32_t kPcrSettleUs = 50;         // the pin settles after a pull change (an RC of a few kOhm and a few pF: well under this)
constexpr std::uint32_t kLightsFadeMaxMs = 500;    // the backlight's ramp (about 150 ms) with room: past it the display is held dark whatever it says

static_assert(board::kPowerPull <= 2u, "board.h kPowerPull: 0 none, 1 up, 2 down");

std::uint32_t pcr_for(std::uint8_t pull) noexcept {
  return regs::kPcrMuxGpio | (pull == 1u ? (kPcrPullEnable | kPcrPullUp) : (pull == 2u ? kPcrPullEnable : 0u));
}

bool pin_high() noexcept { return (regs::gpio_pdir(board::kPowerSwitch) & regs::mask_of(board::kPowerSwitch)) != 0u; }

std::uint32_t now32() noexcept { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

Inject g_inject = Inject::Real;  // main loop only
bool g_screenshot_request = false;  // OS-SCREENSHOT: set by pass() from PowerHold::Result::screenshot, read and cleared by take_screenshot_request() (main loop only)

// ---- The VLPR off (OS-POWER P2) -----------------------------------------------------------------------------------------------------------
// The record of the last shutdown (power_record.h): in the noinit SRAM, which a software reset keeps and a power-up fills with garbage (valid() refuses that). The state machine writes the stages and the
// notes into it, the off platform below the result of the VLPR off; `diag power` prints it after the boot. Main loop, then the off loop.
O3SDK_NOINIT ShutdownRecord g_rec;

// A wait on a status flag, by poll count (the millisecond clock is off): a poll is at least 4 cycles, so the limit is at least 0.8 M cycles. In time that is about 8.9 ms at the 90 MHz core of
// leave_hsrun (the shortest bound), 8.9 to 100 ms in clock_to_crystal (the core falls from 90 to 8 MHz as CLKST changes) and at least 0.4 s at the 2 MHz core of mcg_to_blpe and enter_vlpr (the longest);
// the core never runs at the crystal's 16 MHz in the off. The waits that matter (CLKST, PLLST, PMSTAT) end in microseconds. A wait that runs out ends in a restart (power_off.h), never in a stop.
constexpr std::uint32_t kOffPollLimit = 200'000u;
template <typename Done>
bool poll_until(Done done) noexcept {
  for (std::uint32_t i = 0; i < kOffPollLimit; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// A pin to a GPIO output at `high`: the level first, then the direction, then the pin's function, so a pin leaves its peripheral for a driven level and never floats on the way.
void drive(Pin pin, bool high) noexcept {
  if (high) {
    regs::gpio_psor(pin) = regs::mask_of(pin);
  } else {
    regs::gpio_pcor(pin) = regs::mask_of(pin);
  }
  regs::gpio_pddr(pin) = regs::gpio_pddr(pin) | regs::mask_of(pin);
  regs::port_pcr(pin) = regs::kPcrMuxGpio;
}

// PTC3 as the poll loop needs it: the PORTC clock gate on, the pin an input, the configured pull. The one place of these three writes: init() runs it once, and pins_quiet() runs it again once VLPR is
// entered and every other PORTC pin has been written, so that the poll loop never rests on a state that the sequence only assumed.
void power_switch_input() noexcept {
  regs::sim_scgc5() = regs::sim_scgc5() | regs::kScgc5PortC;  // (boot::early_pins turned every port on; this does not rely on it)
  regs::gpio_pddr(board::kPowerSwitch) = regs::gpio_pddr(board::kPowerSwitch) & ~regs::mask_of(board::kPowerSwitch);  // an input, explicitly: nothing is ever driven on this pin
  regs::port_pcr(board::kPowerSwitch) = pcr_for(board::kPowerPull);
}

// PTC3 must be on none of the pins pins_quiet() drives (the board's quiet table, the I2S master clock, the 39 SDRAM pins) nor on the SDRAM supply key: a pin that is driven stops being the switch, and the
// unit could never wake. Checked at compile time (the host test mk66_power_off_test.cpp holds the same checks, so a change to a table fails the host build too).
constexpr bool quiet_table_has(Pin pin) noexcept {
  for (const board::QuietPin& quiet : board::kQuietPins) {
    if (quiet.pin == pin) {
      return true;
    }
  }
  return false;
}
constexpr bool sdram_pins_have(Pin pin) noexcept {
  for (const Pin& sdram_pin : sdram::kPins) {
    if (sdram_pin == pin) {
      return true;
    }
  }
  return false;
}
static_assert(!quiet_table_has(board::kPowerSwitch), "OS-POWER P2: PTC3 (the Power switch) is in board::kQuietPins, which pins_quiet() drives: the unit could never wake");
static_assert(!sdram_pins_have(board::kPowerSwitch), "OS-POWER P2: PTC3 (the Power switch) is one of the 39 SDRAM pins, which pins_quiet() drives: the unit could never wake");
static_assert(!(board::kPowerSwitch == board::kI2sMclk) && !(board::kPowerSwitch == board::kSdramPower), "OS-POWER P2: PTC3 is a pin that the off drives");

// The MCG_C1 value of PEE -> PBE (RF5): CLKS = 10b (the crystal), FRDIV as init_clocks() wrote it for FBE, IREFS = 0 (the external reference stays selected), IRCLKEN/IREFSTEN clear. The PLL is left locked.
constexpr std::uint8_t kMcgC1Pbe = static_cast<std::uint8_t>((2u << regs::kMcgC1ClksShift) | (clock::kFrdiv << regs::kMcgC1FrdivShift));
static_assert(kMcgC1Pbe == 0xA0u, "RF5: MCG_C1 = CLKS 10b (bits 7:6), FRDIV 4 (bits 5:3), IREFS 0, IRCLKEN 0, IREFSTEN 0");
// RF6: the write of mcg_to_blpe() takes PLLS (MCG_C6 bit 6) to 0 and keeps VDIV (bits 4:0).
static_assert((clock::kVdiv & regs::kMcgC6Plls) == 0u && (clock::kVdiv & regs::kMcgC6Cme0) == 0u && (clock::kVdiv & ~0x1Fu) == 0u,
              "RF6: MCG_C6 = VDIV only: PLLS clear (PBE -> FBE) and CME0 clear (it must be 0 for BLPE, RM p636; this write clears it), LOLIE0 clear");

// Every register fact below is from the K66 reference manual and data sheet (MCG, SMC, SIM, WDOG chapters; the RF rows of spec 4.7 give the page of each, checked 2026-10-17). Runs from flash with the
// interrupts masked; touches no SDRAM (this object is on the caller's stack, the vtable and the code are in flash, the marks are in SRAM_U).
class Mk66OffPlatform final : public OffPlatform {
 public:
  void quiet_cpu() noexcept override {
    asm volatile("cpsid i" ::: "memory");
    regs::syst_csr() = 0;  // the millisecond tick: its reload is for 180 MHz, and nothing reads the clock from here
    asm volatile("dsb\n\tisb" ::: "memory");
  }
  // The watchdog is off since startup.cpp (WDOGEN clear) and nothing re-enables it today. Should a later slice do so, the off is a restart until it also stops it here.
  bool watchdog_running() noexcept override { return (regs::wdog_stctrlh() & regs::kWdogEn) != 0u; }

  // The USB module's 48 MHz oscillator, off [RM 6.7.2: "should be disabled by software prior to VLPR entry"]; usb0::detach_for_power() (stage D) left it running. The module is detached and its interrupt off.
  void irc48m_off() noexcept override { usb0::irc48m_off(); }
  // The one step in HSRUN [RM 16.4.3.3: a clock change in HSRUN is limited to a factor of 2 per step]: SIM_CLKDIV1 to every divider doubled, MCGOUTCLK still the PLL (clock_config.h kExit*: core 90 MHz, bus and
  // FlexBus 45, flash 12.9). Read back (RF9: an invalid ratio is ignored by the register): a register that did not take it would leave HSRUN with the clocks it has, and the exit must not be attempted.
  // No delay follows the write: the RM (16.4.3.3 p352) and AN4503 (p37) prescribe no wait and no status poll after a divider change, nor after clearing RUNM in leave_hsrun(); the only poll they prescribe is
  // PMSTAT on HSRUN ENTRY (boot.cpp). The PMSTAT poll in leave_hsrun() is this code's own check, so that a transition that did not happen ends in a restart.
  bool hsrun_dividers() noexcept override {
    regs::sim_clkdiv1() = clock::kExitClkdiv1;
    if (regs::sim_clkdiv1() != clock::kExitClkdiv1) {
      return false;
    }
    boot::set_core_hz(clock::kExitCoreHz);
    return true;
  }
  // HSRUN -> RUN [RF2, RF4]: PMCTRL[RUNM] = RUN, wait PMSTAT = RUN. Allowed only at a clock RUN allows, which hsrun_dividers() has made.
  bool leave_hsrun() noexcept override {
    regs::smc_pmctrl() = regs::kPmctrlRunmRun;
    return poll_until([] { return regs::smc_pmstat() == regs::kPmstatRun; });
  }
  // In RUN. PEE -> PBE [RF5]: MCG_C1[CLKS] = external reference, as init_clocks() wrote it for FBE; the PLL keeps running until mcg_to_blpe(). The dividers are those of hsrun_dividers() (/2 /4 /4 /14), so
  // every domain is at most 8 MHz, 4 MHz, 4 MHz and 1.14 MHz: the core (limit 4 MHz in VLPR) and the flash (limit 1 MHz) are still over the VLPR limits here. set_dividers(), run in RUN before the VLPR
  // entry, is what brings the clocks inside them. The x11 drop of the source is made here, in RUN, not in HSRUN (the factor-2 rule).
  bool clock_to_crystal() noexcept override {
    regs::mcg_c1() = kMcgC1Pbe;
    if (!poll_until([] { return (regs::mcg_s() & regs::kMcgSClkstMask) == regs::kClkstExternal; })) {
      return false;
    }
    boot::set_core_hz(clock::kExitCrystalCoreHz);
    return true;
  }
  // SIM_CLKDIV1 to the VLPR limits while MCGOUTCLK is the crystal, in RUN [RF9, RM 13.2.18: writable at any time in RUN, not in VLPR; an invalid ratio is ignored]. Read back: a register that did not take it
  // would leave the core at 8 MHz, over the VLPR limit, and the entry must not be attempted.
  bool set_dividers() noexcept override {
    regs::sim_clkdiv1() = clock::kVlprClkdiv1;
    if (regs::sim_clkdiv1() != clock::kVlprClkdiv1) {
      return false;
    }
    boot::set_core_hz(clock::kVlprCoreHz);
    return true;
  }
  // PBE -> FBE -> BLPE [RF6, RF7]: PLLS clear (the PLL is off outside PBE and PEE, C5[PLLCLKEN0] is 0), wait PLLST clear; then C2[LP] set with the crystal still the source: the bypassed low-power MCG state
  // that VLPR needs. No status bit says LP took; CLKST must still read the crystal.
  bool mcg_to_blpe() noexcept override {
    regs::mcg_c6() = static_cast<std::uint8_t>(clock::kVdiv);
    if (!poll_until([] { return (regs::mcg_s() & regs::kMcgSPllst) == 0u; })) {
      return false;
    }
    regs::mcg_c2() = static_cast<std::uint8_t>(regs::mcg_c2() | regs::kMcgC2Lp);
    return (regs::mcg_s() & regs::kMcgSClkstMask) == regs::kClkstExternal;
  }
  // RUN -> VLPR [RF1, RF2, RF3]: PMCTRL[RUNM] = VLPR (PMPROT[AVLP] was set once by init_clocks()), wait PMSTAT = VLPR.
  bool enter_vlpr() noexcept override {
    regs::smc_pmctrl() = regs::kPmctrlRunmVlpr;
    return poll_until([] { return regs::smc_pmstat() == regs::kPmstatVlpr; });
  }

  // The board's quiet state as boot::early_pins() sets it, with two differences: the FT812's PD# stays LOW (stage E put the chip in power-down; the table's HIGH would wake it) and the pins that
  // run something are made quiet too: the I2S master clock, which the audio stop leaves running (MCR[MOE] drives PTE6 from the system clock, whose rate has just changed), and the 39 SDRAM
  // interface pins, driven LOW so that none drives a high level into a supply that may be removed (kOffCutsSdram; the clock pin goes first, so the control lines move with no edge to sample).
  // Runs right after the entry into VLPR. PTC3 is on none of these pins (the static_asserts above); the last act is to put its clock gate, direction and pull back, after every other PORTC write.
  void pins_quiet() noexcept override {
    for (const board::QuietPin& quiet : board::kQuietPins) {
      drive(quiet.pin, quiet.pin == board::kFt812PowerDown ? false : quiet.high);
    }
    drive(board::kI2sMclk, false);
    for (const Pin pin : sdram::kPins) {
      drive(pin, false);
    }
    power_switch_input();
  }
  void sdram_power_off() noexcept override { regs::gpio_pcor(board::kSdramPower) = regs::mask_of(board::kSdramPower); }  // only called when kOffCutsSdram

  bool power_pressed() noexcept override { return pin_high() != board::kPowerActiveLow; }
  void wait_us(std::uint32_t us) noexcept override { boot::delay_us(us); }
  void mark(OffMark mark, OffStep step) noexcept override { g_rec.set_off(static_cast<std::uint8_t>(mark), static_cast<std::uint8_t>(step)); }
  void reset() noexcept override { boot::reset_chip(); }
};

core::PowerShow g_show;  // what the last pass told the app (for `diag power`)

// The lights stage (E): started once by lights_out(), polled by lights_done().
bool g_lights_done = false;
std::uint32_t g_lights_since = 0;

class Mk66PowerHost final : public PowerHost {
 public:
  bool firmware_active() noexcept override { return appimg::firmware_update_active(); }
  bool export_running() noexcept override { return appimg::power_export_running(); }
  // The computer has the card, the disk is writable and not ejected: it may be in the middle of a copy. The same condition as the boot disk's second key press (app_main.cpp disk_boot_pass).
  bool disk_writing() noexcept override {
    const usb0::DiskState d = usb0::disk_state();
    return d.configured && d.present && !d.ejected && d.writable;
  }
  void export_cancel() noexcept override { appimg::power_export_stop(); }

  void quiesce_engine() noexcept override { appimg::power_quiesce(); }

  void audio_down() noexcept override {
    appimg::service_while_blocked(true);  // the codec's waits keep the display serviced, as at the unmute
    audio_stop();
    appimg::audio_closed();  // the app's "outputs open" follows the codec (the latch keeps main.cpp from opening them again)
    appimg::service_while_blocked(false);
  }

  // The settings flush needs no call: appimg::power_quiesce() put the save layer in its power mode at stage A (SaveService::power_begin), which writes the device settings at once, with the same
  // writer, the same file and the same quiet job as the 1.5 s timer. This is the stage's marker in the call order.
  void settings_flush() noexcept override {}
  bool save_busy() noexcept override { return appimg::power_save_busy(); }
  bool save_committing() noexcept override { return appimg::power_save_committing(); }
  void save_cancel() noexcept override { appimg::power_save_cancel(); }

  // Every claim on the card, and in the boot disk a SCSI command in flight (a WRITE(10) may be half done): the pump keeps running in the main loop, so it ends; stage C closes the disk between two commands.
  bool card_busy() noexcept override {
    return storage::device::card_busy() || (storage::device::owner() == storage::CardOwner::Host && usb0::disk_state().busy);
  }
  void card_release() noexcept override {
    if (storage::device::owner() == storage::CardOwner::Host) {
      usb0::disk_close();  // the boot disk: the MSC engine answers NOT READY from here; the computer sees the unplug in stage D without an eject
    } else {
      storage::device::release();  // FatFs lets go of the volume: nothing is written, nothing cancelled
    }
  }

  void usb_drain() noexcept override {
    console::log("power: off");
    usb0::cdc_writer().flush();
  }
  void usb_down() noexcept override { usb0::detach_for_power(); }

  void lights_out() noexcept override {
    g_lights_since = now32();
    input::device::pad_leds().park();                // every pad dark, then the chip in software shutdown (PadLeds::park)
    display::device::display().set_backlight_target(0);  // through the driver's ramp
  }
  bool lights_done() noexcept override {
    if (g_lights_done) {
      return true;
    }
    display::Ft812Display& d = display::device::display();
    const bool timed_out = now32() - g_lights_since >= kLightsFadeMaxMs;
    const bool leds_dark = input::device::pad_leds().parked();
    const bool screen_dark = !d.ready() || d.failed() || (d.backlight() == 0u && d.idle());
    if (!timed_out && !(leds_dark && screen_dark)) {
      return false;
    }
    d.hold_dark();                                                    // the backlight stays off for good
    regs::gpio_psor(board::kFt812ChipSelect) = regs::mask_of(board::kFt812ChipSelect);  // chip select released
    regs::gpio_pcor(board::kFt812PowerDown) = regs::mask_of(board::kFt812PowerDown);    // PD# LOW: the FT812 in power-down
    appimg::power_display_off();                                      // from here appimg::pass() does not touch the display
    g_lights_done = true;
    return true;
  }

  void amp_key_low() noexcept override { regs::gpio_pcor(board::kAudioMute) = regs::mask_of(board::kAudioMute); }  // a GPIO write: it cannot hang
  // After the stable release of F0. `dry` (or a build with the VLPR off switched off): the restart of P1, which drops the amplifier key again. Otherwise the VLPR off, which ends only in a reset: a
  // failed step or a debounced press. The line below is the last one the diag log gets (the SDRAM loses its supply in the sequence).
  void enter_off(bool dry) noexcept override {
    if (dry || O3SDK_POWER_VLPR_OFF == 0) {
      boot::reset_chip();
    }
    console::log("power: VLPR off");
    Mk66OffPlatform off;
    off_run(off);
  }
  void log(const char* text) noexcept override { console::log(text); }
};

Mk66PowerHost& host() noexcept {
  static Mk66PowerHost h;
  return h;
}

core::PowerHold& hold() noexcept {
  static core::PowerHold h(board::kPowerActiveLow);
  return h;
}

PowerService& svc() noexcept {
  static PowerService s(host(), hold(), &g_rec);
  return s;
}

}  // namespace

void init() noexcept { power_switch_input(); }

core::PowerShow pass(std::uint32_t now_ms, bool shift_down, bool ready) noexcept {
  const bool high = g_inject == Inject::Real ? pin_high() : ((g_inject == Inject::Pressed) != board::kPowerActiveLow);
  const core::PowerHold::Result r = hold().update(now_ms, high, shift_down, ready);
  if (r.stuck) {
    console::log("power: held or stuck since boot: waiting for a release");
  }
  g_screenshot_request = g_screenshot_request || r.screenshot;
  svc().step(now_ms, r);
  g_show = svc().show(now_ms, r);
  return g_show;
}

bool take_screenshot_request() noexcept {
  const bool r = g_screenshot_request;
  g_screenshot_request = false;
  return r;
}

bool pressed() noexcept { return hold().pressed(); }

bool locked() noexcept { return svc().locked(); }

bool vlpr_off() noexcept { return O3SDK_POWER_VLPR_OFF != 0; }

Refusal request_off(std::uint32_t now_ms, bool dry) noexcept { return svc().request(now_ms, dry); }

void inject(Inject how) noexcept { g_inject = how; }

PowerReport report() noexcept {
  PowerReport r;
  r.active_low = board::kPowerActiveLow;
  r.pull = board::kPowerPull;
  for (unsigned pull = 0; pull < 3; ++pull) {  // the pin with no pull, the pull-up and the pull-down, then the configured one again
    regs::port_pcr(board::kPowerSwitch) = pcr_for(static_cast<std::uint8_t>(pull));
    boot::delay_us(kPcrSettleUs);
    r.read_high[pull] = pin_high();
  }
  regs::port_pcr(board::kPowerSwitch) = pcr_for(board::kPowerPull);
  boot::delay_us(kPcrSettleUs);
  r.inject = g_inject;
  r.level_high = g_inject == Inject::Real ? pin_high() : ((g_inject == Inject::Pressed) != board::kPowerActiveLow);
  const core::PowerHold& h = hold();
  r.state = h.state();
  r.pressed = h.pressed();
  r.presses = h.presses();
  r.holds = h.holds();
  r.taps = h.taps();
  r.commits = h.commits();
  r.refusals = h.refusals();
  const PowerService& s = svc();
  r.fill = g_show.fill;
  r.stage = s.stage();
  r.dry = s.dry();
  r.deadline_hit = s.deadline_hit();
  r.save_cancelled = s.cancelled_save();
  r.rec = g_rec;
  r.rec_valid = g_rec.valid();
  return r;
}

}  // namespace o3sdk::platform::mk66::power::device
