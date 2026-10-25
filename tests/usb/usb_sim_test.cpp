// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: the full closed-loop property test of the USB stream path (ADR 0012 S2). Millions of simulated SOFs:
// host only (a target-sized run of the same harness is in usb_stream_test.cpp, which also runs on QEMU).
#pragma GCC optimize("O2")

#include <doctest/doctest.h>

#include "usb_sim_harness.h"

using namespace usb_sim;

TEST_CASE("usb sim: IN packetiser and OUT feedback against a host at +/-500 ppm with +/-150 us jitter, 1.5 million packets") {
  struct Scenario {
    const char* name;
    std::int32_t ppm;
    std::int32_t jitter;
    std::uint32_t seed;
  };
  const Scenario scenarios[] = {
      {"host fast", 500, 27000, 1}, {"host slow", -500, 27000, 2}, {"host exact", 0, 27000, 3},
      {"host +137", 137, 27000, 4}, {"clean +500", 500, 0, 5},     {"clean -500", -500, 0, 6},
  };
  constexpr std::uint64_t kPacketsPerScenario = 250000;
  std::uint64_t total = 0;
  for (const Scenario& s : scenarios) {
    SimParams p;
    p.ppm = s.ppm;
    p.jitter = s.jitter;
    p.seed = s.seed;
    p.packets = kPacketsPerScenario;
    const SimResult r = run_sim(p);
    check_run(p, r, s.name);
    total += r.packets;
  }
  CHECK(total >= 1500000u);
}

TEST_CASE("usb sim: at 44.1 kHz exactly (D-OS-8: 1800000 / 441 cycles per sample) the same properties hold") {
  for (std::int32_t ppm : {500, -500}) {
    SimParams p;
    p.ref = CyclesPerSample{1800000u, 441u};
    p.ppm = ppm;
    p.seed = 90 + static_cast<std::uint32_t>(ppm > 0);
    p.packets = 250000;
    check_run(p, run_sim(p), "44.1 kHz");
  }
}

TEST_CASE("usb sim: other channel counts and a different engine phase") {
  for (std::uint32_t channels : {2u, 8u, 11u}) {
    SimParams p;
    p.channels = channels;
    p.ppm = 311;
    p.seed = 20 + channels;
    p.packets = 60000;
    const SimResult r = run_sim(p);
    check_run(p, r, "channels");
  }
}

TEST_CASE("usb sim: a reference clock constant that is 120 ppm off is absorbed by the guard and the trim") {
  // The estimated rate is wrong by a constant, which neither the SOF measurement nor the host can see: the
  // return fill drifts out of the dead zone, the trim pushes it back, and the IN guard does the same for the
  // stems. No packet is lost and every IN packet keeps a legal size.
  for (std::int32_t bias : {120, -120}) {
    SimParams p;
    p.ppm = 80;
    p.rate_bias_ppm = bias;
    p.packets = 150000;
    p.settle = 60000;
    p.seed = 31;
    const SimResult r = run_sim(p);
    const Counters& c = r.counters;
    INFO("bias " << bias);
    CHECK(c.in_size_other == 0);
    CHECK(r.continuity_errors == 0);
    CHECK(c.in_underruns == r.at_settle.in_underruns);
    CHECK(c.in_dropped_blocks == r.at_settle.in_dropped_blocks);
    CHECK(c.out_underruns == r.at_settle.out_underruns);
    CHECK(c.out_dropped_frames == r.at_settle.out_dropped_frames);
    // The guard holds the smoothed IN fill at its band edge (plus the ripple of the block sawtooth), and the
    // trim holds the OUT mean fill a little beyond its dead zone: the ring ends are still far away.
    CHECK(r.avg_fill_min >= kStemFillBand.low - 24u);
    CHECK(r.avg_fill_max <= kStemFillBand.high + 24u);
    CHECK(r.mean_out_fill > 80.0);
    CHECK(r.mean_out_fill < ReturnRing::capacity() - 120.0);
    CHECK(std::abs(r.final_trim_q16) > 100);  // the trim is working against the injected error
    CHECK(std::abs(r.final_trim_q16) <= FeedbackControl::kTrimLimitQ16);
    std::printf("[usb-sim] bias %+4d ppm    IN fill smoothed %u..%u  OUT fill %u..%u mean %.0f trim %d (limit %d) lost in %u/%u out %u/%u\n", bias,
                r.avg_fill_min, r.avg_fill_max, r.out_fill_min, r.out_fill_max, r.mean_out_fill, r.final_trim_q16,
                FeedbackControl::kTrimLimitQ16, c.in_underruns, c.in_dropped_blocks, c.out_underruns, c.out_dropped_frames);
  }
}

TEST_CASE("usb sim: high speed, 5 or 6 frames per microframe") {
  SimParams p;
  p.speed = Speed::High;
  p.ppm = -300;
  p.jitter = 3000;      // 17 us, well inside a 125 us microframe
  p.packets = 200000;
  p.settle = 60000;
  p.seed = 77;
  const SimResult r = run_sim(p);
  const Counters& c = r.counters;
  CHECK(c.in_size_other == 0);
  CHECK(r.min_frames == 5);
  CHECK(r.max_frames == 6);
  CHECK(r.continuity_errors == 0);
  CHECK(c.in_underruns == r.at_settle.in_underruns);
  CHECK(c.in_dropped_blocks == r.at_settle.in_dropped_blocks);
  CHECK(c.out_underruns == r.at_settle.out_underruns);
  CHECK(c.out_dropped_frames == r.at_settle.out_dropped_frames);
  CHECK(r.worst_estimator_ppm < 100.0);
  std::printf("[usb-sim] high speed     est %.1f ppm fb %.1f ppm, IN fill %u..%u, OUT fill %u..%u\n", r.worst_estimator_ppm,
              r.worst_feedback_ppm, c.in_fill_min, c.in_fill_max, r.out_fill_min, r.out_fill_max);
}

TEST_CASE("usb sim: the guard alone holds the fill when the estimator is wrong by 500 ppm") {
  // The estimator is frozen at the nominal rate while the host is 500 ppm fast: the packetiser's phase is
  // wrong by 0.022 frames per ms, and only the ring-fill guard keeps the stem ring from drifting out.
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 10);
  static Pool pool;
  InPacketBuffer buf;
  Rng rng(3);
  const std::uint32_t rate = nominal_frames_q16(Speed::Full);
  // Device clock: blocks every 522,240 cycles; host SOFs every 180,000 * (1 +/- 500 ppm).
  for (std::int32_t ppm : {500, -500}) {
    in.start(Speed::Full, 10);
    ring.flush();
    cnt.reset();
    const std::uint64_t period = static_cast<std::uint64_t>(180000.0 * (1.0 + ppm * 1e-6) * 65536.0);
    std::uint64_t block = 0, k = 0;
    const std::uint64_t phase = rng.below(static_cast<std::uint32_t>(block_period_q16(kRefCyclesPerSample) >> 16));
    std::uint32_t settled_under = 0, settled_drop = 0;
    for (; k < 120000;) {
      const std::uint64_t t_sof = 1000 + ((k * period) >> 16);
      const std::uint64_t t_blk = phase + ((block * block_period_q16(kRefCyclesPerSample)) >> 16);
      if (t_blk <= t_sof) {
        pool.write(ring, static_cast<std::uint32_t>(block));
        ++block;
        continue;
      }
      in.build_packet(rate, buf.samples);
      if (k == 5000) {
        settled_under = cnt.in_underruns;
        settled_drop = cnt.in_dropped_blocks;
        cnt.reset_fill_range();
      }
      ++k;
    }
    INFO("ppm " << ppm);
    CHECK(cnt.in_size_other == 0);
    CHECK(cnt.in_underruns == settled_under);
    CHECK(cnt.in_dropped_blocks == settled_drop);
    CHECK(cnt.in_fill_min >= 45);
    CHECK(cnt.in_fill_max < 4 * kStemBlockFrames);
    CHECK(in.smoothed_fill() >= kStemFillBand.low - 8u);
    CHECK(in.smoothed_fill() <= kStemFillBand.high + 8u);
    std::printf("[usb-sim] guard only    ppm %+4d: IN fill %u..%u  lost %u/%u\n", ppm, cnt.in_fill_min, cnt.in_fill_max, cnt.in_underruns,
                cnt.in_dropped_blocks);
  }
}
