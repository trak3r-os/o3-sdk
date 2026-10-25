// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The K66's local memory controller (LMEM) and its code cache (OS-PERF3). Register facts from the K66 Sub-Family Reference Manual, Rev. 2, chapter 30 "Local Memory
// Controller" (and Table 5-1 for the memory map), cross-checked against NXP's MK66F18 device header, which is a fact table here, not copied.
//
// WHAT THE CHIP HAS. ONE cache, on the Processor Code (PC) bus: 8 KiB, two ways (PCCCR has INVW0 and INVW1), 16-byte lines (the RM's "16-byte-aligned memory address" line fill and
// the tag index CLCR[11:4]: 256 sets), unified for instructions and data. The PC bus carries every access below 0x2000_0000 (RM 30.2.1); of those, SRAM_L is routed to its own
// controller BEFORE the cache (zero wait, never cached), so the cache serves the program flash (and FlexNVM) and the two aliases of the external memories. It is the only cache:
//   - There is NO system-bus cache. The Processor System (PS) bus (0x2000_0000 and up) goes to SRAM_U or "to the CCM bus and the crossbar switch using the Master1 port" (RM 30.4.1.2),
//     nothing in between, and NXP's own feature header says `FSL_FEATURE_LMEM_HAS_SYSTEMBUS_CACHE (0)` for this part. The registers PSCCR / PSCLCR / ... of the bigger K-series parts do not
//     exist here (the LMEM block is the five PC registers below). SDRAM at 0x7000_0000 is a PS-bus address: it is not cached by anything, whatever PCCRMR says for R6..R9 (Table 30-1 lists
//     modes for them because the table is shared with parts that have the system cache).
//   - What the chip offers instead is the ICode alias (Table 5-1): "0x0800_0000-0x0FFF_FFFF SDRAM (Aliased area), mapped to the same space of 0x8800_0000-0x8FFF_FFFF. To alias this space
//     to 0x8800_0000 - 0x8FFF_FFFF, set the appropriate SDRAMC chip select's address mask bit31." (and 0x1800_0000-0x1BFF_FFFF for FlexBus). An access to the alias is a PC-bus access,
//     so region R1 (write-through or non-cacheable, Table 30-1) governs it and the code cache serves it. See sdramc_config.h for how the controller is told to answer there.
//
// THE MODES (Table 30-1): R0 program flash 0x0000_0000-0x07FF_FFFF and R2 FlexNVM are write-through or non-cacheable (a write hit does not write through to flash); R1 the SDRAM alias and R3
// the FlexBus alias likewise; R4 SRAM_L and R5 SRAM_U are non-cacheable only. Reset value of PCCRMR: 0xAA0FA000: R0..R3 write-through, R4 and R5 non-cacheable, R6 and R7 write-back, R8 and
// R9 write-through, the rest non-cacheable. So ENCACHE alone caches flash; nothing else needs setting. Write-through means: a read miss fills a whole 16-byte line, a read hit costs
// nothing on the bus, a write miss goes to the bus only (NO allocate), a write hit updates the line and goes to the bus. A cached copy is therefore never newer than memory, and the only
// incoherence is a bus master other than the core writing memory behind the cache, or the core writing the same bytes through the OTHER alias (0x7000_0000 or 0x8800_0000 are PS-bus and
// bypass the cache): see the single-alias rule in mem_map.h.
//
// COMMANDS. PCCCR: GO (31) starts the command chosen by INVW0 (24), PUSHW0 (25), INVW1 (26), PUSHW1 (27): "invalidate way n" clears every line of the way (write-through lines are never
// modified, so PUSH has nothing to push). GO stays set until the command is done. The line commands (PCCLCR with PCCSAR) invalidate a single line by physical address.

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::lmem {

inline constexpr std::uintptr_t kBase = 0xE0082000u;
inline constexpr std::uintptr_t kPcccr = kBase + 0x00u;   // cache control
inline constexpr std::uintptr_t kPcclcr = kBase + 0x04u;  // cache line control
inline constexpr std::uintptr_t kPccsar = kBase + 0x08u;  // cache search address
inline constexpr std::uintptr_t kPcccvr = kBase + 0x0Cu;  // cache read/write value
inline constexpr std::uintptr_t kPccrmr = kBase + 0x20u;  // cache regions mode

// PCCCR
inline constexpr std::uint32_t kEncache = 1u << 0;       // cache enable
inline constexpr std::uint32_t kEnwrbuf = 1u << 1;       // write buffer enable
inline constexpr std::uint32_t kForceWt = 1u << 2;       // PCCR2: every cacheable space write-through
inline constexpr std::uint32_t kNoAllocate = 1u << 3;    // PCCR3: no allocation on a miss (with kForceWt)
inline constexpr std::uint32_t kInvW0 = 1u << 24;
inline constexpr std::uint32_t kPushW0 = 1u << 25;
inline constexpr std::uint32_t kInvW1 = 1u << 26;
inline constexpr std::uint32_t kPushW1 = 1u << 27;
inline constexpr std::uint32_t kGo = 1u << 31;

inline constexpr std::uint32_t kInvalidateAll = kGo | kInvW0 | kInvW1;
// The value written in one store: invalidate both ways, write buffer on, cache on.
inline constexpr std::uint32_t kEnableAll = kInvalidateAll | kEnwrbuf | kEncache;

// The geometry (see the header's first paragraph).
inline constexpr std::uint32_t kCacheBytes = 8192;
inline constexpr std::uint32_t kLineBytes = 16;
inline constexpr std::uint32_t kWays = 2;
inline constexpr std::uint32_t kSets = kCacheBytes / (kLineBytes * kWays);
static_assert(kSets == 256u, "CLCR[11:4] indexes the tag array: 256 sets");

// PCCLCR / PCCSAR: invalidate one line by its physical address (the line command: LADSEL 1, LCMD 01, then LGO in PCCSAR).
inline constexpr std::uint32_t kLineGo = 1u;                 // PCCLCR[0] / PCCSAR[0], the same bit
inline constexpr std::uint32_t kLineCmdInvalidate = (1u << 24) | (1u << 26);  // LCMD = 01 (invalidate), LADSEL = 1 (physical address)

// PCCRMR: two bits a region, region 0 in bits 31:30 down to region 15 in bits 1:0.
enum class Mode : std::uint8_t { NonCacheable = 0, WriteThrough = 2, WriteBack = 3 };
inline constexpr unsigned kRegionCount = 16;
inline constexpr std::uint32_t kResetRegionModes = 0xAA0FA000u;
constexpr unsigned region_shift(unsigned region) noexcept { return 30u - 2u * region; }
constexpr Mode region_mode(std::uint32_t pccrmr, unsigned region) noexcept {
  const std::uint32_t m = (pccrmr >> region_shift(region)) & 3u;
  return m == 3u ? Mode::WriteBack : (m == 2u ? Mode::WriteThrough : Mode::NonCacheable);
}
constexpr std::uint32_t with_region_mode(std::uint32_t pccrmr, unsigned region, Mode mode) noexcept {
  return (pccrmr & ~(3u << region_shift(region))) | (static_cast<std::uint32_t>(mode) << region_shift(region));
}

// Table 30-1: the region of an address, -1 where the table has none (peripherals, SRAM_U above 1 MiB ...).
constexpr int region_of(std::uint32_t a) noexcept {
  if (a < 0x08000000u) return 0;
  if (a < 0x10000000u) return 1;
  if (a >= 0x10000000u && a < 0x14000000u) return 2;
  if (a >= 0x18000000u && a < 0x1C000000u) return 3;
  if (a >= 0x1C000000u && a < 0x20000000u) return 4;
  if (a >= 0x20000000u && a < 0x20100000u) return 5;
  if (a >= 0x60000000u && a < 0x70000000u) return 6;
  if (a >= 0x70000000u && a < 0x80000000u) return 7;
  if (a >= 0x80000000u && a < 0x90000000u) return 8;
  if (a >= 0x90000000u && a < 0xA0000000u) return 9;
  return -1;
}

// The bus a core access to `a` takes (RM 30.2.1): below 0x2000_0000 the PC bus (cacheable per its region, except SRAM_L), else the PS bus (never cached on this part).
constexpr bool on_code_bus(std::uint32_t a) noexcept { return a < 0x20000000u; }
// True where the code cache can hold a copy of `a` when it is enabled and the region's mode allows: regions R0..R3 (flash, FlexNVM, the two aliases). Not SRAM_L (R4, routed before the
// cache), not SRAM_U or anything on the PS bus.
constexpr bool cacheable_address(std::uint32_t a) noexcept { return region_of(a) >= 0 && region_of(a) <= 3; }
// The first byte of a cache line, the line's base address for an invalidate.
constexpr std::uint32_t line_base(std::uint32_t a) noexcept { return a & ~(kLineBytes - 1u); }
// The set a line falls in: address bits [11:4].
constexpr std::uint32_t set_of(std::uint32_t a) noexcept { return (a >> 4) & (kSets - 1u); }

}  // namespace o3sdk::platform::mk66::lmem
