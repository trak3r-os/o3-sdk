// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioStatus (audio/status.h, OS-005a): the counters of the DMA ISR group, the render group behind its generation counter, the
// snapshot, the per-run reset and the fault latch.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/status.h>

#include <cstdint>

namespace {
using o3sdk::platform::mk66::audio::AudioStatus;
using o3sdk::platform::mk66::audio::RenderSample;
using o3sdk::platform::mk66::audio::StatusSnapshot;
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
