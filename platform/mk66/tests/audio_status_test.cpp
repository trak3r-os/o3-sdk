// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioStatus (audio/status.h, OS-005a): the counters of the DMA ISR group, the render group behind its generation counter, the
// snapshot, the per-run reset and the fault latch, and the `cpu` / `mem` text formatters (audio/diag.h): the bounded text builder, the
// `cpu` golden text and the guard's `shed` line.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/audio/status.h>

#include <cstdint>
#include <cstring>

namespace {
using o3sdk::platform::mk66::audio::AudioStatus;
using o3sdk::platform::mk66::audio::RenderSample;
using o3sdk::platform::mk66::audio::StatusSnapshot;
namespace diag = o3sdk::platform::mk66::audio::diag;
}  // namespace

TEST_CASE("audio status: starts at zero, the period minimum reads 0 until two boundaries have happened") {
  AudioStatus status;
  const StatusSnapshot s = status.snapshot();
  CHECK(s.boundaries == 0u);
  CHECK(s.underruns == 0u);
  CHECK(s.period_min == 0u);
  CHECK(s.period_max == 0u);
  CHECK(s.renders == 0u);
  CHECK(s.generation == 0u);
}

TEST_CASE("audio status: the ISR group counts boundaries, underruns, runs, mutes, slips, faults and the period") {
  AudioStatus status;
  status.isr_boundary(false, false, false, false, 0, 0, 0);          // the first boundary has no period
  status.isr_boundary(false, false, false, false, 0, 261120, 0);
  status.isr_boundary(true, false, false, false, 1, 261121, 0);
  status.isr_boundary(true, false, false, false, 2, 261119, 1);
  status.isr_boundary(true, true, true, true, 3, 261120, 2);
  status.isr_boundary(false, false, false, true, 0, 270000, 0);
  const StatusSnapshot s = status.snapshot();
  CHECK(s.boundaries == 6u);
  CHECK(s.underruns == 3u);
  CHECK(s.consecutive_underruns == 0u);       // the current run ended
  CHECK(s.max_consecutive_underruns == 3u);
  CHECK(s.mute_events == 1u);
  CHECK(s.muted == 1u);
  CHECK(s.slot_slips == 1u);
  CHECK(s.hw_faults == 3u);
  CHECK(s.period_min == 261119u);
  CHECK(s.period_max == 270000u);
}

TEST_CASE("audio status: reset_peaks zeroes the per-run figures and keeps the totals and the live state (OS-PERF3, diag cpu reset)") {
  AudioStatus status;
  status.isr_boundary(false, false, false, false, 0, 0, 0);
  status.isr_boundary(true, true, true, true, 1, 261000, 2);
  status.isr_boundary(true, false, false, true, 2, 262000, 0);  // the run is 2 at the end: it is live state, not a peak
  RenderSample a{};
  a.total = 5000;
  a.latency = 300;
  a.stage[2] = 4000;
  a.stage[3] = 800;
  a.late = true;
  a.over_budget = true;
  status.render_publish(a);
  status.reset_peaks();
  const StatusSnapshot s = status.snapshot();
  CHECK(s.underruns == 0u);
  CHECK(s.max_consecutive_underruns == 0u);
  CHECK(s.mute_events == 0u);
  CHECK(s.slot_slips == 0u);
  CHECK(s.hw_faults == 0u);
  CHECK(s.period_min == 0u);  // all ones reads as "no period yet"
  CHECK(s.period_max == 0u);
  CHECK(s.late_drops == 0u);
  CHECK(s.governor_over == 0u);
  CHECK(s.render_max == 0u);
  CHECK(s.latency_max == 0u);
  CHECK(s.stage_max[2] == 0u);
  CHECK(s.stage_max[3] == 0u);
  CHECK(s.boundaries == 3u);       // totals and the live state stay
  CHECK(s.renders == 1u);
  CHECK(s.render_last == 5000u);
  CHECK(s.stage_last[2] == 4000u);
  CHECK(s.consecutive_underruns == 2u);
  CHECK(s.muted == 1u);
  CHECK(s.generation == 2u);
  // and the maxima rise again from zero
  RenderSample b{};
  b.total = 100;
  b.stage[2] = 60;
  status.render_publish(b);
  const StatusSnapshot t = status.snapshot();
  CHECK(t.render_max == 100u);
  CHECK(t.stage_max[2] == 60u);
}

TEST_CASE("audio status: the fault latch survives reset_peaks, so the sticky fault LED stays on (OS-PERF3, diag cpu reset)") {
  // Each of the four counters the LED goes by (underruns, late drops, slot slips, hardware faults) sets the latch alone, and `diag cpu reset` does not clear it.
  for (unsigned which = 0; which < 4; ++which) {
    AudioStatus status;
    CHECK(status.snapshot().fault_seen == 0u);
    status.isr_boundary(false, false, false, false, 0, 0, 0);
    status.isr_boundary(which == 0, which == 1, false, false, which == 0 ? 1u : 0u, 0, which == 2 ? 1u : 0u);
    if (which == 3) {
      RenderSample late{};
      late.late = true;
      status.render_publish(late);
    }
    StatusSnapshot s = status.snapshot();
    CHECK(s.fault_seen == 1u);
    status.reset_peaks();
    s = status.snapshot();
    CHECK(s.underruns == 0u);
    CHECK(s.slot_slips == 0u);
    CHECK(s.hw_faults == 0u);
    CHECK(s.late_drops == 0u);
    CHECK(s.fault_seen == 1u);  // the counters are zero, the latch is not
    status.reset_peaks();       // and a second reset keeps it too
    CHECK(status.snapshot().fault_seen == 1u);
    status.reset();             // only a full reset (before the writers run) clears it
    CHECK(status.snapshot().fault_seen == 0u);
  }
  // A reset with nothing to report latches nothing; a later fault does.
  AudioStatus clean;
  clean.isr_boundary(false, false, false, false, 0, 261000, 0);
  RenderSample ok{};
  ok.total = 100;
  ok.over_budget = true;  // over the governor's budget is not a fault (its own LED code)
  clean.render_publish(ok);
  clean.reset_peaks();
  CHECK(clean.snapshot().fault_seen == 0u);
  clean.isr_boundary(true, false, false, false, 1, 261000, 0);
  CHECK(clean.snapshot().fault_seen == 1u);
  clean.reset_peaks();
  CHECK(clean.snapshot().fault_seen == 1u);
}

TEST_CASE("audio status: the render group publishes whole samples behind an even generation") {
  AudioStatus status;
  RenderSample a{};
  a.total = 5000;
  a.latency = 300;
  a.stage[0] = 10;
  a.stage[1] = 20;
  a.stage[2] = 4000;
  a.stage[3] = 800;
  a.stage[4] = 100;
  a.input_peak = 77;
  status.render_publish(a);
  StatusSnapshot s = status.snapshot();
  CHECK(s.renders == 1u);
  CHECK(s.render_last == 5000u);
  CHECK(s.render_max == 5000u);
  CHECK(s.latency_last == 300u);
  CHECK(s.stage_last[2] == 4000u);
  CHECK(s.input_peak == 77u);
  CHECK(s.generation == 2u);
  CHECK((s.generation & 1u) == 0u);

  RenderSample b = a;
  b.total = 3000;
  b.latency = 900;
  b.stage[2] = 2500;
  b.late = true;
  b.over_budget = true;
  status.render_publish(b);
  s = status.snapshot();
  CHECK(s.renders == 2u);
  CHECK(s.render_last == 3000u);
  CHECK(s.render_max == 5000u);               // the maximum holds
  CHECK(s.latency_last == 900u);
  CHECK(s.latency_max == 900u);
  CHECK(s.stage_last[2] == 2500u);
  CHECK(s.stage_max[2] == 4000u);
  CHECK(s.late_drops == 1u);
  CHECK(s.governor_over == 1u);
  CHECK(s.generation == 4u);

  status.reset();
  s = status.snapshot();
  CHECK(s.renders == 0u);
  CHECK(s.boundaries == 0u);
  CHECK(s.generation == 0u);
  CHECK(s.period_min == 0u);
}

TEST_CASE("audio diag: the text builder never overruns and always terminates") {
  char buf[16];
  diag::Text t(buf, sizeof buf);
  t.str("0123456789").chr(' ').dec(4294967295u).str(" tail");
  CHECK(t.truncated());
  CHECK(std::strlen(buf) == sizeof buf - 1u);
  CHECK(buf[sizeof buf - 1u] == '\0');
  CHECK(t.size() == sizeof buf - 1u);

  char tiny[1];
  diag::Text z(tiny, sizeof tiny);
  z.str("abc");
  CHECK(tiny[0] == '\0');
  CHECK(z.truncated());

  char none[1] = {'x'};
  diag::Text zero(none, 0);  // capacity 0: nothing is touched
  zero.str("abc").dec(5);
  CHECK(none[0] == 'x');
  CHECK(zero.size() == 0u);
}

TEST_CASE("audio diag: numbers, hex, percent and KiB") {
  char buf[96];
  diag::Text t(buf, sizeof buf);
  t.dec(0).chr(' ').dec(7).chr(' ').dec(4294967295u).chr(' ').hex32(0x70FD0000u).chr(' ').hex32(0).chr(' ');
  t.percent(1, 1000).chr('|').percent(2, 3).chr('|').percent(5, 0).chr('|').percent(261120, 261120).chr('|');
  t.kib(10816u * 1024u).chr('|').kib(1536u).chr('|').kib(0);
  CHECK_FALSE(t.truncated());
  CHECK(std::strcmp(buf, "0 7 4294967295 0x70FD0000 0x00000000 0.1 %|66.6 %|-|100.0 %|10816 KiB|1536 B|0 KiB") == 0);
}

TEST_CASE("audio diag: `diag cpu` golden text") {
  StatusSnapshot s{};
  s.boundaries = 12345;
  s.renders = 12340;
  s.underruns = 2;
  s.consecutive_underruns = 0;
  s.max_consecutive_underruns = 1;
  s.muted = 0;
  s.mute_events = 0;
  s.late_drops = 1;
  s.slot_slips = 0;
  s.hw_faults = 0;
  s.render_last = 3120;
  s.render_max = 4100;
  s.governor_over = 0;
  s.stage_last[0] = 20;
  s.stage_max[0] = 25;
  s.stage_last[1] = 12;
  s.stage_max[1] = 15;
  s.stage_last[2] = 2900;
  s.stage_max[2] = 3800;
  s.stage_last[3] = 150;
  s.stage_max[3] = 190;
  s.stage_last[4] = 60;
  s.stage_max[4] = 90;
  s.stage_last[5] = 11;
  s.stage_max[5] = 13;
  s.stage_last[6] = 30;
  s.stage_max[6] = 40;
  s.stage_last[7] = 70;
  s.stage_max[7] = 80;
  s.stage_last[8] = 5;
  s.stage_max[8] = 6;
  s.latency_last = 410;
  s.latency_max = 1900;
  s.period_min = 261119;
  s.period_max = 261121;
  s.input_peak = 31;
  char buf[768];
  const std::size_t n = diag::format_cpu(s, 261120, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  const char* const expected =
      "audio  blocks 12345  renders 12340  underrun 2 (run 0, max 1)  mute 0 (events 0)  late 1  slips 0  hwfault 0\n"
      "render last 3120  max 4100 cyc = 1.5 % of the block (261120 cyc)  over-budget 0\n"
      "stage   input 20/25  drain 12/15  render 2900/3800  strips 150/190  buses 60/90  master 11/13  convert 30/40  taps 70/80  commit 5/6  (last/max cyc)\n"
      "jitter start-latency 410/1900 cyc  boundary period 261119..261121 cyc  input-peak 31\n";
  CHECK(std::strcmp(buf, expected) == 0);

  // Too small a buffer: bounded, terminated.
  char small[40];
  diag::format_cpu(s, 261120, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);
}

TEST_CASE("diag shed: the guard's counters on one line, the HIL gates' 'shed 0 blocks' text, bounded (OS-MAINGUARD)") {
  char buf[128];
  const std::size_t n = diag::format_shed(0, 0, 0, 0, 8, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "guard  shed 0 blocks in 0 runs  late 0  longest run since boot 0 blocks  n 8\n") == 0);
  diag::format_shed(222, 3, 72, 4, 32, buf, sizeof buf);
  CHECK(std::strcmp(buf, "guard  shed 222 blocks in 72 runs  late 3  longest run since boot 4 blocks  n 32\n") == 0);
  char small[20];
  diag::format_shed(222, 3, 72, 4, 32, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);
}

// New in o3sdk (track3r-os tests format_mem through its own memory tables, platform/mk66/diag_report.h, which has no counterpart here): the
// `mem` text from a small hand-made report, the complete/MISMATCH verdict and the free figure that never underflows.
TEST_CASE("audio diag: `mem` golden text, the region sum verdict and the SRAM_U free figure") {
  static const diag::MemRow sdram[] = {{"arena", 0x70000000u, 1536u * 1024u}, {"log", 0x70180000u, 512u * 1024u}};
  static const diag::BudgetRow sram_l[] = {{"hot", 32u * 1024u}};
  static const diag::BudgetRow sram_u[] = {{"stack", 24u * 1024u}, {"bss", 8u * 1024u}};
  diag::MemReport m{};
  m.sdram = sdram;
  m.sdram_count = 2;
  m.sdram_total = 2048u * 1024u;
  m.sram_l = sram_l;
  m.sram_l_count = 1;
  m.sram_l_size = 64u * 1024u;
  m.sram_l_hot_used = 1660;
  m.sram_l_hot_budget = 32u * 1024u;
  m.sram_u = sram_u;
  m.sram_u_count = 2;
  m.sram_u_size = 192u * 1024u;
  m.sram_u_used = 50000;
  m.sram_u_stack = 24u * 1024u;
  m.sram_u_min_free = 32u * 1024u;
  char buf[512];
  const std::size_t n = diag::format_mem(m, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  const char* const expected =
      "sdram 2048 KiB\n"
      "  arena 0x70000000 1536 KiB\n"
      "  log 0x70180000 512 KiB\n"
      "  regions sum 2048 KiB (complete)\n"
      "sram_l 64 KiB: hot code 1660 B of 32 KiB, tap data 0 B of 0 KiB\n"
      "  budget hot 32 KiB\n"
      "sram_u 192 KiB: image 50000 B, stack 24 KiB, free 122032 B (at least 32 KiB wanted)\n"
      "  budget stack 24 KiB\n"
      "  budget bss 8 KiB\n"
      "  budget sum 32768 B\n";
  CHECK(std::strcmp(buf, expected) == 0);

  m.sdram_total -= 32u;  // the regions no longer add up to the part
  diag::format_mem(m, buf, sizeof buf);
  CHECK(std::strstr(buf, "(MISMATCH)") != nullptr);
  CHECK(std::strstr(buf, "(complete)") == nullptr);

  m.sram_u_used = m.sram_u_size;  // an image that fills SRAM_U: free is 0, not a wrapped number
  diag::format_mem(m, buf, sizeof buf);
  CHECK(std::strstr(buf, "free 0 B") != nullptr);

  char small[40];  // too small a buffer: bounded, terminated
  diag::format_mem(m, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);
}
