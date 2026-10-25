// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pipeline (audio/pipeline.h, OS-005a) driven by hand, with no interrupts: the test plays the DMA ISR and the render line, so
// every interleaving is chosen, not hoped for. Rings of the MK66's geometry; a fake cycle counter that only the test advances.
// Checks slot ownership over 10,000 blocks, the stage report and the governor, late blocks, the underrun mute, the RX slot choice,
// hardware faults and the period figures, and the main-loop starvation guard. The block callback here is a tagged-block fake; the
// test tone (audio_test_tone_test.cpp) is checked behind this same pipeline.
#include <doctest/doctest.h>

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio/pipeline.h>

#include <cstdint>
#include <cstdlib>

namespace {

namespace haudio = o3sdk::hal::audio;
using o3sdk::platform::mk66::audio::Pipeline;
using o3sdk::platform::mk66::audio::StatusSnapshot;
constexpr std::uint32_t kUnmute = o3sdk::platform::mk66::audio::SlotRing<2, 2>::kUnmuteAfter;  // on-time boundaries before the mute lifts

constexpr std::uint32_t kN = haudio::kTxSlots;

// The fake cycle counter: advances only when the test (or a callback) says so; every read also advances it by `step_per_read`
// so the stage figures are non-zero and predictable.
std::uint32_t g_cycles = 0;
std::uint32_t g_step_per_read = 10;
std::uint32_t fake_cycles() noexcept {
  g_cycles += g_step_per_read;
  return g_cycles;
}

struct alignas(haudio::kTxRingBytes) Tx {
  std::int16_t s[haudio::kTxPhysSlots * haudio::kBlockSamples];
};
struct alignas(haudio::kRxRingBytes) Rx {
  std::int16_t s[haudio::kRxPhysSlots * haudio::kBlockSamples];
};
Tx g_tx;
Rx g_rx;

std::uint32_t fnv(const std::int16_t* b) {
  std::uint32_t h = 2166136261u;
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    h = (h ^ static_cast<std::uint16_t>(b[i])) * 16777619u;
  }
  return h;
}

// A callback that writes a tagged block, reports stage cycles, and may add a fixed cost to the cycle counter.
struct Cb {
  std::uint32_t calls = 0;
  std::uint32_t extra_cycles = 0;
  std::uint32_t silenced = 0;
  std::uint32_t last_input_tag = 0;
  std::uint32_t last_flags = 0;
};
Cb g_cb;

std::uint32_t tag_of(const std::int16_t* b) { return static_cast<std::uint16_t>(b[0]) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(b[1])) << 16); }

void fill_tagged(std::int16_t* b, std::uint32_t tag) {
  b[0] = static_cast<std::int16_t>(tag & 0xFFFFu);
  b[1] = static_cast<std::int16_t>(tag >> 16);
  for (std::uint32_t i = 2; i < haudio::kBlockSamples; ++i) {
    b[i] = static_cast<std::int16_t>((tag * 31u + i * 7u) & 0x7FFFu);
  }
}

void cb_render(void* user, const haudio::BlockIo& io) noexcept {
  Cb& c = *static_cast<Cb*>(user);
  ++c.calls;
  c.last_flags = io.flags;
  c.last_input_tag = tag_of(io.in);
  g_cycles += c.extra_cycles;
  if ((io.flags & haudio::kFlagSilence) != 0u) {
    ++c.silenced;
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      io.out[i] = 0;
    }
  } else {
    fill_tagged(io.out, c.calls);
  }
  io.report->stage_cycles[haudio::kStageInput] = 11;
  io.report->stage_cycles[haudio::kStageDrain] = 22;
  io.report->stage_cycles[haudio::kStageRender] = 3333;
  io.report->stage_cycles[haudio::kStageConvert] = 444;
  io.report->input_peak = 99;
}

constexpr std::uint32_t kTestWindow = 8;  // the starvation window the guard tests run with (the product default is kStarveBlocks = 64)

void setup(Pipeline& p, std::uint32_t cycles_per_block = 261120, bool guard = false) {
  for (std::int16_t& v : g_tx.s) {
    v = 0;
  }
  for (std::int16_t& v : g_rx.s) {
    v = 0;
  }
  g_cb = Cb{};
  g_cycles = 1000;
  g_step_per_read = 10;
  Pipeline::Config config{};
  config.tx = g_tx.s;
  config.rx = g_rx.s;
  config.render = haudio::BlockCallback{&cb_render, &g_cb};
  config.cycles = &fake_cycles;
  config.cycles_per_block = cycles_per_block;
  config.starve_guard = guard;
  const bool first = !p.guard_armed();
  p.init(config);
  if (guard && first) {
    p.set_starve_blocks(kTestWindow);  // the guard tests below are written for N = 8; init() keeps a window set since (one test checks that)
  }
}

// The ISR as the platform plays it at boundary b: the slot now playing is b mod P; RX block b has just completed in slot b mod 4
// and the DMA is writing the next one.
void isr(Pipeline& p, std::uint32_t b, std::uint32_t stamp, std::uint32_t faults = 0) {
  fill_tagged(g_rx.s + (b % haudio::kRxPhysSlots) * haudio::kBlockSamples, b);
  p.on_boundary(b % haudio::kTxPhysSlots, (b + 1u) % haudio::kRxPhysSlots, stamp, faults);
}

const std::int16_t* playing(std::uint32_t b) { return g_tx.s + (b % haudio::kTxPhysSlots) * haudio::kBlockSamples; }

}  // namespace

TEST_CASE("pipeline: 10,000 blocks, every boundary plays exactly the block rendered for it, no slot is written while it plays") {
  Pipeline p;
  setup(p);
  static std::uint32_t checksum_of_call[10100];
  std::uint32_t bad = 0;
  std::uint32_t first_bad = 0;
  std::uint32_t torn = 0;
  std::uint32_t zero_block_sum;
  {
    alignas(8) std::int16_t zeros[haudio::kBlockSamples] = {};
    zero_block_sum = fnv(zeros);
  }
  std::uint32_t heard_at_start = fnv(playing(0));  // slot 0 plays from "boundary 0"
  for (std::uint32_t b = 1; b <= 10000; ++b) {
    // The slot that has just finished must still hold what it held when it started.
    if (fnv(playing(b - 1u)) != heard_at_start) {
      ++torn;
    }
    isr(p, b, 1000u + b * 261120u);
    // The DAC hears the slot that just started.
    heard_at_start = fnv(playing(b));
    bool ok;
    if (b < kN) {
      ok = heard_at_start == zero_block_sum;
    } else {
      const std::uint32_t call = b - kN + 1;
      ok = heard_at_start == checksum_of_call[call] && tag_of(playing(b)) == call;
    }
    if (!ok && bad++ == 0) {
      first_bad = b;
    }
    p.render();
    checksum_of_call[g_cb.calls] = fnv(g_tx.s + Pipeline::Ring::slot_for(b, p.ring().base()) * haudio::kBlockSamples);
  }
  CHECK_MESSAGE(bad == 0u, "first bad boundary " << first_bad);
  CHECK(torn == 0u);
  const StatusSnapshot s = p.status().snapshot();
  CHECK(s.boundaries == 10000u);
  CHECK(s.renders == 10000u);
  CHECK(s.underruns == 0u);
  CHECK(s.late_drops == 0u);
  CHECK(s.slot_slips == 0u);
  CHECK(s.mute_events == 0u);
  CHECK(g_cb.calls == 10000u);
  CHECK(s.period_min == 261120u);
  CHECK(s.period_max == 261120u);
}

TEST_CASE("pipeline: the render copies the newest complete RX block, and reports its stages and the governor") {
  Pipeline p;
  setup(p, 100000);                    // budget 60 000 cycles
  g_cb.extra_cycles = 0;
  isr(p, 1, 5000);
  p.render();
  CHECK(g_cb.last_input_tag == 1u);    // RX block 1 completed at boundary 1: the slot behind the one being written
  isr(p, 2, 105000);
  p.render();
  CHECK(g_cb.last_input_tag == 2u);
  StatusSnapshot s = p.status().snapshot();
  CHECK(s.input_peak == 99u);
  CHECK(s.stage_last[haudio::kStageRender] == 3333u);
  CHECK(s.stage_last[haudio::kStageConvert] == 444u);
  CHECK(s.stage_last[haudio::kStageCommit] > 0u);            // the pipeline's own copy was timed
  CHECK(s.stage_last[haudio::kStageInput] >= 11u + 10u);     // the callback's figure plus the RX copy
  CHECK(s.latency_last > 0u);
  CHECK(s.governor_over == 0u);
  CHECK(s.render_max < p.budget_cycles());
  CHECK(p.budget_cycles() == 60000u);                        // 60 % of the block

  // A render that costs 70,000 cycles is over the 60 % budget: counted, not acted on.
  g_cb.extra_cycles = 70000;
  isr(p, 3, 205000);
  p.render();
  s = p.status().snapshot();
  CHECK(s.governor_over == 1u);
  CHECK(s.render_max >= 70000u);
  CHECK(s.late_drops == 0u);
  g_cb.extra_cycles = 0;
  isr(p, 4, 305000);
  p.render();
  CHECK(p.status().snapshot().governor_over == 1u);          // back under: no further count
}

TEST_CASE("pipeline: an RX block is never read while the DMA writes it, whichever phase the RX boundary has") {
  Pipeline p;
  setup(p);
  // RX boundary just after the TX one: the ISR sees the DMA still writing block b (slot b mod 4), the newest complete is b - 1.
  for (std::uint32_t b = 1; b <= 40; ++b) {
    fill_tagged(g_rx.s + ((b - 1u) % 4u) * haudio::kBlockSamples, b - 1u);
    p.on_boundary(b % haudio::kTxPhysSlots, b % 4u, b * 100u, 0);
    p.render();
    CHECK(g_cb.last_input_tag == b - 1u);
  }
}

TEST_CASE("pipeline: a render that finishes after its slot started to play is dropped, counted, and never written") {
  Pipeline p;
  setup(p);
  // The block rendered for boundary s plays at s + kN - 1 (kN = kTxSlots, 2 or 3; the scripts below are written in kN).
  static_assert(kN == 2u || kN == 3u, "the scripts below cover two and three slots");
  isr(p, 1, 100);
  p.render();                       // call 1, for boundary 1: plays at boundary kN
  for (std::uint32_t b = 2; b <= kN; ++b) {
    isr(p, b, b * 100u);
  }
  CHECK(tag_of(playing(kN)) == 1u);
  CHECK(p.status().snapshot().underruns == 0u);
  // Boundary kN + 1 passes before the render for boundary 2 runs: the render then serves the newest boundary (kN + 1); the
  // block for boundary 2 is an underrun at boundary kN + 1.
  isr(p, kN + 1u, (kN + 1u) * 100u);
  CHECK(p.status().snapshot().underruns == 1u);
  p.render();                       // call 2, serving boundary kN + 1 on time (plays at 2 kN)
  // The blocks of boundaries 3 .. kN were never rendered either: kN - 2 more underruns, then call 2's block plays.
  for (std::uint32_t b = kN + 2u; b <= 2u * kN; ++b) {
    isr(p, b, b * 100u);
  }
  CHECK(tag_of(playing(2u * kN)) == 2u);  // the block of call 2
  CHECK(p.status().snapshot().underruns == kN - 1u);
  p.render();                       // call 3, boundary 2 kN
  CHECK(g_cb.calls == 3u);
  p.render();                       // a pend that arrives for a boundary already rendered does nothing
  CHECK(g_cb.calls == 3u);

  // A render that overruns: boundaries 2 .. kN happen between its begin and its commit (the test plays the ISR inside the
  // callback, as the real one preempts the render); its slot has started to play by then.
  for (std::int16_t& v : g_tx.s) {
    v = 0;
  }
  Pipeline q;
  struct Overrun {
    Pipeline* pipeline;
    std::uint32_t boundary;
  } over{&q, kN};
  Pipeline::Config config{};
  config.tx = g_tx.s;
  config.rx = g_rx.s;
  config.cycles = &fake_cycles;
  config.cycles_per_block = 261120;
  config.render = haudio::BlockCallback{
      [](void* user, const haudio::BlockIo& io) noexcept {
        Overrun& o = *static_cast<Overrun*>(user);
        fill_tagged(io.out, 777u);
        io.report->input_peak = 1;
        for (std::uint32_t b = 2; b <= o.boundary; ++b) {
          isr(*o.pipeline, b, 500);  // the DMA ISR preempts the render
        }
      },
      &over};
  q.init(config);
  isr(q, 1, 100);
  q.render();                       // the callback runs for boundary 1 and boundaries 2 .. kN pass inside it
  const StatusSnapshot s = q.status().snapshot();
  CHECK(s.late_drops == 1u);        // finished after boundary kN, where its slot plays: dropped
  CHECK(s.boundaries == kN);
  // The block was not written into any slot: every slot is still the zeroed ring.
  alignas(8) std::int16_t zeros[haudio::kBlockSamples] = {};
  for (std::uint32_t slot = 0; slot < haudio::kTxPhysSlots; ++slot) {
    CHECK(fnv(playing(slot)) == fnv(zeros));
  }
}

TEST_CASE("pipeline: three underruns in a row mute, zero the stale slot, and tell the render to write silence") {
  Pipeline p;
  setup(p);
  static_assert(kN == 2u || kN == 3u, "the scripts below cover two and three slots");
  isr(p, 1, 100);
  p.render();                        // call 1 for boundary 1 (plays at kN)
  for (std::uint32_t b = 2; b <= kN; ++b) {
    isr(p, b, b * 100u);
  }
  CHECK(tag_of(playing(kN)) == 1u);
  // The render stalls from boundary 2 on: the block of boundary 2 is due at boundary kN + 1.
  isr(p, kN + 1u, 300);              // block for boundary 2 never came: underrun 1
  isr(p, kN + 2u, 400);              // underrun 2
  // The stale slot replays what it held: call 1's block when the ring has two slots (kN + 2 is the slot of kN), the zeroed ring with four.
  CHECK(tag_of(playing(kN + 2u)) == (haudio::kTxPhysSlots == 2u ? 1u : 0u));
  CHECK(p.status().snapshot().mute_events == 0u);
  const std::uint32_t mute_at = kN + 3u;   // underrun 3: mute, the stale slot is zeroed
  isr(p, mute_at, 500);
  StatusSnapshot s = p.status().snapshot();
  CHECK(s.underruns == 3u);
  CHECK(s.max_consecutive_underruns == 3u);
  CHECK(s.mute_events == 1u);
  CHECK(s.muted == 1u);
  alignas(8) std::int16_t zeros[haudio::kBlockSamples] = {};
  CHECK(fnv(playing(mute_at)) == fnv(zeros));
  p.render();                        // call 2, serving boundary mute_at
  CHECK(g_cb.last_flags == haudio::kFlagSilence);
  CHECK(g_cb.silenced == 1u);
  // The blocks of the stalled boundaries mute_at - kN + 2 .. mute_at - 1 were never rendered: kN - 2 more underruns (still muted,
  // the slot zeroed) until call 2's block plays. The last underrun is at `last_underrun`.
  const std::uint32_t last_underrun = mute_at + (kN - 2u);
  for (std::uint32_t b = mute_at + 1u; b <= last_underrun; ++b) {
    isr(p, b, b * 100u);
    CHECK(p.status().snapshot().muted == 1u);
    p.render();
    CHECK(g_cb.last_flags == haudio::kFlagSilence);
  }
  CHECK(p.status().snapshot().underruns == 3u + (kN - 2u));
  // kUnmute - 1 on-time blocks with silence flagged, then the mute lifts and the callback is no longer asked for silence.
  for (std::uint32_t b = last_underrun + 1u; b < last_underrun + kUnmute; ++b) {
    isr(p, b, b * 100u);
    CHECK(p.status().snapshot().muted == 1u);
    p.render();
    CHECK(g_cb.last_flags == haudio::kFlagSilence);
  }
  isr(p, last_underrun + kUnmute, 99999);
  CHECK(p.status().snapshot().muted == 0u);
  p.render();
  CHECK(g_cb.last_flags == 0u);
  CHECK(g_cb.silenced == kUnmute + (kN - 2u));
}

TEST_CASE("pipeline: hardware faults and the boundary period are published") {
  Pipeline p;
  setup(p);
  isr(p, 1, 1000);
  isr(p, 2, 1000 + 261120);
  isr(p, 3, 1000 + 261120 + 261125, 2);
  isr(p, 4, 1000 + 261120 + 261125 + 261100, 1);
  const StatusSnapshot s = p.status().snapshot();
  CHECK(s.hw_faults == 3u);
  CHECK(s.period_min == 261100u);
  CHECK(s.period_max == 261125u);
  CHECK(s.boundaries == 4u);
}

// OS-PERF3: the record of the slowest render, as its stages (`diag bench worst`), and `diag cpu reset`.
TEST_CASE("pipeline: the slowest render is kept with its stages, and a reset forgets it") {
  Pipeline p;
  setup(p, 1000000);
  CHECK(p.worst_render().total == 0u);
  g_cb.extra_cycles = 100;
  isr(p, 1, 5000);
  p.render();
  const Pipeline::WorstRender first = p.worst_render();
  CHECK(first.total > 100u);
  CHECK(first.stage[haudio::kStageRender] == 3333u);  // the stages of that very render
  CHECK(first.stage[haudio::kStageDrain] == 22u);
  g_cb.extra_cycles = 5000;                            // a slower one replaces it
  isr(p, 2, 261120u + 5000u);
  p.render();
  const Pipeline::WorstRender second = p.worst_render();
  CHECK(second.total > first.total + 4000u);
  g_cb.extra_cycles = 300;                             // a faster one does not
  isr(p, 3, 2u * 261120u + 5000u);
  p.render();
  const Pipeline::WorstRender third = p.worst_render();
  CHECK(third.total == second.total);
  for (unsigned i = 0; i < haudio::kStageCount; ++i) {
    CHECK(third.stage[i] == second.stage[i]);
  }
  p.reset_peaks();
  CHECK(p.worst_render().total == 0u);
  CHECK(p.status().snapshot().render_max == 0u);       // the status' maxima went with it
  CHECK(p.status().snapshot().renders == 3u);          // and its totals stayed
  isr(p, 4, 3u * 261120u + 5000u);
  p.render();
  CHECK(p.worst_render().total > 0u);
}

// ---- OS-MAINGUARD: the main-loop starvation guard -------------------------------------------------------------------------------------------
// A render over 60 % of the block (261120 cycles: budget 156672) with no main_pass() in between is "stale"; kStarveBlocks of them and the render sheds.
namespace {

constexpr std::uint32_t kHeavy = 200000;  // 77 % of the block: over the budget, under the block (the render finishes before the next boundary)
constexpr std::uint32_t kLight = 20000;   // 8 %
constexpr std::uint32_t kStarve = kTestWindow;

// One boundary and its render, the callback costing `cost` cycles.
void block(Pipeline& p, std::uint32_t b, std::uint32_t cost) {
  g_cb.extra_cycles = cost;
  isr(p, b, b * 261120u);
  p.render();
}

const std::int16_t* block_for(const Pipeline& p, std::uint32_t seq) {
  return g_tx.s + Pipeline::Ring::slot_for(seq, p.ring().base()) * haudio::kBlockSamples;
}

bool is_zero(const std::int16_t* b) {
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    if (b[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("pipeline guard: a frozen beat and renders over 60 % shed after kStarveBlocks, the slot is published, nothing underruns") {
  static_assert(kStarve == 8u, "the window of these tests");
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < kStarve; ) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == kStarve);
  CHECK(p.shed_blocks() == 0u);
  block(p, ++b, kHeavy);  // the 9th
  CHECK(g_cb.calls == kStarve);  // the callback did not run
  CHECK(p.shed_blocks() == 1u);
  CHECK(p.shed_runs() == 1u);
  CHECK(p.shed_late() == 0u);
  CHECK(p.ring().slot_seq(Pipeline::Ring::slot_for(b, p.ring().base())) == b);  // published: the ISR will find it
  // What the status sees: eight renders, eight over budget, no late, no underrun; the shed is none of them.
  for (; b < 40; ) {
    block(p, ++b, kHeavy);
  }
  const StatusSnapshot s = p.status().snapshot();
  CHECK(s.underruns == 0u);
  CHECK(s.late_drops == 0u);
  CHECK(s.renders == kStarve);
  CHECK(s.governor_over == kStarve);
  CHECK(p.shed_blocks() == 40u - kStarve);  // the beat never moved: every block since the 9th is a shed
  CHECK(p.shed_runs() == 1u);
  CHECK(p.shed_longest() == 40u - kStarve);
  CHECK(g_cb.calls == kStarve);
}

TEST_CASE("pipeline guard: the window N is tunable: the default is kStarveBlocks, 0 and 300 clamp to 1 and 255, N = 32 sheds on the 33rd stale render and not before") {
  static_assert(o3sdk::platform::mk66::audio::kStarveBlocks == 64u, "the default N (fqa.9 unit sweep)");
  Pipeline p;
  CHECK(p.starve_blocks() == o3sdk::platform::mk66::audio::kStarveBlocks);  // a new Pipeline starts at the default
  setup(p, 261120, true);
  p.set_starve_blocks(0);
  CHECK(p.starve_blocks() == 1u);
  p.set_starve_blocks(300);
  CHECK(p.starve_blocks() == 255u);
  p.set_starve_blocks(32);
  CHECK(p.starve_blocks() == 32u);
  std::uint32_t b = 0;
  for (; b < 32u; ) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == 32u);  // 32 stale renders, none shed
  CHECK(p.shed_blocks() == 0u);
  block(p, ++b, kHeavy);  // the 33rd
  CHECK(g_cb.calls == 32u);
  CHECK(p.shed_blocks() == 1u);
  CHECK(p.shed_runs() == 1u);
  CHECK(p.ring().slot_seq(Pipeline::Ring::slot_for(b, p.ring().base())) == b);  // published
  // init() keeps the tuning (a restart of the audio path), like the guard's arming
  setup(p, 261120, true);
  CHECK(p.starve_blocks() == 32u);
}

TEST_CASE("pipeline guard: a new N takes effect at the next block, lowered or raised, also inside a shed run (which ends and ramps in)") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < 5u; ) {
    block(p, ++b, kHeavy);  // 5 stale renders of the window 8
  }
  p.set_starve_blocks(5);
  block(p, ++b, kHeavy);  // the next block already sheds: the count (5) has reached the new N
  CHECK(g_cb.calls == 5u);
  CHECK(p.shed_blocks() == 1u);
  block(p, ++b, kHeavy);
  CHECK(p.shed_blocks() == 2u);
  p.set_starve_blocks(40);  // raised inside the run: the very next block renders again
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == 6u);
  CHECK(p.shed_blocks() == 2u);
  const std::int16_t* first = block_for(p, b);  // the first real block after the run ramps in (frame 0 is 1/64 of its level)
  CHECK(std::abs(static_cast<int>(first[0])) <= std::abs(static_cast<int>(first[haudio::kBlockSamples - 2u])) / 8 + 1);
  // the count goes on from where it was (5 + 1): 40 in all before the next shed, a new run
  for (std::uint32_t i = 0; i < 40u - 6u; ++i) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == 40u);
  CHECK(p.shed_blocks() == 2u);
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == 40u);
  CHECK(p.shed_blocks() == 3u);
  CHECK(p.shed_runs() == 2u);
}

TEST_CASE("pipeline guard: a beat that moves every pass never sheds, whatever the load") {
  Pipeline p;
  setup(p, 261120, true);
  for (std::uint32_t b = 1; b <= 200; ++b) {
    p.main_pass();
    block(p, b, 250000);  // 96 %
  }
  CHECK(g_cb.calls == 200u);
  CHECK(p.shed_blocks() == 0u);
  // one pass per two blocks is enough too
  for (std::uint32_t b = 201; b <= 400; ++b) {
    if ((b & 1u) != 0u) {
      p.main_pass();
    }
    block(p, b, kHeavy);
  }
  CHECK(g_cb.calls == 400u);
  CHECK(p.shed_blocks() == 0u);
}

TEST_CASE("pipeline guard: a shed run lasts until one main pass, then kStarveBlocks more renders, then it sheds again") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < kStarve + 5u; ) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == kStarve);
  CHECK(p.shed_blocks() == 5u);
  p.main_pass();  // a whole pass ran
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == kStarve + 1u);  // rendering again
  CHECK(p.shed_blocks() == 5u);
  for (std::uint32_t i = 1; i < kStarve; ++i) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == 2u * kStarve);  // kStarve renders after the beat
  CHECK(p.shed_blocks() == 5u);
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == 2u * kStarve);  // and the next sheds
  CHECK(p.shed_blocks() == 6u);
  CHECK(p.shed_runs() == 2u);
  CHECK(p.shed_longest() == 5u);
  CHECK(p.status().snapshot().underruns == 0u);
}

TEST_CASE("pipeline guard: light load never sheds, and a light render decays the count (an SD pass, the 0.2 s unmute)") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < 300; ) {
    block(p, ++b, kLight);  // the beat is frozen for 300 blocks (435 ms)
  }
  CHECK(g_cb.calls == 300u);
  CHECK(p.shed_blocks() == 0u);
  // The count decays: kStarve - 1 heavy renders, one light, kStarve - 1 heavy, one light ... never reaches kStarve.
  for (std::uint32_t round = 0; round < 20; ++round) {
    for (std::uint32_t i = 0; i < kStarve - 1u; ++i) {
      block(p, ++b, kHeavy);
    }
    block(p, ++b, kLight);
  }
  CHECK(p.shed_blocks() == 0u);
  CHECK(g_cb.calls == 300u + 20u * kStarve);
  // while a plain run of heavy ones after a light one sheds on the 9th
  for (std::uint32_t i = 0; i < kStarve + 1u; ++i) {
    block(p, ++b, kHeavy);
  }
  CHECK(p.shed_blocks() == 1u);
}

TEST_CASE("pipeline guard: off by default, a frozen beat and heavy renders leave the script unchanged") {
  Pipeline p;
  setup(p);
  for (std::uint32_t b = 1; b <= 60; ++b) {
    block(p, b, kHeavy);
  }
  CHECK(g_cb.calls == 60u);
  CHECK(p.shed_blocks() == 0u);
  p.arm_guard();  // armed on a running pipeline (the MK66 main does this once before its loop)
  p.set_starve_blocks(kStarve);
  for (std::uint32_t b = 61; b <= 61u + kStarve; ++b) {
    block(p, b, kHeavy);
  }
  CHECK(p.shed_blocks() == 1u);
  CHECK(sizeof(o3sdk::platform::mk66::audio::AudioStatus) == 38u * sizeof(std::uint32_t));  // the probe layout did not grow
}

TEST_CASE("pipeline guard: the shed is a fade out from the last frame, then zeros, and the first real block ramps in: no step") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < kStarve; ) {
    block(p, ++b, kHeavy);
  }
  const std::int16_t* last = block_for(p, b);  // the last real block, as the DAC will play it
  const std::int32_t last_l = last[haudio::kBlockSamples - 2u];
  const std::int32_t last_r = last[haudio::kBlockSamples - 1u];
  CHECK(last_l != 0);
  block(p, ++b, kHeavy);
  const std::int16_t* fade = block_for(p, b);
  for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
    const std::int32_t g = static_cast<std::int32_t>(haudio::kBlock - 1u - i);
    CHECK(fade[2u * i] == static_cast<std::int16_t>((last_l * g) / 64));
    CHECK(fade[2u * i + 1u] == static_cast<std::int16_t>((last_r * g) / 64));
  }
  CHECK(fade[haudio::kBlockSamples - 2u] == 0);
  // The largest step: from the last real frame to the first faded one, and between faded frames, is one ramp step (last / 64) plus rounding.
  const std::int32_t step_max = (last_l < 0 ? -last_l : last_l) / 64 + 1;
  CHECK(std::abs(last_l - fade[0]) <= step_max);
  for (std::uint32_t i = 1; i < haudio::kBlock; ++i) {
    CHECK(std::abs(fade[2u * i] - fade[2u * (i - 1u)]) <= step_max);
  }
  block(p, ++b, kHeavy);
  CHECK(is_zero(block_for(p, b)));  // later blocks of the run are zeros
  p.main_pass();
  block(p, ++b, kHeavy);
  // the first real block after the run: ramped (i + 1) / 64
  alignas(8) std::int16_t want[haudio::kBlockSamples];
  fill_tagged(want, g_cb.calls);
  const std::int16_t* heard = block_for(p, b);
  for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
    const std::int32_t g = static_cast<std::int32_t>(i + 1u);
    CHECK(heard[2u * i] == static_cast<std::int16_t>((static_cast<std::int32_t>(want[2u * i]) * g) / 64));
    CHECK(heard[2u * i + 1u] == static_cast<std::int16_t>((static_cast<std::int32_t>(want[2u * i + 1u]) * g) / 64));
  }
  CHECK(std::abs(static_cast<std::int32_t>(heard[0])) <= static_cast<std::int32_t>(want[0]) / 64 + 1);  // starts at about zero: continuous with the zeros before
  block(p, ++b, kHeavy);
  const std::int16_t* after = block_for(p, b);  // and the block after it is not ramped
  fill_tagged(want, g_cb.calls);
  CHECK(fnv(after) == fnv(want));
}

TEST_CASE("pipeline guard: a late shed (its slot already plays) is not published and is counted as late") {
  Pipeline p;
  setup(p, 261120, true);
  for (std::uint32_t b = 1; b <= kStarve + 1u; ++b) {
    block(p, b, kHeavy);
  }
  CHECK(p.shed_blocks() == 1u);
  const std::uint32_t b = kStarve + 1u;
  // The ISR runs N - 1 boundaries while a render for boundary b is still going: that render's ticket is stale.
  isr(p, b + 1u, (b + 1u) * 261120u);
  isr(p, b + 2u, (b + 2u) * 261120u);
  const Pipeline::Ring::Ticket stale{b, Pipeline::Ring::slot_for(b, p.ring().base()), b * 261120u};
  const std::uint32_t before = p.ring().slot_seq(stale.write_slot);
  const std::uint32_t sum = fnv(block_for(p, b));
  p.shed(stale);
  CHECK(p.shed_blocks() == 2u);
  CHECK(p.shed_late() == 1u);
  CHECK(p.ring().slot_seq(stale.write_slot) == before);  // not published
  CHECK(fnv(block_for(p, b)) == sum);                    // and the slot, which is playing, was not written
}

TEST_CASE("pipeline guard: a shed with the ring muted publishes on time, keeps the mute, and the mute lifts on the shed blocks as on rendered ones") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < kStarve; ) {
    block(p, ++b, kHeavy);
  }
  // Five boundaries pass with no render at all (the render line was held off): boundary s plays the block of s - 2, so the last three of them underrun, the mute latches.
  for (std::uint32_t i = 0; i < 5u; ++i) {
    isr(p, b + 1u, (b + 1u) * 261120u);
    ++b;
  }
  CHECK(p.ring().muted());
  const std::uint32_t calls = g_cb.calls;
  p.render();  // serves the newest boundary, the count is at the limit: a shed
  CHECK(g_cb.calls == calls);
  CHECK(p.shed_blocks() == 1u);
  CHECK(p.ring().slot_seq(Pipeline::Ring::slot_for(b, p.ring().base())) == b);
  CHECK(p.ring().muted());  // a shed does not lift it by itself
  const std::uint32_t underruns = p.status().snapshot().underruns;
  for (std::uint32_t i = 0; i < Pipeline::Ring::kUnmuteAfter + 2u; ++i) {
    block(p, ++b, kHeavy);
  }
  CHECK_FALSE(p.ring().muted());  // 32 on-time boundaries, rendered or shed
  CHECK(p.status().snapshot().underruns <= underruns + 1u);  // boundary b + 1 still plays the block nobody rendered; every later one is on time
  CHECK(g_cb.calls == calls);
}

TEST_CASE("pipeline guard: a long run stops its length at 255, and reset_peaks restarts the counters from the render") {
  Pipeline p;
  setup(p, 261120, true);
  std::uint32_t b = 0;
  for (; b < kStarve + 300u; ) {
    block(p, ++b, kHeavy);
  }
  CHECK(p.shed_blocks() == 300u);
  CHECK(p.shed_runs() == 1u);
  CHECK(p.shed_longest() == 255u);  // stops at 255
  p.reset_peaks();
  CHECK(p.shed_blocks() == 300u);   // the main loop does not write them: the render does, at its next block
  block(p, ++b, kHeavy);
  CHECK(p.shed_blocks() == 1u);     // zeroed, then this block
  CHECK(p.shed_runs() == 0u);
  CHECK(p.shed_late() == 0u);
  CHECK(p.shed_longest() == 255u);  // since boot
  block(p, ++b, kHeavy);
  CHECK(p.shed_blocks() == 2u);     // one reset is one zeroing
}

TEST_CASE("pipeline guard: the beat wraps at 2^16 without a false stall or a missed move") {
  Pipeline p;
  setup(p, 261120, true);
  p.seed_beat_for_test(0xFFFFu);
  std::uint32_t b = 0;
  for (; b < kStarve + 3u; ) {
    block(p, ++b, kHeavy);
  }
  CHECK(g_cb.calls == kStarve);   // the seeded beat is the first one seen: the count starts there
  CHECK(p.shed_blocks() == 3u);
  p.main_pass();                  // 0xFFFF -> 0
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == kStarve + 1u);
  CHECK(p.shed_blocks() == 3u);
  for (std::uint32_t i = 1; i < kStarve; ++i) {
    block(p, ++b, kHeavy);
  }
  block(p, ++b, kHeavy);
  CHECK(p.shed_blocks() == 4u);
  p.main_pass();                  // 0 -> 1
  block(p, ++b, kHeavy);
  CHECK(g_cb.calls == 2u * kStarve + 1u);
}
