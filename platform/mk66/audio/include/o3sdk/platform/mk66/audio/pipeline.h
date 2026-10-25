// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pipeline: the two entry points the platform's interrupts call, around the slot protocol, the block callback and the
// status (ADR 0013 D1, option B). The platform (src/audio.cpp) owns the hardware and calls
//
//   on_boundary()  from the DMA slot-boundary ISR (priority kAudioDma): number the boundary, detect an underrun, publish the
//                  counters; the caller then pends the render line. Bounded, touches audio data only to zero a stale slot after
//                  the underrun mute (256 bytes, 64 word stores, about 1.1 us [est], only on that path). It stays in the ISR on
//                  purpose: when the mute matters the render is the one that is late or stalled, so a zeroing done by the render
//                  would wait for exactly the context that has stopped delivering.
//   render()       from the render line (priority kRender): copy the newest complete RX block, call the block callback,
//                  commit the result to the TX slot if it is still on time, publish the timing.
//
// Slots. TX is `hal::audio::kTxPhysSlots` slots of one block each, RX `kRxPhysSlots`, both interleaved stereo int16 and
// declared aligned by the platform that owns them (ADR 0011 lesson 1; the DMA's address modulo needs the ring aligned to its
// own size). The render never writes the TX slot that is playing (slot_ring.h) and never reads an RX slot the DMA is
// writing: it reads the slot behind the one being written, which the DMA does not reach again for two more boundaries, and
// it copies it first thing into a private block. The callback sees that private input copy and fills a private output block,
// which the pipeline then copies into the TX slot: the callback never touches a DMA ring.
//
// Load governor stub: a render above 60 % of the block period is counted (`governor_over`). Nothing in the pipeline acts on that
// count; the starvation guard below is what keeps the main loop alive under an overload.
//
// Main-loop starvation guard (OS-MAINGUARD, ADR 0013 amendment). The render line is above thread mode and serves the NEWEST unrendered
// boundary, so a render of about 100 % of the block (plus the ISRs and a spike) leaves its pend latched: the next render starts the moment the
// last one returns and the main loop (display, input, LEDs) never runs. With kTxSlots 3 nothing underruns, so the underrun mute that
// used to shed load by accident never latches. The guard puts a floor under the main loop:
//   * the main loop calls main_pass() once per pass (a beat: one word, single writer = the main loop);
//   * a render over the governor budget (60 %) while the beat has not moved is "stale"; N = starve_blocks() (default kStarveBlocks) stale renders in a row, and the render
//     SHEDS: it publishes a faded silent block without calling the callback, block after block, until the beat moves (a whole main pass
//     has run); then N more renders. Any render under 60 % ends the count (the main loop had the rest of that block), so a light
//     load never sheds, however long a main-loop pass or the 0.2 s unmute takes.
//   * the shed block is a fade out from the last frame published (the first of a run) or zeros, the first block after a run ramps in: no step.
// What a shed hides, and where it shows: it is not a render (`renders`, `governor_over` and the stage figures do not see it); a shed whose slot
// is already playing (late) is not published and the ISR has counted it as an underrun; the callback does not run, so the engine and its
// sequencers skip the block (the song holds for 1.45 ms). The counters are shed_blocks() / shed_late() / shed_runs() / shed_longest(): written
// by the render only, never by the main loop (reset_peaks() hands the render a request count; the render zeroes the counters at its next block).
// Off unless Config::starve_guard or arm_guard() (the MK66 main loop arms it; the tests that do not play a main loop leave it off).

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/platform/mk66/audio/slot_ring.h>
#include <o3sdk/platform/mk66/audio/status.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::audio {

// The cycle counter the pipeline times the render with (hal::cycles on the MK66; a fake in the tests). Called through this pointer
// from SRAM_L: an indirect call, which the "no call into flash" rule allows.
using CycleFn = std::uint32_t (*)() noexcept;

inline constexpr std::uint32_t kGovernorPercent = 60;
// OS-MAINGUARD: over-budget renders in a row, with no main-loop pass in between, before the render starts to shed (64 blocks = about 93 ms without a pass; the main
// loop floor is about 1.5 % of the CPU). The DEFAULT of Pipeline::starve_blocks(): `guard n <1..255>` sets it at run time (OS-MAINGUARD tuning), the render reads
// it once per block. 64 from the fqa.9 unit sweep: Dense8 sheds nothing from N 32 up, Dense10 and Heavy12 still shed and stay controllable.
inline constexpr std::uint32_t kStarveBlocks = 64;
inline constexpr std::uint32_t kStarveBlocksMax = 255;  // stale_ is a byte

class Pipeline {
 public:
  using Ring = SlotRing<hal::audio::kTxPhysSlots, hal::audio::kTxSlots>;

  struct Config {
    std::int16_t* tx;                    // kTxPhysSlots blocks, 4-byte aligned
    const std::int16_t* rx;              // kRxPhysSlots blocks, 4-byte aligned
    hal::audio::BlockCallback render;
    CycleFn cycles;
    std::uint32_t cycles_per_block;      // the platform's block period in the units of `cycles`
    bool starve_guard;                   // OS-MAINGUARD: shed blocks while the main loop starves (needs main_pass() from that loop); false = off
  };

  // Main thread, before the interrupts are enabled: also resets the ring and the status (a restart, or the next test).
  void init(const Config& config) noexcept;

  // ISR context (priority kAudioDma). `tx_slot`: the TX slot the DMA is playing now; `rx_slot`: the RX slot it is writing now
  // (both 0..P-1, from the DMA's source / destination address); `stamp`: the cycle counter read as the first thing the ISR
  // did; `hw_faults`: how many error flags the platform found and cleared (I2S FIFO, sync, eDMA).
  O3SDK_AUDIO_HOT void on_boundary(std::uint32_t tx_slot, std::uint32_t rx_slot, std::uint32_t stamp, std::uint32_t hw_faults) noexcept;

  // Render context (priority kRender).
  O3SDK_AUDIO_HOT void render() noexcept;
  // Render context: one block of a shed run for the boundary of `ticket` (render() calls it; public so a test can hand it a ticket whose slot has started to play).
  O3SDK_AUDIO_HOT void shed(const Ring::Ticket& ticket) noexcept;

  // The cycle counter at the slot boundary that started the render now running (the ticket's stamp), for a callback that needs the block's time base (in
  // track3r-os the MIDI timing core, OS-005c2: the block's first sample is heard (kTxSlots - 1) blocks after it). Render context only, valid inside the block callback.
  std::uint32_t render_stamp() const noexcept { return render_stamp_; }

  // OS-MAINGUARD. Main loop, once per pass: the beat the render reads. Single writer (the main loop): a plain read-modify-write of its own word,
  // no other context stores to it (init() runs in the main thread too). 16 bits, wraps: the render only compares for equality, and 65,536 passes
  // between two renders (1.45 ms) cannot happen.
  void main_pass() noexcept { main_beat_.store(static_cast<std::uint16_t>(main_beat_.load(std::memory_order_relaxed) + 1u), std::memory_order_relaxed); }
  // Main thread: arms the guard on a running pipeline (init() takes Config::starve_guard). Call it before the first main_pass() of the loop.
  void arm_guard() noexcept { guard_.store(true, std::memory_order_relaxed); }
  bool guard_armed() const noexcept { return guard_.load(std::memory_order_relaxed); }
  // The starvation window N: stale renders in a row before the shed. One byte, single writer (the main loop, `guard n`), read once per block by the render with a relaxed load: a new value
  // takes effect at the next block. set_starve_blocks() clamps to 1..kStarveBlocksMax. init() leaves it alone (a restart keeps the tuning, like arm_guard()); a new Pipeline starts at kStarveBlocks.
  std::uint32_t starve_blocks() const noexcept { return starve_n_.load(std::memory_order_relaxed); }
  void set_starve_blocks(std::uint32_t n) noexcept {
    starve_n_.store(static_cast<std::uint8_t>(n < 1u ? 1u : (n > kStarveBlocksMax ? kStarveBlocksMax : n)), std::memory_order_relaxed);
  }
  // The guard's counters (render writes, any context reads; the main loop never writes them). A late shed is counted in both shed_blocks() and shed_late().
  // shed_blocks, shed_late and shed_runs count since boot or the last reset_peaks() (shed_late and shed_runs wrap at 65,536); shed_longest is since boot,
  // in blocks, and stops at 255.
  std::uint32_t shed_blocks() const noexcept { return shed_blocks_.load(std::memory_order_relaxed); }
  std::uint32_t shed_late() const noexcept { return shed_late_.load(std::memory_order_relaxed); }
  std::uint32_t shed_runs() const noexcept { return shed_runs_.load(std::memory_order_relaxed); }
  std::uint32_t shed_longest() const noexcept { return shed_longest_.load(std::memory_order_relaxed); }
  // Tests only: starts the beat near the wrap.
  void seed_beat_for_test(std::uint16_t v) noexcept { main_beat_.store(v, std::memory_order_relaxed); }

  const AudioStatus& status() const noexcept { return status_; }
  // OS-PERF3: `diag cpu reset`, main loop (status.h); also forgets the worst render's record. The shed counters are zeroed by the RENDER (their one
  // writer): this only moves the request count (single writer: the main loop), which the render compares with its own copy at its next block.
  void reset_peaks() noexcept {
    status_.reset_peaks();
    worst_total_.store(0, std::memory_order_relaxed);
    reset_req_.store(static_cast<std::uint8_t>(reset_req_.load(std::memory_order_relaxed) + 1u), std::memory_order_relaxed);
  }
  // OS-PERF3, `diag bench worst`: the stage figures of the SLOWEST render since the last reset (the maxima in `diag cpu` are of different renders; this is one block's whole cost split by
  // stage, which says what the worst case is made of). Written by the render, one word at a time, the total last; the main loop reads the total, the stages and the total again and tries
  // again (a few times) if the total moved, so a record being replaced is not shown half old, half new unless it keeps changing.
  struct WorstRender {
    std::uint32_t total;
    std::uint32_t stage[hal::audio::kStageCount];
  };
  WorstRender worst_render() const noexcept {
    WorstRender w{};
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
      w.total = worst_total_.load(std::memory_order_relaxed);
      for (unsigned i = 0; i < hal::audio::kStageCount; ++i) {
        w.stage[i] = worst_stage_[i].load(std::memory_order_relaxed);
      }
      if (worst_total_.load(std::memory_order_relaxed) == w.total) {
        break;
      }
    }
    return w;
  }
  const Ring& ring() const noexcept { return ring_; }
  std::uint32_t budget_cycles() const noexcept { return budget_; }

 private:
  Ring ring_;
  AudioStatus status_;
  std::int16_t* tx_ = nullptr;
  const std::int16_t* rx_ = nullptr;
  hal::audio::BlockCallback render_{nullptr, nullptr};
  CycleFn cycles_ = nullptr;
  std::uint32_t budget_ = 0;

  // ISR only
  std::uint32_t last_stamp_ = 0;
  bool have_last_ = false;
  // OS-MAINGUARD flags, here because they fill the padding behind have_last_ (packed by hand in track3r-os, whose SRAM_U had a 24 B margin): bytes, each with one writer.
  std::atomic<bool> guard_{false};  // main thread writes (init(), arm_guard()), the render reads it once per block
  bool shedding_ = false;           // render only: a shed run is going on (until the beat moves)
  bool ramp_in_ = false;            // render only: the next real block follows a shed run: it ramps 0 -> 1
  std::uint32_t consecutive_ = 0;
  // ISR writes, render reads: the RX slot being written at the latest boundary
  std::atomic<std::uint32_t> rx_slot_{0};

  // render only: the stamp of the boundary this render serves (render_stamp())
  std::uint32_t render_stamp_ = 0;

  // render only: the private blocks (the callback sees a stable input and fills an output nobody else touches)
  alignas(8) std::int16_t in_[hal::audio::kBlockSamples];
  alignas(8) std::int16_t out_[hal::audio::kBlockSamples];
  hal::audio::BlockReport report_;

  // OS-MAINGUARD, 18 bytes in all (packed by hand, as above). Render writes the counters, any context reads (single writer: the render).
  std::atomic<std::uint32_t> shed_blocks_{0};
  std::atomic<std::uint16_t> main_beat_{0};  // main loop writes (main_pass(), the only writer), the render reads
  std::uint16_t seen_beat_ = 0;              // render only: the beat at the last render
  std::atomic<std::uint16_t> shed_late_{0};
  std::atomic<std::uint16_t> shed_runs_{0};
  std::uint8_t stale_ = 0;                   // render only: over-budget renders in a row since the beat last moved
  std::uint8_t run_len_ = 0;                 // render only: blocks shed in the current run (stops at 255)
  std::atomic<std::uint8_t> shed_longest_{0};
  std::atomic<std::uint8_t> reset_req_{0};   // main loop writes (reset_peaks(), the only writer), the render reads
  std::uint8_t reset_seen_ = 0;              // render only: reset_req_ as the render last saw it
  // N (starve_blocks()): a byte in the padding before worst_total_, so the Pipeline does not grow (a u32 would cost 8 B). The main loop writes (set_starve_blocks(), the only
  // writer), the render reads it once per block; a byte load/store is one instruction on the M4, atomic by itself.
  std::atomic<std::uint8_t> starve_n_{static_cast<std::uint8_t>(kStarveBlocks)};

  // render writes (worst_render()), any context reads
  std::atomic<std::uint32_t> worst_total_{0};
  std::atomic<std::uint32_t> worst_stage_[hal::audio::kStageCount] = {};
};

}  // namespace o3sdk::platform::mk66::audio
