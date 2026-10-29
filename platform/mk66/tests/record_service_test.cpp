// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record service (platform/mk66/audio/record_service.h; docs/M2C-PLAN.md section 6, slice RC): hal::record::Recorder over RecTap and the wrap of the render callback, on a real
// engine's transport. What it proves: an idle recorder changes nothing and does nothing (the golden and a work counter); a take of the input or of the master is the block as it was,
// in order; an aligned take starts AT THE SAMPLE of the transport's sixteenth (the master carries the sample's own index, so one sample off is a failure) and ends the same way; a
// Bar and a Pattern are the transport's and the app's pattern length; a transport that stops ends a take with a stop pending; a Level take keeps its pre-roll in order; a full buffer is
// counted and never written past. The unit's own costs (cpu while recording, the codec's lag) are for the unit.
#include <doctest/doctest.h>

#include <o3sdk/dsp/engine.h>
#include <o3sdk/dsp/transport.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/record.h>
#include <o3sdk/platform/mk66/audio/record_service.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3sdk;
namespace hr = o3sdk::hal::record;
using o3sdk::platform::mk66::audio::RecordService;

namespace {

constexpr std::uint32_t kN = hal::audio::kBlock;

// What the master carries: the sample's own index (15 bits, so it fits an int16 and is never negative), the right channel its complement.
std::int16_t master_l(std::uint64_t n) { return static_cast<std::int16_t>(n & 0x7FFFu); }
std::int16_t master_r(std::uint64_t n) { return static_cast<std::int16_t>(-static_cast<std::int32_t>(n & 0x7FFFu)); }
// What the codec delivers by default: two different ramps.
std::int16_t ramp_l(std::uint64_t n, int) { return static_cast<std::int16_t>((n * 3u + 1u) & 0x7FFFu); }
std::int16_t ramp_r(std::uint64_t n, int) { return static_cast<std::int16_t>((n * 5u + 2u) & 0x7FFFu); }

struct Rig {
  using InFn = std::int16_t (*)(std::uint64_t n, int channel);

  std::unique_ptr<dsp::Engine> engine = std::make_unique<dsp::Engine>();
  RecordService svc;
  hal::audio::BlockCallback cb{};
  std::uint64_t pos = 0;  // samples rendered: the index the master carries
  std::uint32_t inner_calls = 0;
  InFn in_fn = nullptr;   // null: the two ramps
  alignas(4) std::int16_t in[2u * kN] = {};
  alignas(4) std::int16_t out[2u * kN] = {};
  std::vector<std::int16_t> heard;  // every block's out, as the main output plays it

  Rig() {
    engine->start();
    cb = svc.wrap(hal::audio::BlockCallback{&Rig::inner, this}, *engine);
  }
  static void inner(void* user, const hal::audio::BlockIo& io) noexcept {
    Rig& r = *static_cast<Rig*>(user);
    float a[kN];
    float b[kN];
    r.engine->render(nullptr, nullptr, a, b, kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
      io.out[2u * i] = master_l(r.pos + i);
      io.out[2u * i + 1u] = master_r(r.pos + i);
    }
    ++r.inner_calls;
  }
  void block() {
    for (std::uint32_t i = 0; i < kN; ++i) {
      in[2u * i] = in_fn != nullptr ? in_fn(pos + i, 0) : ramp_l(pos + i, 0);
      in[2u * i + 1u] = in_fn != nullptr ? in_fn(pos + i, 1) : ramp_r(pos + i, 1);
    }
    hal::audio::BlockReport report{};
    const hal::audio::BlockIo io{in, out, 0u, &report};
    cb.fn(cb.user, io);
    heard.insert(heard.end(), out, out + 2u * kN);
    pos += kN;
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  // Until the take is Done (at most `limit` blocks).
  bool run_until_done(unsigned limit) {
    for (unsigned i = 0; i < limit && svc.state() != hr::State::Done; ++i) {
      block();
    }
    return svc.state() == hr::State::Done;
  }
  // The sample at which the transport's sixteenth `k` begins, as dsp::frame_at() defines it: the first sample whose position has reached the tick.
  std::uint64_t step_sample(std::uint64_t k) const {
    const std::uint64_t inc = dsp::ticks_per_sample_q32(engine->tempo());
    const std::uint64_t tick_q32 = (k * dsp::kTicksPerSixteenth) << 32;
    return (tick_q32 + inc - 1u) / inc;
  }
  // The first sixteenth that begins at or after the sample `from`.
  std::uint64_t step_at_or_after(std::uint64_t from) const {
    std::uint64_t k = 0;
    while (step_sample(k) < from) {
      ++k;
    }
    return k;
  }
};

constexpr std::int16_t kGuard = 0x5A5A;

hr::Take take_of(std::int16_t* buf, std::uint32_t frames, hr::Channels ch = hr::Channels::Mix, hr::Align align = hr::Align::Free) {
  hr::Take t;
  t.buf = buf;
  t.capacity_frames = frames;
  t.channels = ch;
  t.align = align;
  return t;
}

}  // namespace

TEST_CASE("record service: an idle recorder changes nothing and does no work") {
  Rig plain;  // the same chain with nothing armed ...
  Rig other;
  plain.blocks(300);
  CHECK(plain.svc.active_blocks() == 0u);
  CHECK(plain.svc.state() == hr::State::Idle);
  CHECK(plain.svc.frames() == 0u);
  // ... leaves the output as the inner callback made it, block for block (the golden of a recorder-less chain).
  for (std::uint64_t n = 0; n < plain.pos; ++n) {
    REQUIRE(plain.heard[2u * n] == master_l(n));
    REQUIRE(plain.heard[2u * n + 1u] == master_r(n));
  }
  // A take that is Done and not yet released costs nothing more either.
  alignas(4) static std::int16_t buf[256];
  REQUIRE(other.svc.arm(take_of(buf, 128)));
  REQUIRE(other.run_until_done(10));
  const std::uint32_t active = other.svc.active_blocks();
  other.blocks(50);
  CHECK(other.svc.active_blocks() == active);
  other.svc.release();
  other.blocks(50);
  CHECK(other.svc.active_blocks() == active);
  CHECK(other.svc.state() == hr::State::Idle);
}

TEST_CASE("record service: arm refuses what it cannot record, and the wrapped callback runs the inner one first") {
  Rig r;
  alignas(4) static std::int16_t buf[1024];
  CHECK_FALSE(r.svc.arm(take_of(nullptr, 100)));
  CHECK_FALSE(r.svc.arm(take_of(buf, 0)));
  CHECK_FALSE(r.svc.arm(take_of(reinterpret_cast<std::int16_t*>(reinterpret_cast<std::uintptr_t>(buf) + 2u), 100)));  // not 4-byte aligned
  hr::Take level = take_of(buf, 1000, hr::Channels::Mix, hr::Align::Level);
  level.threshold = 0;  // a level of 0 is no level
  CHECK_FALSE(r.svc.arm(level));
  level.threshold = 1000;
  level.preroll_frames = 2000;  // no room behind the ring
  CHECK_FALSE(r.svc.arm(level));
  CHECK(r.svc.state() == hr::State::Idle);
  REQUIRE(r.svc.arm(take_of(buf, 100)));
  CHECK_FALSE(r.svc.arm(take_of(buf, 100)));  // not Idle
  r.block();
  CHECK(r.inner_calls == 1u);
  CHECK(r.svc.state() == hr::State::Recording);
}

TEST_CASE("record service: a take of the input is the block as it was, in order; a full buffer is counted and never written past") {
  Rig r;
  r.blocks(3);
  constexpr std::uint32_t kCap = 1000;  // 15 blocks and 40 frames: the 16th block is cut and its 24 frames are the overrun
  alignas(4) static std::int16_t buf[2u * kCap + 64u];
  for (std::int16_t& s : buf) {
    s = kGuard;
  }
  REQUIRE(r.svc.arm(take_of(buf, kCap, hr::Channels::Stereo)));
  CHECK(r.svc.state() == hr::State::Recording);  // a Free take is armed at once (it starts at the next block)
  const std::uint64_t start = r.pos;
  REQUIRE(r.run_until_done(40));
  CHECK(r.svc.frames() == kCap);
  CHECK(r.svc.overrun() == 24u);
  for (std::uint32_t i = 0; i < kCap; ++i) {
    REQUIRE(buf[2u * i] == ramp_l(start + i, 0));
    REQUIRE(buf[2u * i + 1u] == ramp_r(start + i, 1));
  }
  for (std::uint32_t i = 2u * kCap; i < 2u * kCap + 64u; ++i) {
    REQUIRE(buf[i] == kGuard);  // nothing past the capacity
  }
  r.blocks(10);
  CHECK(r.svc.frames() == kCap);
  CHECK(r.svc.overrun() == 24u);
  r.svc.release();
  CHECK(r.svc.state() == hr::State::Idle);
}

TEST_CASE("record service: a mono take is the mix, the left or the right channel") {
  Rig r;
  alignas(4) static std::int16_t buf[3][256];
  const hr::Channels kinds[3] = {hr::Channels::Mix, hr::Channels::Left, hr::Channels::Right};
  for (unsigned k = 0; k < 3; ++k) {
    r.blocks(1);
    REQUIRE(r.svc.arm(take_of(buf[k], 192, kinds[k])));
    const std::uint64_t start = r.pos;
    REQUIRE(r.run_until_done(10));
    for (std::uint32_t i = 0; i < 192; ++i) {
      const std::int32_t l = ramp_l(start + i, 0);
      const std::int32_t rr = ramp_r(start + i, 1);
      const std::int32_t want = k == 0 ? (l + rr + 1) >> 1 : (k == 1 ? l : rr);
      REQUIRE(buf[k][i] == want);
    }
    r.svc.release();
  }
}

TEST_CASE("record service: a take of the master is what the main output played, and the tap is latched by arm") {
  Rig r;
  r.blocks(2);
  alignas(4) static std::int16_t buf[512];
  r.svc.set_tap(hr::Tap::Master);
  CHECK(r.svc.tap() == hr::Tap::Master);
  REQUIRE(r.svc.arm(take_of(buf, 128, hr::Channels::Stereo)));
  r.svc.set_tap(hr::Tap::Input);  // refused while a take is not Idle
  CHECK(r.svc.tap() == hr::Tap::Master);
  const std::uint64_t start = r.pos;
  REQUIRE(r.run_until_done(10));
  for (std::uint32_t i = 0; i < 128; ++i) {
    REQUIRE(buf[2u * i] == master_l(start + i));
    REQUIRE(buf[2u * i + 1u] == master_r(start + i));
  }
  r.svc.release();
  r.svc.set_tap(hr::Tap::Input);
  CHECK(r.svc.tap() == hr::Tap::Input);
}

TEST_CASE("record service: a take on a step starts AT THE SAMPLE of the sixteenth, at a tempo that does not divide the block") {
  Rig r;
  REQUIRE(r.engine->set_tempo(133'333));  // 4,961.4 samples to a sixteenth: a step is rarely on a block boundary
  REQUIRE(r.engine->play());
  r.blocks(5);
  alignas(4) static std::int16_t buf[2048];
  r.svc.set_tap(hr::Tap::Master);
  REQUIRE(r.svc.arm(take_of(buf, 600, hr::Channels::Stereo, hr::Align::Step)));
  CHECK(r.svc.state() == hr::State::Waiting);
  const std::uint64_t k = r.step_at_or_after(r.pos);
  const std::uint64_t first = r.step_sample(k);
  REQUIRE(first >= r.pos);
  REQUIRE(r.run_until_done(200));
  REQUIRE(r.svc.frames() == 600u);
  for (std::uint32_t i = 0; i < 600; ++i) {
    REQUIRE(buf[2u * i] == master_l(first + i));
    REQUIRE(buf[2u * i + 1u] == master_r(first + i));
  }
  CHECK(first % kN != 0u);  // the step really was inside a block
}

TEST_CASE("record service: every sixteenth of a long run is found at its own sample") {
  Rig r;
  REQUIRE(r.engine->set_tempo(97'001));
  REQUIRE(r.engine->play());
  r.blocks(1);
  alignas(4) static std::int16_t buf[64];
  r.svc.set_tap(hr::Tap::Master);
  for (unsigned round = 0; round < 40; ++round) {
    REQUIRE(r.svc.arm(take_of(buf, 8, hr::Channels::Left, hr::Align::Step)));
    const std::uint64_t k = r.step_at_or_after(r.pos);
    REQUIRE(r.run_until_done(200));
    REQUIRE(r.svc.frames() == 8u);
    REQUIRE(buf[0] == master_l(r.step_sample(k)));
    REQUIRE(buf[7] == master_l(r.step_sample(k) + 7u));
    r.svc.release();
    r.blocks(1);
  }
}

TEST_CASE("record service: Bar and Pattern are the transport's grid and the app's pattern length") {
  using RS = RecordService;
  // The index RecTap judges: 0 at a pattern boundary, 16 at another bar, 1 at any other sixteenth.
  CHECK(RS::tap_step(0, 16) == 0u);
  CHECK(RS::tap_step(16, 16) == 0u);
  CHECK(RS::tap_step(5, 16) == 1u);
  CHECK(RS::tap_step(16, 6) == 16u);  // a bar that is not a pattern start
  CHECK(RS::tap_step(18, 6) == 0u);
  CHECK(RS::tap_step(48, 6) == 0u);
  CHECK(RS::tap_step(32, 32) == 0u);
  CHECK(RS::tap_step(16, 32) == 16u);

  Rig r;
  REQUIRE(r.engine->set_tempo(300'000));  // 2,205 samples to a sixteenth
  REQUIRE(r.engine->play());
  r.blocks(100);  // 6,400 samples: inside step 2
  alignas(4) static std::int16_t buf[256];
  r.svc.set_tap(hr::Tap::Master);
  // Bar: the next multiple of 16, whatever the pattern's length.
  REQUIRE(r.svc.arm(take_of(buf, 64, hr::Channels::Mix, hr::Align::Bar)));
  REQUIRE(r.run_until_done(700));
  CHECK(buf[0] == static_cast<std::int16_t>((master_l(r.step_sample(16)) + master_r(r.step_sample(16)) + 1) >> 1));
  r.svc.release();
  // Pattern of 6 steps: the next multiple of 6 after the bar's start (step 18).
  r.svc.set_pattern_steps(6);
  CHECK(r.svc.pattern_steps() == 6u);
  REQUIRE(r.svc.arm(take_of(buf, 64, hr::Channels::Mix, hr::Align::Pattern)));
  const std::uint64_t k = r.step_at_or_after(r.pos);
  const std::uint64_t want = ((k + 5u) / 6u) * 6u;
  REQUIRE(r.run_until_done(700));
  CHECK(buf[0] == static_cast<std::int16_t>((master_l(r.step_sample(want)) + master_r(r.step_sample(want)) + 1) >> 1));
  r.svc.release();
  r.svc.set_pattern_steps(0);
  CHECK(r.svc.pattern_steps() == 1u);
  r.svc.set_pattern_steps(100000);
  CHECK(r.svc.pattern_steps() == RS::kMaxPatternSteps);
}

TEST_CASE("record service: stop_at and auto_steps end a take at the sample of a step: the take is exactly the frames between two samples") {
  Rig r;
  REQUIRE(r.engine->set_tempo(133'333));
  REQUIRE(r.engine->play());
  r.blocks(3);
  alignas(4) static std::int16_t buf[20000];
  r.svc.set_tap(hr::Tap::Master);
  // Start on a step, end on the next one.
  REQUIRE(r.svc.arm(take_of(buf, 8000, hr::Channels::Mix, hr::Align::Step)));
  const std::uint64_t k = r.step_at_or_after(r.pos);
  while (r.svc.state() != hr::State::Recording) {
    REQUIRE(r.pos < 20000u);
    r.block();
  }
  r.svc.stop_at(hr::Align::Step);
  REQUIRE(r.run_until_done(100));
  CHECK(r.svc.frames() == r.step_sample(k + 1u) - r.step_sample(k));
  CHECK(r.svc.overrun() == 0u);
  CHECK(buf[0] == static_cast<std::int16_t>((master_l(r.step_sample(k)) + master_r(r.step_sample(k)) + 1) >> 1));
  r.svc.release();

  // auto_steps: two sixteenths after its first.
  hr::Take t = take_of(buf, 19000, hr::Channels::Mix, hr::Align::Step);
  t.auto_steps = 2;
  REQUIRE(r.svc.arm(t));
  const std::uint64_t k2 = r.step_at_or_after(r.pos);
  REQUIRE(r.run_until_done(400));
  CHECK(r.svc.frames() == r.step_sample(k2 + 2u) - r.step_sample(k2));
  r.svc.release();
}

TEST_CASE("record service: a transport that stops ends a take that was told to stop on a boundary, where it stops") {
  Rig r;
  r.svc.set_pattern_steps(255);
  REQUIRE(r.engine->set_tempo(120'000));
  REQUIRE(r.engine->play());
  r.blocks(4);
  alignas(4) static std::int16_t buf[4096];
  r.svc.set_tap(hr::Tap::Master);
  REQUIRE(r.svc.arm(take_of(buf, 4000)));  // Free: from the next block
  const std::uint64_t start = r.pos;
  r.svc.stop_at(hr::Align::Pattern);       // a boundary that does not come within the run
  r.blocks(6);
  CHECK(r.svc.state() == hr::State::Recording);
  REQUIRE(r.engine->stop());
  const std::uint64_t stop_block = r.pos;  // the stop is applied at frame 0 of the next block
  REQUIRE(r.run_until_done(5));
  CHECK(r.svc.frames() == stop_block - start);
  CHECK(r.svc.overrun() == 0u);
}

TEST_CASE("record service: a take disarmed before it began is Done with no frames") {
  Rig r;  // transport stopped: an aligned take waits
  alignas(4) static std::int16_t buf[512];
  REQUIRE(r.svc.arm(take_of(buf, 256, hr::Channels::Mix, hr::Align::Bar)));
  r.blocks(20);
  CHECK(r.svc.state() == hr::State::Waiting);
  r.svc.disarm();
  r.blocks(1);
  CHECK(r.svc.state() == hr::State::Done);
  CHECK(r.svc.frames() == 0u);
  r.svc.release();
  // The same for a Level take that never heard its level.
  hr::Take t = take_of(buf, 512, hr::Channels::Mix, hr::Align::Level);
  t.threshold = 20000;
  t.preroll_frames = 64;
  REQUIRE(r.svc.arm(t));
  r.blocks(5);
  CHECK(r.svc.state() == hr::State::Listening);
  r.svc.disarm();
  r.blocks(1);
  CHECK(r.svc.state() == hr::State::Done);
  CHECK(r.svc.frames() == 0u);
  CHECK(r.svc.settle() == 0u);
}

namespace {
// Quiet noise (|x| <= 50), then, from the sample kTrigger on, a signal of at least 2,000: a level of 1,000 is crossed at exactly that sample.
constexpr std::uint64_t kTrigger = 20 * kN + 23;
std::int16_t level_input(std::uint64_t n, int channel) {
  const std::int32_t noise = static_cast<std::int32_t>((n * 37u + static_cast<std::uint64_t>(channel) * 11u) % 101u) - 50;
  return static_cast<std::int16_t>(n < kTrigger ? noise : 2000 + noise);
}
}  // namespace

TEST_CASE("record service: a Level take keeps its pre-roll in order, and the frames before it are zero") {
  for (const std::uint32_t preroll : {0u, 1u, 100u, 640u}) {
    Rig r;
    r.in_fn = &level_input;
    alignas(4) static std::int16_t buf[4096];
    for (std::int16_t& s : buf) {
      s = kGuard;
    }
    hr::Take t = take_of(buf, 2048, hr::Channels::Left, hr::Align::Level);
    t.threshold = 1000;
    t.preroll_frames = preroll;
    REQUIRE(r.svc.arm(t));
    CHECK(r.svc.state() == hr::State::Listening);
    r.blocks(15);  // quiet: nothing is recorded yet
    CHECK(r.svc.state() == hr::State::Listening);
    CHECK(r.svc.frames() == 0u);
    REQUIRE(r.run_until_done(100));
    const std::uint32_t total = r.svc.frames();
    const std::uint32_t lead = r.svc.settle();
    CHECK(r.svc.settle() == lead);  // idempotent
    REQUIRE(total == 2048u);
    REQUIRE(lead < total);
    // The trigger frame: the first one at or above the level.
    std::uint32_t q = lead;
    while (q < total && buf[q] < 1000) {
      ++q;
    }
    REQUIRE(q < total);
    const std::uint32_t pre = q - lead;  // the frames of pre-roll the take kept
    CHECK(pre >= preroll);
    CHECK(pre <= ((preroll + 1u) & ~1u) + 1u);  // (the pre-roll rounds up to even; one frame more may be kept to start the take on an even frame)
    for (std::uint32_t i = 0; i < lead; ++i) {
      REQUIRE(buf[i] == 0);
    }
    for (std::uint32_t i = lead; i < total; ++i) {
      REQUIRE(buf[i] == level_input(kTrigger - pre + (i - lead), 0));  // in order, from the oldest kept frame to the end
    }
    CHECK(r.svc.settle() == lead);
  }
}

TEST_CASE("record service: hal::record helpers and a Done take's states") {
  CHECK(hr::channel_count(hr::Channels::Stereo) == 2u);
  CHECK(hr::channel_count(hr::Channels::Mix) == 1u);
  CHECK(hr::ring_frames(100) == 100u + kN);
  CHECK(hr::ring_frames(101) == 102u + kN);
  Rig r;
  alignas(4) static std::int16_t buf[256];
  CHECK(r.svc.state() == hr::State::Idle);
  REQUIRE(r.svc.arm(take_of(buf, 64)));
  r.svc.disarm();
  r.blocks(2);
  CHECK(r.svc.state() == hr::State::Done);
  r.svc.release();
  CHECK(r.svc.state() == hr::State::Idle);
}
