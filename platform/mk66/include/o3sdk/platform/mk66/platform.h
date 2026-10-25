// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the MK66 platform layer offers the layers above it once the reset sequence (src/startup.cpp) has run: the
// hal::Clock on SysTick, the boot report (why the chip reset, the crash record the last run left) and the stage and
// module slots a fault handler stamps into a crash record. Implemented only in the MK66 image build; the declarations
// compile everywhere so the header check passes on every platform. (The watchdog is not here: the reset sequence unlocks
// and disables it, and its re-enable point is a marked block in main.cpp, see README.md.) What only the image's own sources
// use (the delays, the idle hook, the status LED, the I2C master) is declared in src/internal.h.

#include <o3sdk/core/tick.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/platform/mk66/crash_record.h>

#include <cstdint>

namespace o3sdk::platform::mk66 {

// hal::Clock on the Cortex-M SysTick at 1 kHz from the 180 MHz core clock: a millisecond count in the SysTick handler
// and, from the down-counter inside the current millisecond, microsecond resolution. The same method as the QEMU layer's
// clock (platform/qemu/clock.h), with this core's 180 MHz.
//
// Shared state: the millisecond count has one writer, the SysTick handler, and is read only inside a critical section
// (PRIMASK saved and restored, so a read from an ISR is fine), because a 64-bit access is two words.
class SysTickClock final : public hal::Clock {
 public:
  core::Tick now() const noexcept override;
  std::uint64_t uptime_ms() const noexcept override;
};

// The one system clock; its state lives with the SysTick handler. Valid once the reset sequence has started SysTick.
SysTickClock& system_clock() noexcept;

// Reset sources, as the Reset Control Module reports them for the reset that started this run (RCM_SRS0 in the low byte,
// RCM_SRS1 in the next). Bit meanings: K66 RM "RCM": SRS0 bit 7 POR, 6 PIN, 5 WDOG, 3 LOL, 2 LOC, 1 LVD, 0 WAKEUP;
// SRS1 bit 2 SW (SYSRESETREQ, which a crash record's reset is), 1 LOCKUP, 0 JTAG.
struct BootInfo {
  std::uint16_t reset_sources;
  bool crash_record_found;  // a valid record was in the noinit block at reset: see boot_crash_record()
};
inline constexpr std::uint16_t kResetPower = 0x0080;
inline constexpr std::uint16_t kResetWatchdog = 0x0020;
inline constexpr std::uint16_t kResetSoftware = 0x0400;
inline constexpr std::uint16_t kResetLockup = 0x0200;

const BootInfo& boot_info() noexcept;

// The crash record that was waiting at reset, or nullptr. Points at a copy taken before anything could overwrite it; it
// stays valid for the whole run. The record is still in its noinit block, so another crash before it is cleared counts on
// from it (the count is the number of consecutive crashes: a boot counter for quarantine, ADR 0002).
const crash::CrashRecord* boot_crash_record() noexcept;

// Erases the stored record (and the boot copy's claim on it): call it once the record has been reported. A later crash
// then starts the count at 1 again.
void clear_crash_record() noexcept;

// Where the OS is, for the next crash record. Written ahead of time, read by the fault handlers; a word and its
// complement, so a handler can tell a value from RAM garbage. `module` is the ADR 0002 module being run, 0 for none.
void set_stage(crash::Stage stage) noexcept;
void set_module(std::uint16_t module) noexcept;

// A software panic: records a crash with exception = crash::kPanicException and the caller's context, then resets, as
// a fault would. For "this can never happen" paths (a pure virtual call, an exhausted fixed pool): never returns.
[[noreturn]] void panic() noexcept;

// The SDRAM nothing else took (mem_map.h, "SDRAM"): from the end of `.sdram_bss` (every O3SDK_BIG object), aligned to
// mem::kSdramArenaAlign, to the end of the SDRAM, at its system-bus address (0x7000_0000 + n). Nothing hands it out or clears it:
// it holds the boot's memory test pattern until the app writes it. The app owns it (samples, M4). Valid once the reset sequence has run.
struct SdramArena {
  std::uintptr_t base;
  std::uint32_t bytes;
};
SdramArena sdram_arena() noexcept;

}  // namespace o3sdk::platform::mk66
