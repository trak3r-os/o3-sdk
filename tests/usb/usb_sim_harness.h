// SPDX-License-Identifier: GPL-3.0-or-later
// The simulated host and device of the USB stream tests (OS-006a): the real RateEstimator, InStream, OutStream,
// ReturnReader and FeedbackControl driven by a host whose SOF clock is off by +/-500 ppm with +/-150 us of ISR
// jitter on every stamp, and an engine that delivers a stem block and pulls a return block every 128 frames.
// Header-only so that the full property test (usb_sim_test.cpp, host) and the target-sized run in
// usb_stream_test.cpp (also on QEMU) share it.
#pragma once

#include <doctest/doctest.h>

#include <o3sdk/usb/rate.h>
#include <o3sdk/usb/stream.h>

#include "usb_test_util.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace usb_sim {

using namespace o3sdk::usb;
using usb_test::Rng;

// One engine block of the device clock in Q16 CPU cycles: 128 frames x (num / den) cycles per frame.
inline std::uint64_t block_period_q16(CyclesPerSample ref) {
  return (static_cast<std::uint64_t>(kStemBlockFrames) * ref.num * 65536u + ref.den / 2u) / ref.den;
}

inline std::int16_t s16(std::uint32_t v) {
  const std::uint16_t u = static_cast<std::uint16_t>(v & 0xFFFFu);
  std::int16_t s;
  std::memcpy(&s, &u, 2);
  return s;
}
// The test pattern: frame g of channel c carries g + 1000 c (mod 2^16); the return pattern carries g (left) and
// g + 5000 (right).
inline std::int16_t stem_sample(std::uint32_t g, std::uint32_t c) { return s16(g + 1000u * c); }

inline double ppm_diff(double value, double truth) { return std::fabs(value - truth) / truth * 1e6; }

// ---- the simulated host and device ---------------------------------------------------------------------------

struct SimParams {
  Speed speed = Speed::Full;
  std::int32_t ppm = 0;            // host SOF clock vs the device clock: host period = nominal * (1 + ppm / 1e6)
  std::int32_t jitter = 27000;     // +/- CYCCNT cycles of ISR latency on every SOF stamp (27,000 = 150 us)
  std::uint64_t packets = 100000;  // SOFs simulated
  std::uint32_t settle = 8000;     // SOFs before the "after start-up" checks begin
  std::uint32_t channels = 10;
  std::uint32_t seed = 1;
  CyclesPerSample ref = kRefCyclesPerSample;  // the device's clock: CPU cycles per sample (the build's, or the other one)
  std::int32_t rate_bias_ppm = 0;     // error injected into the estimated rate (a wrong reference-clock constant)
  std::uint32_t full_verify = 30000;  // SOFs whose packets are verified sample by sample; later ones: first and last frame
};

struct SimResult {
  Counters counters;
  Counters at_settle;
  std::uint64_t packets = 0;
  std::uint32_t min_frames = 1000, max_frames = 0;       // IN packet sizes seen
  double worst_estimator_ppm = 0;                        // |estimated rate - true rate| after settle
  double worst_feedback_ppm = 0;                         // |advertised feedback - true| after settle (with trim)
  double worst_base_feedback_ppm = 0;                    // the same without the fill trim
  std::uint32_t continuity_errors = 0;                   // wrong sample values or unexplained jumps (IN and OUT)
  std::uint32_t out_blocks_checked = 0;
  std::uint32_t in_packets_checked = 0;
  std::uint32_t avg_fill_min = 0xFFFFFFFFu, avg_fill_max = 0;  // the IN guard's smoothed fill, after settle
  double mean_out_fill = 0;                              // over the last 2000 SOFs
  std::uint32_t out_fill_min = 0xFFFFFFFFu, out_fill_max = 0;  // after settle
  std::int32_t final_trim_q16 = 0;
  bool locked = false;
};

inline SimResult run_sim(const SimParams& p) {
  SimResult res;
  Counters& cnt = res.counters;
  StemRing stem_ring;
  ReturnRing return_ring;
  InStream in(stem_ring, cnt);
  OutStream out(return_ring, cnt);
  ReturnReader ret(return_ring, cnt);
  RateEstimator est(p.speed, frames_multiplier(p.ref));
  FeedbackControl fbctl;
  fbctl.reset();
  in.start(p.speed, p.channels);

  const std::uint32_t sof_cycles = nominal_sof_cycles(p.speed);
  // Host SOF period in Q16 device cycles. 1 ms of the host is sof_cycles * (1 + ppm / 1e6) device cycles.
  const std::uint64_t period_q16 =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(sof_cycles) * 65536.0 * (1.0 + p.ppm * 1e-6)));
  const double true_frames_per_sof =
      static_cast<double>(sof_cycles) * (1.0 + p.ppm * 1e-6) / (static_cast<double>(p.ref.num) / p.ref.den);
  const double truth_q16 = true_frames_per_sof * 65536.0;
  const double truth_native = p.speed == Speed::Full ? true_frames_per_sof * 16384.0 : truth_q16;

  Rng rng(p.seed);
  const std::uint64_t sof_phase = 40000 + rng.below(sof_cycles);
  const std::uint64_t block_period = block_period_q16(p.ref);
  const std::uint64_t block_phase = rng.below(static_cast<std::uint32_t>(block_period >> 16));
  const std::uint32_t cyc_base = 0xFFFF0000u;  // CYCCNT starts just below its wrap

  alignas(4) static std::int16_t pool[8][kMaxChannels][kStemBlockFrames];
  InPacketBuffer inbuf;
  OutPacketBuffer outbuf;
  alignas(4) std::int16_t left[kStemBlockFrames], right[kStemBlockFrames];

  std::uint64_t block = 0;
  std::uint64_t k = 0;
  std::uint32_t fb_last = p.speed == Speed::Full ? static_cast<std::uint32_t>(std::llround(44.1 * 16384.0))
                                                 : static_cast<std::uint32_t>(std::llround(44100.0 / 8000.0 * 65536.0));
  std::uint32_t host_acc = 0;
  std::uint32_t host_frame = 0;  // next return frame number the host will send

  // IN verification state.
  bool in_have_prev = false;
  std::uint32_t in_prev_end = 0;
  // OUT verification state.
  bool out_have_prev = false;
  std::uint32_t out_prev_end = 0;
  std::uint32_t out_prev_dropped = 0;
  double fill_sum = 0;
  std::uint32_t fill_n = 0;

  const std::uint32_t fb_shift = p.speed == Speed::Full ? 14u : 16u;

  while (k < p.packets) {
    const std::uint64_t t_sof = sof_phase + ((k * period_q16) >> 16);
    const std::int32_t jit = p.jitter == 0 ? 0 : rng.sym(p.jitter);
    const std::uint64_t t_isr = t_sof + static_cast<std::uint64_t>(static_cast<std::int64_t>(jit));
    const std::uint64_t t_block = block_phase + ((block * block_period) >> 16);

    if (t_block <= t_isr) {
      // ---- engine block: the stems tap writes block `block`, the return source pulls one ----
      const std::uint32_t slot = static_cast<std::uint32_t>(block % 8u);
      StemBlock blk{};
      const std::uint32_t g_first = static_cast<std::uint32_t>(block) * kStemBlockFrames;
      for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
        for (std::uint32_t j = 0; j < kStemBlockFrames; ++j) {
          pool[slot][c][j] = stem_sample(g_first + j, c);
        }
        blk.ch[c] = pool[slot][c];
      }
      stem_ring.write(&blk, 1);

      const std::uint32_t d0 = cnt.out_dropped_frames, u0 = cnt.out_underruns;
      const bool real = ret.pull(left, right, kStemBlockFrames);
      if (real) {
        ++res.out_blocks_checked;
        const std::uint32_t g0 = static_cast<std::uint16_t>(left[0]);
        const bool full = k < p.full_verify || (block & 31u) == 0u;
        bool ok = true;
        for (std::uint32_t i = 0; i < kStemBlockFrames; i = full ? i + 1u : kStemBlockFrames - 1u) {
          ok = ok && left[i] == s16(g0 + i) && right[i] == s16(g0 + i + 5000u);
          if (!full && i == kStemBlockFrames - 1u) {
            break;
          }
        }
        const bool lossless = cnt.out_dropped_frames == out_prev_dropped && cnt.out_dropped_frames == d0 && cnt.out_underruns == u0;
        if (out_have_prev && lossless) {
          ok = ok && g0 == (out_prev_end & 0xFFFFu);
        }
        res.continuity_errors += ok ? 0u : 1u;
        out_have_prev = true;
        out_prev_end = g0 + kStemBlockFrames;
        out_prev_dropped = cnt.out_dropped_frames;
      } else {
        out_have_prev = false;
      }
      ++block;
      continue;
    }

    // ---- SOF interrupt k ----
    cnt.note_sof();
    est.on_sof(cyc_base + static_cast<std::uint32_t>(t_isr), static_cast<std::uint32_t>(k) & (p.speed == Speed::Full ? 0x7FFu : 0x3FFFu));

    // The host's OUT packet for this frame, sized from the last feedback it read.
    host_acc += fb_last;
    const std::uint32_t n_out = host_acc >> fb_shift;
    host_acc &= (1u << fb_shift) - 1u;
    const std::uint32_t n_out_clamped = n_out < kReturnMaxChunk ? n_out : kReturnMaxChunk;
    for (std::uint32_t i = 0; i < n_out_clamped; ++i) {
      outbuf.frames[i] = (static_cast<std::uint32_t>(static_cast<std::uint16_t>(s16(host_frame + i))) |
                          (static_cast<std::uint32_t>(static_cast<std::uint16_t>(s16(host_frame + i + 5000u))) << 16));
    }
    host_frame += n_out_clamped;
    out.on_packet(outbuf.frames, n_out_clamped * 4u);

    // The IN packet.
    std::uint32_t rate = est.frames_per_sof_q16();
    rate = static_cast<std::uint32_t>(static_cast<std::int64_t>(rate) + static_cast<std::int64_t>(rate) * p.rate_bias_ppm / 1000000);
    const Counters before = cnt;
    const std::uint32_t n = in.build_packet(rate, inbuf.samples);
    res.min_frames = n < res.min_frames ? n : res.min_frames;
    res.max_frames = n > res.max_frames ? n : res.max_frames;
    if (cnt.in_prime_packets != before.in_prime_packets) {
      bool zero = true;
      for (std::uint32_t i = 0; i < n * p.channels; ++i) {
        zero = zero && inbuf.samples[i] == 0;
      }
      res.continuity_errors += zero ? 0u : 1u;
      in_have_prev = false;
    } else if (cnt.in_underruns != before.in_underruns || cnt.in_dropped_blocks != before.in_dropped_blocks) {
      in_have_prev = false;  // a counted loss: the stream jumps, nothing to verify
    } else {
      ++res.in_packets_checked;
      const std::uint32_t g0 = static_cast<std::uint16_t>(inbuf.samples[0]);
      const bool full = k < p.full_verify || (k & 63u) == 0u;
      bool ok = true;
      for (std::uint32_t f = 0; f < n; f = full ? f + 1u : (f == 0u ? n - 1u : n)) {
        for (std::uint32_t c = 0; c < p.channels; ++c) {
          ok = ok && inbuf.samples[f * p.channels + c] == stem_sample(g0 + f, c);
        }
      }
      if (in_have_prev) {
        ok = ok && g0 == (in_prev_end & 0xFFFFu);
      }
      res.continuity_errors += ok ? 0u : 1u;
      in_have_prev = true;
      in_prev_end = g0 + n;
    }

    // Feedback for the next host frame.
    const std::uint32_t fill = out.fill();
    const std::uint32_t adjusted = fbctl.update(rate, fill);
    fb_last = feedback_value(p.speed, adjusted);
    cnt.note_feedback(fb_last);

    if (k == p.settle) {
      res.at_settle = cnt;
      cnt.reset_fill_range();
      res.worst_estimator_ppm = res.worst_feedback_ppm = res.worst_base_feedback_ppm = 0;
    }
    if (k > p.settle) {
      res.worst_estimator_ppm = std::fmax(res.worst_estimator_ppm, ppm_diff(rate, truth_q16));
      res.worst_feedback_ppm = std::fmax(res.worst_feedback_ppm, ppm_diff(fb_last, truth_native));
      res.worst_base_feedback_ppm =
          std::fmax(res.worst_base_feedback_ppm, ppm_diff(feedback_value(p.speed, rate), truth_native));
      res.out_fill_min = fill < res.out_fill_min ? fill : res.out_fill_min;
      res.out_fill_max = fill > res.out_fill_max ? fill : res.out_fill_max;
      const std::uint32_t avg = in.smoothed_fill();
      res.avg_fill_min = avg < res.avg_fill_min ? avg : res.avg_fill_min;
      res.avg_fill_max = avg > res.avg_fill_max ? avg : res.avg_fill_max;
    }
    if (k + 2000 >= p.packets) {
      fill_sum += fill;
      ++fill_n;
    }
    ++k;
  }
  res.packets = k;
  res.mean_out_fill = fill_n != 0 ? fill_sum / fill_n : 0;
  res.final_trim_q16 = fbctl.trim_q16();
  res.locked = est.locked();
  return res;
}


// Stem blocks the engine hands to the stream: kBlocks blocks of planes, reused round robin.
struct Pool {
  alignas(4) std::int16_t s[8][kMaxChannels][kStemBlockFrames];
  void write(StemRing& ring, std::uint32_t block) {
    StemBlock blk{};
    for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
      for (std::uint32_t j = 0; j < kStemBlockFrames; ++j) {
        s[block % 8][c][j] = stem_sample(block * kStemBlockFrames + j, c);
      }
      blk.ch[c] = s[block % 8][c];
    }
    ring.write(&blk, 1);
  }
};


// The properties of a settled run (ADR 0012 S2: sizes always 44/45, no loss after start-up, estimates and
// feedback within 50 ppm, fills inside their bands).
inline void check_run(const SimParams& p, const SimResult& r, const char* name) {
  const Counters& c = r.counters;
  INFO(name << ": ppm " << p.ppm << " jitter " << p.jitter << " packets " << r.packets);
  // Whole run.
  CHECK(c.in_size_other == 0);
  CHECK(r.min_frames == frames_lo(p.speed));
  CHECK(r.max_frames == frames_hi(p.speed));
  CHECK(r.continuity_errors == 0);
  CHECK(c.out_bad_packets == 0);
  CHECK(r.in_packets_checked > r.packets / 2);
  CHECK(r.out_blocks_checked > r.packets / 4);
  // After start-up: no loss of any kind, fill in band, estimates within 50 ppm.
  CHECK(c.in_underruns == r.at_settle.in_underruns);
  CHECK(c.in_dropped_blocks == r.at_settle.in_dropped_blocks);
  CHECK(c.out_underruns == r.at_settle.out_underruns);
  CHECK(c.out_dropped_frames == r.at_settle.out_dropped_frames);
  CHECK(r.avg_fill_min >= kStemFillBand.low);   // the smoothed fill never left the band
  CHECK(r.avg_fill_max <= kStemFillBand.high);
  CHECK(c.in_fill_min >= 45);                    // and the sawtooth never got near an underrun
  CHECK(c.in_fill_max < 4 * kStemBlockFrames);
  CHECK(r.worst_estimator_ppm < 50.0);
  CHECK(r.worst_base_feedback_ppm < 50.0);
  CHECK(r.worst_feedback_ppm < 50.0);
  CHECK(r.out_fill_min >= 100);
  CHECK(r.out_fill_max <= ReturnRing::capacity() - 100u);
  CHECK(std::fabs(r.mean_out_fill - kReturnTargetFrames) < kReturnDeadFrames + 8.0);
  CHECK(r.locked);
#if !defined(__arm__)
  std::printf("[usb-sim] %-14s ppm %+5d jit %5d packets %8llu | est %5.1f ppm  fb(base) %5.1f  fb(total) %5.1f | IN fill %u..%u (smoothed %u..%u)  OUT fill %u..%u mean %.0f trim %d | "
              "sizes %u..%u  lost since start: in %u/%u out %u/%u\n",
              name, p.ppm, p.jitter, static_cast<unsigned long long>(r.packets), r.worst_estimator_ppm, r.worst_base_feedback_ppm,
              r.worst_feedback_ppm, c.in_fill_min, c.in_fill_max, r.avg_fill_min, r.avg_fill_max, r.out_fill_min, r.out_fill_max, r.mean_out_fill,
              r.final_trim_q16, r.min_frames, r.max_frames, c.in_underruns, c.in_dropped_blocks, c.out_underruns, c.out_dropped_frames);
#endif
}

}  // namespace usb_sim
