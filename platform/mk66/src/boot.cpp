// SPDX-License-Identifier: GPL-3.0-or-later
//
// The steps of the MK66 reset sequence (startup.cpp calls them in order): pins that must be safe first, the cycle counter,
// the clock tree, interrupts, SysTick, and the SDRAM. Register values come from the *_config.h headers, which are checked
// at compile time; this file only writes them, in the order the reference manual asks for.
//
// Nothing here may hang silently: every wait on hardware is bounded, and a wait that runs out stops the image with a
// blink code (boot::fail) instead of spinning with a dark LED and a muted amplifier.
#include "internal.h"
#include "regs.h"

#include <o3sdk/hal/irq.h>
#include <o3sdk/platform/mk66/blink.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/crash_record.h>
#include <o3sdk/platform/mk66/idle_hook.h>
#include <o3sdk/platform/mk66/irq_map.h>
#include <o3sdk/platform/mk66/mem_map.h>
#include <o3sdk/platform/mk66/memtest.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/resources.h>
#include <o3sdk/platform/mk66/sdramc_config.h>
#include <o3sdk/platform/mk66/sections.h>

#ifndef O3SDK_SDRAM_FULL_TEST
#define O3SDK_SDRAM_FULL_TEST 1
#endif
#ifndef O3SDK_SDRAM_ICODE_ALIAS
#error "O3SDK_SDRAM_ICODE_ALIAS is the CMake option of that name (platform/mk66/CMakeLists.txt, the one place of its default)"
#endif

// Linker script symbols (mk66.ld): the free SDRAM behind `.sdram_bss`.
extern "C" {
extern std::uint8_t __sdram_arena_start[];
extern std::uint8_t __sdram_arena_end[];
}

namespace o3sdk::platform::mk66 {

namespace boot {
namespace {

// The core clock the busy-wait delays are sized for: updated as the clock tree comes up.
std::uint32_t g_core_hz = clock::kFeiHz;

// Upper bound on any wait for a hardware flag, in polls. A poll is a few cycles, so at the slowest clock this ends in about a
// second: far longer than a crystal needs to start (milliseconds) or the PLL to lock (about a millisecond), short enough
// that a dead crystal is a blink code, not a mystery.
constexpr std::uint32_t kPollLimit = 1'500'000u;

template <typename Predicate>
bool wait_until(Predicate done) noexcept {
  for (std::uint32_t i = 0; i < kPollLimit; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// Drives a pin as a GPIO output at `high`: level first, then the direction, so the pin never glitches the other way.
// `pcr` is the whole port control word (the default is the plain GPIO function, mux 1).
void gpio_output(Pin pin, bool high, std::uint32_t pcr = regs::kPcrMuxGpio) noexcept {
  regs::port_pcr(pin) = pcr;
  if (high) {
    regs::gpio_psor(pin) = regs::mask_of(pin);
  } else {
    regs::gpio_pcor(pin) = regs::mask_of(pin);
  }
  regs::gpio_pddr(pin) = regs::gpio_pddr(pin) | regs::mask_of(pin);
}

// The SDRAM power key (PTA24) HIGH with its PCR (board.h kSdramPowerPcr: high drive, slew-rate limited). Idempotent: the reset
// sequence drives it first (early_pins) and the SDRAM bring-up drives it again, the same value on an already high pin, no glitch.
static_assert((board::kSdramPowerPcr & (7u << regs::kPcrMuxShift)) == regs::kPcrMuxGpio, "the power key's PCR must select the GPIO function");
void rail_on() noexcept { gpio_output(board::kSdramPower, true, board::kSdramPowerPcr); }

std::uint32_t sdram_words() noexcept { return mem::kSdramSize / 4u; }

// The SDRAM as the memory tests see it: raw volatile accesses through the controller's window.
struct SdramMem {
  volatile std::uint32_t* words;
  std::uint32_t read(std::uint32_t index) const { return words[index]; }
  void write(std::uint32_t index, std::uint32_t value) const { words[index] = value; }
  void write8(std::uint32_t byte_index, std::uint8_t value) const {
    reinterpret_cast<volatile std::uint8_t*>(words)[byte_index] = value;
  }
};

}  // namespace

void led_set(bool on) noexcept {
  // "on" is a level of the pin through the LED's polarity (board.h: the LED is active low).
  const bool high = on != board::kStatusLedActiveLow;
  if (high) {
    regs::gpio_psor(board::kStatusLed) = regs::mask_of(board::kStatusLed);
  } else {
    regs::gpio_pcor(board::kStatusLed) = regs::mask_of(board::kStatusLed);
  }
}

void early_pins() noexcept {
  regs::sim_scgc5() = regs::sim_scgc5() | regs::kScgc5Ports;
  // OS-BUG-PADS step 3. The power key (PTA24, board.h kSdramPower) is the first pin driven and the amplifier mute (the first
  // kQuietPinsAtOnce entries of kQuietPins: LOW, which cannot back-power anything) follows at once. Then a bounded wait, so that the
  // HIGH quiet pins below (FT812 chip select and PD#, the status LED, MIDI TX) are not driven into a rail that is still ramping.
  // How long the rail takes is not measured (open question, board.h), so kRailSettleUs is a compromise, not a guarantee.
  rail_on();
  static_assert(board::kQuietPinsAtOnce == 1 && board::kQuietPinCount > board::kQuietPinsAtOnce && board::kQuietPins[0].pin == board::kAudioMute &&
                    !board::kQuietPins[0].high,
                "the pins driven before the rail wait are the amplifier mute, LOW");
  for (unsigned i = 0; i < board::kQuietPinsAtOnce; ++i) {
    gpio_output(board::kQuietPins[i].pin, board::kQuietPins[i].high);
  }
  // The wait, on the cycle counter at the reset clock. Not delay_us(): its core clock variable lives in .data, which the C runtime
  // has not copied yet (startup.cpp step 4). The cycle count is a constant; the loop is bounded by it (each poll is at least a cycle)
  // even if the counter does not run, and the counter is restarted by enable_dwt() in step 5 as before.
  constexpr std::uint32_t kRailSettleCycles = static_cast<std::uint32_t>((std::uint64_t{clock::kFeiHz} * board::kRailSettleUs) / 1'000'000u);
  enable_dwt();
  const std::uint32_t start = regs::dwt_cyccnt();
  for (std::uint32_t spin = 0; spin < kRailSettleCycles; ++spin) {
    if (static_cast<std::uint32_t>(regs::dwt_cyccnt() - start) >= kRailSettleCycles) {
      break;
    }
  }
  // The rest of the board's quiet state (board.h kQuietPins), in table order: each pin is driven to its quiet level first, so the
  // amplifier never hears this image and nothing that matters floats once the rail has had its time.
  for (unsigned i = board::kQuietPinsAtOnce; i < board::kQuietPinCount; ++i) {
    gpio_output(board::kQuietPins[i].pin, board::kQuietPins[i].high);
  }
}

void enable_dwt() noexcept {
  regs::demcr() = regs::demcr() | regs::kDemcrTrcena;
  regs::dwt_cyccnt() = 0;
  regs::dwt_ctrl() = regs::dwt_ctrl() | regs::kDwtCyccntena;
}

void delay_us(std::uint32_t us) noexcept {
  const std::uint32_t wait = us * (g_core_hz / 1'000'000u);
  const std::uint32_t start = regs::dwt_cyccnt();
  // Bounded by `wait` polls (each is at least one cycle) even if the counter is not running.
  for (std::uint32_t spin = 0; spin < wait; ++spin) {
    if (static_cast<std::uint32_t>(regs::dwt_cyccnt() - start) >= wait) {
      return;
    }
  }
}

namespace {
// Main thread only (see set_idle_hook in internal.h): no interrupt reads or writes either. The rule that it is never entered from
// itself is the object's (idle_hook.h, tested).
IdleHook g_idle;
}  // namespace

void set_idle_hook(IdleHook::Function hook) noexcept { g_idle.set(hook); }

// OS-BUG-PADS: the SysTick uptime (ms) at which the board's power key (PTA24, the SDRAM's; whether it feeds the pad boards is open) was switched on, read by the
// input bring-up (the pad bus waits a second after it). Step 3 raises the key in early_pins, the first pin action of the reset, before SysTick
// starts (the uptime clock counts from start_systick), so the stamp is 0: the real time since the rail is a little longer than the uptime, which
// only makes the wait at least a second. A constant, no variable.
std::uint32_t rail_up_ms() noexcept { return 0; }

void delay_ms(std::uint32_t ms) noexcept {
  for (std::uint32_t i = 0; i < ms; ++i) {
    delay_us(1000);
    g_idle.run();
  }
}

[[noreturn]] void fail(std::uint8_t pulses) noexcept {
  for (std::uint32_t t = 0;; t += 50) {
    led_set(blink::led_on(pulses, t));
    delay_ms(50);
  }
}

// The LED from SRAM_L (track3r-os's main.cpp did the same): a pulse from here means `.ramfunc_l` was copied from flash and runs. The
// register accessors are inline, so nothing here calls into flash (tools/check_ramfunc.py, ctest <app>_ramfunc).
O3SDK_RAMFUNC_L void led_from_sram_l(bool on) noexcept {
  const bool high = on != board::kStatusLedActiveLow;
  if (high) {
    regs::gpio_psor(board::kStatusLed) = regs::mask_of(board::kStatusLed);
  } else {
    regs::gpio_pcor(board::kStatusLed) = regs::mask_of(board::kStatusLed);
  }
}

void play_code(std::uint8_t pulses, std::uint32_t periods) noexcept {
  const std::uint32_t total_ms = periods * blink::period_ms(pulses);
  for (std::uint32_t t = 0; t < total_ms; t += 10) {
    led_from_sram_l(blink::led_on(pulses, t));
    delay_ms(10);
  }
  led_from_sram_l(false);
}

// ---- Clocks ---------------------------------------------------------------------------------------------------------
// The clock sequence (F_CPU = 180 MHz) with the waits the reference
// manual asks for between the steps, each bounded:
//   FEI (reset state) -> FBE (crystal) -> HSRUN -> PBE (PLL locked) -> PEE (PLL is MCGOUTCLK).
void init_clocks() noexcept {
  using namespace regs;
  // Write-once after reset: allow HSRUN (and the low-power modes, for later). If it was written already
  // without AHSRUN the HSRUN wait below stops the image with a code.
  smc_pmprot() = static_cast<std::uint8_t>(kPmprotAhsrun | kPmprotAvlp | kPmprotAlls | kPmprotAvlls);

  // FEI -> FBE. 16 MHz crystal, very-high-frequency range, oscillator (EREFS), 10 pF load; the external reference becomes
  // the clock source and the FLL reference divides to 31.25 kHz (FRDIV = 4: / 512).
  osc0_cr() = static_cast<std::uint8_t>(kOscErclken | kOscSc2p | kOscSc8p);
  mcg_c2() = static_cast<std::uint8_t>((2u << kMcgC2RangeShift) | kMcgC2Erefs);
  mcg_c1() = static_cast<std::uint8_t>((2u << kMcgC1ClksShift) | (clock::kFrdiv << kMcgC1FrdivShift));
  if (!wait_until([] { return (mcg_s() & kMcgSOscinit0) != 0; }) ||     // crystal oscillating
      !wait_until([] { return (mcg_s() & kMcgSIrefst) == 0; }) ||       // FLL reference is the external one
      !wait_until([] { return (mcg_s() & kMcgSClkstMask) == kClkstExternal; })) {  // MCGOUTCLK is the crystal
    fail(blink::kClockFailure);
  }
  g_core_hz = clock::kCrystalHz;

  // HSRUN: required above 120 MHz.
  smc_pmctrl() = kPmctrlRunmHsrun;
  if (!wait_until([] { return smc_pmstat() == kPmstatHsrun; })) {
    fail(blink::kClockFailure);
  }

  // FBE -> PBE: PLL on (reference / 2 = 8 MHz, x 45 = 360 MHz VCO, / 2 = 180 MHz), wait until selected and locked.
  mcg_c5() = static_cast<std::uint8_t>(clock::kPrdiv);
  mcg_c6() = static_cast<std::uint8_t>(kMcgC6Plls | clock::kVdiv);
  if (!wait_until([] { return (mcg_s() & (kMcgSPllst | kMcgSLock0)) == (kMcgSPllst | kMcgSLock0); })) {
    fail(blink::kClockFailure);
  }

  // Dividers while still on the crystal (they only ever divide further): core /1, bus /2, FlexBus /2, flash /7.
  sim_clkdiv1() = clock::kClkdiv1;

  // PBE -> PEE: the PLL becomes MCGOUTCLK.
  mcg_c1() = static_cast<std::uint8_t>((0u << kMcgC1ClksShift) | (clock::kFrdiv << kMcgC1FrdivShift));
  if (!wait_until([] { return (mcg_s() & kMcgSClkstMask) == kClkstPll; })) {
    fail(blink::kClockFailure);
  }
  g_core_hz = clock::kCoreHz;
}

// ---- Interrupts -----------------------------------------------------------------------------------------------------
void init_interrupts() noexcept {
  using namespace regs;
  scb_vtor() = mem::kFlashBase;  // the vector table is the one in flash, at 0: handlers are named, not patched in at run time

  // Priority grouping 0: all four implemented bits are preemption priority; ADR 0013 D1 depends on a plain 16-level order.
  scb_aircr() = kAircrVectkey;

  for (unsigned line = 0; line < kIrqCount; ++line) {
    nvic_ipr(line) = hal::irq::priority(role_of(static_cast<Irq>(line)));
  }
  scb_shp(4) = hal::irq::priority(kMemManageRole);
  scb_shp(5) = hal::irq::priority(kBusFaultRole);
  scb_shp(6) = hal::irq::priority(kUsageFaultRole);
  scb_shp(11) = hal::irq::priority(kSvCallRole);
  scb_shp(14) = hal::irq::priority(kPendSvRole);
  scb_shp(15) = hal::irq::priority(kSysTickRole);

  // MemManage, BusFault and UsageFault get their own handlers instead of escalating to HardFault: the crash record
  // then says which. Integer divide by zero traps (UsageFault DIVBYZERO) instead of quietly returning 0.
  scb_shcsr() = scb_shcsr() | kShcsrFaultsEnable;
  scb_ccr() = scb_ccr() | kCcrDiv0Trp;
  asm volatile("dsb\n\tisb" ::: "memory");
}

void start_systick() noexcept {
  using namespace regs;
  syst_rvr() = clock::kSysTickReload;
  syst_cvr() = 0;  // any write clears the counter
  syst_csr() = kSystCsrEnable | kSystCsrTickint | kSystCsrClksource;
}

// ---- SDRAM ----------------------------------------------------------------------------------------------------------
// Power on, pins, controller, then the JEDEC-style sequence: clock and CKE running, precharge all, refresh on, mode
// register set. Register words are sdramc_config.h's. A failed test stops the image with a code: nothing else may be
// built on a memory that does not work.
void init_sdram() noexcept {
  using namespace regs;
  // Power key high (already so since early_pins, the same value again), then the 10 ms wait: the part has its supply.
  // (rail_up_ms() is not set here any more: the rail came up in early_pins, see there.)
  rail_on();
  delay_ms(sdram::kPowerUpDelayMs);

  // Clocks to the FlexBus and the SDRAM controller.
  sim_scgc7() = sim_scgc7() | kScgc7Flexbus | kScgc7Sdramc;

  // SIM_SOPT2: CLKOUT carries the FlexBus clock to the SDRAM; FlexBus security level 3 lets the CPU reach it at all.
  sim_sopt2() = (sim_sopt2() & ~(kSopt2ClkoutselMask | kSopt2FbslMask)) | sdram::kSimSopt2ClkoutFlexBus | sdram::kSimSopt2Fbsl;
  // The shared pins carry byte enables (DQM) instead of chip selects and the like.
  fb_cspmcr() = (fb_cspmcr() & ~sdram::kCspmcrMask) | sdram::kCspmcrValue;

  // The 39 pins to the FlexBus function. CLKOUT and CKE start with this: from here the SDRAM sees a clock.
  for (const Pin pin : sdram::kPins) {
    port_pcr(pin) = sdram::kPinMux << kPcrMuxShift;
  }
  delay_us(sdram::kClockStableDelayUs);

  // Controller: refresh timing, then block 0 (16-bit port, command bit 20, CAS 3, 16 MiB at 0x70000000), valid.
  at<std::uint16_t>(sdram::kCtrlAddress) = static_cast<std::uint16_t>(sdram::kCtrl);
  at<std::uint32_t>(sdram::kAc0Address) = sdram::kAc0;
  at<std::uint32_t>(sdram::kCm0Address) = sdram::kCm0;

  // Precharge all: the next access to the block issues PALL (IP clears itself).
  at<std::uint32_t>(sdram::kAc0Address) = at<std::uint32_t>(sdram::kAc0Address) | sdram::kAcIp;
  at<std::uint8_t>(mem::kSdramBase) = sdram::kCommandAccessValue;

  // Auto refresh on, and wait for more than eight refresh intervals before the mode register is written.
  at<std::uint32_t>(sdram::kAc0Address) = at<std::uint32_t>(sdram::kAc0Address) | sdram::kAcRe;
  delay_us(sdram::kRefreshSettleDelayUs);

  // Mode register set: the next access issues MRS, and the address of that access carries the mode word.
  at<std::uint32_t>(sdram::kAc0Address) = at<std::uint32_t>(sdram::kAc0Address) | sdram::kAcImrs;
  at<std::uint8_t>(sdram::kMrsAddress) = sdram::kCommandAccessValue;
  asm volatile("dsb" ::: "memory");

  // Tests. The LED stays on while they run: its dark phase is "tests finished".
  led_set(true);
  const SdramMem memory{reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(mem::kSdramBase))};
  memtest::Result result = memtest::wiring_test(memory, sdram_words());
#if O3SDK_SDRAM_FULL_TEST
  if (result.ok) {
    result = memtest::fill_test(memory, sdram_words(), 0x5EED5EEDu);
  }
#endif
  if (!result.ok) {
    fail(blink::kSdramFailure);
  }
#if O3SDK_SDRAM_ICODE_ALIAS
  // OS-PERF3: after the tests (they ran through 0x7000_0000 with the base decode), the controller also answers at the ICode alias 0x0800_0000 (sdramc_config.h kCm0WithAlias). Nothing
  // reads the alias until a user of it exists; 0x7000_0000 decodes as before. Interrupts are on: no SDRAM access is in flight (the audio rings, the display staging and the stacks are SRAM).
  at<std::uint32_t>(sdram::kCm0Address) = sdram::kCm0WithAlias;
  asm volatile("dsb" ::: "memory");
#endif
  led_set(false);
}

// ---- eDMA ------------------------------------------------------------------------------------------------------------
// The one place the eDMA's global configuration is written (OS-005a; the display driver of OS-007c relies on it and writes neither
// DMA_CR nor a DCHPRI): the clock gates of the eDMA and the DMAMUX, DMA_CR (EMLM, EDBG, fixed-priority arbitration), and the
// arbitration priority of every channel from the ownership table (resources.h): the audio channels highest, the display's next, a
// permutation of 0..15 so that no two channels ever share one. HALT is set while the priorities are rewritten (the chip's reset
// priorities are the channel numbers, so the rewrite passes through duplicates; with HALT no channel can arbitrate meanwhile and
// nothing is requested yet anyway). Every channel's request, error-interrupt, done and interrupt flags are cleared. Runs once, in
// the reset sequence, before the constructors and `main`.
void dma_init() noexcept {
  using namespace regs;
  sim_scgc7() = sim_scgc7() | kScgc7Dma;
  sim_scgc6() = sim_scgc6() | kScgc6Dmamux;
  dma_cr() = edma::kCrValue | edma::kCrHalt;
  for (const resources::DmaRow& row : resources::kDma) {
    dma_dchpri(row.channel) = row.arbitration;
  }
  dma_cerq() = kDmaClearAll;   // CAER: no channel requests enabled
  dma_ceei() = kDmaClearAll;   // CAEE: no error interrupts
  dma_cdne() = kDmaClearAll;   // CADN: no DONE flags
  dma_cerr() = kDmaClearAll;   // CAEI: no error flags
  dma_cint() = kDmaClearAll;   // CAIR: no interrupt requests
  dma_cr() = edma::kCrValue;
  asm volatile("dsb" ::: "memory");
}

}  // namespace boot

// ---- The SDRAM arena (mem_map.h "SDRAM"): what the linker left behind `.sdram_bss` -----------------------------------------------
SdramArena sdram_arena() noexcept {
  const auto start = reinterpret_cast<std::uintptr_t>(__sdram_arena_start);
  const auto end = reinterpret_cast<std::uintptr_t>(__sdram_arena_end);
  return SdramArena{start, static_cast<std::uint32_t>(end - start)};
}

}  // namespace o3sdk::platform::mk66
