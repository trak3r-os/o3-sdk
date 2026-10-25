// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/pipeline.h>

#include <cstring>

namespace o3sdk::platform::mk66::audio {
namespace {

constexpr std::uint32_t kBlockSamples = hal::audio::kBlockSamples;

// Copies one block. Explicit loops on halfwords: the compiler turns them into word moves, and nothing here calls memcpy
// (a call from SRAM_L into flash needs a long-call veneer; the hot path keeps its calls inside itself).
O3SDK_AUDIO_INLINE void copy_block(std::int16_t* dst, const std::int16_t* src) noexcept {  // always inline: three call sites made the compiler outline it into flash (OS-MAINGUARD)
  for (std::uint32_t i = 0; i < kBlockSamples; ++i) {
    dst[i] = src[i];
  }
}

// Field by field: an aggregate `= {}` of this size is a memset call, and the hot path calls nothing in flash (see
// tools/check_ramfunc.py).
O3SDK_AUDIO_INLINE void clear_report(hal::audio::BlockReport& r) noexcept {  // always inline: render() is in SRAM_L
  for (unsigned i = 0; i < hal::audio::kStageCount; ++i) {
    r.stage_cycles[i] = 0;
  }
  r.input_peak = 0;
}

// The ISR's one data access (the stale slot after an underrun mute): 64 word stores, the slot being 4-byte aligned by the ring's
// declaration. About 190 cycles, 1.1 us at 180 MHz [est: a str, an add, a compare and a branch per word], on the underrun-while-muted
// path only, on top of the ~200 cycles of the rest of on_boundary: the ISR stays under 3 us in the worst case. A four-byte memcpy is an
// inline store, not a call.
O3SDK_AUDIO_INLINE void zero_block(std::int16_t* dst) noexcept {  // always inline: two call sites (the ISR and shed), never outlined into flash
  constexpr std::uint32_t zero = 0;
  for (std::uint32_t i = 0; i < kBlockSamples; i += 2u) {
    std::memcpy(dst + i, &zero, sizeof zero);
  }
}

// OS-MAINGUARD: a block's frames ramp in 0 -> 1 (frame i of kBlock gets (i + 1) / kBlock), the first real block after a shed run. 128 multiplies,
// about 600 cycles [est], once per run.
O3SDK_AUDIO_INLINE void ramp_in_block(std::int16_t* b) noexcept {
  static_assert(hal::audio::kBlock == 64u, "the ramps divide by kBlock");
  for (std::uint32_t i = 0; i < hal::audio::kBlock; ++i) {
    const std::int32_t g = static_cast<std::int32_t>(i + 1u);
    b[2u * i] = static_cast<std::int16_t>((static_cast<std::int32_t>(b[2u * i]) * g) / 64);
    b[2u * i + 1u] = static_cast<std::int16_t>((static_cast<std::int32_t>(b[2u * i + 1u]) * g) / 64);
  }
}

// The first shed block of a run: each channel fades from the last frame of the block before (what the DAC played last) to zero over the block.
O3SDK_AUDIO_INLINE void fade_out_block(std::int16_t* b) noexcept {
  const std::int32_t last_l = b[kBlockSamples - 2u];
  const std::int32_t last_r = b[kBlockSamples - 1u];
  for (std::uint32_t i = 0; i < hal::audio::kBlock; ++i) {
    const std::int32_t g = static_cast<std::int32_t>(hal::audio::kBlock - 1u - i);
    b[2u * i] = static_cast<std::int16_t>((last_l * g) / 64);
    b[2u * i + 1u] = static_cast<std::int16_t>((last_r * g) / 64);
  }
}

// A single-writer counter: the render is its only writer, so a load and a store do.
template <typename T>
O3SDK_AUDIO_INLINE void bump(std::atomic<T>& a) noexcept {
  a.store(static_cast<T>(a.load(std::memory_order_relaxed) + 1u), std::memory_order_relaxed);
}

}  // namespace

void Pipeline::init(const Config& config) noexcept {
  tx_ = config.tx;
  rx_ = config.rx;
  render_ = config.render;
  cycles_ = config.cycles;
  // cycles_per_block * kGovernorPercent / 100 in 32 bits, exactly: with c = 100 q + r, c * P / 100 = q * P + r * P / 100 (floor), and q * P
  // fits for every c as P <= 100. The 64-bit form pulled the library's 800-byte 64-bit division helper into the image for one start-up line.
  static_assert(kGovernorPercent <= 100u, "the governor budget is a percentage of the block: it cannot overflow 32 bits");
  budget_ = (config.cycles_per_block / 100u) * kGovernorPercent + ((config.cycles_per_block % 100u) * kGovernorPercent) / 100u;
  ring_.reset();
  status_.reset();
  last_stamp_ = 0;
  have_last_ = false;
  consecutive_ = 0;
  rx_slot_.store(0, std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < kBlockSamples; ++i) {
    in_[i] = 0;
    out_[i] = 0;
  }
  clear_report(report_);
  guard_.store(config.starve_guard, std::memory_order_relaxed);
  shedding_ = false;
  ramp_in_ = false;
  seen_beat_ = 0;
  stale_ = 0;
  run_len_ = 0;
  main_beat_.store(0, std::memory_order_relaxed);
  reset_req_.store(0, std::memory_order_relaxed);
  reset_seen_ = 0;
  shed_blocks_.store(0, std::memory_order_relaxed);
  shed_late_.store(0, std::memory_order_relaxed);
  shed_runs_.store(0, std::memory_order_relaxed);
  shed_longest_.store(0, std::memory_order_relaxed);
}

void Pipeline::on_boundary(std::uint32_t tx_slot, std::uint32_t rx_slot, std::uint32_t stamp, std::uint32_t hw_faults) noexcept {
  const Ring::Boundary b = ring_.on_boundary(tx_slot, stamp);
  if (b.zero_slot) {
    zero_block(tx_ + b.slot * kBlockSamples);  // the ISR's one data access: a stale slot must not loop as a buzz
  }
  consecutive_ = b.underrun ? consecutive_ + 1u : 0u;
  const std::uint32_t period = have_last_ ? stamp - last_stamp_ : 0u;
  last_stamp_ = stamp;
  have_last_ = true;
  rx_slot_.store(rx_slot & (hal::audio::kRxPhysSlots - 1u), std::memory_order_relaxed);
  status_.isr_boundary(b.underrun, b.slipped, b.mute_started, ring_.muted(), consecutive_, period, hw_faults);
}

// OS-MAINGUARD: one block of a shed run: no callback, a faded or silent block published to the slot if that slot has not started to play.
void Pipeline::shed(const Ring::Ticket& ticket) noexcept {
  if (!shedding_) {
    shedding_ = true;
    run_len_ = 0;
    bump(shed_runs_);
    fade_out_block(out_);
  } else {
    zero_block(out_);
  }
  bump(shed_blocks_);
  if (run_len_ != 255u) {
    ++run_len_;
  }
  if (run_len_ > shed_longest_.load(std::memory_order_relaxed)) {
    shed_longest_.store(run_len_, std::memory_order_relaxed);
  }
  if (ring_.on_time(ticket)) {
    copy_block(tx_ + ticket.write_slot * kBlockSamples, out_);
    ring_.published(ticket);
  } else {
    bump(shed_late_);  // the slot already plays its old content: the ISR counted the underrun
  }
}

void Pipeline::render() noexcept {
  Ring::Ticket ticket;
  if (!ring_.begin_render(ticket)) {
    return;  // nothing new: a pend that arrived while the previous render was finishing the same boundary
  }
  const bool guard = guard_.load(std::memory_order_relaxed);
  if (guard) {
    const std::uint8_t req = reset_req_.load(std::memory_order_relaxed);
    if (req != reset_seen_) {  // `diag cpu reset`: the counters start again from this block (the render is their one writer)
      reset_seen_ = req;
      shed_blocks_.store(0, std::memory_order_relaxed);
      shed_late_.store(0, std::memory_order_relaxed);
      shed_runs_.store(0, std::memory_order_relaxed);
    }
    const std::uint16_t beat = main_beat_.load(std::memory_order_relaxed);
    if (beat != seen_beat_) {  // the main loop completed a pass since the last render: the starvation count starts over
      seen_beat_ = beat;
      stale_ = 0;
    }
    if (stale_ >= starve_n_.load(std::memory_order_relaxed)) {  // N is read once per block: `guard n` takes effect at the next one
      shed(ticket);
      return;
    }
    if (shedding_) {  // a shed run ends: the beat moved (count 0), or `guard n` raised N past the count; the first real block ramps in
      shedding_ = false;
      ramp_in_ = true;
    }
  }
  const std::uint32_t t_entry = cycles_();
  render_stamp_ = ticket.stamp;

  // The newest complete RX block is the one behind the slot the DMA is writing.
  const std::uint32_t rx_slot = (rx_slot_.load(std::memory_order_relaxed) + hal::audio::kRxPhysSlots - 1u) & (hal::audio::kRxPhysSlots - 1u);
  copy_block(in_, rx_ + rx_slot * kBlockSamples);
  const std::uint32_t t_copied = cycles_();

  clear_report(report_);
  hal::audio::BlockIo io{in_, out_, ring_.muted() ? hal::audio::kFlagSilence : 0u, &report_};
  render_.fn(render_.user, io);

  const std::uint32_t t_rendered = cycles_();
  const bool late = !ring_.on_time(ticket);
  if (!late) {
    if (ramp_in_) {
      ramp_in_ = false;
      ramp_in_block(out_);
    }
    copy_block(tx_ + ticket.write_slot * kBlockSamples, out_);
    ring_.published(ticket);
  }
  const std::uint32_t t_end = cycles_();

  RenderSample sample;
  sample.total = t_end - t_entry;
  sample.latency = t_entry - ticket.stamp;
  for (unsigned i = 0; i < kStageCount; ++i) {
    sample.stage[i] = report_.stage_cycles[i];
  }
  sample.stage[hal::audio::kStageInput] += t_copied - t_entry;  // the RX copy belongs to the input stage
  sample.stage[hal::audio::kStageCommit] = t_end - t_rendered;
  sample.input_peak = report_.input_peak;
  sample.late = late;
  sample.over_budget = sample.total > budget_;
  status_.render_publish(sample);
  if (guard) {
    stale_ = sample.over_budget ? static_cast<std::uint8_t>(stale_ + 1u) : std::uint8_t{0};  // a render under the budget left the main loop the rest of its block: the count decays to zero
  }
  if (sample.total > worst_total_.load(std::memory_order_relaxed)) {  // the slowest render so far, as its stages (OS-PERF3)
    for (unsigned i = 0; i < kStageCount; ++i) {
      worst_stage_[i].store(sample.stage[i], std::memory_order_relaxed);
    }
    worst_total_.store(sample.total, std::memory_order_relaxed);
  }
}

}  // namespace o3sdk::platform::mk66::audio
