// SPDX-License-Identifier: GPL-3.0-or-later
//
// o3sdk port: the case that drives the tracker's Graph (the scope and spectrum rings against its output) is cut, with the Graph's include and its cycle stub; the cases are
// track3r-os's own otherwise.
//
// OS-005b: the taps (audio/taps.h) under load, with their overrun policies. The scope and spectrum rings against the output the graph made, the
// writer lapping a reader that never reads, the StemTap's pool against the OS-006a lifetime contract (a block the reader holds survives four
// more stem blocks and is overwritten when the fifth begins: the boundary itself is tested, not a margin), the stem ring's drop-oldest policy
// and the REC take. Host and Cortex-M4F; the concurrent cases are in audio_taps_thread_test.cpp (host, TSan).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/taps.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/usb/stream.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

using namespace o3sdk::platform::mk66::audio;
using o3sdk::test::Rng;
namespace ha = o3sdk::hal::audio;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

}  // namespace

TEST_CASE("taps: a sample ring gives the newest window exactly, zero in front before it has filled, and the writer never waits") {
  static o3sdk::test::StaticSlot<ScopeRing> slot;
  ScopeRing& ring = slot.make();
  alignas(4) std::int16_t block[kBS];
  alignas(4) std::int16_t window[512];
  std::uint32_t counter = 0;
  auto write_block = [&]() {
    for (std::uint32_t i = 0; i < kBS; ++i) {
      block[i] = static_cast<std::int16_t>(counter++);
    }
    ring.write(block, kBS);
  };
  // Before it has filled: the front of the window is zero, the tail is what was written.
  write_block();
  ScopeRing::Window w = ring.read_latest(window, 512);
  CHECK(w.whole);
  CHECK(w.head == kBS);
  for (std::uint32_t i = 0; i < 512u - kBS; ++i) {
    CHECK(window[i] == 0);
  }
  for (std::uint32_t i = 0; i < kBS; ++i) {
    CHECK(window[512u - kBS + i] == static_cast<std::int16_t>(i));
  }
  // Overrun: the writer goes round the ring many times with nobody reading; it never blocks, and the newest window is exact.
  for (int k = 0; k < 1000; ++k) {
    write_block();
  }
  w = ring.read_latest(window, 512);
  CHECK(w.whole);
  CHECK(w.head == 1001u * kBS);
  for (std::uint32_t i = 0; i < 512u; ++i) {
    CHECK(window[i] == static_cast<std::int16_t>(counter - 512u + i));
  }
  // The largest window the ring promises is its capacity less the block the writer may be inside.
  CHECK(ScopeRing::max_window() == ScopeRing::capacity() - kBS);
  alignas(4) std::int16_t big[ScopeRing::max_window()];
  w = ring.read_latest(big, ScopeRing::max_window());
  CHECK(w.whole);
  for (std::uint32_t i = 0; i < ScopeRing::max_window(); ++i) {
    CHECK(big[i] == static_cast<std::int16_t>(counter - ScopeRing::max_window() + i));
  }
}

TEST_CASE("taps: the stem pool obeys the OS-006a lifetime contract: a held block survives four more, the fifth overwrites it") {
  static o3sdk::test::StaticSlot<StemTap> slot;
  static StemPool pool;
  StemTap& tap = slot.make(pool);
  tap.set_enabled(true);
  tap.set_channels(2);
  tap.set_slot(0, 1u << 0);  // channel 0 = source 0
  tap.set_slot(1, 1u << 1);  // channel 1 = source 1
  alignas(4) static std::int16_t src[2][kB];
  const std::int16_t* sources[16] = {};
  sources[0] = src[0];
  sources[1] = src[1];
  std::uint32_t render_blocks = 0;
  auto render_stem_block = [&]() {  // two render blocks make one stem block; the samples are the running frame number and its complement
    for (unsigned half = 0; half < StemTap::kHalves; ++half) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        const std::uint32_t frame = render_blocks * kB + i;
        src[0][i] = static_cast<std::int16_t>(frame & 0x7FFFu);
        src[1][i] = static_cast<std::int16_t>(~frame & 0x7FFFu);
      }
      tap.push(sources);
      ++render_blocks;
    }
  };
  CHECK(StemTap::kPool == 5u);
  CHECK(StemTap::kHalves == 2u);
  o3sdk::usb::StemReader reader(tap.ring());
  alignas(4) std::int16_t buf[o3sdk::usb::kStemBlockFrames * 2];
  render_stem_block();  // stem block 0
  CHECK(tap.published() == 1u);
  // The reader takes ten frames of block 0 and holds the rest.
  CHECK(reader.read_interleaved(buf, 10, 2) == 10u);
  for (std::uint32_t f = 0; f < 10u; ++f) {
    CHECK(buf[2u * f] == static_cast<std::int16_t>(f));
  }
  // Four more stem blocks are written (B+1..B+4): the ring (3 deep) loses the oldest, the held block is untouched.
  for (int k = 0; k < 4; ++k) {
    render_stem_block();
  }
  CHECK(tap.published() == 5u);
  const std::uint32_t rest = o3sdk::usb::kStemBlockFrames - 10u;
  CHECK(reader.read_interleaved(buf, rest, 2) == rest);
  for (std::uint32_t f = 0; f < rest; ++f) {
    CHECK(buf[2u * f] == static_cast<std::int16_t>(10u + f));            // block 0, intact after four more were written
    CHECK(buf[2u * f + 1u] == static_cast<std::int16_t>(~(10u + f) & 0x7FFFu));
  }
  // The reader's next block is the oldest the ring kept (block 2: block 1 was dropped and counted), intact.
  CHECK(reader.read_interleaved(buf, 4, 2) == 4u);
  CHECK(reader.dropped_blocks() == 1u);
  for (std::uint32_t f = 0; f < 4u; ++f) {
    CHECK(buf[2u * f] == static_cast<std::int16_t>((2u * o3sdk::usb::kStemBlockFrames + f) & 0x7FFFu));
  }

  // The boundary: hold block 5's first frames, then write blocks 6..9 (four more, all fine) and watch block 5's slot start to be reused by 10.
  StemTap& tap2 = slot.make(pool);
  tap2.set_enabled(true);
  tap2.set_channels(1);
  tap2.set_slot(0, 1u << 0);
  o3sdk::usb::StemReader r2(tap2.ring());
  render_blocks = 0;
  auto render_block2 = [&]() {
    for (unsigned half = 0; half < StemTap::kHalves; ++half) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        src[0][i] = static_cast<std::int16_t>((render_blocks * kB + i) & 0x7FFFu);
      }
      tap2.push(sources);
      ++render_blocks;
    }
  };
  render_block2();
  CHECK(r2.read_interleaved(buf, 1, 1) == 1u);  // holds block 0 (its pointers), frame 0 read
  for (int k = 0; k < 4; ++k) {
    render_block2();  // blocks 1..4: block 0's slot is still its own
  }
  alignas(4) std::int16_t probe[kB];
  // Frames 1..63 of block 0 are still its ramp; render one half of block 5 and they are not (the slot is reused, round robin of five).
  CHECK(r2.read_interleaved(probe, kB - 1u, 1) == kB - 1u);
  for (std::uint32_t f = 0; f < kB - 1u; ++f) {
    CHECK(probe[f] == static_cast<std::int16_t>(1u + f));
  }
}

TEST_CASE("taps: the stem tap is off until enabled, restarts on a block boundary when enabled, and a disabled tap writes nothing") {
  static o3sdk::test::StaticSlot<StemTap> slot;
  static StemPool pool;
  StemTap& tap = slot.make(pool);
  alignas(4) static std::int16_t src[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    src[i] = 1000;
  }
  const std::int16_t* sources[16] = {};
  sources[0] = src;
  for (int k = 0; k < 6; ++k) {
    tap.push(sources);
  }
  CHECK(tap.published() == 0u);  // disabled: not one block
  tap.set_enabled(true);
  tap.push(sources);  // the first half of a stem block
  CHECK(tap.published() == 0u);
  tap.push(sources);
  CHECK(tap.published() == 1u);
  tap.set_enabled(false);
  tap.push(sources);
  tap.set_enabled(true);
  tap.push(sources);  // starts over at a half 0: a stem block is whole or absent
  CHECK(tap.published() == 1u);
  tap.push(sources);
  CHECK(tap.published() == 2u);
  // Defaults are profile P10: T1..T8, then Master L and R.
  CHECK(tap.channels() == 10u);
  for (unsigned c = 0; c < 8; ++c) {
    CHECK(tap.slot(c) == (1u << c));
  }
  CHECK(tap.slot(8) == (1u << stem_src::kMasterL));
  CHECK(tap.slot(9) == (1u << stem_src::kMasterR));
  tap.set_slot(99, 1);  // out of range: ignored
  tap.set_channels(99);
  CHECK(tap.channels() == StemTap::kChannels);
}

TEST_CASE("taps: the REC take is a state machine with one writer at a time, stops at its capacity, counts what did not fit, and can be stopped") {
  static o3sdk::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[4 * kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    l[i] = static_cast<std::int16_t>(100 + i);
    r[i] = static_cast<std::int16_t>(-100 - static_cast<int>(i));
  }
  rec.push(l, r, kB);  // idle: nothing happens
  CHECK(rec.frames() == 0u);
  CHECK(rec.state() == RecTap::State::Idle);
  rec.release();  // not Done: nothing happens
  CHECK(rec.state() == RecTap::State::Idle);
  CHECK(rec.arm(take, 100, 2));
  CHECK(rec.state() == RecTap::State::Armed);
  rec.push(l, r, kB);
  CHECK(rec.frames() == kB);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(take[2u * i] == l[i]);
    CHECK(take[2u * i + 1u] == r[i]);
  }
  rec.push(l, r, kB);  // 36 frames fit
  CHECK(rec.frames() == 100u);
  CHECK(rec.overrun_frames() == kB - 36u);
  CHECK(rec.state() == RecTap::State::Done);
  rec.push(l, r, kB);  // Done: further blocks are not even counted
  CHECK(rec.frames() == 100u);
  CHECK(rec.overrun_frames() == kB - 36u);
  rec.release();
  CHECK(rec.state() == RecTap::State::Idle);
  // disarm(): the take ends at the next block, with what it has.
  CHECK(rec.arm(take, 4 * kB, 1));
  rec.push(l, r, kB);
  rec.disarm();
  rec.push(l, r, kB);
  CHECK(rec.state() == RecTap::State::Done);
  CHECK(rec.frames() == kB);
  CHECK(rec.overrun_frames() == 0u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(take[i] == static_cast<std::int16_t>((static_cast<std::int32_t>(l[i]) + r[i]) >> 1));  // mono: the mix
  }
}

// OS-LOOPER L2c review minor: a take with a step count (STOP AT BARS, a loop) that fills the memory exactly does not end in the block that fills it: its end step may be the first sample of the next block
// (a loop of exactly the capacity is a loop). A non-loop take whose boundary does not come there ends one block later, with the frames it was cut of counted as overrun (the recorder reads that as
// "memory filled", as before); the take itself holds exactly the capacity either way.
TEST_CASE("taps: a take with a step count that fills the memory exactly ends one block later with an overrun, or cleanly when its end step is the next block's first sample") {
  static o3sdk::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[2 * kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    l[i] = static_cast<std::int16_t>(100 + i);
    r[i] = l[i];
  }
  for (const int variant : {0, 1, 2}) {  // 0: no boundary comes; 1: it is the first sample of the next block; 2: it is inside the next block
    CHECK(rec.arm(take, 2 * kB, 1, RecTap::Align::Free, 1u));
    rec.push(l, r, kB);
    rec.push(l, r, kB);  // the memory is exactly full
    CHECK(rec.frames() == 2 * kB);
    CHECK(rec.state() == RecTap::State::Armed);  // not ended by capacity: it waits for its end step
    CHECK(rec.overrun_frames() == 0u);
    if (variant != 0) {
      rec.on_step(1u, variant == 1 ? 0u : 10u);
    }
    rec.push(l, r, kB);
    CHECK(rec.state() == RecTap::State::Done);
    CHECK(rec.frames() == 2 * kB);
    CHECK(rec.overrun_frames() == (variant == 0 ? kB : (variant == 1 ? 0u : 10u)));
    CHECK(rec.ended_on_boundary() == (variant != 0));
    rec.release();
  }
}

// OS-SREC-LEVEL (E-SREC-11): a REC take that starts on a level. The scan is exact at the threshold (>= on the magnitude, either sign, only the channel the tap records), and -32768 is a magnitude
// of 32768 (it is never negated in int16).
TEST_CASE("taps: a level take starts on the first frame whose magnitude reaches the threshold, exactly, on the recorded channel, and -32768 does not overflow") {
  static o3sdk::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[2u * 4u * kB];
  // One fresh tap, one block of silence but for `lv` / `rv` at frame 10; true when it started.
  const auto starts = [&](std::int32_t thresh, int lv, int rv, RecTap::Channel ch) {
    std::memset(l, 0, sizeof(l));
    std::memset(r, 0, sizeof(r));
    l[10] = static_cast<std::int16_t>(lv);
    r[10] = static_cast<std::int16_t>(rv);
    rec.set_channel(ch);
    CHECK(rec.arm_level(take, 4 * kB, 2, thresh, 0u));
    CHECK(rec.state() == RecTap::State::Listening);
    rec.push(l, r, kB);
    const bool started = rec.state() == RecTap::State::Armed;
    rec.disarm();
    rec.push(l, r, kB);
    rec.release();
    CHECK(rec.state() == RecTap::State::Idle);
    return started;
  };
  using C = RecTap::Channel;
  CHECK_FALSE(starts(1000, 999, 0, C::Mix));
  CHECK(starts(1000, 1000, 0, C::Mix));
  CHECK_FALSE(starts(1000, -999, 0, C::Mix));
  CHECK(starts(1000, -1000, 0, C::Mix));
  CHECK(starts(1000, -1001, 0, C::Mix));
  CHECK_FALSE(starts(1000, 0, 999, C::Mix));
  CHECK(starts(1000, 0, 1000, C::Mix));
  CHECK(starts(1000, 0, -1000, C::Mix));
  CHECK_FALSE(starts(1000, 0, 5000, C::Left));  // only the recorded channel counts
  CHECK(starts(1000, 5000, 0, C::Left));
  CHECK_FALSE(starts(1000, 5000, 0, C::Right));
  CHECK(starts(1000, 0, -5000, C::Right));
  CHECK(starts(32767, -32768, 0, C::Mix));  // (-(-32768) overflows int16: a magnitude of 32768 reaches the highest threshold)
  CHECK(starts(1, -32768, 0, C::Mix));
  CHECK(starts(32767, 0, -32768, C::Mix));
  CHECK(starts(32767, 32767, 0, C::Mix));
  CHECK_FALSE(starts(32767, -32766, 0, C::Mix));
  rec.set_channel(C::Mix);
}

TEST_CASE("taps: a level take is refused without a threshold or room for its ring, and stopped while listening it records nothing") {
  static o3sdk::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[2u * 4u * kB];
  std::memset(l, 0, sizeof(l));
  std::memset(r, 0, sizeof(r));
  CHECK(RecTap::ring_frames(0) == kB);
  CHECK(RecTap::ring_frames(9) == 10u + kB);  // the pre-roll is rounded up to even
  CHECK_FALSE(rec.arm(take, 4 * kB, 2, RecTap::Align::Level));  // a level take is armed by arm_level()
  CHECK_FALSE(rec.arm_level(take, 4 * kB, 2, 0, 0u));
  CHECK_FALSE(rec.arm_level(take, 4 * kB, 2, 32768, 0u));
  CHECK_FALSE(rec.arm_level(take, kB, 2, 100, 0u));  // the ring alone fills the region
  CHECK_FALSE(rec.arm_level(take, 4 * kB, 3, 100, 0u));
  CHECK(rec.state() == RecTap::State::Idle);
  CHECK(rec.arm_level(take, 4 * kB, 2, 100, 0u));
  CHECK_FALSE(rec.arm(take, 4 * kB, 2));  // one writer at a time
  rec.push(l, r, kB);
  rec.push(l, r, kB);
  CHECK(rec.state() == RecTap::State::Listening);
  CHECK(rec.frames() == 0u);
  CHECK(rec.settle_ring() == 0u);  // nothing to put in order
  rec.disarm();
  rec.push(l, r, kB);
  CHECK(rec.state() == RecTap::State::Done);  // cancelled while listening: nothing was recorded
  CHECK(rec.frames() == 0u);
  CHECK(rec.settle_ring() == 0u);
  rec.release();
  CHECK(rec.state() == RecTap::State::Idle);
}
