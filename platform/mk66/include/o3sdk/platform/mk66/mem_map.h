// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The memory map of the MK66FX1M0 Tracker OG: the single source of every region address and size (ADR 0013 section D7,
// ADR 0011 lesson 1). `mk66.ld` is generated from the constants below by tools/gen_ld.py, so the linker script cannot
// drift from this header; a ctest regenerates it and checks the result.
//
// What keeps the generator trivial: it reads every `inline constexpr std::uint32_t kName = <expression>;` statement in this
// file, the expression made of integer literals, earlier constants and + - * / % << >> & | ^ ( ) only, and fails on one it
// cannot evaluate (or that would wrap in 32 bits). Nothing the linker needs may be computed any other way. The rest of the
// file (the budget table, the functions, the static_asserts) is for the compiler and the host tests.
//
// Chip facts: K66 Sub-Family Reference Manual, "Memory map" (program flash 0x0000_0000, SRAM_L 0x1FFF_0000 on the code
// bus, SRAM_U 0x2000_0000 on the system bus, SDRAM through the FlexBus SDRAM controller at 0x7000_0000). The stack top is 0x2003_0000, the end of SRAM_U.

#include <cstdint>

namespace o3sdk::platform::mk66::mem {

inline constexpr std::uint32_t kKiB = 1024u;
inline constexpr std::uint32_t kMiB = 1024u * 1024u;

// ---- Chip ----------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kFlashBase = 0x00000000u;
inline constexpr std::uint32_t kFlashSize = 1u * kMiB;
// Flash configuration field (FSEC, FOPT, ...): sixteen bytes the chip reads at reset to decide its own protection.
// A wrong value can permanently lock the MCU. See flash_config.h; the linker places it here and asserts so.
inline constexpr std::uint32_t kFlashConfigOffset = 0x400u;
inline constexpr std::uint32_t kFlashConfigSize = 16u;
// Cortex-M4 exception table (16 system vectors) plus the MK66's 100 device interrupts, one word each. It must end
// below the flash configuration field.
inline constexpr std::uint32_t kVectorTableBytes = (16u + 100u) * 4u;

inline constexpr std::uint32_t kSramLBase = 0x1FFF0000u;
inline constexpr std::uint32_t kSramLSize = 64u * kKiB;
inline constexpr std::uint32_t kSramUBase = 0x20000000u;
inline constexpr std::uint32_t kSramUSize = 192u * kKiB;

inline constexpr std::uint32_t kSdramBase = 0x70000000u;
inline constexpr std::uint32_t kSdramSize = 16u * kMiB;
// OS-PERF3: the SDRAM's second address, the ICode alias (K66 RM Table 5-1: 0x0800_0000-0x0FFF_FFFF "SDRAM (Aliased area)"). A core access there takes the code bus, so the LMEM code
// cache can hold it (lmem.h), where an access to 0x7000_0000 takes the system bus and nothing caches it. The same bytes answer at both (the controller ignores address bits 30:27 once
// sdramc_config.h kCm0WithAlias is written at boot). THE SINGLE-ALIAS RULE: a byte that is cached must be read AND written through the alias, by every core access, for as long as the
// cache holds it; the cache is write-through and does not allocate on a write miss, so with one alias in use it is always coherent, and nothing but the core touches SDRAM (the SDHC
// is polled, the display's eDMA reads SRAM staging, USB and I2S are in SRAM_U). A write through 0x7000_0000 behind a cached line leaves the line stale (the perf notes).
// In the kit nothing uses the alias yet (every O3SDK_BIG object is reached at 0x7000_0000); the boot still widens the controller's decode as the proven
// tree does (O3SDK_SDRAM_ICODE_ALIAS), and the first user (the sample arena, M4) takes the rule above with it.
inline constexpr std::uint32_t kSdramAliasBase = 0x08000000u;
constexpr std::uint32_t sdram_alias(std::uint32_t sdram_address) noexcept { return sdram_address - kSdramBase + kSdramAliasBase; }

// ---- SRAM_L: hot code ---------------------------------------------------------------------------------------------------
// `.ramfunc_l` at the base of SRAM_L: code copied there from flash by the reset sequence, run from the code bus with no wait state and
// no cache in the way. In M2 it holds the audio ISRs and the pipeline (about 4 KiB); the kit's engine and the units run from flash
// with the LMEM code cache on, and moving the engine's hot loop here is a later, measured change. The linker script asserts the
// section stays within kRamfuncLBudget, and the image check lets a vector point into that much of SRAM_L and no further. The rest
// of SRAM_L, from kSramLFreeBase to the end, is free: nothing is placed there yet (later: hot data). track3r-os's tap memory and
// module slab are gone with the tracker.
inline constexpr std::uint32_t kRamfuncLBudget = 32u * kKiB;
inline constexpr std::uint32_t kSramLFreeBase = kSramLBase + kRamfuncLBudget;
// The free half's first object (M2c slice U, docs/M2C-PLAN.md section 7): `.tapmem_l`, NOLOAD, at kSramLFreeBase: the USB stems' pool and ring (audio/taps.h StemPool, O3SDK_TAP_MEM_L in
// sections.h), which the render writes a block at a time and the USB interrupt reads a packet at a time. About 15.8 KB today (11 channels x 5 blocks x 128 frames, the staging blocks and the
// ring); the budget is what the linker script asserts. Not cleared by the reset sequence: the StemTap's constructor zeroes it before main.
inline constexpr std::uint32_t kSramLDataBudget = 20u * kKiB;
// The SRAM_L window the update path's validator accepts for a vector (update/hex_validator.h pins its copy to this): track3r-os's, 50 KiB. It is not
// a section of this image; o3sdk's kRamfuncLBudget fits inside, so the validator accepts o3sdk's images and track3r-os's (the way back).
inline constexpr std::uint32_t kRamfuncLSize = 0xC800u;

// ---- SRAM_U: data, bss, noinit, stack (ADR 0013 D7: about 160 of 192 KiB, at least 32 free) ------------------------
// Low to high: `.noinit` (the crash record, the stage word), the fault stack, the audio DMA rings, `.data`, `.bss`, free, the main
// stack growing down from the top. One stack serves thread mode and every ISR (MSP only, no RTOS), 24 KiB as in D7.
inline constexpr std::uint32_t kStackSize = 24u * kKiB;
inline constexpr std::uint32_t kStackTop = kSramUBase + kSramUSize;
// A separate small stack the fault handlers switch to, so a handler survives the very overflow it is reporting.
inline constexpr std::uint32_t kFaultStackSize = 512u;
// The crash record (crash_record.h: 64 bytes) and the stage word pair that survive a reset, in `.noinit`.
inline constexpr std::uint32_t kCrashBlockSize = 128u;
// The audio DMA rings sit in their own SRAM_U section, aligned to the largest of them (the RX ring, 1 KiB: hal::audio::kRxRingBytes):
// the eDMA's address modulo needs a ring on a multiple of its own size.
inline constexpr std::uint32_t kDmaRingAlign = 1024u;
// The SRAM_U budget the linker script enforces on the real image: at least 32 KiB between the end of `.bss` and the stack stay free.
inline constexpr std::uint32_t kSramUMinFree = 32u * kKiB;

// ---- SDRAM: `.sdram_bss`, then the app's arena -----------------------------------------------------------------------
// Low to high: `.sdram_bss`, every O3SDK_BIG object (kit/core/include/o3sdk/core/placement.h), NOLOAD, zeroed by the reset sequence
// after the SDRAM is up and tested and before the static constructors; then the free arena up to the end of the SDRAM, which nothing
// places or clears (it holds the memory test's pattern at boot) and the app owns (samples, M4). Both sizes are known at link time
// only: the linker script exports __sdram_bss_start / __sdram_bss_end and __sdram_arena_start / __sdram_arena_end (platform.h
// sdram_arena() reads the arena), and asserts that `.sdram_bss` fits. track3r-os's eleven fixed regions were the tracker's.
// The arena starts on kSdramArenaAlign (the SDRAM controller's bursts and the 16-byte cache lines of the alias).
inline constexpr std::uint32_t kSdramArenaAlign = 32u;

// ---- For the compiler and the tests ----------------------------------------------------------------------------------
// A word-sized access at [address, address + length) lies wholly inside internal SRAM (SRAM_L then SRAM_U are
// contiguous). The fault handler uses it before it trusts an exception frame pointer.
constexpr bool in_sram(std::uint32_t address, std::uint32_t length) noexcept {
  const std::uint64_t first = address;
  const std::uint64_t last = first + length;  // one past the end
  return first >= kSramLBase && last <= static_cast<std::uint64_t>(kSramUBase) + kSramUSize;
}

// ---- SRAM_U budget, row by row ------------------------------------------------------------------------------------------
// Estimates, not measurements: what the linker enforces is kSramUMinFree. The rows say where the SRAM_U is meant to go, so a
// slice can static_assert its statics against its own row (the audio rings do).
struct Budget {
  const char* name;
  std::uint32_t bytes;
};
inline constexpr Budget kSramUBudget[] = {
    {"audio_io_rings", 2048u},            // TX ring 1 KiB (3 slots, 4 physical) + RX ring 1 KiB: powers of two for the eDMA modulo
    {"display_staging", 13u * kKiB},      // the FT812 driver's two staging buffers (frame 4 + 8192 B, upload chunk 4 + 4096 B)
    {"kit_app", 112u * kKiB},             // the engine, the runtime, the app's working state, the drivers (O3SDK_BIG objects go to the SDRAM)
    {"stack", kStackSize},                // thread mode and every ISR (one stack, no RTOS)
    {"crash_block_fault_stack", kCrashBlockSize + kFaultStackSize},
};
inline constexpr unsigned kSramUBudgetCount = sizeof(kSramUBudget) / sizeof(kSramUBudget[0]);

template <unsigned N>
constexpr std::uint32_t budget_sum(const Budget (&rows)[N]) noexcept {
  std::uint32_t total = 0;
  for (unsigned i = 0; i < N; ++i) {
    total += rows[i].bytes;
  }
  return total;
}
constexpr std::uint32_t budget_bytes(const Budget* rows, unsigned count, const char* name) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const char* a = rows[i].name;
    const char* b = name;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') {
      return rows[i].bytes;
    }
  }
  return 0u;
}

static_assert(kFlashBase == 0u, "the K66 fetches its initial SP and PC from address 0 (and mk66.ld.in puts the table there)");
static_assert(kSramLBase + kSramLSize == kSramUBase, "SRAM_L and SRAM_U are one contiguous 256 KiB block");
static_assert(kStackTop == 0x20030000u, "_estack: initial SP at the end of SRAM_U");
static_assert(kSramLSize + kSramUSize == 256u * kKiB, "MK66FX1M0 has 256 KiB of SRAM");
static_assert(kFlashSize == 1024u * kKiB, "MK66FX1M0 has 1 MiB of program flash");
static_assert(kVectorTableBytes <= kFlashConfigOffset, "the vector table must end below the flash configuration field");
static_assert(kFlashConfigOffset + kFlashConfigSize <= kFlashSize, "flash configuration field inside flash");
static_assert(kFlashConfigOffset == 0x400u && kFlashConfigSize == 16u, "the field is 0x400..0x40F (K66 RM: flash configuration field)");
static_assert(kStackSize + kFaultStackSize + kCrashBlockSize < kSramUSize / 2u, "stacks and the crash block are a minor part of SRAM_U");
static_assert(kCrashBlockSize % 8u == 0u && kFaultStackSize % 8u == 0u && kStackSize % 8u == 0u, "AAPCS: 8-byte aligned stacks");

static_assert(kRamfuncLBudget <= kSramLSize && kRamfuncLBudget % 4u == 0u, "the hot-code budget fits SRAM_L, in whole words");
static_assert(kRamfuncLBudget <= kRamfuncLSize, "the hot-code budget fits the window the update path's validator accepts");
static_assert(kSramLFreeBase + (kSramLSize - kRamfuncLBudget) == kSramLBase + kSramLSize, "the free part of SRAM_L runs to its end");
static_assert(kSramLDataBudget <= kSramLSize - kRamfuncLBudget && kSramLDataBudget % 8u == 0u, "the tap data fits the free part of SRAM_L");
static_assert(kSdramArenaAlign % 16u == 0u && kSdramBase % kSdramArenaAlign == 0u, "the arena is aligned to whole cache lines");
static_assert(sdram_alias(kSdramBase + kSdramSize - 1u) < 0x10000000u, "the whole SDRAM fits the ICode alias window 0x0800_0000-0x0FFF_FFFF");
static_assert(budget_sum(kSramUBudget) + kSramUMinFree <= kSramUSize, "ADR 0013 D7: the SRAM_U rows leave at least 32 KiB free");
static_assert(budget_bytes(kSramUBudget, kSramUBudgetCount, "stack") == kStackSize, "the stack row is the stack");

}  // namespace o3sdk::platform::mk66::mem
