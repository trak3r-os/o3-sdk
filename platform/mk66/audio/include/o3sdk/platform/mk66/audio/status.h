// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AudioStatus (ADR 0013 D6, hazards H9/H10): what the audio path publishes about itself, for the main loop (the output gate, the
// LED verdict, a CPU figure) and, later, a console. Every field has exactly ONE writing context and is a std::atomic word, so there
// is no ad-hoc flag and no lock:
//
//   ISR group     written only by the DMA slot-boundary ISR (priority kAudioDma): boundary, underrun and mute counters, the
//                 period between boundaries. Plain relaxed words; each counter is monotonic, so a reader may see them
//                 from slightly different instants and nothing breaks.
//   render group  written only by the render (priority kRender): cycles per stage, jitter, the governor, the input peak.
//                 These are read together (a stage figure next to its total), so they sit behind a generation counter, a
//                 seqlock: the writer makes it odd, writes, makes it even; a reader retries (bounded) if it saw an odd value
//                 or a change. The writer never waits for a reader.
//   readers       any lower-priority context (the main loop) through snapshot().
//
// Nothing here is a cycle claim on the host: the counters are exact, the cycle figures are whatever the platform's counter says
// (DWT on the MK66).

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::audio {

inline constexpr unsigned kStageCount = hal::audio::kStageCount;

// A plain copy of every figure, as the formatter and the tests see it.
struct StatusSnapshot {
  // ISR group
  std::uint32_t boundaries;
  std::uint32_t underruns;
  std::uint32_t consecutive_underruns;      // the current run
  std::uint32_t max_consecutive_underruns;
  std::uint32_t mute_events;                // times three underruns in a row muted the output
  std::uint32_t muted;                      // 1 while the underrun mute is active
  std::uint32_t slot_slips;                 // boundaries the ISR missed (the slot count and the DMA disagreed)
  std::uint32_t hw_faults;                  // I2S FIFO / sync errors and eDMA errors seen at a boundary
  std::uint32_t period_min;                 // cycles between two boundaries (0 until two have happened)
  std::uint32_t period_max;
  // render group
  std::uint32_t renders;
  std::uint32_t late_drops;                 // blocks finished after their slot started to play
  std::uint32_t governor_over;              // renders above the governor's budget (60 % of the block)
  std::uint32_t render_last;                // cycles, boundary-stamp-relative work: the whole render including the commit
  std::uint32_t render_max;
  std::uint32_t latency_last;               // cycles from the boundary stamp to the first instruction of the render
  std::uint32_t latency_max;
  std::uint32_t stage_last[kStageCount];
  std::uint32_t stage_max[kStageCount];
  std::uint32_t input_peak;
  std::uint32_t generation;                 // render group generation: even = the render figures are one whole publish, odd = torn
  // 1: an underrun, a late drop, a slot slip or a hardware fault has been seen since boot (the counters above now, or the latch `diag cpu reset` keeps), else 0. The fault LED goes by it.
  std::uint32_t fault_seen;
};

// One render's figures, published by the render context.
struct RenderSample {
  std::uint32_t total;
  std::uint32_t latency;
  std::uint32_t stage[kStageCount];
  std::uint32_t input_peak;
  bool late;
  bool over_budget;
};

class AudioStatus {
 public:
  // Every figure back to zero. Only while neither writer can run (before start, or between tests).
  void reset() noexcept;

  // OS-PERF3 (`diag cpu reset` in track3r-os): the per-run figures back to zero while the audio runs, from the main loop: the maxima (render, start-latency, every stage, the period range), the
  // over-budget and late counts and the fault counters (underruns, the longest run, mute events, slips, hardware faults). The totals (boundaries, renders) and the live state (the
  // current run, the mute) are left. EXCEPTION to "one writing context a field": every store is a single 32-bit store of 0 (the period minimum: all ones), and the writers' own
  // updates are read-modify-writes done wholly inside their interrupt, which the main loop cannot interrupt, so a store lands before or after an update, never inside one; the
  // worst case is an update lost to the reset, which is its meaning. No seqlock needed: the render group's reader is the same main loop.
  // THE FAULT LATCH: the four fault counters (underruns, late drops, slot slips, hardware faults) are what the sticky fault LED goes by, and the reset must not turn it off. They are taken
  // with an exchange (LDREX/STREX: an interrupt that lands between the two clears the monitor and the exchange retries, so no fault falls between the read and the zero), and if any was
  // non-zero `fault_latched_` is set. That word has ONE writer, this function (and reset(), before the writers run); snapshot().fault_seen reads it.
  void reset_peaks() noexcept;

  // ---- ISR context: the one writer of the ISR group ------------------------------------------------------------------
  // `period` is the cycles since the previous boundary (0 for the first); `consecutive` the length of the current underrun
  // run; `hw_faults` the number of error flags the platform saw at this boundary.
  O3SDK_AUDIO_HOT void isr_boundary(bool underrun, bool slipped, bool mute_started, bool muted, std::uint32_t consecutive,
                    std::uint32_t period, std::uint32_t hw_faults) noexcept;

  // ---- Render context: the one writer of the render group ------------------------------------------------------------
  O3SDK_AUDIO_HOT void render_publish(const RenderSample& sample) noexcept;

  // ---- Any context -------------------------------------------------------------------------------------------------------
  StatusSnapshot snapshot() const noexcept;

 private:
  // Folded into the writers above (which run from SRAM_L): a call out of the hot path would need a long-call veneer. `order` is
  // relaxed for the ISR group; the render group passes the seqlock's data order (status.cpp).
  O3SDK_AUDIO_INLINE static void bump(std::atomic<std::uint32_t>& a, std::memory_order order = std::memory_order_relaxed) noexcept {
    a.store(a.load(std::memory_order_relaxed) + 1u, order);
  }
  O3SDK_AUDIO_INLINE static void raise(std::atomic<std::uint32_t>& a, std::uint32_t v, std::memory_order order = std::memory_order_relaxed) noexcept {
    if (v > a.load(std::memory_order_relaxed)) {
      a.store(v, order);
    }
  }

  // ISR group
  std::atomic<std::uint32_t> boundaries_{0};
  std::atomic<std::uint32_t> underruns_{0};
  std::atomic<std::uint32_t> consecutive_{0};
  std::atomic<std::uint32_t> max_consecutive_{0};
  std::atomic<std::uint32_t> mute_events_{0};
  std::atomic<std::uint32_t> muted_{0};
  std::atomic<std::uint32_t> slips_{0};
  std::atomic<std::uint32_t> hw_faults_{0};
  std::atomic<std::uint32_t> period_min_{0xFFFFFFFFu};
  std::atomic<std::uint32_t> period_max_{0};
  // render group, behind generation_ (odd while being written)
  std::atomic<std::uint32_t> generation_{0};
  std::atomic<std::uint32_t> renders_{0};
  std::atomic<std::uint32_t> late_drops_{0};
  std::atomic<std::uint32_t> governor_over_{0};
  std::atomic<std::uint32_t> render_last_{0};
  std::atomic<std::uint32_t> render_max_{0};
  std::atomic<std::uint32_t> latency_last_{0};
  std::atomic<std::uint32_t> latency_max_{0};
  std::atomic<std::uint32_t> stage_last_[kStageCount] = {};
  std::atomic<std::uint32_t> stage_max_[kStageCount] = {};
  std::atomic<std::uint32_t> input_peak_{0};
  // main loop (reset_peaks): a fault counter was non-zero when `diag cpu reset` zeroed it. Never cleared except by reset().
  std::atomic<std::uint32_t> fault_latched_{0};
};

// The layout is how the figures are read before a console exists (track3r-os platform/mk66/README.md, "Reading the status"): a probe reads the object at
// `o3sdk_audio_status_ptr` as 38 consecutive 32-bit words in the order of the members above (ISR group: boundaries, underruns,
// consecutive run, max run, mute events, muted, slips, hw faults, period min, period max; render group: generation, renders, late
// drops, governor over, render last, render max, latency last, latency max, 9 stage-last, 9 stage-max, input peak; then the fault latch; the stages are hal::audio kStage* in
// order: input, drain, render, strips, buses, master, convert, taps, commit). Adding or removing a member changes this assertion
// and that description together.
static_assert(sizeof(AudioStatus) == 38u * sizeof(std::uint32_t), "AudioStatus is 38 32-bit words: update the layout described above");

}  // namespace o3sdk::platform::mk66::audio
