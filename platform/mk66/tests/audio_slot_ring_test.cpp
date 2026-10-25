// SPDX-License-Identifier: GPL-3.0-or-later
//
// The slot protocol of the audio engine (audio/slot_ring.h, OS-005a): which slot a render fills, when a block is an underrun, the
// three-in-a-row mute and its recovery, and the late-block rule, against an independent reference model, for the ring geometries
// 2 of 2, 2 of 4, 3 of 4 and 4 of 4 (render-ahead depth of physical slots), over thousands of boundaries with random render
// timing.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/slot_ring.h>

#include <cstdint>

namespace {

using o3sdk::platform::mk66::audio::SlotRing;

// Deterministic xorshift32: a failing run is reproduced from its seed.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : s_(seed) {}
  std::uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  std::uint32_t below(std::uint32_t n) { return next() % n; }

 private:
  std::uint32_t s_;
};

// The reference: what the protocol's text says, written without the ring's code. Block t is "the render for boundary t"; it plays at
// boundary t + N - 1 in physical slot (t + N - 1) mod P, and is on time iff fewer than N - 1 boundaries passed when it was committed.
struct Model {
  std::uint32_t n;
  std::uint32_t p;
  static constexpr std::uint32_t kMax = 40000;
  bool committed[kMax] = {};
  std::uint32_t consecutive = 0;
  std::uint32_t good = 0;
  bool muted = false;

  struct Verdict {
    bool underrun;
    bool zero;
    bool mute_started;
  };
  Verdict boundary(std::uint32_t b) {
    Verdict v{false, false, false};
    if (b >= n) {
      v.underrun = !committed[b - n + 1];
    }
    if (v.underrun) {
      good = 0;
      ++consecutive;
      if (consecutive >= 3 && !muted) {
        muted = true;
        v.mute_started = true;
      }
      v.zero = muted;
    } else {
      consecutive = 0;
      if (muted && ++good >= SlotRing<2, 2>::kUnmuteAfter) {
        muted = false;
        good = 0;
      }
    }
    return v;
  }
};

// Runs `boundaries` boundaries of random render timing through a ring and the model, with the physical slot contents tracked
// separately, and checks everything. `mode` picks the timing mix.
template <std::uint32_t P, std::uint32_t N>
void run(std::uint32_t seed, std::uint32_t boundaries, std::uint32_t late_percent, std::uint32_t skip_percent) {
  static Model model;
  model = Model{};
  model.n = N;
  model.p = P;
  SlotRing<P, N> ring;
  Rng rng(seed);

  // Physical slot contents: the id of the block in each slot (0 = the zeroed ring or zeroed by the ISR).
  std::uint32_t slot[P] = {};
  struct Inflight {
    bool active = false;
    typename SlotRing<P, N>::Ticket ticket{};
    std::uint32_t finish_at = 0;
  } inflight;

  std::uint32_t underruns = 0;
  std::uint32_t writes_into_playing = 0;
  std::uint32_t wrong_played = 0;
  std::uint32_t verdict_mismatch = 0;
  std::uint32_t muted_mismatch = 0;
  std::uint32_t late = 0;
  std::uint32_t mutes = 0;

  for (std::uint32_t b = 1; b <= boundaries; ++b) {
    // ---- the boundary ISR
    const std::uint32_t hw = b % P;
    const typename SlotRing<P, N>::Boundary bd = ring.on_boundary(hw, b * 1000u);
    const Model::Verdict want = model.boundary(b);
    verdict_mismatch += (bd.underrun != want.underrun || bd.zero_slot != want.zero || bd.mute_started != want.mute_started || bd.slipped ||
                         bd.seq != b || bd.slot != hw)
                            ? 1u
                            : 0u;
    muted_mismatch += ring.muted() != model.muted ? 1u : 0u;
    underruns += bd.underrun ? 1u : 0u;
    mutes += bd.mute_started ? 1u : 0u;
    if (bd.zero_slot) {
      slot[bd.slot] = 0;  // the ISR's one data access
    }
    // The DAC: the slot that starts to play holds the block rendered for it, or it is an underrun and holds something older.
    if (b >= N) {
      const std::uint32_t expect_block = b - N + 1;
      if (!bd.underrun && slot[hw] != expect_block) {
        ++wrong_played;
      }
      if (bd.underrun && slot[hw] == expect_block) {
        ++wrong_played;  // an underrun must mean the block is NOT there
      }
    }

    // ---- the render context after the boundary
    if (inflight.active && inflight.finish_at == b) {
      // The render finishes now (right after this boundary's ISR): commit if on time.
      const bool on_time = ring.on_time(inflight.ticket);
      const bool model_on_time = (b - inflight.ticket.seq) < N - 1u;
      if (on_time != model_on_time) {
        ++verdict_mismatch;
      }
      if (on_time) {
        // Never into the slot that is playing now.
        if (inflight.ticket.write_slot == hw) {
          ++writes_into_playing;
        }
        slot[inflight.ticket.write_slot] = inflight.ticket.seq;
        ring.published(inflight.ticket);
        model.committed[inflight.ticket.seq] = true;
      } else {
        ++late;
      }
      inflight.active = false;
    }
    if (!inflight.active) {
      // Maybe the render is held off (a higher-priority load): it then serves the newest boundary when it gets to run. Here: skipped
      // boundaries are modelled by simply not starting a render at this boundary with some probability; the next one that starts
      // takes the newest sequence number.
      if (rng.below(100) >= skip_percent) {
        typename SlotRing<P, N>::Ticket t{};
        if (ring.begin_render(t)) {
          if (t.seq != b || t.write_slot != ((b + N - 1u) % P) || t.stamp != b * 1000u) {
            ++verdict_mismatch;
          }
          const std::uint32_t r = rng.below(100);
          if (r < late_percent) {
            inflight.active = true;
            inflight.ticket = t;
            inflight.finish_at = b + 1u + rng.below(N + 1u);  // finishes 1 .. N + 1 boundaries later
          } else {
            // Prompt: committed before the next boundary.
            if (!ring.on_time(t)) {
              ++verdict_mismatch;
            }
            if (t.write_slot == hw) {
              ++writes_into_playing;
            }
            slot[t.write_slot] = t.seq;
            ring.published(t);
            model.committed[t.seq] = true;
          }
        }
      }
    }
  }
  INFO("P " << P << " N " << N << " seed " << seed);
  CHECK(verdict_mismatch == 0u);
  CHECK(muted_mismatch == 0u);
  CHECK(wrong_played == 0u);
  CHECK(writes_into_playing == 0u);
  CHECK(underruns > 0u);  // the timing mix does produce underruns, so the checks above were exercised
  CHECK(late > 0u);
  CHECK(mutes > 0u);
  CHECK(ring.seq() == boundaries);
}

}  // namespace

TEST_CASE("slot ring: reference model, 2 slots of 2, random render timing") {
  run<2, 2>(1, 20000, 8, 6);
  run<2, 2>(2, 20000, 3, 2);
}
TEST_CASE("slot ring: reference model, 2 slots of a 4-slot ring") {
  run<4, 2>(3, 20000, 8, 6);
}
TEST_CASE("slot ring: reference model, render-ahead of 3 (the fallback constant) in a 4-slot ring") {
  run<4, 3>(4, 20000, 12, 8);
  run<4, 3>(5, 20000, 30, 20);
}
TEST_CASE("slot ring: reference model, render-ahead of 4 in a 4-slot ring") {
  run<4, 4>(6, 20000, 15, 10);
}

TEST_CASE("slot ring: a clean run never underruns and the first N - 1 boundaries are the zeroed ring") {
  SlotRing<2, 2> ring;
  for (std::uint32_t b = 1; b <= 1000; ++b) {
    const auto bd = ring.on_boundary(b % 2u, b);
    CHECK_FALSE(bd.underrun);
    CHECK_FALSE(bd.zero_slot);
    SlotRing<2, 2>::Ticket t;
    CHECK(ring.begin_render(t));
    CHECK(t.seq == b);
    CHECK(t.write_slot == (b + 1u) % 2u);  // the slot that just finished: the other one
    CHECK(ring.on_time(t));
    ring.published(t);
  }
  CHECK(ring.seq() == 1000u);
  CHECK_FALSE(ring.muted());
}

TEST_CASE("slot ring: a render pended twice for one boundary runs once; none before the first boundary") {
  SlotRing<2, 2> ring;
  SlotRing<2, 2>::Ticket t;
  CHECK_FALSE(ring.begin_render(t));       // no boundary yet
  ring.on_boundary(1, 100);
  CHECK(ring.begin_render(t));
  CHECK_FALSE(ring.begin_render(t));       // the same boundary again: coalesced
  ring.on_boundary(0, 200);
  ring.on_boundary(1, 300);                // two boundaries while the render was held off: it serves the newest
  CHECK(ring.begin_render(t));
  CHECK(t.seq == 3u);
  CHECK(t.stamp == 300u);
}

TEST_CASE("slot ring: a missed boundary is noticed (the slot count and the hardware disagree) and the ring resynchronises") {
  SlotRing<4, 3> ring;
  ring.on_boundary(1, 10);
  ring.on_boundary(2, 20);
  // The ISR missed the boundary to slot 3: the hardware is at slot 0 when the count expects 3.
  const auto bd = ring.on_boundary(0, 40);
  CHECK(bd.slipped);
  CHECK(bd.seq == 3u);
  CHECK(bd.slot == 0u);
  // From now on the count follows the hardware: the next boundary is slot 1 and not a slip.
  const auto next = ring.on_boundary(1, 50);
  CHECK_FALSE(next.slipped);
  SlotRing<4, 3>::Ticket t;
  CHECK(ring.begin_render(t));
  CHECK(t.write_slot == SlotRing<4, 3>::slot_for(4u, ring.base()));
}

TEST_CASE("slot ring: an underrun is not a lost block: the slot content is stale, never torn, and the mute zeroes it") {
  SlotRing<2, 2> ring;
  SlotRing<2, 2>::Ticket t;
  // Three boundaries with no render: from boundary 2 (N = 2) each finds its block missing.
  const auto b1 = ring.on_boundary(1, 1);
  CHECK_FALSE(b1.underrun);                // the zeroed ring plays first
  const auto b2 = ring.on_boundary(0, 2);
  CHECK(b2.underrun);
  CHECK_FALSE(b2.zero_slot);
  const auto b3 = ring.on_boundary(1, 3);
  CHECK(b3.underrun);
  CHECK_FALSE(b3.zero_slot);
  CHECK_FALSE(ring.muted());
  const auto b4 = ring.on_boundary(0, 4);
  CHECK(b4.underrun);
  CHECK(b4.mute_started);                  // the third in a row
  CHECK(b4.zero_slot);                     // and the stale slot is zeroed
  CHECK(ring.muted());
  const auto b5 = ring.on_boundary(1, 5);
  CHECK(b5.underrun);
  CHECK_FALSE(b5.mute_started);            // already muted: not a new event
  CHECK(b5.zero_slot);                     // but every further stale slot is zeroed
  CHECK(ring.begin_render(t));
  CHECK(t.seq == 5u);
}

// ---- N = 3 in the 4-slot ring (the shipping configuration, OS-CPU10 slice 3): the block rendered for boundary s plays at s + 2 ----
namespace {
using Ring3 = SlotRing<4, 3>;
static_assert(o3sdk::hal::audio::kTxSlots == 3u && o3sdk::hal::audio::kTxPhysSlots == 4u, "these cases are the shipping ring");
}  // namespace

TEST_CASE("slot ring N = 3: a clean run never underruns, the first two boundaries are the zeroed ring, and the render never writes a slot that is playing or next") {
  Ring3 ring;
  for (std::uint32_t b = 1; b <= 1000; ++b) {
    const auto bd = ring.on_boundary(b % 4u, b);
    CHECK_FALSE(bd.underrun);
    CHECK_FALSE(bd.zero_slot);
    Ring3::Ticket t;
    CHECK(ring.begin_render(t));
    CHECK(t.seq == b);
    CHECK(t.write_slot == (b + 2u) % 4u);        // plays at boundary b + 2
    CHECK(t.write_slot != b % 4u);               // not the slot the DMA plays now
    CHECK(t.write_slot != (b + 1u) % 4u);        // nor the one it plays next (the block rendered one boundary ago)
    CHECK(ring.on_time(t));
    ring.published(t);
  }
  CHECK(ring.seq() == 1000u);
  CHECK_FALSE(ring.muted());
}

TEST_CASE("slot ring N = 3: a render one block late is still on time and nothing underruns (the case N = 2 loses)") {
  Ring3 ring;
  Ring3::Ticket pending{};
  bool have = false;
  for (std::uint32_t b = 1; b <= 1000; ++b) {
    const auto bd = ring.on_boundary(b % 4u, b);
    CHECK_FALSE(bd.underrun);
    if (have) {
      CHECK(ring.on_time(pending));              // boundary b has passed since the ticket of b - 1: its slot plays at b + 1
      ring.published(pending);
    }
    CHECK(ring.begin_render(pending));
    have = true;
  }
  CHECK_FALSE(ring.muted());
}

TEST_CASE("slot ring N = 3: a render two blocks late misses its slot: it is not on time (dropped), the boundary underruns once") {
  Ring3 ring;
  Ring3::Ticket t{};
  ring.on_boundary(1, 1);
  CHECK(ring.begin_render(t));                   // for boundary 1, plays at boundary 3
  CHECK(ring.on_time(t));
  const auto b2 = ring.on_boundary(2, 2);
  CHECK_FALSE(b2.underrun);                      // the zeroed ring plays
  CHECK(ring.on_time(t));                        // one boundary late: still on time
  const auto b3 = ring.on_boundary(3, 3);
  CHECK(b3.underrun);                            // the slot of boundary 3 should hold the block of boundary 1
  CHECK_FALSE(ring.on_time(t));                  // the slot has started to play: the late block must not be written
  CHECK(ring.slot_seq(3) == 0u);                 // and it was not
  CHECK(ring.begin_render(t));                   // the render then serves the newest boundary
  CHECK(t.seq == 3u);
  CHECK(ring.on_time(t));
  ring.published(t);
  CHECK(ring.on_boundary(0, 4).underrun);        // boundary 2's block was skipped: a second underrun in a row, still below the mute
  CHECK_FALSE(ring.muted());
  CHECK_FALSE(ring.on_boundary(1, 5).underrun);  // boundary 3's block plays at 5
}

TEST_CASE("slot ring N = 3: three underruns in a row mute; the stale slot is zeroed; the mute lifts after kUnmuteAfter on-time boundaries") {
  Ring3 ring;
  CHECK_FALSE(ring.on_boundary(1, 1).underrun);  // boundaries 1 and 2: the zeroed ring (N - 1 = 2)
  CHECK_FALSE(ring.on_boundary(2, 2).underrun);
  const auto b3 = ring.on_boundary(3, 3);
  CHECK(b3.underrun);
  CHECK_FALSE(b3.zero_slot);
  const auto b4 = ring.on_boundary(0, 4);
  CHECK(b4.underrun);
  CHECK_FALSE(ring.muted());
  const auto b5 = ring.on_boundary(1, 5);
  CHECK(b5.underrun);
  CHECK(b5.mute_started);                        // the third in a row
  CHECK(b5.zero_slot);
  CHECK(ring.muted());
  // The render comes back, serving the newest boundary (5, plays at 7). Boundary 6 still lacks its block (4): one more zeroed
  // underrun; from boundary 7 on every block is on time and the mute lifts after kUnmuteAfter of them.
  Ring3::Ticket t;
  CHECK(ring.begin_render(t));
  CHECK(t.seq == 5u);
  ring.published(t);
  const auto b6 = ring.on_boundary(2, 6);
  CHECK(b6.underrun);
  CHECK(b6.zero_slot);
  CHECK_FALSE(b6.mute_started);
  CHECK(ring.begin_render(t));
  CHECK(t.seq == 6u);
  ring.published(t);
  const std::uint32_t kLast = 6u + Ring3::kUnmuteAfter;
  for (std::uint32_t b = 7; b <= kLast; ++b) {
    const auto bd = ring.on_boundary(b % 4u, b);
    CHECK_FALSE(bd.underrun);
    CHECK(ring.muted() == (b < kLast));
    CHECK(ring.begin_render(t));
    CHECK(t.seq == b);
    ring.published(t);
  }
  CHECK_FALSE(ring.muted());
}
