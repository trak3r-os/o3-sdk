// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `mem` for the o3-sdk image: the memory map of mem_map.h and the figures only the linked image knows (the bytes of hot code, of SRAM_U in use and of `.sdram_bss`, from the linker
// symbols) turned into the report the audio library's formatter prints (audio/diag.h). Nothing here reads hardware: the console's `mem` handler passes the linker symbols' differences.
// track3r-os fills the same report from its eleven fixed SDRAM regions (diag_report.h); here the SDRAM is `.sdram_bss` (every O3SDK_BIG object, the USB log ring among them) and the
// app's arena after it, and SRAM_L holds the hot code and nothing else (no tap memory).

#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/mem_map.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

struct MemFacts {
  std::uint32_t hot_code_bytes;     // __ramfunc_l_end - __ramfunc_l_start
  std::uint32_t sram_u_image_bytes; // __bss_end - __noinit_start: .noinit + the fault stack + .dma_rings + .data + .bss
  std::uint32_t sdram_bss_bytes;    // __sdram_bss_end - __sdram_bss_start
};

// The rows live here; `report` points into this object, so it must not be copied.
struct MemTables {
  audio::diag::MemRow sdram[2];
  audio::diag::BudgetRow sram_l[2];
  audio::diag::BudgetRow sram_u[mem::kSramUBudgetCount];
  audio::diag::MemReport report;
};

inline void fill_mem_tables(MemTables& t, const MemFacts& facts) noexcept {
  const std::uint32_t bss = facts.sdram_bss_bytes < mem::kSdramSize ? facts.sdram_bss_bytes : mem::kSdramSize;
  t.sdram[0] = audio::diag::MemRow{".sdram_bss (O3SDK_BIG objects)", mem::kSdramBase, bss};
  t.sdram[1] = audio::diag::MemRow{"arena (the app's, free)", mem::kSdramBase + bss, mem::kSdramSize - bss};
  t.sram_l[0] = audio::diag::BudgetRow{".ramfunc_l (hot code)", mem::kRamfuncLBudget};
  t.sram_l[1] = audio::diag::BudgetRow{"free", mem::kSramLSize - mem::kRamfuncLBudget};
  for (unsigned i = 0; i < mem::kSramUBudgetCount; ++i) {
    t.sram_u[i] = audio::diag::BudgetRow{mem::kSramUBudget[i].name, mem::kSramUBudget[i].bytes};
  }
  t.report.sdram = t.sdram;
  t.report.sdram_count = 2;
  t.report.sdram_total = mem::kSdramSize;
  t.report.sram_l = t.sram_l;
  t.report.sram_l_count = 2;
  t.report.sram_l_size = mem::kSramLSize;
  t.report.sram_l_hot_used = facts.hot_code_bytes;
  t.report.sram_l_hot_budget = mem::kRamfuncLBudget;
  t.report.sram_l_data_used = 0;
  t.report.sram_l_data_budget = 0;
  t.report.sram_u = t.sram_u;
  t.report.sram_u_count = mem::kSramUBudgetCount;
  t.report.sram_u_size = mem::kSramUSize;
  t.report.sram_u_used = facts.sram_u_image_bytes;
  t.report.sram_u_stack = mem::kStackSize;
  t.report.sram_u_min_free = mem::kSramUMinFree;
}

}  // namespace o3sdk::platform::mk66::console
