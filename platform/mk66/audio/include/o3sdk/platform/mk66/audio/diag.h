// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag cpu` and `diag mem` as pure text formatters (ADR 0011 item 1, ADR 0013 D6/D8). There is no console yet (the CDC
// `diag` is OS-006b/007); these turn the status and the memory map into the lines it will print, so they are unit-tested now
// and bind to the console later. No printf (it costs kilobytes of flash and may allocate): a bounded text builder with the
// few conversions needed. Nothing here runs on the real-time path.

#include <o3sdk/platform/mk66/audio/status.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::audio::diag {

// Appends to a caller-supplied buffer: never writes past `cap`, always leaves a NUL terminator (cap >= 1), and remembers
// that it ran out. Every method returns *this so a line is one expression.
class Text {
 public:
  Text(char* buf, std::size_t cap) noexcept;
  Text& str(const char* s) noexcept;
  Text& chr(char c) noexcept;
  Text& dec(std::uint32_t v) noexcept;
  Text& hex32(std::uint32_t v) noexcept;                 // 0x%08X
  Text& percent(std::uint64_t part, std::uint64_t whole) noexcept;  // "1.6 %": one decimal, rounded down; "-" if whole is 0
  Text& kib(std::uint32_t bytes) noexcept;               // "11456 KiB", or "1536 B" when not a whole KiB
  Text& nl() noexcept { return chr('\n'); }
  std::size_t size() const noexcept { return len_; }
  bool truncated() const noexcept { return truncated_; }

 private:
  char* buf_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool truncated_ = false;
};

// `diag cpu`. `cycles_per_block` is the block period in the units of the status' cycle figures.
std::size_t format_cpu(const StatusSnapshot& s, std::uint32_t cycles_per_block, char* out, std::size_t cap) noexcept;

// `diag cpu`, the line after format_cpu (OS-MAINGUARD): the main-loop starvation guard's shed blocks since boot or `cpu reset` (the render zeroes them at its next block), how many of
// them were late (their slot was already playing), the shed runs, and the longest run since boot (the main pass's cost in blocks when the guard is working).
// `diag cpu` and the HIL gates read "guard  shed 0 blocks": anything else under a normal load is a finding. The line ends with the window in force, "n <window>" (Pipeline::starve_blocks(), `guard n`).
std::size_t format_shed(std::uint32_t blocks, std::uint32_t late, std::uint32_t runs, std::uint32_t longest, std::uint32_t window, char* out, std::size_t cap) noexcept;

struct MemRow {  // a region with an address (an SDRAM region)
  const char* name;
  std::uint32_t base;
  std::uint32_t size;
};
struct BudgetRow {  // a line of an SRAM budget
  const char* name;
  std::uint32_t bytes;
};
struct MemReport {
  const MemRow* sdram;
  unsigned sdram_count;
  std::uint32_t sdram_total;       // bytes the part has
  const BudgetRow* sram_l;         // the ADR 0013 D7 budget of the code bus SRAM
  unsigned sram_l_count;
  std::uint32_t sram_l_size;
  std::uint32_t sram_l_hot_used;   // bytes of .ramfunc_l the image has
  std::uint32_t sram_l_hot_budget;
  std::uint32_t sram_l_data_used;  // bytes of .tapmem_l the image has (the audio taps' memory)
  std::uint32_t sram_l_data_budget;
  const BudgetRow* sram_u;         // the ADR 0013 D7 budget of the system bus SRAM
  unsigned sram_u_count;
  std::uint32_t sram_u_size;
  std::uint32_t sram_u_used;       // .noinit + .data + .bss the image has
  std::uint32_t sram_u_stack;
  std::uint32_t sram_u_min_free;
};

// `diag mem`.
std::size_t format_mem(const MemReport& m, char* out, std::size_t cap) noexcept;

}  // namespace o3sdk::platform::mk66::audio::diag
