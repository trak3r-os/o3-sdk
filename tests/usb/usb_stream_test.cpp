// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: rate estimator, IN packetiser, feedback, return path, and a target-sized closed-loop run (ADR 0012 S2).
// This file also runs on QEMU (the `qemu_usb` suite): nothing here may need threads, files or a lot of time; the
// full property test is usb_sim_test.cpp (host only).
// The closed-loop runs push thousands of simulated packets: optimise this file's own harness code so it does not
// dominate the ASan run (the library under test is compiled with the project's flags).
#pragma GCC optimize("O2")

#include <doctest/doctest.h>

#include <o3sdk/usb/rate.h>
#include <o3sdk/usb/stream.h>

#include "usb_sim_harness.h"
#include "usb_test_util.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace o3sdk::usb;
using namespace usb_sim;
using usb_test::Rng;


// ---- constants and packing ----------------------------------------------------------------------------------

TEST_CASE("usb rate: nominal constants follow from the one reference-clock constant") {
  const CyclesPerSample today{4080u, 1u};         // MCLK = 180 MHz x 16/255
  const CyclesPerSample retuned{1800000u, 441u};  // D-OS-8: 180 MHz x 196/3125 = 256 x 44,100 Hz
  CHECK(nominal_sof_cycles(Speed::Full) == 180000u);
  CHECK(nominal_sof_cycles(Speed::High) == 22500u);
  // Frames per SOF in Q16.16, against independent arithmetic, for the clock the build uses and for both clocks.
  const auto exact = [](Speed s, CyclesPerSample r) {
    return static_cast<std::uint32_t>(std::llround(nominal_sof_cycles(s) * 65536.0 * r.den / r.num));
  };
  CHECK(nominal_frames_q16(Speed::Full) == exact(Speed::Full, kRefCyclesPerSample));
  CHECK(nominal_frames_q16(Speed::High) == exact(Speed::High, kRefCyclesPerSample));
  CHECK(nominal_frames_q16(Speed::Full, today) == exact(Speed::Full, today));
  CHECK(nominal_frames_q16(Speed::Full, retuned) == exact(Speed::Full, retuned));
  CHECK(nominal_frames_q16(Speed::High, today) == exact(Speed::High, today));
  CHECK(nominal_frames_q16(Speed::High, retuned) == exact(Speed::High, retuned));
  // The numbers the ADR and the descriptors quote.
  CHECK(nominal_frames_q16(Speed::Full, today) == 2891294u);       // 44.1176 frames per ms
  CHECK(feedback_value(Speed::Full, nominal_frames_q16(Speed::Full, today)) == 722824u);  // ADR 0012: 0x0B0788
  CHECK(feedback_value(Speed::Full, nominal_frames_q16(Speed::Full, today)) == 0x0B0788u);
  CHECK(feedback_value(Speed::High, nominal_frames_q16(Speed::High, today)) == 0x000583C4u);
  CHECK(nominal_frames_q16(Speed::Full, retuned) == 2890138u);     // 44.1 exactly, 2,890,137.6 rounded
  const std::uint32_t fb441 = feedback_value(Speed::Full, nominal_frames_q16(Speed::Full, retuned));
  CHECK((fb441 == 722534u || fb441 == 722535u));                   // 44.1 x 2^14 = 722,534.4, to the nearest LSB
  CHECK(nominal_frames_q16(Speed::High, retuned) == 361267u);      // 5.5125 per microframe
  // The multiplier of the estimator is derived from the same constant.
  CHECK(frames_multiplier(today) == 269488144u);                  // 2^40 / 4080
  CHECK(kRefMultiplier == frames_multiplier(kRefCyclesPerSample));
  CHECK(frames_multiplier(CyclesPerSample{255u, 1u}) == 0u);      // slower than 256 cycles per sample: refused
}

TEST_CASE("usb feedback: Q10.14 in 3 bytes at FS, Q16.16 in 4 bytes at HS") {
  std::uint8_t b[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  CHECK(feedback_bytes(Speed::Full) == 3);
  CHECK(feedback_bytes(Speed::High) == 4);
  CHECK(pack_feedback(Speed::Full, 0x0B0788u, b) == 3);
  CHECK((b[0] == 0x88 && b[1] == 0x07 && b[2] == 0x0B));
  CHECK(b[3] == 0xEE);  // the fourth byte is not touched
  CHECK(pack_feedback(Speed::High, 0x000583C4u, b) == 4);
  CHECK((b[0] == 0xC4 && b[1] == 0x83 && b[2] == 0x05 && b[3] == 0x00));
  CHECK(pack_feedback(Speed::High, 0x12345678u, b) == 4);
  CHECK((b[0] == 0x78 && b[1] == 0x56 && b[2] == 0x34 && b[3] == 0x12));
  // Q16.16 -> Q10.14 rounds to nearest and fits 24 bits; 10 integer bits cap the rate below 1024 frames.
  CHECK(feedback_value(Speed::Full, 4u << 16) == 4u << 14);
  CHECK(feedback_value(Speed::Full, (4u << 16) + 1u) == (4u << 14));
  CHECK(feedback_value(Speed::Full, (4u << 16) + 2u) == (4u << 14) + 1u);
  CHECK(feedback_value(Speed::Full, 0xFFFFFFFFu) == 0xFFFFFFu);
  CHECK(feedback_value(Speed::High, 0x00ABCDEFu) == 0x00ABCDEFu);
}

// ---- rate estimator -------------------------------------------------------------------------------------------

namespace {

struct EstimatorRun {
  double worst_ppm = 0;
  std::uint32_t resyncs = 0;
  bool locked = false;
};

// Feeds the estimator SOFs of a host that runs `ppm` off, with +/-jitter noise; `skip(k)` says which SOFs are
// lost (not delivered). Returns the worst error after `settle` SOFs.
template <typename Skip>
EstimatorRun run_estimator(Speed speed, std::int32_t ppm, std::int32_t jitter, std::uint32_t n, std::uint32_t settle, std::uint32_t seed,
                           std::uint32_t stamp0, Skip skip) {
  RateEstimator est(speed);
  Rng rng(seed);
  const std::uint32_t nominal = nominal_sof_cycles(speed);
  const std::uint64_t period_q16 = static_cast<std::uint64_t>(std::llround(nominal * 65536.0 * (1.0 + ppm * 1e-6)));
  const double truth = nominal_frames_q16(speed) * (1.0 + ppm * 1e-6);
  EstimatorRun r;
  const std::uint32_t mask = speed == Speed::Full ? 0x7FFu : 0x3FFFu;
  for (std::uint32_t k = 0; k < n; ++k) {
    if (skip(k)) {
      continue;
    }
    const std::uint64_t t = 100000u + ((k * period_q16) >> 16) + static_cast<std::uint64_t>(static_cast<std::int64_t>(jitter == 0 ? 0 : rng.sym(jitter)));
    est.on_sof(stamp0 + static_cast<std::uint32_t>(t), k & mask);
    if (k > settle) {
      r.worst_ppm = std::fmax(r.worst_ppm, ppm_diff(est.frames_per_sof_q16(), truth));
    }
  }
  r.resyncs = est.resyncs();
  r.locked = est.locked();
  return r;
}

}  // namespace

TEST_CASE("usb rate: estimator converges on clean SOFs at +/-500 ppm") {
  for (std::int32_t ppm : {-500, -37, 0, 250, 500}) {
    const EstimatorRun r = run_estimator(Speed::Full, ppm, 0, 14000, 8000, 1, 0, [](std::uint32_t) { return false; });
    INFO("ppm " << ppm << " worst " << r.worst_ppm);
    CHECK(r.worst_ppm < 3.0);  // Q16 truncation and the Q10.14-independent arithmetic: well under 1 ppm in practice
    CHECK(r.resyncs == 0);
  }
}

TEST_CASE("usb rate: estimator stays under 50 ppm with +/-150 us of stamp jitter") {
  double worst = 0;
  for (std::int32_t ppm : {-500, 0, 500}) {
    for (std::uint32_t seed : {1u, 2u, 3u}) {
      const EstimatorRun r = run_estimator(Speed::Full, ppm, 27000, 60000, 8000, seed, 0xFFFFF000u, [](std::uint32_t) { return false; });
      worst = std::fmax(worst, r.worst_ppm);
      CHECK(r.worst_ppm < 50.0);
      CHECK(r.locked);
    }
  }
  MESSAGE("estimator worst error after 8000 SOFs: " << worst << " ppm");
}

TEST_CASE("usb rate: a 32-SOF average could not do this (the reason for the tracking loop)") {
  // Difference of two stamps 32 SOFs apart, with the same jitter: the noise floor of a 32 ms estimate.
  Rng rng(9);
  double worst = 0;
  for (int i = 0; i < 20000; ++i) {
    const double a = rng.sym(27000), b = rng.sym(27000);
    worst = std::fmax(worst, std::fabs(a - b) / (32.0 * 180000.0) * 1e6);
  }
  CHECK(worst > 1000.0);  // thousands of ppm against a 50 ppm target
}

TEST_CASE("usb rate: lost and duplicated SOFs") {
  // One SOF lost every 50: the frame numbers bridge the gap, no resync, accuracy intact.
  const EstimatorRun a = run_estimator(Speed::Full, 300, 27000, 60000, 8000, 4, 0, [](std::uint32_t k) { return k % 50 == 49; });
  CHECK(a.resyncs == 0);
  CHECK(a.worst_ppm < 60.0);
  // A burst of 10 lost SOFs is longer than the bridged gap: one resync, then the loop tracks again.
  const EstimatorRun b = run_estimator(Speed::Full, -200, 27000, 60000, 12000, 5, 0, [](std::uint32_t k) { return k >= 5000 && k < 5010; });
  CHECK(b.resyncs == 1);
  CHECK(b.worst_ppm < 50.0);
  // The same frame number twice is ignored.
  RateEstimator est;
  est.on_sof(1000, 5);
  est.on_sof(181000, 6);
  const std::uint32_t before = est.frames_per_sof_q16();
  est.on_sof(999999, 6);
  CHECK(est.frames_per_sof_q16() == before);
  CHECK(est.sof_count() == 2);
}

TEST_CASE("usb rate: before any SOF the estimate is the nominal rate; reset restores it") {
  RateEstimator est;
  CHECK(est.frames_per_sof_q16() == nominal_frames_q16(Speed::Full));
  CHECK(est.period_cycles_q8() == 180000u * 256u);
  CHECK_FALSE(est.locked());
  est.on_sof(0, 0);
  est.on_sof(180100, 1);
  est.reset(Speed::High);
  CHECK(est.frames_per_sof_q16() == nominal_frames_q16(Speed::High));
  CHECK(est.sof_count() == 0);
}

TEST_CASE("usb rate: the SOF counter wraps without restarting the estimator") {
  RateEstimator est;
  const std::uint32_t period = 180000;
  std::uint32_t stamp = 0xFFFFF000u;
  std::uint32_t k = 0;
  for (; k < 6000; ++k) {  // locks
    est.on_sof(stamp, k & 0x7FFu);
    stamp += period;
  }
  CHECK(est.locked());
  CHECK(est.sof_count() == 6000u);
  est.force_sof_count(0xFFFFFFFEu);  // two SOFs before the 32-bit counter wraps
  for (int i = 0; i < 6; ++i, ++k) {
    est.on_sof(stamp, k & 0x7FFu);
    stamp += period;
  }
  CHECK(est.sof_count() == 4u);       // wrapped through zero
  CHECK(est.locked());                // the first-SOF logic did not run again
  CHECK(est.resyncs() == 0u);
  CHECK(est.frames_per_sof_q16() == nominal_frames_q16(Speed::Full));  // exact SOFs: the nominal rate, undisturbed
}

TEST_CASE("usb rate: the estimator follows either reference clock") {
  for (const CyclesPerSample ref : {CyclesPerSample{4080u, 1u}, CyclesPerSample{1800000u, 441u}}) {
    RateEstimator est(Speed::Full, frames_multiplier(ref));
    CHECK(est.frames_per_sof_q16() == nominal_frames_q16(Speed::Full, ref));
    // A host 200 ppm fast, clean stamps: converges on the reference's rate scaled by the host's error.
    const std::uint64_t period_q16 = static_cast<std::uint64_t>(std::llround(180000.0 * 65536.0 * (1.0 + 200e-6)));
    for (std::uint32_t k = 0; k < 12000; ++k) {
      est.on_sof(1000u + static_cast<std::uint32_t>((k * period_q16) >> 16), k & 0x7FFu);
    }
    const double truth = nominal_frames_q16(Speed::Full, ref) * (1.0 + 200e-6);
    CHECK(ppm_diff(est.frames_per_sof_q16(), truth) < 3.0);
  }
}

TEST_CASE("usb rate: high speed tracks a microframe period") {
  const EstimatorRun r = run_estimator(Speed::High, 400, 3000, 120000, 40000, 6, 0xFFFF0000u, [](std::uint32_t) { return false; });
  CHECK(r.worst_ppm < 100.0);
}

// ---- IN packetiser ------------------------------------------------------------------------------------------

TEST_CASE("usb packetiser: sizes are always 44 or 45 whatever the inputs") {
  Rng rng(7);
  InPacketiser pk;
  for (int i = 0; i < 200000; ++i) {
    const std::uint32_t rate = (i % 5 == 0) ? rng.next() : (43u << 16) + rng.below(3u << 16);  // including nonsense
    const std::uint32_t fill = rng.below(600);
    const std::uint32_t n = pk.next(Speed::Full, rate, fill, kStemFillBand);
    CHECK((n == 44u || n == 45u));
  }
  InPacketiser hs;
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t n = hs.next(Speed::High, rng.next(), rng.below(600), kStemFillBand);
    CHECK((n == 5u || n == 6u));
  }
}

TEST_CASE("usb packetiser: the mix of 44 and 45 follows the rate; the guard overrides without upsetting the phase") {
  InPacketiser pk;
  const std::uint32_t rate = (44u << 16) + 16384u;  // 44.25: one packet in four carries 45
  std::uint32_t n45 = 0;
  for (int i = 0; i < 4000; ++i) {
    n45 += pk.next(Speed::Full, rate, 200, kStemFillBand) == 45u ? 1u : 0u;
  }
  CHECK(n45 == 1000u);
  // Below the band: always 44; above: always 45; inside: back to the cadence.
  InPacketiser g;
  for (int i = 0; i < 50; ++i) {
    CHECK(g.next(Speed::Full, rate, kStemFillBand.low - 1u, kStemFillBand) == 44u);
  }
  for (int i = 0; i < 50; ++i) {
    CHECK(g.next(Speed::Full, rate, kStemFillBand.high + 1u, kStemFillBand) == 45u);
  }
  std::uint32_t n45_after = 0;
  for (int i = 0; i < 400; ++i) {
    n45_after += g.next(Speed::Full, rate, 200, kStemFillBand) == 45u ? 1u : 0u;
  }
  CHECK(n45_after == 100u);  // the phase accumulator was not disturbed by the overrides
  // Band edges are inside the band.
  InPacketiser e;
  CHECK(e.next(Speed::Full, (44u << 16), kStemFillBand.low, kStemFillBand) == 44u);
  CHECK(e.next(Speed::Full, (45u << 16), kStemFillBand.high, kStemFillBand) == 45u);
}

// ---- stem ring reader / IN stream -----------------------------------------------------------------------------


TEST_CASE("usb stream: priming sends silence of a legal size, then audio") {
  static Pool pool;
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 10);
  InPacketBuffer buf;
  const std::uint32_t rate = nominal_frames_q16(Speed::Full);
  pool.write(ring, 0);
  pool.write(ring, 1);
  for (int i = 0; i < 3; ++i) {  // two blocks queued: not enough (the start-up fill is the whole ring, 3 blocks)
    const std::uint32_t n = in.build_packet(rate, buf.samples);
    CHECK((n == 44u || n == 45u));
    for (std::uint32_t k = 0; k < n * 10; ++k) {
      CHECK(buf.samples[k] == 0);
    }
  }
  CHECK(cnt.in_prime_packets == 3);
  CHECK(in.smoothed_fill() == 0);
  pool.write(ring, 2);  // three blocks: primed
  const std::uint32_t n = in.build_packet(rate, buf.samples);
  CHECK(cnt.in_prime_packets == 3);
  CHECK(buf.samples[0] == stem_sample(0, 0));
  CHECK(buf.samples[9] == stem_sample(0, 9));
  CHECK(buf.samples[10] == stem_sample(1, 0));
  CHECK(buf.samples[(n - 1) * 10 + 3] == stem_sample(n - 1, 3));
  CHECK(cnt.in_size_other == 0);
  CHECK(cnt.in_size_lo + cnt.in_size_hi == 4);
  CHECK(cnt.in_fill_max == 384);
  CHECK(in.smoothed_fill() == 384);
}

TEST_CASE("usb stream: underrun finishes the packet with silence, counts, and re-primes") {
  static Pool pool;
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 8);
  // Zeroed: the scan below looks for the last frame that carries audio, and a packet of 44 frames (the 44,100 Hz clock, D-OS-8) leaves
  // frame 44 unwritten, which on the target would be the stack's paint, not silence.
  InPacketBuffer buf{};
  for (std::uint32_t b = 0; b < 3; ++b) {
    pool.write(ring, b);
  }
  std::uint32_t frames = 0;
  std::uint32_t packets = 0;
  while (cnt.in_underruns == 0 && packets < 30) {
    frames += in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
    ++packets;
  }
  CHECK(cnt.in_underruns == 1);
  CHECK(frames > 384);  // the 384 queued frames were delivered, the tail of the last packet is silence
  CHECK(packets == 9);
  std::uint32_t real = 0;  // frames of the last packet that carry audio
  for (std::uint32_t f = 0; f < 45; ++f) {
    if (buf.samples[f * 8] != 0) {
      real = f + 1;
    }
  }
  CHECK(real > 0);
  CHECK(real < 45);
  // Re-primed: silence until three blocks are queued again.
  const std::uint32_t before = cnt.in_prime_packets;
  in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
  CHECK(cnt.in_prime_packets == before + 1);
  for (std::uint32_t b = 3; b < 6; ++b) {
    pool.write(ring, b);
  }
  in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
  CHECK(cnt.in_prime_packets == before + 1);
  CHECK(buf.samples[0] == stem_sample(3 * kStemBlockFrames, 0));
}

TEST_CASE("usb stream: overrun drops the oldest block, keeps the newest and counts") {
  static Pool pool;
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 10);
  for (std::uint32_t b = 0; b < 6; ++b) {  // the USB side was away for six blocks
    pool.write(ring, b);
  }
  InPacketBuffer buf;
  in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
  CHECK(cnt.in_dropped_blocks == 3);  // blocks 0..2 were overwritten (3 usable slots)
  CHECK(buf.samples[0] == stem_sample(3 * kStemBlockFrames, 0));  // the oldest survivor is block 3
  CHECK(cnt.in_underruns == 0);
}

TEST_CASE("usb stream: stopping and restarting discards stale blocks") {
  static Pool pool;
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 10);
  for (std::uint32_t b = 0; b < 3; ++b) {
    pool.write(ring, b);
  }
  InPacketBuffer buf;
  in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
  CHECK(cnt.in_prime_packets == 0);
  in.stop();
  CHECK_FALSE(in.active());
  in.start(Speed::Full, 10);
  CHECK(in.active());
  in.build_packet(nominal_frames_q16(Speed::Full), buf.samples);
  CHECK(cnt.in_prime_packets == 1);  // flushed: waits for three fresh blocks again
}

TEST_CASE("usb stream: the guard follows the smoothed fill, not the block sawtooth") {
  static Pool pool;
  StemRing ring;
  Counters cnt;
  InStream in(ring, cnt);
  in.start(Speed::Full, 2);
  InPacketBuffer buf;
  // A device that produces exactly what the rate asks for (44.5 frames per packet), blocks arriving as the
  // ring drains: the smoothed fill stays inside the band, the guard never acts, and the cadence is the natural
  // half 44, half 45.
  std::uint32_t block = 0;
  std::uint32_t n45 = 0;
  const std::uint32_t rate = (44u << 16) + 32768u;
  for (std::uint64_t i = 0; i < 2000; ++i) {
    while (block < 3 + (89u * i) / 256u && ring.size_from_producer() < 3) {
      pool.write(ring, block++);
    }
    n45 += in.build_packet(rate, buf.samples) == 45u ? 1u : 0u;
  }
  CHECK(cnt.in_underruns == 0);
  CHECK(cnt.in_dropped_blocks == 0);
  CHECK(n45 + cnt.in_prime_packets >= 1000u - 5u);
  CHECK(n45 + cnt.in_prime_packets <= 1000u + 5u);
}

// ---- return path ----------------------------------------------------------------------------------------------

namespace {

void send_frames(OutStream& out, std::uint32_t first, std::uint32_t n) {
  OutPacketBuffer p;
  for (std::uint32_t i = 0; i < n; ++i) {
    p.frames[i] = static_cast<std::uint32_t>(static_cast<std::uint16_t>(s16(first + i))) |
                  (static_cast<std::uint32_t>(static_cast<std::uint16_t>(s16(first + i + 5000u))) << 16);
  }
  out.on_packet(p.frames, n * 4u);
}

}  // namespace

TEST_CASE("usb return: silence while priming, then real frames in order") {
  ReturnRing ring;
  Counters cnt;
  OutStream out(ring, cnt);
  ReturnReader ret(ring, cnt);
  alignas(4) std::int16_t l[kStemBlockFrames], r[kStemBlockFrames];
  std::uint32_t sent = 0;
  for (int i = 0; i < 5; ++i) {  // 220 frames: below kReturnPrimeFrames
    send_frames(out, sent, 44);
    sent += 44;
  }
  CHECK_FALSE(ret.pull(l, r, kStemBlockFrames));
  for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
    CHECK((l[i] == 0 && r[i] == 0));
  }
  CHECK(cnt.out_underruns == 0);  // priming is not an underrun
  send_frames(out, sent, 45);
  sent += 45;
  send_frames(out, sent, 45);
  sent += 45;  // 310 frames queued; priming keeps the newest 256, so the first real frame is number 54
  CHECK(ret.pull(l, r, kStemBlockFrames));
  for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
    CHECK(l[i] == s16(54 + i));
    CHECK(r[i] == s16(54 + i + 5000u));
  }
  CHECK(cnt.out_packets == 7);
  CHECK(cnt.out_fill_max == 265);  // fill seen before the last packet was written
  CHECK(cnt.out_fill_min == 0);
  CHECK(cnt.out_dropped_frames == 0);  // trimming the start-up surplus is not an overrun
}

TEST_CASE("usb return: underrun gives silence for the missing part, counts, and re-primes") {
  ReturnRing ring;
  Counters cnt;
  OutStream out(ring, cnt);
  ReturnReader ret(ring, cnt);
  alignas(4) std::int16_t l[kStemBlockFrames], r[kStemBlockFrames];
  std::uint32_t sent = 0;
  for (int i = 0; i < 6; ++i) {  // 264 frames: primes with 256 (the oldest 8 are discarded)
    send_frames(out, sent, 44);
    sent += 44;
  }
  CHECK(ret.pull(l, r, kStemBlockFrames));  // frames 8..135, 128 left
  CHECK(l[0] == s16(8));
  send_frames(out, sent, 20);
  sent += 20;                               // 148 queued
  CHECK(ret.pull(l, r, kStemBlockFrames));  // frames 136..263, 20 left
  CHECK(l[127] == s16(263));
  CHECK_FALSE(ret.pull(l, r, kStemBlockFrames));  // only 20 frames: delivered first, then silence
  CHECK(cnt.out_underruns == 1);
  CHECK(l[0] == s16(264));
  CHECK(l[19] == s16(283));
  CHECK(l[20] == 0);
  CHECK(l[kStemBlockFrames - 1] == 0);
  // Re-primed: silence until the ring is back to kReturnPrimeFrames.
  send_frames(out, sent, 44);
  sent += 44;
  CHECK_FALSE(ret.pull(l, r, kStemBlockFrames));
  CHECK(cnt.out_underruns == 1);
  CHECK(l[0] == 0);
}

TEST_CASE("usb return: overrun drops the oldest frames and counts them") {
  ReturnRing ring;
  Counters cnt;
  OutStream out(ring, cnt);
  ReturnReader ret(ring, cnt);
  alignas(4) std::int16_t l[kStemBlockFrames], r[kStemBlockFrames];
  std::uint32_t sent = 0;
  for (int i = 0; i < 24; ++i) {  // 24 x 45 = 1080 frames into a ring that holds 464
    send_frames(out, sent, 45);
    sent += 45;
  }
  CHECK(ret.pull(l, r, kStemBlockFrames));
  CHECK(cnt.out_dropped_frames == 1080 - 464);
  // The newest 464 survive; priming then keeps the newest 256 of those.
  CHECK(l[0] == s16(1080 - 256));
  CHECK(l[kStemBlockFrames - 1] == s16(1080 - 256 + 127));
  CHECK(cnt.out_underruns == 0);
}

TEST_CASE("usb return: a restart request flushes at the next pull and primes again") {
  ReturnRing ring;
  Counters cnt;
  OutStream out(ring, cnt);
  ReturnReader ret(ring, cnt);
  alignas(4) std::int16_t l[kStemBlockFrames], r[kStemBlockFrames];
  std::uint32_t sent = 0;
  for (int i = 0; i < 8; ++i) {
    send_frames(out, sent, 45);
    sent += 45;
  }
  CHECK(ret.pull(l, r, kStemBlockFrames));
  ret.request_restart();  // the host re-selected alt 1: everything queued is stale
  send_frames(out, sent, 45);
  sent += 45;
  CHECK_FALSE(ret.pull(l, r, kStemBlockFrames));  // flushed, priming again
  CHECK(l[0] == 0);
  CHECK(cnt.out_underruns == 0);
  for (int i = 0; i < 6; ++i) {
    send_frames(out, sent, 45);
    sent += 45;
  }
  CHECK(ret.pull(l, r, kStemBlockFrames));
  CHECK(l[0] == s16(sent - 256));  // primed on the newest 256 frames
}

TEST_CASE("usb return: malformed OUT packets are counted and cannot corrupt the ring") {
  ReturnRing ring;
  Counters cnt;
  OutStream out(ring, cnt);
  OutPacketBuffer p;
  for (std::uint32_t i = 0; i < kReturnMaxChunk; ++i) {
    p.frames[i] = i;
  }
  out.on_packet(p.frames, 7);  // one whole frame and a stray 3 bytes
  CHECK(cnt.out_bad_packets == 1);
  CHECK(out.fill() == 1);
  out.on_packet(p.frames, 0);  // an empty packet is legal (a host that has nothing to send)
  CHECK(cnt.out_bad_packets == 1);
  CHECK(out.fill() == 1);
  out.on_packet(p.frames, 4u * kReturnMaxChunk);
  CHECK(cnt.out_bad_packets == 1);
  CHECK(out.fill() == 1 + kReturnMaxChunk);
  out.on_packet(p.frames, 4u * kReturnMaxChunk + 40u);  // longer than the ring accepts: cut
  CHECK(cnt.out_bad_packets == 2);
  CHECK(out.fill() == 1 + 2 * kReturnMaxChunk);
  CHECK(cnt.out_packets == 4);
}

TEST_CASE("usb feedback trim: silent inside the dead zone, restoring and bounded outside it") {
  const std::uint32_t rate = nominal_frames_q16(Speed::Full);
  const auto settle = [&](FeedbackControl& fb, std::uint32_t fill, int packets) {
    std::uint32_t last = rate;
    for (int i = 0; i < packets; ++i) {
      last = fb.update(rate, fill);
    }
    return last;
  };
  // Not armed until the ring has reached the prime level: the rate passes through untouched.
  FeedbackControl fb;
  fb.reset();
  CHECK(fb.update(rate, 100) == rate);
  CHECK(fb.trim_q16() == 0);
  CHECK(fb.update(rate, 250) == rate);
  CHECK(fb.update(rate, kReturnPrimeFrames) == rate + static_cast<std::uint32_t>(fb.trim_q16()));
  CHECK(std::abs(fb.trim_q16()) <= 1);
  // Inside the dead zone nothing happens, however long it lasts.
  FeedbackControl inside;
  inside.reset();
  inside.update(rate, kReturnPrimeFrames);
  CHECK(settle(inside, kReturnTargetFrames + kReturnDeadFrames - 2, 8000) == rate);
  CHECK(inside.trim_q16() == 0);
  CHECK(settle(inside, kReturnTargetFrames - kReturnDeadFrames + 2, 8000) == rate);
  CHECK(inside.trim_q16() == 0);
  // Too full: advertise less, in proportion to how far beyond the zone (4 Q10.14 LSB = 16 Q16 units per frame).
  FeedbackControl full;
  full.reset();
  full.update(rate, kReturnPrimeFrames);
  const std::uint32_t less = settle(full, kReturnTargetFrames + kReturnDeadFrames + 10, 8000);
  CHECK(less < rate);
  CHECK(full.trim_q16() <= -150);
  CHECK(full.trim_q16() >= -170);  // 10 frames beyond the zone: about -160
  // Too empty: advertise more.
  FeedbackControl empty;
  empty.reset();
  empty.update(rate, kReturnPrimeFrames);
  CHECK(settle(empty, kReturnTargetFrames - kReturnDeadFrames - 10, 8000) > rate);
  CHECK(empty.trim_q16() >= 150);
  // Far beyond: clamped at +/-128 LSB (512 Q16.16 units).
  FeedbackControl clamp;
  clamp.reset();
  clamp.update(rate, kReturnPrimeFrames);
  settle(clamp, 460, 20000);
  CHECK(clamp.trim_q16() == -FeedbackControl::kTrimLimitQ16);
  CHECK(settle(clamp, 0, 20000) == rate + static_cast<std::uint32_t>(FeedbackControl::kTrimLimitQ16));
  // The trim comes back to zero when the fill does.
  settle(clamp, kReturnTargetFrames, 20000);
  CHECK(clamp.trim_q16() == 0);
}

// ---- closed loop, target sized ---------------------------------------------------------------------------------

TEST_CASE("usb sim lite: the stream path at both reference clocks, 40,000 SOFs each way") {
  // The real classes against a simulated host at +/-500 ppm with +/-150 us of jitter, for the clock the firmware
  // has today (4080 cycles per sample) and the one D-OS-8 would give (1800000 / 441). Small enough for the QEMU
  // run; usb_sim_test.cpp runs the same harness for millions of SOFs on the host.
  struct Rate {
    const char* name;
    CyclesPerSample ref;
  };
  const Rate rates[] = {{"4080/1", CyclesPerSample{4080u, 1u}}, {"1800000/441", CyclesPerSample{1800000u, 441u}}};
  for (const Rate& rate : rates) {
    for (std::int32_t ppm : {500, -500}) {
      SimParams p;
      p.ref = rate.ref;
      p.ppm = ppm;
      p.seed = 40u + static_cast<std::uint32_t>(ppm > 0);
      p.packets = 40000;
      p.settle = 8000;
      p.full_verify = 4000;
      check_run(p, run_sim(p), rate.name);
    }
  }
}
