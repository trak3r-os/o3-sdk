// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/diag.h>

#include <o3sdk/core/fast_math.h>

namespace o3sdk::platform::mk66::audio::diag {

Text::Text(char* buf, std::size_t cap) noexcept : buf_(buf), cap_(cap) {
  if (cap_ != 0) {
    buf_[0] = '\0';
  }
}

Text& Text::chr(char c) noexcept {
  if (cap_ == 0) {
    return *this;
  }
  if (len_ + 1 < cap_) {
    buf_[len_++] = c;
    buf_[len_] = '\0';
  } else {
    truncated_ = true;
  }
  return *this;
}

Text& Text::str(const char* s) noexcept {
  for (; *s != '\0'; ++s) {
    chr(*s);
  }
  return *this;
}

Text& Text::dec(std::uint32_t v) noexcept {
  char digits[10];
  unsigned n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (n != 0u) {
    chr(digits[--n]);
  }
  return *this;
}

Text& Text::hex32(std::uint32_t v) noexcept {
  str("0x");
  for (int shift = 28; shift >= 0; shift -= 4) {
    chr("0123456789ABCDEF"[(v >> shift) & 0xFu]);
  }
  return *this;
}

Text& Text::percent(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0u) {
    return str("-");
  }
  // One decimal of a percent needs only the leading bits of the ratio. Both are scaled down (shifts: no division) until each is below 2^22,
  // so that part * 1000 fits 32 bits and the division below is the Cortex-M4's UDIV, not the 64-bit helper (about 800 bytes of flash that
  // nothing else in the image links). Below 2^22, which is every cycle count of a block, the result is exact and unchanged.
  while (part > 0x3FFFFFu || whole > 0x3FFFFFu) {
    part >>= 1;
    whole >>= 1;
  }
  const std::uint32_t w = whole != 0u ? static_cast<std::uint32_t>(whole) : 1u;
  const std::uint32_t tenths = static_cast<std::uint32_t>(part) * 1000u / w;  // permille: one decimal of a percent
  dec(tenths / 10u);
  chr('.');
  dec(tenths % 10u);
  return str(" %");
}

Text& Text::kib(std::uint32_t bytes) noexcept {
  if (bytes % 1024u == 0u) {
    return dec(bytes / 1024u).str(" KiB");
  }
  return dec(bytes).str(" B");
}

namespace {

// "last/max" of one stage.
void stage(Text& t, const char* name, std::uint32_t last, std::uint32_t max) noexcept {
  t.str("  ").str(name).chr(' ').dec(last).chr('/').dec(max);
}

}  // namespace

std::size_t format_cpu(const StatusSnapshot& s, std::uint32_t cycles_per_block, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("audio  blocks ").dec(s.boundaries).str("  renders ").dec(s.renders);
  t.str("  underrun ").dec(s.underruns).str(" (run ").dec(s.consecutive_underruns).str(", max ").dec(s.max_consecutive_underruns).chr(')');
  t.str("  mute ").dec(s.muted).str(" (events ").dec(s.mute_events).chr(')');
  t.str("  late ").dec(s.late_drops).str("  slips ").dec(s.slot_slips).str("  hwfault ").dec(s.hw_faults).nl();

  t.str("render last ").dec(s.render_last).str("  max ").dec(s.render_max).str(" cyc = ").percent(s.render_max, cycles_per_block);
  t.str(" of the block (").dec(cycles_per_block).str(" cyc)  over-budget ").dec(s.governor_over).nl();

  t.str("stage ");
  static constexpr const char* kNames[kStageCount] = {"input", "drain", "render", "strips", "buses", "master", "convert", "taps", "commit"};
  static_assert(kStageCount == 9, "one name per stage (hal/audio.h kStage*)");
  for (unsigned i = 0; i < kStageCount; ++i) {
    stage(t, kNames[i], s.stage_last[i], s.stage_max[i]);
  }
  t.str("  (last/max cyc)").nl();

  t.str("jitter start-latency ").dec(s.latency_last).chr('/').dec(s.latency_max).str(" cyc  boundary period ");
  t.dec(s.period_min).str("..").dec(s.period_max).str(" cyc  input-peak ").dec(s.input_peak).nl();
  return t.size();
}

std::size_t format_shed(std::uint32_t blocks, std::uint32_t late, std::uint32_t runs, std::uint32_t longest, std::uint32_t window, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("guard  shed ").dec(blocks).str(" blocks in ").dec(runs).str(" runs  late ").dec(late).str("  longest run since boot ").dec(longest).str(" blocks  n ").dec(window).nl();
  return t.size();
}

std::size_t format_mem(const MemReport& m, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  std::uint64_t sdram_sum = 0;
  t.str("sdram ").kib(m.sdram_total).nl();
  for (unsigned i = 0; i < m.sdram_count; ++i) {
    t.str("  ").str(m.sdram[i].name).chr(' ').hex32(m.sdram[i].base).chr(' ').kib(m.sdram[i].size).nl();
    sdram_sum += m.sdram[i].size;
  }
  t.str("  regions sum ").dec(static_cast<std::uint32_t>(sdram_sum / 1024u)).str(" KiB").str(sdram_sum == m.sdram_total ? " (complete)" : " (MISMATCH)").nl();

  t.str("sram_l ").kib(m.sram_l_size).str(": hot code ").dec(m.sram_l_hot_used).str(" B of ").kib(m.sram_l_hot_budget);
  t.str(", tap data ").dec(m.sram_l_data_used).str(" B of ").kib(m.sram_l_data_budget).nl();
  for (unsigned i = 0; i < m.sram_l_count; ++i) {
    t.str("  budget ").str(m.sram_l[i].name).chr(' ').kib(m.sram_l[i].bytes).nl();
  }

  const std::uint32_t free_bytes = m.sram_u_size > m.sram_u_used + m.sram_u_stack ? m.sram_u_size - m.sram_u_used - m.sram_u_stack : 0u;
  t.str("sram_u ").kib(m.sram_u_size).str(": image ").dec(m.sram_u_used).str(" B, stack ").kib(m.sram_u_stack);
  t.str(", free ").dec(free_bytes).str(" B (at least ").kib(m.sram_u_min_free).str(" wanted)").nl();
  std::uint64_t budget_sum = 0;
  for (unsigned i = 0; i < m.sram_u_count; ++i) {
    t.str("  budget ").str(m.sram_u[i].name).chr(' ').kib(m.sram_u[i].bytes).nl();
    budget_sum += m.sram_u[i].bytes;
  }
  t.str("  budget sum ").dec(static_cast<std::uint32_t>(budget_sum)).str(" B").nl();
  return t.size();
}

}  // namespace o3sdk::platform::mk66::audio::diag
