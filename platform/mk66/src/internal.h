// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Declarations shared between the translation units of the MK66 image (src/, and the hardware halves of display/, input/ and audio/).
// Private to the image: no pure library includes it.

#include <o3sdk/platform/mk66/crash_record.h>
#include <o3sdk/platform/mk66/i2c_bus.h>
#include <o3sdk/platform/mk66/i2c_config.h>
#include <o3sdk/platform/mk66/idle_hook.h>
#include <o3sdk/platform/mk66/sections.h>

#include <cstdint>

namespace o3sdk::dsp {
class Engine;
}

namespace o3sdk::platform::mk66::boot {

// ---- boot.cpp: the reset sequence's steps, in the order startup.cpp calls them -------------------------------------
void early_pins() noexcept;        // port clocks on; audio output muted; status LED an output, off
void enable_dwt() noexcept;        // cycle counter running (hal::cycles(), the delays below)
void init_clocks() noexcept;       // crystal -> HSRUN -> PLL: 180 MHz core, 90 MHz bus and FlexBus. Fails visibly, never hangs.
void init_interrupts() noexcept;   // NVIC priorities from hal/irq.h, fault handlers enabled, divide-by-zero traps
void start_systick() noexcept;     // 1 kHz tick; the clock reads valid from here
void init_sdram() noexcept;        // power, pins, controller, JEDEC sequence, then the wiring and capacity tests
void dma_init() noexcept;          // eDMA clock gates, DMA_CR and every DCHPRI from resources.h: the ONE owner of the eDMA's global configuration
void capture_reset_cause() noexcept;

// Busy-wait delays on the cycle counter, bounded whatever the counter does. Accurate once init_clocks() has finished;
// before that they are sized for the clock the core is running at (see clock_config.h kFeiHz), good to tens of percent.
void delay_us(std::uint32_t us) noexcept;
void delay_ms(std::uint32_t ms) noexcept;

// OS-003f: a function delay_ms() calls once every millisecond it waits (never from inside itself, and never from an interrupt: the
// main thread only), so that the display keeps being serviced while a bring-up blocks for most of a second (the codec's waits are
// delay_ms()): the boot screen's swap and the backlight ramp would otherwise wait for the main loop. nullptr (the default, and what
// every wait before the display exists sees) is no hook. The object is idle_hook.h.
void set_idle_hook(IdleHook::Function hook) noexcept;

// ---- The status LED (board.h kStatusLed, active low) and the blink codes (blink.h) ------------------------------------------
// led_set: from flash, for the reset sequence. led_from_sram_l: the same pin from a function in SRAM_L (`.ramfunc_l`), for main's
// patterns: a pulse from it is also the proof that the reset sequence copied the hot code and that it runs (README.md). Declared with
// O3SDK_RAMFUNC_L like its definition (boot.cpp): `long_call` is a property of the call site's view of the callee, and without it
// the linker puts a flash veneer in front of every call from flash to SRAM_L.
void led_set(bool on) noexcept;
O3SDK_RAMFUNC_L void led_from_sram_l(bool on) noexcept;
// Plays blink code `pulses` (blink.h) `periods` times, blocking on delay_ms() (so the idle hook runs), then leaves the LED off.
// Main thread, boot-time reports only.
void play_code(std::uint8_t pulses, std::uint32_t periods) noexcept;
// Plays blink code `pulses` for ever. For a failure at boot: it is all the image can say.
[[noreturn]] void fail(std::uint8_t pulses) noexcept;

// ---- crash_store.cpp ------------------------------------------------------------------------------------------------
void capture_boot_record() noexcept;                       // copies a valid noinit record aside, at boot
crash::CrashRecord load_stored_record() noexcept;          // raw copy of the noinit block's record (may be garbage)
void store_record(const crash::CrashRecord& r) noexcept;
std::uint32_t load_context() noexcept;                     // crash::kUnknownContext if the stage words are not trustworthy
void reset_context() noexcept;                             // stage Reset, no module: the first thing a run does, so a crash that early does not claim the last run's stage
[[noreturn]] void reset_chip() noexcept;                   // SYSRESETREQ
std::uint32_t uptime_ms_low() noexcept;                    // clock.cpp: SysTick milliseconds since boot, no lock (fault path)

}  // namespace o3sdk::platform::mk66::boot

// ---- cache.cpp: the LMEM code cache (OS-PERF3; lmem.h has the facts) -----------------------------------------------------------------------
namespace o3sdk::platform::mk66::cache {
bool enabled() noexcept;               // PCCCR ENCACHE
std::uint32_t control() noexcept;      // PCCCR as it reads
std::uint32_t region_modes() noexcept; // PCCRMR
bool enable() noexcept;                // invalidate both ways and turn the cache and its write buffer on; false: the command did not finish
bool disable() noexcept;               // cache and write buffer off (flash is read-only, so nothing is lost)
bool invalidate_all() noexcept;        // both ways, when the cache is on (a no-op otherwise); false: the command did not finish
bool invalidate_range(std::uint32_t address, std::uint32_t bytes) noexcept;  // the lines of [address, address + bytes), when the cache is on and the range is cacheable
}  // namespace o3sdk::platform::mk66::cache

// ---- i2c.cpp: the I2C master of the board's two buses (OS-005a for the codec on I2C0, shared with the input drivers, OS-007d) -------
namespace o3sdk::platform::mk66::i2c {

using Result = I2cResult;

// Clock gate, pins (open drain), divider, enable of one instance (I2C0: the control bus, PTD8/PTD9; I2C2: the grid bus, PTA12/PTA13).
// Releases a bus a slave is holding (a reset in the middle of a transaction): up to nine SCL pulses, then a STOP. Main thread, after
// the clock tree is up; calling it again is harmless (the codec's `init()` and the input bring-up each call it for I2C0).
Result init(i2c_cfg::Instance which) noexcept;
Result init() noexcept;  // I2C0, the codec's bus
// The instance's master as the seam the input drivers use (i2c_bus.h). One object per instance, shared by every client.
I2cBus& bus(i2c_cfg::Instance which) noexcept;
// 16-bit register write / read of a 16-bit-register device on I2C0 (the SGTL5000: register address and value big endian). Polled with
// every wait bounded in time (5 ms a byte); a failure leaves the bus released. Main thread only.
Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept;
Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept;

}  // namespace o3sdk::platform::mk66::i2c

// ---- the image's composition root: what the firmware update path (update/src/fw_host_mk66.cpp) and the console ask of it ----------------------
// Declarations identical to track3r-os's (src/internal.h); the definitions are the console slice's (console/src/appimg.cpp, docs/M2B-PLAN.md
// section 3). The rest of track3r-os's appimg:: (the tracker's app, project loads, power, MIDI, disk) has no counterpart here.
namespace o3sdk::platform::mk66::appimg {
// OS-HW1: around a long blocking call made from the main loop (`diag audio start`: the codec's 0.5 s ramp) the display is serviced from the idle hook,
// as it is during the boot's waits. `on` false puts back what was there: no hook, or the hook of the boot screen's hold while that lasts.
void service_while_blocked(bool on) noexcept;
// OS-OUTON: whether the outputs are open (audio_opened() set it), and the other way: audio_closed() says the outputs were muted behind the app's back (`fw update`'s quiesce
// before the hand-over), so that a Play waits for the gate again and main.cpp opens them again once the update path let go. The tone image: false, no-op.
void audio_closed() noexcept;
// OS-006c: stops the sequencer's transport (a Stop on its ring, the badge says so), for the hand-over to the bootloader (`fw update`).
void stop_transport() noexcept;
// OS-007o: the card is in use by the app: a project is loading or a save job is running. The firmware update path (`fw put|rm|arm|update`) refuses while it is true,
// and the save layer refuses to start while the update path holds the card (SaveService::attach_claim): FatFs on the volume is not re-entrant and a hand-over to the
// bootloader must not cut a save. Main loop only.
bool storage_in_use() noexcept;

// ---- console slice (docs/M2B-PLAN.md section 5): the image's own, defined with the others in console/src/appimg.cpp ----------------------------------------------------------
// The composition root gives the console the engine whose transport stop_transport() stops (console::attach does, from the Runtime's context).
void bind_engine(dsp::Engine& engine) noexcept;
// OS-OUTON, as in track3r-os: whether the outputs are open (audio_opened() sets it, audio_closed() clears it), and whether the firmware update path holds the card or is
// handing over (main never raises the amplifier then; it is also the card's claim). Main loop only.
bool audio_outputs_open() noexcept;
void audio_opened() noexcept;
bool firmware_update_active() noexcept;
}  // namespace o3sdk::platform::mk66::appimg
