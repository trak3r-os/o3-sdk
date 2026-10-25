// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 memory map (mem_map.h), pinned by value, and the properties that make it a map. Runs on the host. The static_asserts in
// the header already stop a build that breaks the sums; these tests pin the numbers so that a deliberate change to the map is a
// deliberate change to a test too. (The SDRAM's split between `.sdram_bss` and the arena is the linker's: tools/gen_ld.py and the
// image's link check it.)
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/mem_map.h>

#include <cstdint>

namespace mem = o3sdk::platform::mk66::mem;

TEST_CASE("mk66 mem_map: the chip's regions") {
  CHECK(mem::kFlashBase == 0x00000000u);
  CHECK(mem::kFlashSize == 1048576u);
  CHECK(mem::kSramLBase == 0x1FFF0000u);
  CHECK(mem::kSramLSize == 65536u);
  CHECK(mem::kSramUBase == 0x20000000u);
  CHECK(mem::kSramUSize == 196608u);
  CHECK(mem::kSdramBase == 0x70000000u);
  CHECK(mem::kSdramSize == 16777216u);
  // SRAM_L and SRAM_U are one contiguous block whose top is _estack.
  CHECK(mem::kSramLBase + mem::kSramLSize == mem::kSramUBase);
  CHECK(mem::kStackTop == 0x20030000u);
}

TEST_CASE("mk66 mem_map: SDRAM is .sdram_bss then the app's arena, and every byte has an ICode alias") {
  CHECK(mem::kSdramArenaAlign == 32u);
  CHECK(mem::kSdramBase % mem::kSdramArenaAlign == 0u);
  CHECK(mem::kSdramAliasBase == 0x08000000u);
  CHECK(mem::sdram_alias(mem::kSdramBase) == mem::kSdramAliasBase);
  CHECK(mem::sdram_alias(mem::kSdramBase + mem::kSdramSize - 4u) == 0x08FFFFFCu);
}

TEST_CASE("mk66 mem_map: SRAM_L is the hot code's budget and free space; SRAM_U has room") {
  CHECK(mem::kRamfuncLBudget == 32u * 1024u);
  CHECK(mem::kSramLFreeBase == 0x1FFF8000u);
  CHECK(mem::kRamfuncLBudget <= mem::kSramLSize);
  CHECK(mem::kRamfuncLSize == 0xC800u);  // the update path's validator window: track3r-os's 50 KiB
  CHECK(mem::kRamfuncLBudget <= mem::kRamfuncLSize);
  // ADR 0013 D7: SRAM_U stacks 24 KiB; the crash block, the fault stack and the main stack leave well over half of SRAM_U.
  CHECK(mem::kStackSize == 24u * 1024u);
  CHECK(mem::kStackSize + mem::kFaultStackSize + mem::kCrashBlockSize < mem::kSramUSize / 2u);
  CHECK(mem::kStackSize % 8u == 0u);
  CHECK(mem::kFaultStackSize % 8u == 0u);
  CHECK(mem::kCrashBlockSize % 8u == 0u);
  CHECK(mem::kDmaRingAlign == 1024u);
}

TEST_CASE("mk66 mem_map: flash layout") {
  CHECK(mem::kFlashConfigOffset == 0x400u);
  CHECK(mem::kFlashConfigSize == 16u);
  CHECK(mem::kVectorTableBytes == (16u + 100u) * 4u);
  CHECK(mem::kVectorTableBytes <= mem::kFlashConfigOffset);
}

TEST_CASE("mk66 mem_map: in_sram() accepts what is in SRAM and nothing else") {
  CHECK(mem::in_sram(0x1FFF0000u, 4));
  CHECK(mem::in_sram(0x2002FFFCu, 4));
  CHECK(mem::in_sram(0x20000000u, 32));
  CHECK(mem::in_sram(0x2002FFE0u, 32));
  CHECK_FALSE(mem::in_sram(0x2002FFE4u, 32));   // runs off the end
  CHECK_FALSE(mem::in_sram(0x1FFEFFFCu, 4));    // just below
  CHECK_FALSE(mem::in_sram(0x20030000u, 4));    // just past
  CHECK_FALSE(mem::in_sram(0x00000000u, 4));    // flash
  CHECK_FALSE(mem::in_sram(0x70000000u, 4));    // SDRAM
  CHECK_FALSE(mem::in_sram(0xFFFFFFFCu, 8));    // wraps around 32 bits: must not pass
  CHECK_FALSE(mem::in_sram(0xFFFFFFFFu, 0xFFFFFFFFu));
}

TEST_CASE("mk66 mem_map: the SRAM_U budget holds (ADR 0013 D7)") {
  std::uint32_t u_sum = 0;
  for (unsigned i = 0; i < mem::kSramUBudgetCount; ++i) {
    INFO("row " << mem::kSramUBudget[i].name);
    CHECK(mem::kSramUBudget[i].bytes != 0u);
    u_sum += mem::kSramUBudget[i].bytes;
  }
  CHECK(u_sum == mem::budget_sum(mem::kSramUBudget));
  CHECK(u_sum + mem::kSramUMinFree <= mem::kSramUSize);       // at least 32 KiB free (H14 out)
  CHECK(mem::kSramUSize - u_sum >= 32u * 1024u);
  CHECK(mem::kSramUMinFree == 32u * 1024u);
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "audio_io_rings") == 2048u);  // the ADR's 2 x 768 B as powers of two: room for the 3-slot fallback
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "stack") == mem::kStackSize);
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "nope") == 0u);
}
