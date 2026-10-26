// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock-follow loop and the stamp map (midi/clock_dll.h, OS-005c) against simulated external clocks: steady, +-1 % tempo offset, jitter of 50 us (a DIN master) and
// of +-1 ms (a USB host), a tempo ramp and a tempo step, dropped clocks. For each case the lock time and the steady-state phase error are measured against the ideal
// clock the simulation was generated from (the radar card's targets: lock within a beat, phase rms <= 30 us from a DIN master, never a jump). Set
// O3SDK_SEQ_REPORT=1 to print the table. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3sdk/core/qtime.h>
#include <o3sdk/platform/mk66/midi/clock_dll.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
namespace core = o3sdk::core;
using o3sdk::test::Rng;

namespace {

constexpr double kFs = 180000000.0 * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;  // samples per second

#if defined(__arm__)
constexpr double kScaleSeconds = 0.25;
#else
constexpr double kScaleSeconds = 1.0;
#endif

struct Sim {
  const char* name;
  double bpm0;          // the master's tempo at the start
  double bpm1;          // at the end (a ramp) or after the step
  int profile;          // 0 constant, 1 linear ramp over the run, 2 step at the middle
  double jitter_us;     // uniform +-
  double drop_pct;      // clocks that never arrive
  double nominal_bpm;   // what the loop assumed before the first interval
  double seconds;
  std::uint32_t seed;
  std::uint32_t burst_at;  // a run of 6 missing clocks starting here (0: none)
  double steady_rms_limit_us;
  double lock_clocks_limit;   // the loop's own locked() flag, in clocks from the first (for a step: from the step)
  std::uint32_t track_mhz;    // the tracking bandwidth, millihertz (500: FOLLOW NORMAL)
};

struct Metrics {
  double lock_clocks = 0;   // the loop's locked() flag first true
  double lock_ms = 0;
  double settle_clocks = 0; // the phase error stays within 1.5 x its steady worst case (and a sample) from here on
  double rms_us = 0;
  double max_us = 0;
  double tempo_err_pct = 0;
  std::uint32_t tick_mismatch = 0;
  std::uint32_t clocks = 0;
  std::uint32_t reacquires = 0;
  std::uint32_t missing = 0;
  double max_step_us = 0;  // the largest change between two consecutive intervals the loop gave (slew, not jump)
};

Metrics simulate(const Sim& sim) {
  midi::ClockDll::Config dcfg;
  dcfg.track_mhz = sim.track_mhz;
  midi::ClockDll dll(dcfg);
  const double nominal_period = kFs * 60.0 / (sim.nominal_bpm * 24.0);
  dll.reset(0, static_cast<core::Q32>(nominal_period * 4294967296.0));
  Rng rng(sim.seed);
  // `total` is the length of a ramp, in clocks of the full run: the target runs a fraction of it, over the first part of the same ramp (the same lag, not a steeper one)
  const std::uint32_t total = static_cast<std::uint32_t>(sim.seconds * sim.bpm0 * 24.0 / 60.0);
  const std::uint32_t run_clocks = static_cast<std::uint32_t>(sim.seconds * kScaleSeconds * sim.bpm0 * 24.0 / 60.0);
  const std::uint32_t half = run_clocks / 2;

  static double err[20000];  // samples, one per accepted clock (indexed by clock number)
  static bool seen[20000];
  const std::uint32_t cap = run_clocks < 20000u ? run_clocks : 20000u;
  for (std::uint32_t i = 0; i < cap; ++i) {
    seen[i] = false;
  }
  double t_true = 1000.0;  // samples: the first clock
  double t_first = t_true;
  double bpm = sim.bpm0;
  Metrics m;
  double prev_used = 0, prev_interval = 0;
  bool have_prev = false;
  std::uint32_t prev_c = 0;
  std::uint32_t first_locked = 0;
  bool locked_seen = false;
  std::uint32_t after_step = 0;  // the clock from which `locked` is looked for again (a step re-acquires)
  std::uint32_t missing_run = 0;
  for (std::uint32_t c = 0; c < cap; ++c) {
    // the master's tempo at this clock
    if (sim.profile == 1) {
      bpm = sim.bpm0 + (sim.bpm1 - sim.bpm0) * static_cast<double>(c) / static_cast<double>(total);
    } else if (sim.profile == 2) {
      bpm = c < half ? sim.bpm0 : sim.bpm1;
    }
    const double period = kFs * 60.0 / (bpm * 24.0);
    if (c != 0u) {
      t_true += period;
    }
    bool drop = c != 0u && (rng.below(10000) < static_cast<std::uint32_t>(sim.drop_pct * 100.0));
    if (sim.burst_at != 0u && c >= sim.burst_at && c < sim.burst_at + 6u) {
      drop = true;
    }
    if (drop) {
      ++missing_run;
      continue;
    }
    missing_run = 0;
    const double jitter = sim.jitter_us != 0.0 ? ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * sim.jitter_us * kFs / 1.0e6 : 0.0;
    const double obs = t_true + jitter;
    const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>(obs * 4294967296.0));
    if (r == midi::ClockDll::Result::Ignored) {
      continue;
    }
    ++m.clocks;
    if (dll.tick() != c) {
      ++m.tick_mismatch;
    }
    if (sim.profile == 2 && c == half) {
      locked_seen = false;  // the step: look for the lock again
      after_step = c;
    }
    if (!locked_seen && dll.locked()) {
      locked_seen = true;
      first_locked = c - after_step;
    }
    const double used = static_cast<double>(dll.time_of_tick(dll.tick())) / 4294967296.0;
    if (c < cap) {
      err[c] = used - t_true;
      seen[c] = true;
    }
    if (have_prev && c == prev_c + 1u) {
      const double interval = used - prev_used;
      if (prev_interval != 0.0 && c >= 3u) {
        const double step = std::fabs(interval - prev_interval) / kFs * 1.0e6;
        if (step > m.max_step_us && (r == midi::ClockDll::Result::Tracked)) {
          m.max_step_us = step;
        }
      }
      prev_interval = interval;
    } else {
      prev_interval = 0.0;
    }
    prev_used = used;
    prev_c = c;
    have_prev = true;
  }
  // steady state: the last quarter of the clocks that were seen
  double sum2 = 0, mx = 0;
  std::uint32_t n = 0;
  for (std::uint32_t c = cap - cap / 4u; c < cap; ++c) {  // the last quarter (a step at the middle has settled by then)
    if (!seen[c]) {
      continue;
    }
    sum2 += err[c] * err[c];
    mx = std::fabs(err[c]) > mx ? std::fabs(err[c]) : mx;
    ++n;
  }
  const double rms = n != 0u ? std::sqrt(sum2 / n) : 0.0;
  m.rms_us = rms / kFs * 1.0e6;
  m.max_us = mx / kFs * 1.0e6;
  // lock: the first clock from which every error stays within 1.5 x the steady-state worst case (and at least a sample, 22.7 us)
  const double thr = 1.5 * mx + 1.0;
  std::uint32_t lock = cap;
  for (std::uint32_t c = cap; c-- > 0;) {
    if (!seen[c]) {
      continue;
    }
    if (std::fabs(err[c]) > thr) {
      break;
    }
    lock = c;
  }
  m.settle_clocks = sim.profile == 2 ? (lock > half ? static_cast<double>(lock - half) : 0.0) : static_cast<double>(lock);
  m.lock_clocks = locked_seen ? static_cast<double>(first_locked) : 1.0e9;
  m.lock_ms = locked_seen ? m.lock_clocks * 60.0 / (sim.bpm0 * 24.0) * 1000.0 : -1.0;
  (void)t_first;
  const double tempo = static_cast<double>(dll.bpm_x100()) / 100.0;
  const double want = sim.profile == 0 ? sim.bpm0 : (sim.profile == 2 ? sim.bpm1 : sim.bpm0 + (sim.bpm1 - sim.bpm0) * static_cast<double>(cap) / static_cast<double>(total));
  m.tempo_err_pct = (tempo - want) / want * 100.0;
  m.reacquires = dll.stats().reacquires;
  m.missing = dll.stats().missing;
  return m;
}

}  // namespace

TEST_CASE("clock map: a stamp is a time on the sample timeline, before or after the reference, across the counter's wrap") {
  midi::ClockMap map;
  map.set_ref(100000, 0xFFFFFF00u);  // the counter is about to wrap
  const core::CyclesPerSample r{4080u, 1u};
  // the reference itself
  CHECK(map.to_rel(0xFFFFFF00u, 100000, r) == 0);
  // 4080 cycles later (across the wrap): one sample
  CHECK(map.to_rel(0xFFFFFF00u + 4080u, 100000, r) == static_cast<core::SQ32>(core::kQ32One));
  // 4080 cycles earlier: minus one sample
  CHECK(map.to_rel(0xFFFFFF00u - 4080u, 100000, r) == -static_cast<core::SQ32>(core::kQ32One));
  // the origin is another sample: the reference is 100000 - 99990 = 10 samples after it
  CHECK(map.to_rel(0xFFFFFF00u, 99990, r) == 10 * static_cast<core::SQ32>(core::kQ32One));
  // half a sample
  CHECK(map.to_rel(0xFFFFFF00u + 2040u, 100000, r) == static_cast<core::SQ32>(core::kQ32Half));
  // a DIN byte before the reference: 57,600 cycles = 14.1176 samples
  const core::SQ32 back = map.to_rel(0xFFFFFF00u - 57600u, 100000, r);
  CHECK(back < -14 * static_cast<core::SQ32>(core::kQ32One));
  CHECK(back > -15 * static_cast<core::SQ32>(core::kQ32One));
  // the 44,100 Hz clock: 441 samples are 1,800,000 cycles
  const core::CyclesPerSample r2{1800000u, 441u};
  CHECK(map.to_rel(0xFFFFFF00u + 1800000u, 100000, r2) == 441 * static_cast<core::SQ32>(core::kQ32One));
}

TEST_CASE("clock dll: the first clocks, the fit, glitches, dropouts, a long silence and the tick count") {
  midi::ClockDll dll;
  const core::Q32 spt = core::samples_per_tick_q32(12000);
  dll.reset(500, 8u * spt);
  CHECK_FALSE(dll.has_phase());
  const core::SQ32 one = static_cast<core::SQ32>(core::kQ32One);
  const core::SQ32 base = 1000 * one;
  const core::SQ32 period = static_cast<core::SQ32>(8u * spt);  // 919 samples
  const auto at = [&](std::int64_t k) { return base + static_cast<core::SQ32>(k) * period; };
  CHECK(dll.on_clock(base) == midi::ClockDll::Result::Started);
  CHECK(dll.has_phase());
  CHECK_FALSE(dll.running());
  CHECK(dll.tick() == 0u);
  // a clock 1 sample later is no clock of any tempo we follow
  CHECK(dll.on_clock(base + one) == midi::ClockDll::Result::Ignored);
  // the first clocks are fitted: provisional until `fit_clocks` of them, then the loop runs, with the period of the straight line through them
  for (std::uint32_t k = 1; k < 12; ++k) {
    CHECK(dll.on_clock(at(k)) == midi::ClockDll::Result::Acquiring);
    CHECK(dll.tick() == k);
    CHECK(dll.has_phase());
  }
  CHECK(dll.running());
  CHECK(dll.period() == static_cast<core::Q32>(period));  // exact: the points are exactly on a line
  CHECK(dll.bpm_x100() == 12000u);
  CHECK(dll.time_of_tick(11) == at(11));
  // the next clocks on time are tracked with no error, and the loop's times are exact
  CHECK(dll.on_clock(at(12)) == midi::ClockDll::Result::Tracked);
  CHECK(dll.tick() == 12u);
  CHECK(dll.time_of_tick(12) == at(12));
  CHECK(dll.time_of_tick(13) == at(13));  // the prediction
  CHECK(dll.time_of_tick(15) == at(15));  // extrapolated
  CHECK(dll.time_of_tick(10) == at(10));  // and back
  CHECK_FALSE(dll.locked());              // eight good clocks are needed
  for (std::uint32_t k = 13; k < 40; ++k) {
    CHECK(dll.on_clock(at(k)) == midi::ClockDll::Result::Tracked);
  }
  CHECK(dll.locked());
  CHECK(dll.tick() == 39u);
  // a missing clock: the loop advances over it
  CHECK(dll.on_clock(at(41)) == midi::ClockDll::Result::Dropout);
  CHECK(dll.tick() == 41u);
  CHECK(dll.stats().missing == 1u);
  // a clock far too early is ignored; the next one on time is the 42nd
  CHECK(dll.on_clock(at(41) + period / 5) == midi::ClockDll::Result::Ignored);
  CHECK(dll.on_clock(at(42)) == midi::ClockDll::Result::Tracked);
  CHECK(dll.tick() == 42u);
  // a long silence: the loop starts again from the next clock, the count of ticks goes on
  CHECK(dll.on_clock(at(200)) == midi::ClockDll::Result::Reacquired);
  CHECK(dll.tick() == 200u);
  CHECK_FALSE(dll.running());
  CHECK(dll.on_clock(at(201)) == midi::ClockDll::Result::Acquiring);
  CHECK(dll.tick() == 201u);
  // staleness
  CHECK_FALSE(dll.stale(at(202), 8));
  CHECK(dll.stale(at(220), 8));
  // a clock that arrives early twice in a row is a tempo that went up: the second one is taken as the next clock, and the count stays right
  midi::ClockDll up;
  up.reset(0, 8u * spt);
  for (std::uint32_t k = 0; k < 30; ++k) {
    (void)up.on_clock(static_cast<core::SQ32>(k) * period);
  }
  CHECK(up.tick() == 29u);
  const core::SQ32 fast = period * 3 / 5;  // 40 % faster
  std::uint32_t expect_tick = 29;
  for (std::uint32_t k = 1; k <= 30; ++k) {
    (void)up.on_clock(29 * period + static_cast<core::SQ32>(k) * fast);
    ++expect_tick;
    CHECK(up.tick() == expect_tick);
  }
  CHECK(up.stats().reacquires >= 1u);
  CHECK(std::llabs(static_cast<std::int64_t>(up.bpm_x100()) - 20000) < 100);  // 120 BPM * 5/3 = 200 BPM
}

TEST_CASE("clock dll: lock time and steady-state phase error against simulated external clocks") {
  const bool report = std::getenv("O3SDK_SEQ_REPORT") != nullptr;
  //                     name                                   bpm0   bpm1  prof  jit   drop  nominal  secs  seed burst  rms   lock  B mHz
  const Sim sims[] = {
      {"steady 120 BPM",                                       120.0, 120.0, 0,    0.0,  0.0,  120.0,   20.0, 1,   0,     1.0,  24.0, 500},
      {"steady 120, DIN jitter +-50 us",                       120.0, 120.0, 0,   50.0,  0.0,  120.0,   30.0, 2,   0,    12.0,  30.0, 500},
      {"+1 % off nominal, jitter +-50 us",                     121.2, 121.2, 0,   50.0,  0.0,  120.0,   30.0, 3,   0,    12.0,  30.0, 500},
      {"-1 % off nominal, jitter +-50 us",                     118.8, 118.8, 0,   50.0,  0.0,  120.0,   30.0, 4,   0,    12.0,  30.0, 500},
      {"jitter +-500 us (USB 1 ms frames)",                    120.0, 120.0, 0,  500.0,  0.0,  120.0,   60.0, 5,   0,   100.0,  30.0, 500},
      {"  the same, FOLLOW LOOSE (0.25 Hz)",                   120.0, 120.0, 0,  500.0,  0.0,  120.0,   60.0, 6,   0,    70.0,  30.0, 250},
      {"jitter +-1 ms",                                        120.0, 120.0, 0, 1000.0,  0.0,  120.0,   60.0, 7,   0,   200.0,  60.0, 500},
      {"drift +1 % over the run, jitter +-50 us",              120.0, 121.2, 1,   50.0,  0.0,  120.0,   30.0, 14,  0,    45.0,  30.0, 500},
      {"drift -1 % over the run, jitter +-50 us",              120.0, 118.8, 1,   50.0,  0.0,  120.0,   30.0, 15,  0,    45.0,  30.0, 500},
      {"tempo ramp 120 -> 140 in 60 s, jitter +-50 us",        120.0, 140.0, 1,   50.0,  0.0,  120.0,   60.0, 8,   0,   350.0,  30.0, 500},
      {"tempo step 120 -> 150, jitter +-50 us",                120.0, 150.0, 2,   50.0,  0.0,  120.0,   30.0, 9,   0,    12.0,  30.0, 500},
      {"dropouts 5 %, jitter +-50 us",                         120.0, 120.0, 0,   50.0,  5.0,  120.0,   30.0, 10,  0,    12.0,  30.0, 500},
      {"burst of 6 missing clocks, jitter +-50 us",            120.0, 120.0, 0,   50.0,  0.0,  120.0,   30.0, 11,  300,  12.0,  30.0, 500},
      {"fast 300 BPM, jitter +-50 us",                         300.0, 300.0, 0,   50.0,  0.0,  300.0,   20.0, 12,  0,    12.0,  30.0, 500},
      {"slow 60 BPM, jitter +-50 us",                           60.0,  60.0, 0,   50.0,  0.0,   60.0,   40.0, 13,  0,    14.0,  30.0, 500},
  };
  if (report) {
    std::printf("dll  %-46s %10s %9s %9s %10s %9s %8s %8s\n", "case", "locked at", "lock ms", "settle", "rms us", "max us", "tempo %", "slew us");
  }
  for (const Sim& s : sims) {
    const Metrics m = simulate(s);
    CHECK_MESSAGE(m.tick_mismatch == 0u, s.name);
    CHECK_MESSAGE(m.rms_us <= s.steady_rms_limit_us, s.name);
    CHECK_MESSAGE(m.lock_clocks <= s.lock_clocks_limit, s.name);
    CHECK_MESSAGE(m.lock_ms >= 0.0, s.name);
    CHECK_MESSAGE(m.max_step_us < 1500.0, s.name);  // slew, not a jump: the interval between two clocks changes by a fraction of the error that caused it
    if (s.profile != 1) {
      CHECK_MESSAGE(std::fabs(m.tempo_err_pct) < 0.2, s.name);
    }
    if (report) {
      std::printf("dll  %-46s %10.0f %9.1f %9.0f %10.2f %9.2f %8.3f %8.1f  (reacq %u, missing %u)\n", s.name, m.lock_clocks, m.lock_ms, m.settle_clocks, m.rms_us, m.max_us,
                  m.tempo_err_pct, m.max_step_us, static_cast<unsigned>(m.reacquires), static_cast<unsigned>(m.missing));
    }
  }
}
