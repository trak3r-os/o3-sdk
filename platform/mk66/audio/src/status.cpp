// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/status.h>

namespace o3sdk::platform::mk66::audio {
namespace {

// The seqlock's data accesses. On the target and the host they are relaxed, ordered by the generation counter and a fence on each
// side (cheap: a single core, the writer is an interrupt that preempts the reader). ThreadSanitizer does not model fences, so
// under it the same protocol runs with every access sequentially consistent, which it does model and which is correct too.
#if defined(__SANITIZE_THREAD__)
constexpr std::memory_order kData = std::memory_order_seq_cst;
inline void fence_release() noexcept {}
inline void fence_acquire() noexcept {}
#else
constexpr std::memory_order kData = std::memory_order_relaxed;
inline void fence_release() noexcept { std::atomic_thread_fence(std::memory_order_release); }
inline void fence_acquire() noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
#endif

}  // namespace

void AudioStatus::reset() noexcept {
  boundaries_.store(0, std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);
  consecutive_.store(0, std::memory_order_relaxed);
  max_consecutive_.store(0, std::memory_order_relaxed);
  mute_events_.store(0, std::memory_order_relaxed);
  muted_.store(0, std::memory_order_relaxed);
  slips_.store(0, std::memory_order_relaxed);
  hw_faults_.store(0, std::memory_order_relaxed);
  period_min_.store(0xFFFFFFFFu, std::memory_order_relaxed);
  period_max_.store(0, std::memory_order_relaxed);
  generation_.store(0, std::memory_order_relaxed);
  renders_.store(0, std::memory_order_relaxed);
  late_drops_.store(0, std::memory_order_relaxed);
  governor_over_.store(0, std::memory_order_relaxed);
  render_last_.store(0, std::memory_order_relaxed);
  render_max_.store(0, std::memory_order_relaxed);
  latency_last_.store(0, std::memory_order_relaxed);
  latency_max_.store(0, std::memory_order_relaxed);
  for (unsigned i = 0; i < kStageCount; ++i) {
    stage_last_[i].store(0, std::memory_order_relaxed);
    stage_max_[i].store(0, std::memory_order_relaxed);
  }
  input_peak_.store(0, std::memory_order_relaxed);
  fault_latched_.store(0, std::memory_order_relaxed);
}

void AudioStatus::reset_peaks() noexcept {
  // The fault counters are read and zeroed in one step each (status.h, THE FAULT LATCH): what they held is what the sticky LED must keep showing.
  const std::uint32_t faults = underruns_.exchange(0, std::memory_order_relaxed) | slips_.exchange(0, std::memory_order_relaxed) | hw_faults_.exchange(0, std::memory_order_relaxed) |
                               late_drops_.exchange(0, std::memory_order_relaxed);
  if (faults != 0u) {
    fault_latched_.store(1, std::memory_order_relaxed);
  }
  max_consecutive_.store(0, std::memory_order_relaxed);
  mute_events_.store(0, std::memory_order_relaxed);
  period_min_.store(0xFFFFFFFFu, std::memory_order_relaxed);
  period_max_.store(0, std::memory_order_relaxed);
  governor_over_.store(0, std::memory_order_relaxed);
  render_max_.store(0, std::memory_order_relaxed);
  latency_max_.store(0, std::memory_order_relaxed);
  for (unsigned i = 0; i < kStageCount; ++i) {
    stage_max_[i].store(0, std::memory_order_relaxed);
  }
}

void AudioStatus::isr_boundary(bool underrun, bool slipped, bool mute_started, bool muted, std::uint32_t consecutive,
                               std::uint32_t period, std::uint32_t hw_faults) noexcept {
  bump(boundaries_);
  if (underrun) {
    bump(underruns_);
  }
  consecutive_.store(consecutive, std::memory_order_relaxed);
  raise(max_consecutive_, consecutive);
  if (mute_started) {
    bump(mute_events_);
  }
  muted_.store(muted ? 1u : 0u, std::memory_order_relaxed);
  if (slipped) {
    bump(slips_);
  }
  if (hw_faults != 0u) {
    hw_faults_.store(hw_faults_.load(std::memory_order_relaxed) + hw_faults, std::memory_order_relaxed);
  }
  if (period != 0u) {
    if (period < period_min_.load(std::memory_order_relaxed)) {
      period_min_.store(period, std::memory_order_relaxed);
    }
    raise(period_max_, period);
  }
}

void AudioStatus::render_publish(const RenderSample& sample) noexcept {
  // Seqlock writer: odd while the group is inconsistent.
  const std::uint32_t g = generation_.load(std::memory_order_relaxed);
  generation_.store(g + 1u, kData);
  fence_release();

  bump(renders_, kData);
  if (sample.late) {
    bump(late_drops_, kData);
  }
  if (sample.over_budget) {
    bump(governor_over_, kData);
  }
  render_last_.store(sample.total, kData);
  raise(render_max_, sample.total, kData);
  latency_last_.store(sample.latency, kData);
  raise(latency_max_, sample.latency, kData);
  for (unsigned i = 0; i < kStageCount; ++i) {
    stage_last_[i].store(sample.stage[i], kData);
    raise(stage_max_[i], sample.stage[i], kData);
  }
  input_peak_.store(sample.input_peak, kData);

  generation_.store(g + 2u, std::memory_order_release);
}

StatusSnapshot AudioStatus::snapshot() const noexcept {
  StatusSnapshot s{};
  s.boundaries = boundaries_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  s.consecutive_underruns = consecutive_.load(std::memory_order_relaxed);
  s.max_consecutive_underruns = max_consecutive_.load(std::memory_order_relaxed);
  s.mute_events = mute_events_.load(std::memory_order_relaxed);
  s.muted = muted_.load(std::memory_order_relaxed);
  s.slot_slips = slips_.load(std::memory_order_relaxed);
  s.hw_faults = hw_faults_.load(std::memory_order_relaxed);
  const std::uint32_t pmin = period_min_.load(std::memory_order_relaxed);
  s.period_min = pmin == 0xFFFFFFFFu ? 0u : pmin;
  s.period_max = period_max_.load(std::memory_order_relaxed);

  // Seqlock reader: bounded retries. A snapshot is whole only if the generation read before and after the figures is the same
  // and even; if all tries fail (a writer that runs flat out) the figures are returned as read and the generation is made odd, so
  // a consumer can tell a torn snapshot from a whole one.
  constexpr unsigned kTries = 8;
  bool whole = false;
  for (unsigned attempt = 0; attempt < kTries && !whole; ++attempt) {
    const std::uint32_t g1 = generation_.load(std::memory_order_acquire);
    s.renders = renders_.load(kData);
    s.late_drops = late_drops_.load(kData);
    s.governor_over = governor_over_.load(kData);
    s.render_last = render_last_.load(kData);
    s.render_max = render_max_.load(kData);
    s.latency_last = latency_last_.load(kData);
    s.latency_max = latency_max_.load(kData);
    for (unsigned i = 0; i < kStageCount; ++i) {
      s.stage_last[i] = stage_last_[i].load(kData);
      s.stage_max[i] = stage_max_[i].load(kData);
    }
    s.input_peak = input_peak_.load(kData);
    fence_acquire();
    const std::uint32_t g2 = generation_.load(std::memory_order_relaxed);
    whole = g1 == g2 && (g1 & 1u) == 0u;
    s.generation = whole ? g1 : (g1 | 1u);
  }
  s.fault_seen = (fault_latched_.load(std::memory_order_relaxed) != 0u || s.underruns != 0u || s.late_drops != 0u || s.slot_slips != 0u || s.hw_faults != 0u) ? 1u : 0u;
  return s;
}

}  // namespace o3sdk::platform::mk66::audio
