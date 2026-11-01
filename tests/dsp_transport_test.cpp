// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/dsp/transport.h>
#include <o3sdk/hal/audio.h>

#include "support/exact_grid.h"
#include "support/need.h"
#include "support/rng.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace o3sdk;

namespace {

// Steps of a grid seen by a run of `blocks` blocks of `frames`, as absolute sample positions.
std::vector<std::uint64_t> walk(std::uint32_t mbpm, std::uint32_t period, std::uint32_t offset, std::uint32_t swing, unsigned blocks,
                                std::uint32_t frames, std::vector<std::uint64_t>* indices = nullptr) {
  dsp::Transport t;
  t.playing = true;
  t.tempo_mbpm = mbpm;
  t.inc = dsp::ticks_per_sample_q32(mbpm);
  t.inc_lo = dsp::ticks_per_sample_lo(mbpm);  // (what the engine's own transport carries)
  t.frames = frames;
  std::vector<std::uint64_t> at;
  for (unsigned b = 0; b < blocks; ++b) {
    dsp::for_each_step(t, period, offset, swing, [&](std::uint32_t frame, std::uint64_t k) {
      at.push_back(static_cast<std::uint64_t>(b) * frames + frame);
      if (indices != nullptr) {
        indices->push_back(k);
      }
    });
    t.advance(frames);
  }
  return at;
}

// The Transport a hook sees for the block the engine just rendered (Engine::block_frame()), with the previous block's bound.
dsp::Transport transport_of(const core::ClockFrame& f, std::uint32_t frames) {
  dsp::Transport t;
  t.playing = f.playing;
  t.started = f.started;
  t.stopped = f.stopped;
  t.tempo_mbpm = f.tempo_mbpm;
  t.pos = f.pos;
  t.inc = f.inc;
  t.pos_lo = f.pos_lo;
  t.inc_lo = f.inc_lo;
  t.prev_gap = f.prev_gap;
  t.frames = frames;
  t.following = f.follow;
  return t;
}

}  // namespace

TEST_CASE("transport: a minute at 120 BPM is 120 quarters to the tick") {
  const std::uint64_t inc = dsp::ticks_per_sample_q32(120'000);
  const auto rate = static_cast<std::uint64_t>(core::kRefRateHz + 0.5f);
  const std::uint64_t pos = inc * rate * 60u;
  const std::uint64_t ticks = pos >> 32;
  CHECK(ticks >= 120u * dsp::kPpqn - 1u);
  CHECK(ticks <= 120u * dsp::kPpqn);
}

TEST_CASE("transport: tempo is clamped to 20..300 BPM") {
  CHECK(dsp::ticks_per_sample_q32(1'000) == dsp::ticks_per_sample_q32(dsp::kMinTempoMilliBpm));
  CHECK(dsp::ticks_per_sample_q32(900'000) == dsp::ticks_per_sample_q32(dsp::kMaxTempoMilliBpm));
  CHECK(dsp::ticks_per_sample_q32(240'000) > dsp::ticks_per_sample_q32(120'000));
}

TEST_CASE("transport: frame_at finds a tick inside the block, and nothing outside it") {
  dsp::Transport t;
  t.playing = true;
  t.inc = dsp::ticks_per_sample_q32(120'000);
  t.frames = 64;
  t.pos = 0;
  CHECK(dsp::frame_at(t, 0) == 0);
  const std::uint64_t last = (t.end() - 1u) >> 32;
  CHECK(dsp::frame_at(t, last) >= 0);
  CHECK(dsp::frame_at(t, last + 1u) == -1);
  t.playing = false;
  CHECK(dsp::frame_at(t, 0) == -1);
}

TEST_CASE("transport: sixteenths at 120 BPM land every 5512.5 samples, each exactly once, across block edges") {
  std::vector<std::uint64_t> k;
  const auto at = walk(120'000, dsp::kTicksPerSixteenth, 0, 0, 2000, 64, &k);
  REQUIRE(at.size() >= 23u);
  for (std::size_t i = 0; i < at.size(); ++i) {
    CHECK(k[i] == i);  // in order, none skipped, none twice
    const double expected = static_cast<double>(i) * 5512.5;
    CHECK(static_cast<double>(at[i]) >= expected - 1.0);
    CHECK(static_cast<double>(at[i]) <= expected + 1.0);
  }
}

TEST_CASE("transport: the grid does not depend on the block size") {
  const auto a = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 4000, 64);
  const auto b = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 8000, 32);
  const auto c = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 256000, 1);
  REQUIRE(a.size() >= 30u);
  for (std::size_t i = 0; i < 30; ++i) {
    CHECK(a[i] == b[i]);
    CHECK(a[i] == c[i]);
  }
}

TEST_CASE("transport: swing delays the odd steps only, by its ticks; an offset moves every step") {
  const auto straight = walk(120'000, dsp::kTicksPerSixteenth, 0, 0, 1000, 64);
  const auto swung = walk(120'000, dsp::kTicksPerSixteenth, 0, 80, 1000, 64);
  const auto late = walk(120'000, dsp::kTicksPerSixteenth, 120, 0, 1000, 64);
  REQUIRE(straight.size() >= 8u);
  REQUIRE(swung.size() >= 8u);
  REQUIRE(late.size() >= 8u);
  const double per_tick = 5512.5 / 240.0;
  for (std::size_t i = 0; i < 8; ++i) {
    const double shift = (i % 2u == 1u) ? 80.0 * per_tick : 0.0;
    CHECK(static_cast<double>(swung[i]) == doctest::Approx(static_cast<double>(straight[i]) + shift).epsilon(0.0005));
    CHECK(static_cast<double>(late[i]) == doctest::Approx(static_cast<double>(straight[i]) + 120.0 * per_tick).epsilon(0.0005));
  }
}

TEST_CASE("transport: samples_per_sixteenth matches the grid") {
  CHECK(dsp::samples_per_sixteenth(120'000) == doctest::Approx(5512.5f));
  CHECK(dsp::samples_per_sixteenth(60'000) == doctest::Approx(11025.0f));
}

// ---- Exact landing (dsp/transport.h): every step on the sample the rational arithmetic gives ------------------------------------------------------------------------------------

TEST_CASE("transport: ticks_per_sample_lo rounds the Q32.64 increment up, never below the exact rate and never a unit above it") {
  const std::uint64_t den = 60u * test::kRateMilliHz;
  unsigned long checked = 0;
  for (std::uint32_t mbpm = dsp::kMinTempoMilliBpm; mbpm <= dsp::kMaxTempoMilliBpm; mbpm += 7u) {
    const unsigned __int128 inc = (static_cast<unsigned __int128>(dsp::ticks_per_sample_q32(mbpm)) << 32) | dsp::ticks_per_sample_lo(mbpm);
    const unsigned __int128 exact = (static_cast<unsigned __int128>(mbpm) * dsp::kPpqn) << 64;  // the rate times den, in Q32.64
    const unsigned __int128 got = inc * den;
    CHECK(got >= exact);
    CHECK(got - exact < den);  // above it by less than one unit of the last place
    ++checked;
  }
  CHECK(checked > 40000u);
  // A rate that is exact has nothing to round: 120 BPM is 2 quarters a second, 1920 ticks, over 44,100 samples is not whole, but 125 BPM at 5,292 a sixteenth is 240 / 5,292 exactly.
  CHECK(dsp::ticks_per_sample_lo(dsp::kMinTempoMilliBpm - 1u) == dsp::ticks_per_sample_lo(dsp::kMinTempoMilliBpm));  // clamped like the Q32 word
}

TEST_CASE("transport: advance adds n increments to the Q32.64 position, whatever the block size") {
  test::Rng rng(5);
  for (const std::uint32_t mbpm : {20'000u, 98'135u, 125'000u, 133'333u, 299'999u}) {
    dsp::Transport t;
    t.inc = dsp::ticks_per_sample_q32(mbpm);
    t.inc_lo = dsp::ticks_per_sample_lo(mbpm);
    unsigned __int128 want = 0;
    const unsigned __int128 inc = (static_cast<unsigned __int128>(t.inc) << 32) | t.inc_lo;
    for (unsigned i = 0; i < 2000; ++i) {
      const std::uint32_t n = 1u + rng.below(1024);
      t.advance(n);
      want += inc * n;
      CHECK(t.pos == static_cast<std::uint64_t>(want >> 32));
      CHECK(t.pos_lo == static_cast<std::uint32_t>(want));
    }
    t.frames = 64;
    CHECK(t.at(0) == t.pos);
    CHECK(t.last() == t.at(63));
    CHECK(t.end() == t.at(64));
  }
}

TEST_CASE("transport: a take of n bars that starts on Play's first sample is exactly n bars, at tempos whose sixteenths are whole samples") {
  for (const std::uint32_t mbpm : {125'000u, 150'000u, 100'000u, 90'000u, 180'000u}) {
    const std::uint32_t sixteenth = static_cast<std::uint32_t>(test::exact_frame(mbpm, dsp::kTicksPerSixteenth));
    CHECK(test::exact_frame(mbpm, 2u * dsp::kTicksPerSixteenth) == 2u * sixteenth);  // (whole: every sixteenth is the same number of samples)
    for (const unsigned bars : {1u, 2u, 4u, 8u}) {
      INFO("tempo " << mbpm << " bars " << bars);
      for (const std::uint32_t frames : {64u, 128u, 1000u}) {
        const auto at = walk(mbpm, dsp::kTicksPerSixteenth, 0, 0, bars * 16u * sixteenth / frames + 3u, frames);
        NEED(at.size() > bars * 16u);
        CHECK(at[0] == 0u);                                              // Play's first sample
        CHECK(at[bars * 16u] == static_cast<std::uint64_t>(bars) * 16u * sixteenth);  // the take's end: exactly n bars on, not one sample later
      }
    }
  }
  CHECK(test::exact_frame(125'000u, 16u * dsp::kTicksPerSixteenth) == 84'672u);  // the one-bar take of the unit test (U5 row 33)
}

// Every step of a grid at one tempo, over `bars` bars, against the rational: nothing early, nothing late. `swing` and `offset` as for_each_step.
static void check_grid(std::uint32_t mbpm, unsigned bars, std::uint32_t frames, std::uint32_t offset, std::uint32_t swing, unsigned long& steps, unsigned long& bad) {
  const std::uint32_t sixteenth = static_cast<std::uint32_t>(test::exact_frame(mbpm, dsp::kTicksPerSixteenth) + 1u);  // (an upper bound of a step, for the run's length)
  std::vector<std::uint64_t> k;
  const auto at = walk(mbpm, dsp::kTicksPerSixteenth, offset, swing, bars * 16u * sixteenth / frames + 2u, frames, &k);
  for (std::size_t i = 0; i < at.size() && k[i] < bars * 16u; ++i) {
    const std::uint64_t tick = k[i] * dsp::kTicksPerSixteenth + offset + ((k[i] & 1u) != 0u ? swing : 0u);
    ++steps;
    if (at[i] != test::exact_frame(mbpm, tick)) {
      ++bad;
    }
  }
}

TEST_CASE("transport: over 16 bars every step lands on the sample the exact arithmetic gives, at every tempo the engine allows") {
  unsigned long steps = 0;
  unsigned long bad = 0;
  for (std::uint32_t mbpm = dsp::kMinTempoMilliBpm; mbpm <= dsp::kMaxTempoMilliBpm; mbpm += 1000u) {  // every whole BPM
    check_grid(mbpm, 16, 1024, 0, 0, steps, bad);
  }
  // The tempos where the sixteenth is not whole, where a rounded-up increment is early within the first bar (98.135 BPM, step 7), and the ends of the range.
  test::Rng rng(11);
  std::vector<std::uint32_t> tempos = {20'000u, 20'001u, 98'135u, 119'999u, 120'001u, 133'333u, 299'999u, 300'000u};
  for (unsigned i = 0; i < 300; ++i) {
    tempos.push_back(dsp::kMinTempoMilliBpm + rng.below(dsp::kMaxTempoMilliBpm - dsp::kMinTempoMilliBpm + 1u));
  }
  for (const std::uint32_t mbpm : tempos) {
    check_grid(mbpm, 16, 64, 0, 0, steps, bad);
  }
  CHECK(steps > 100000u);
  CHECK(bad == 0u);
}

TEST_CASE("transport: a long run (a thousand bars) stays on the exact sample, and so do swing and an offset") {
  unsigned long steps = 0;
  unsigned long bad = 0;
  for (const std::uint32_t mbpm : {125'000u, 98'135u, 133'333u, 299'999u, 20'001u, 150'000u}) {
    check_grid(mbpm, mbpm < 50'000u ? 100u : 1000u, 1000, 0, 0, steps, bad);
  }
  for (const std::uint32_t mbpm : {125'000u, 98'135u, 133'333u, 299'999u}) {
    check_grid(mbpm, 64, 192, 0, 80, steps, bad);    // swung
    check_grid(mbpm, 64, 192, 120, 119, steps, bad);  // half a step late, and swung
  }
  CHECK(steps > 50000u);
  CHECK(bad == 0u);
}

TEST_CASE("transport: every tick is reached by exactly one block, at the frame the exact arithmetic gives, with the low words") {
  for (const std::uint32_t mbpm : {98'135u, 125'000u, 20'000u, 300'000u}) {
    for (const std::uint32_t frames : {64u, 97u}) {
      dsp::Transport t;
      t.playing = true;
      t.inc = dsp::ticks_per_sample_q32(mbpm);
      t.inc_lo = dsp::ticks_per_sample_lo(mbpm);
      t.frames = frames;
      const std::uint64_t ticks = 3000;  // the first 3,000 ticks: the blocks that reach them
      std::uint64_t base = 0;            // samples before this block
      std::vector<unsigned> owners(ticks + 1u, 0u);
      while ((t.pos >> 32) <= ticks) {
        const std::uint64_t first = (t.pos >> 32) > 2u ? (t.pos >> 32) - 2u : 0u;
        const std::uint64_t past = (t.end() >> 32) + 2u < ticks ? (t.end() >> 32) + 2u : ticks;
        for (std::uint64_t tick = first; tick <= past; ++tick) {  // (the ticks around this block: no other block can own one)
          const int f = dsp::frame_at(t, tick);
          if (f >= 0) {
            ++owners[tick];
            CHECK(base + static_cast<std::uint64_t>(f) == test::exact_frame(mbpm, tick));
          }
        }
        base += frames;
        t.advance(frames);
      }
      for (std::uint64_t tick = 0; tick <= ticks; ++tick) {
        CHECK(owners[tick] == 1u);
      }
    }
  }
}

TEST_CASE("transport: a Transport without low words (a master's, or one built by hand) behaves as it did") {
  dsp::Transport t;
  t.playing = true;
  t.inc = dsp::ticks_per_sample_q32(125'000u);
  t.frames = 64;
  CHECK(t.pos_lo == 0u);
  CHECK(t.inc_lo == 0u);
  CHECK(t.last() == t.pos + t.inc * 63u);
  CHECK(t.end() == t.pos + t.inc * 64u);
  CHECK(dsp::frame_at(t, 0) == 0);
  // A floored increment lands the whole-sample step one sample late: the behaviour the low words exist to avoid (and the master's grid keeps, being its own).
  t.pos = 0;
  std::uint64_t block = 0;
  while (dsp::frame_at(t, dsp::kTicksPerSixteenth) < 0) {
    t.pos += t.inc * 64u;
    ++block;
  }
  CHECK(block * 64u + static_cast<std::uint64_t>(dsp::frame_at(t, dsp::kTicksPerSixteenth)) == 5293u);
}

// ---- A tempo change in the middle of a run, on the engine's own transport ---------------------------------------------------------

TEST_CASE("engine: a tempo change in the middle of a run keeps every step on the exact sample, before and after it, none lost and none doubled") {
  constexpr std::uint32_t kN = hal::audio::kBlock;
  struct Segment {
    std::uint32_t mbpm;
    unsigned blocks;
  };
  // Down, up, to the extremes, to a tempo whose sixteenth is whole samples (125 BPM: 5,292), and back: each change lands on frame 0 of a block (Engine::set_tempo is applied there).
  // Seven changes cannot meet the boundary between two blocks' ownership (a tick within an increment of it): the sweep further down does, thousands of times.
  const std::vector<Segment> plan = {{120'000, 700}, {133'333, 500}, {20'000, 600}, {300'000, 900}, {125'000, 1000}, {98'135, 700}, {125'000, 400}};
  auto engine = std::make_unique<dsp::Engine>();
  engine->start();
  REQUIRE(engine->set_tempo(plan[0].mbpm));
  REQUIRE(engine->play());

  // The exact model: the position in units of 1 / (60 * rate_mhz) ticks, piecewise linear with a slope of mbpm * 960 per sample, continuous at each change. Segment i owns the samples
  // [begin_i, begin_i + blocks_i * kN); a tick is reached by the first sample whose position is at or above it.
  struct Seg {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t pos0;  // the position at `begin`
    std::uint64_t slope;
  };
  std::vector<Seg> segs;
  std::uint64_t at = 0;
  std::uint64_t pos = 0;
  for (const Segment& g : plan) {
    const std::uint64_t slope = static_cast<std::uint64_t>(g.mbpm) * dsp::kPpqn;
    const std::uint64_t len = static_cast<std::uint64_t>(g.blocks) * kN;
    segs.push_back({at, at + len, pos, slope});
    pos += len * slope;
    at += len;
  }
  const std::uint64_t scale = 60u * test::kRateMilliHz;
  const auto exact_sample = [&](std::uint64_t tick) -> std::uint64_t {
    const std::uint64_t target = tick * scale;
    for (const Seg& g : segs) {
      const std::uint64_t last = g.pos0 + (g.end - 1u - g.begin) * g.slope;  // the position of the segment's last sample
      if (last >= target || &g == &segs.back()) {
        return g.begin + (target <= g.pos0 ? 0u : (target - g.pos0 + g.slope - 1u) / g.slope);
      }
    }
    return 0;
  };

  std::vector<std::uint64_t> got;
  std::vector<std::uint64_t> idx;
  std::uint64_t sample = 0;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    if (i != 0u) {
      REQUIRE(engine->set_tempo(plan[i].mbpm));
    }
    for (unsigned b = 0; b < plan[i].blocks; ++b) {
      float l[kN];
      float r[kN];
      engine->render(nullptr, nullptr, l, r, kN);
      const core::ClockFrame& f = engine->block_frame();
      REQUIRE(f.tempo_mbpm == plan[i].mbpm);
      const dsp::Transport t = transport_of(f, kN);
      dsp::for_each_step(t, dsp::kTicksPerSixteenth, 0u, 0u, [&](std::uint32_t frame, std::uint64_t k) {
        got.push_back(sample + frame);
        idx.push_back(k);
      });
      sample += kN;
    }
  }
  REQUIRE(sample == at);
  REQUIRE(got.size() > 60u);
  // none lost and none doubled: the steps are 0, 1, 2, ... in order, and every one is on the exact sample
  unsigned bad = 0;
  for (std::size_t j = 0; j < got.size(); ++j) {
    bad += idx[j] != j ? 1u : 0u;
    bad += got[j] != exact_sample(idx[j] * dsp::kTicksPerSixteenth) ? 1u : 0u;
  }
  CHECK(bad == 0u);
  // and the run reached the step the model says it should have by its last sample
  std::uint64_t expect_steps = 0;
  while (exact_sample(expect_steps * dsp::kTicksPerSixteenth) < at) {
    ++expect_steps;
  }
  CHECK(got.size() == expect_steps);
}

// ---- A tempo change between two blocks: every tick has one owner (Transport::prev_gap) ------------------------------------------------

namespace {

// The exact position of the engine's own transport, in units of 1 / (60 * rate_mhz) ticks: piecewise linear, a block of tempo T adds kN * T * 960 a block, continuous at each change
// (the engine applies a change at frame 0 of a block, whose position is still the old increment's end). exact_sample(tick) is the first sample whose position reaches the tick.
class ExactRun {
 public:
  void add_block(std::uint32_t mbpm) {
    const std::uint64_t slope = static_cast<std::uint64_t>(mbpm) * dsp::kPpqn;
    pos0_.push_back(pos_);
    slope_.push_back(slope);
    pos_ += slope * hal::audio::kBlock;
  }
  std::uint64_t samples() const { return static_cast<std::uint64_t>(pos0_.size()) * hal::audio::kBlock; }
  std::uint64_t exact_sample(std::uint64_t tick) const {
    constexpr std::uint64_t kN = hal::audio::kBlock;
    const std::uint64_t target = tick * 60u * test::kRateMilliHz;
    // the first block whose last sample reaches the target (positions grow with the block index); past the last, the last block's slope goes on
    std::size_t lo = 0;
    std::size_t hi = pos0_.size();
    while (lo + 1u < hi) {
      const std::size_t mid = (lo + hi) / 2u;
      if (pos0_[mid - 1u] + (kN - 1u) * slope_[mid - 1u] >= target) {
        hi = mid;
      } else {
        lo = mid;
      }
    }
    const std::size_t b = (lo + 1u == hi && pos0_[lo] + (kN - 1u) * slope_[lo] >= target) || lo + 1u == pos0_.size() ? lo : lo + 1u;
    return b * kN + (target <= pos0_[b] ? 0u : (target - pos0_[b] + slope_[b] - 1u) / slope_[b]);
  }

 private:
  std::vector<std::uint64_t> pos0_;
  std::vector<std::uint64_t> slope_;
  std::uint64_t pos_ = 0;
};

struct Grid {
  std::uint32_t period;
  std::uint32_t offset;
  std::uint32_t swing;
  std::uint64_t tick(std::uint64_t k) const { return k * period + offset + ((k & 1u) != 0u ? swing : 0u); }
};

}  // namespace

// A change at every block boundary: random tempo, random phase (the position at the boundary is wherever the last blocks left it). A tick falls between the last frame of block A and the
// first of block B (inside an increment of the boundary) with a probability of about the change of increment over the grid, so a hundred thousand changes meet it on the 1-tick grid a
// few thousand times, on the 40-tick (MIDI clock) grid a few hundred, on the 240-tick (a sixteenth) a few tens. Each grid sees its steps 0, 1, 2, ... in order, each exactly once
// (a tempo up used to play one twice, a tempo down to lose one), and on the sample the exact arithmetic gives.
TEST_CASE("engine: a tempo change between any two blocks leaves every tick with exactly one owner, on the 1-tick, the 40-tick and the 240-tick grids, at random phases and tempos") {
  constexpr std::uint32_t kN = hal::audio::kBlock;
  constexpr unsigned kBlocks = 150'000;
  const Grid grids[3] = {{1, 0, 0}, {dsp::kTicksPerMidiClock, 0, 0}, {dsp::kTicksPerSixteenth, 0, 90}};  // (the sixteenths swung to 62.5 %)
  std::vector<std::uint64_t> got[3];
  std::vector<std::uint64_t> idx[3];
  std::uint64_t gap[3] = {0, 0, 0};  // ticks owned at frame 0 although before the block's position: between the last frame of one block and the first of the next
  auto engine = std::make_unique<dsp::Engine>();
  engine->start();
  REQUIRE(engine->set_tempo(120'000));
  REQUIRE(engine->play());
  test::Rng rng(0xC0FFEEu);
  ExactRun run;
  unsigned changes = 0;
  unsigned continuous = 0;
  std::uint32_t mbpm = 120'000;
  for (unsigned b = 0; b < kBlocks; ++b) {
    std::uint32_t next = mbpm;
    switch (b == 0u ? 3u : rng.below(4)) {
      case 0:  // anywhere in the range
        next = dsp::kMinTempoMilliBpm + rng.below(dsp::kMaxTempoMilliBpm - dsp::kMinTempoMilliBpm + 1u);
        break;
      case 1:  // a slew: a fraction of a BPM either way
        next = mbpm + rng.below(2001u) - 1000u;
        break;
      case 2:  // a few BPM
        next = mbpm + rng.below(20001u) - 10000u;
        break;
      default:  // no change
        break;
    }
    next = next < dsp::kMinTempoMilliBpm ? dsp::kMinTempoMilliBpm : (next > dsp::kMaxTempoMilliBpm ? dsp::kMaxTempoMilliBpm : next);
    if (next != mbpm) {
      REQUIRE(engine->set_tempo(next));
      ++changes;
    }
    mbpm = next;
    float l[kN];
    float r[kN];
    engine->render(nullptr, nullptr, l, r, kN);
    const core::ClockFrame& f = engine->block_frame();
    REQUIRE(f.tempo_mbpm == mbpm);
    run.add_block(mbpm);
    const dsp::Transport t = transport_of(f, kN);
    CHECK(t.has_prev() == (b != 0u));  // (every block after Play's continues the run)
    continuous += t.has_prev() ? 1u : 0u;
    for (int g = 0; g < 3; ++g) {
      dsp::for_each_step(t, grids[g].period, grids[g].offset, grids[g].swing, [&](std::uint32_t frame, std::uint64_t k) {
        got[g].push_back(static_cast<std::uint64_t>(b) * kN + frame);
        idx[g].push_back(k);
        gap[g] += frame == 0u && (grids[g].tick(k) << 32) < t.pos ? 1u : 0u;
      });
    }
  }
  MESSAGE("changes ", changes, ", ticks between two blocks: 1-tick ", gap[0], ", 40-tick ", gap[1], ", 240-tick ", gap[2]);
  CHECK(changes > 80'000u);
  CHECK(gap[0] > 1000u);  // (the ticks the sweep exists for: with a change of increment before the block, the one the old bound of each block lost or owned twice)
  CHECK(gap[1] > 20u);
  CHECK(gap[2] > 3u);
  CHECK(continuous == kBlocks - 1u);
  const std::uint64_t total = run.samples();
  for (int g = 0; g < 3; ++g) {
    unsigned bad = 0;
    for (std::size_t j = 0; j < got[g].size(); ++j) {
      bad += idx[g][j] != j ? 1u : 0u;  // none lost, none doubled, in order
      bad += got[g][j] != run.exact_sample(grids[g].tick(idx[g][j])) ? 1u : 0u;
    }
    CHECK(bad == 0u);
    std::uint64_t expect = 0;  // and no step the exact arithmetic reaches inside the run is missing
    while (run.exact_sample(grids[g].tick(expect)) < total) {
      ++expect;
    }
    CHECK(got[g].size() == expect);
    CHECK(got[g].size() > (g == 0 ? 100'000u : 1'000u));
  }
}

// The same sweep on a transport a master gives (Engine::follow: its own position and increment, integers, the increment slewed from block to block and held to the engine's range).
TEST_CASE("engine: a master that slews its increment from block to block leaves every tick with exactly one owner, on the same three grids") {
  constexpr std::uint32_t kN = hal::audio::kBlock;
  constexpr unsigned kBlocks = 60'000;
  const Grid grids[3] = {{1, 0, 0}, {dsp::kTicksPerMidiClock, 0, 0}, {dsp::kTicksPerSixteenth, 0, 90}};
  std::vector<std::uint64_t> got[3];
  std::vector<std::uint64_t> idx[3];
  auto engine = std::make_unique<dsp::Engine>();
  engine->start();
  test::Rng rng(0xBEEF5u);
  const std::uint64_t inc_lo = dsp::ticks_per_sample_q32(dsp::kMinTempoMilliBpm);
  const std::uint64_t inc_hi = dsp::ticks_per_sample_q32(dsp::kMaxTempoMilliBpm);
  std::uint64_t pos = 0;
  std::uint64_t inc = dsp::ticks_per_sample_q32(120'000);
  // the master's position at the start of each block, and its increment: a tick is reached by the first sample with pos + f * inc >= tick << 32 (whole numbers: exact)
  std::vector<std::uint64_t> pos_of;
  std::vector<std::uint64_t> inc_of;
  for (unsigned b = 0; b < kBlocks; ++b) {
    switch (rng.below(3)) {
      case 0:
        inc = inc_lo + (static_cast<std::uint64_t>(rng.next()) % (inc_hi - inc_lo));
        break;
      case 1:  // a slew of a few thousandths of a percent
        inc += static_cast<std::uint64_t>(rng.below(1u << 20)) - (1u << 19);
        break;
      default:
        break;
    }
    inc = inc < inc_lo ? inc_lo : (inc > inc_hi ? inc_hi : inc);
    core::ClockFrame m;
    m.follow = true;
    m.playing = true;
    m.started = b == 0u;
    m.pos = pos;
    m.inc = inc;
    m.tempo_mbpm = 120'000;
    engine->follow(m);
    float l[kN];
    float r[kN];
    engine->render(nullptr, nullptr, l, r, kN);
    const core::ClockFrame& f = engine->block_frame();
    REQUIRE(f.follow);
    REQUIRE(f.pos == pos);
    REQUIRE(f.inc == inc);
    const dsp::Transport t = transport_of(f, kN);
    CHECK(t.has_prev() == (b != 0u));
    pos_of.push_back(pos);
    inc_of.push_back(inc);
    for (int g = 0; g < 3; ++g) {
      dsp::for_each_step(t, grids[g].period, grids[g].offset, grids[g].swing, [&](std::uint32_t frame, std::uint64_t k) {
        got[g].push_back(static_cast<std::uint64_t>(b) * kN + frame);
        idx[g].push_back(k);
      });
    }
    pos += inc * kN;  // (a master's position is continuous: the sum of its increments)
  }
  const auto exact_sample = [&](std::uint64_t tick) -> std::uint64_t {
    const std::uint64_t q = tick << 32;
    std::size_t lo = 0;  // the first block whose last sample reaches q
    std::size_t hi = pos_of.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2u;
      if (pos_of[mid] + (kN - 1u) * inc_of[mid] >= q) {
        hi = mid;
      } else {
        lo = mid + 1u;
      }
    }
    if (lo == pos_of.size()) {
      return ~std::uint64_t{0};  // (beyond the run)
    }
    return lo * kN + (q <= pos_of[lo] ? 0u : (q - pos_of[lo] + inc_of[lo] - 1u) / inc_of[lo]);
  };
  const std::uint64_t total = static_cast<std::uint64_t>(kBlocks) * kN;
  for (int g = 0; g < 3; ++g) {
    unsigned bad = 0;
    for (std::size_t j = 0; j < got[g].size(); ++j) {
      bad += idx[g][j] != j ? 1u : 0u;
      bad += got[g][j] != exact_sample(grids[g].tick(idx[g][j])) ? 1u : 0u;
    }
    CHECK(bad == 0u);
    std::uint64_t expect = 0;
    while (exact_sample(grids[g].tick(expect)) < total) {
      ++expect;
    }
    CHECK(got[g].size() == expect);
    CHECK(got[g].size() > 500u);
  }
}

TEST_CASE("engine: Play, Stop, a master taking over and letting go, and a locate give the next block no previous one; a steady run keeps it") {
  constexpr std::uint32_t kN = hal::audio::kBlock;
  auto engine = std::make_unique<dsp::Engine>();
  engine->start();
  float l[kN];
  float r[kN];
  const auto block = [&]() -> const core::ClockFrame& {
    engine->render(nullptr, nullptr, l, r, kN);
    return engine->block_frame();
  };
  CHECK_FALSE(block().prev_gap != 0u);  // stopped
  REQUIRE(engine->play());
  CHECK_FALSE(block().prev_gap != 0u);  // the first block of a run
  for (int i = 0; i < 5; ++i) {
    const core::ClockFrame& f = block();
    CHECK(f.prev_gap != 0u);
    CHECK(f.prev_gap >= f.inc);
    CHECK(f.prev_gap <= f.inc + 1u);  // (the previous block's last frame is one increment behind: a unit of the low words at most)
  }
  REQUIRE(engine->stop());
  CHECK_FALSE(block().prev_gap != 0u);
  REQUIRE(engine->play());
  CHECK_FALSE(block().prev_gap != 0u);  // Play again: position 0
  CHECK(block().prev_gap != 0u);
  // a master takes over mid-run, at some other position
  core::ClockFrame m;
  m.follow = true;
  m.playing = true;
  m.pos = 5000ull << 32;
  m.inc = dsp::ticks_per_sample_q32(140'000);
  m.tempo_mbpm = 140'000;
  engine->follow(m);
  CHECK_FALSE(block().prev_gap != 0u);  // (it is not the run the engine had)
  for (int i = 0; i < 3; ++i) {
    m.pos += m.inc * kN;
    engine->follow(m);
    CHECK(block().prev_gap != 0u);
  }
  // a locate: the master's position jumps (a Song Position, a Continue elsewhere)
  m.pos = 9000ull << 32;
  engine->follow(m);
  CHECK_FALSE(block().prev_gap != 0u);
  m.pos += m.inc * kN;
  engine->follow(m);
  CHECK(block().prev_gap != 0u);
  // a locate backwards, and one by less than a tick (never a legal Song Position, but it must not carry a bound across)
  m.pos -= 3u * m.inc;
  engine->follow(m);
  CHECK_FALSE(block().prev_gap != 0u);
  // the master lets go: the engine's own transport stops
  const core::ClockFrame& after = block();
  CHECK_FALSE(after.follow);
  CHECK_FALSE(after.prev_gap != 0u);
  CHECK_FALSE(after.playing);
}
