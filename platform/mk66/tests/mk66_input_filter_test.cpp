// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lockout debounce and the publishing state of a key (key_filter.h) and the encoder's detent accumulator (detent.h): bounce
// patterns, the consistency refusals, a queue that refuses an edge, the counter wrap, a fast spin, a reversal. Runs on the host.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/input/detent.h>
#include <o3sdk/platform/mk66/input/key_filter.h>

#include <cstdint>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)               \
  do {                           \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);             \
    if (!must_ok_) {             \
      return;                    \
    }                            \
  } while (0)

namespace input = o3sdk::platform::mk66::input;
using o3sdk::core::Tick;

namespace {

constexpr std::uint32_t kLockout = 5'000;

// A filter and a log of what it published, driven like the scanner drives it: edges, service(now), then publish everything.
struct Rig {
  input::KeyFilter<8> filter{kLockout};
  struct Out {
    unsigned key;
    bool down;
    Tick time;
  };
  Out out[64] = {};
  unsigned count = 0;
  void step(Tick now) {
    filter.service(now);
    unsigned key = 0;
    bool down = false;
    Tick time = 0;
    while (filter.next_unpublished(key, down, time)) {
      MUST(count < 64u);
      out[count++] = Out{key, down, time};
      filter.mark_published(key);
    }
  }
};

}  // namespace

TEST_CASE("key filter: the first edge costs no latency") {
  Rig r;
  r.filter.edge(3, true);
  r.step(10'000);
  MUST(r.count == 1);
  CHECK(r.out[0].key == 3);
  CHECK(r.out[0].down);
  CHECK(r.out[0].time == 10'000);  // accepted in the pass of the edge
  CHECK(r.filter.is_held(3));
}

TEST_CASE("key filter: a bounce after the press is absorbed, the release comes once") {
  Rig r;
  r.filter.edge(1, true);
  r.step(100'000);       // press accepted
  r.filter.edge(1, false);
  r.step(101'000);       // bounce: 1 ms after the press, inside the lockout
  r.filter.edge(1, true);
  r.step(102'000);       // contact closes again: raw is back to accepted, nothing happened
  CHECK(r.count == 1);
  CHECK(r.filter.is_held(1));
  r.filter.edge(1, false);
  r.step(200'000);       // the real release, long after the lockout
  MUST(r.count == 2);
  CHECK_FALSE(r.out[1].down);
  CHECK(r.out[1].time == 200'000);
  CHECK(r.filter.filtered() >= 1u);  // the pair of bounce edges
}

TEST_CASE("key filter: a release inside the lockout is delayed to its end, never lost") {
  Rig r;
  r.filter.edge(2, true);
  r.step(100'000);
  r.filter.edge(2, false);
  r.step(102'000);  // 2 ms after the press: held back
  CHECK(r.count == 1);
  r.step(104'999);
  CHECK(r.count == 1);
  r.step(105'000);  // the lockout is over
  MUST(r.count == 2);
  CHECK_FALSE(r.out[1].down);
  CHECK(r.out[1].time == 105'000);
}

TEST_CASE("key filter: chatter on the release is absorbed") {
  Rig r;
  r.filter.edge(0, true);
  r.step(100'000);
  r.filter.edge(0, false);
  r.step(150'000);  // released for real
  r.filter.edge(0, true);  // a bounce of the opening contact, 1 ms later...
  r.step(151'000);
  r.filter.edge(0, false); // ...and open again
  r.step(152'000);
  CHECK(r.count == 2);  // press, release: nothing else
  CHECK_FALSE(r.filter.is_held(0));
}

TEST_CASE("key filter: edges that repeat the state are refused and counted") {
  Rig r;
  r.filter.edge(4, false);  // a release of a key that is up
  CHECK(r.filter.filtered() == 1u);
  r.filter.edge(4, true);
  r.filter.edge(4, true);   // a press of a key that is down
  CHECK(r.filter.filtered() == 2u);
  r.step(10'000);
  CHECK(r.count == 1);
  r.filter.edge(8, true);   // out of range for 8 keys
  r.filter.edge(200, true);
  CHECK(r.filter.filtered() == 4u);
  r.step(20'000);
  CHECK(r.count == 1);
}

TEST_CASE("key filter: a refused edge is offered again, and a refused press and release collapse") {
  Rig r;
  input::KeyFilter<8>& f = r.filter;
  f.edge(5, true);
  f.service(100'000);
  unsigned key = 0;
  bool down = false;
  Tick time = 0;
  MUST(f.next_unpublished(key, down, time));  // the queue refuses it: not marked
  MUST(f.next_unpublished(key, down, time));  // still there next pass
  CHECK(key == 5);
  CHECK(down);
  // press and release both accepted while refused: the consumer never saw the key, the states agree
  f.edge(5, false);
  f.service(200'000);
  CHECK_FALSE(f.next_unpublished(key, down, time));
  CHECK_FALSE(f.is_held(5));
}

TEST_CASE("key filter: edges are offered in the order they were accepted, not in key order, each with its own time") {
  Rig r;
  r.filter.edge(6, true);
  r.filter.service(50'000);
  r.filter.edge(2, true);
  r.filter.service(51'000);
  unsigned key = 0;
  bool down = false;
  Tick time = 0;
  MUST(r.filter.next_unpublished(key, down, time));
  CHECK(key == 6);  // pressed first, so told first, though 2 is the lower key
  CHECK(time == 50'000);
  r.filter.mark_published(key);
  MUST(r.filter.next_unpublished(key, down, time));
  CHECK(key == 2);
  CHECK(time == 51'000);
}

TEST_CASE("key filter: two keys the chip reports in one drain come out in the chip's order (Shift, then F8, is Shift + F8)") {
  // Shift is button 29 and F8 button 7 in the buttons' numbering: the lower key, F8, was the LATER edge.
  Rig r;
  input::KeyFilter<64> f(kLockout);
  f.edge(29, true);  // Shift first
  f.edge(7, true);   // then F8, in the same drain
  f.service(10'000);
  unsigned key = 0;
  bool down = false;
  Tick time = 0;
  MUST(f.next_unpublished(key, down, time));
  CHECK(key == 29);
  f.mark_published(key);
  MUST(f.next_unpublished(key, down, time));
  CHECK(key == 7);
  f.mark_published(key);
  CHECK_FALSE(f.next_unpublished(key, down, time));
}

TEST_CASE("key filter: an edge held back by the lockout is published when it is accepted, after what was accepted meanwhile") {
  Rig r;
  r.filter.edge(5, true);
  r.step(100'000);       // 5 pressed and published
  r.filter.edge(5, false);  // released 2 ms later: held back by the lockout
  r.step(102'000);
  r.filter.edge(1, true);   // key 1 pressed after that release, accepted at once
  r.step(103'000);
  r.step(106'000);          // the lockout of 5 ends: its release is accepted now
  MUST(r.count == 3);
  CHECK(r.out[0].key == 5);
  CHECK(r.out[1].key == 1);  // accepted first
  CHECK(r.out[2].key == 5);  // its release was reported first but accepted last: the debounce delay is what puts it behind
  CHECK_FALSE(r.out[2].down);
}

TEST_CASE("key filter: the order counter wraps without changing the order") {
  input::KeyFilter<8> f(kLockout);
  unsigned key = 0;
  bool down = false;
  Tick time = 0;
  for (unsigned round = 0; round < 70'000u; ++round) {  // past 65,536 raw edges
    f.edge(3, true);
    f.service(round * 20'000u);
    MUST(f.next_unpublished(key, down, time));
    f.mark_published(key);
    f.edge(3, false);
    f.service(round * 20'000u + 10'000u);
    MUST(f.next_unpublished(key, down, time));
    f.mark_published(key);
  }
  f.edge(6, true);
  f.edge(2, true);
  f.service(2'000'000'000u);
  MUST(f.next_unpublished(key, down, time));
  CHECK(key == 6);  // still in the order reported, after the counter wrapped
}

TEST_CASE("key filter: release_all lets the held keys go through the normal path") {
  Rig r;
  r.filter.edge(1, true);
  r.filter.edge(7, true);
  r.step(100'000);
  MUST(r.count == 2);
  CHECK(r.filter.release_all() == 2u);
  r.step(100'500);  // inside the lockout: not yet
  CHECK(r.count == 2);
  r.step(110'000);
  MUST(r.count == 4);
  CHECK_FALSE(r.out[2].down);
  CHECK_FALSE(r.out[3].down);
  CHECK(r.filter.held() == 0u);
}

TEST_CASE("key filter: the lockout arithmetic survives the tick wrap") {
  Rig r;
  r.filter.edge(0, true);
  r.step(0xFFFFFF00u);
  r.filter.edge(0, false);
  r.step(0xFFFFFF00u + 2'000u);  // 2 ms later: held back
  CHECK(r.count == 1);
  r.step(0xFFFFFF00u + 6'000u);  // 6 ms later, past the wrap: accepted
  MUST(r.count == 2);
  CHECK_FALSE(r.out[1].down);
}

TEST_CASE("key filter: a key that never changed is not locked out at any time, even in the first millisecond of the run") {
  Rig r;
  r.filter.edge(0, true);
  r.step(0);
  CHECK(r.count == 1);
}

TEST_CASE("key filter: release_except lets the others go and keeps the keys the chip has just named") {
  Rig r;
  r.filter.edge(1, true);
  r.filter.edge(4, true);
  r.filter.edge(7, true);
  r.step(100'000);
  MUST(r.count == 3);
  CHECK(r.filter.release_except(1u << 4) == 2u);  // 1 and 7 go, 4 stays
  r.step(110'000);
  MUST(r.count == 5);
  CHECK(r.filter.held() == (1u << 4));
  CHECK_FALSE(r.out[3].down);
  CHECK_FALSE(r.out[4].down);
}

TEST_CASE("key filter: 64 keys fit the masks") {
  input::KeyFilter<64> f(kLockout);
  f.edge(63, true);
  f.edge(0, true);
  f.service(100'000);
  CHECK(f.is_held(63));
  CHECK(f.is_held(0));
  CHECK(f.held() == ((std::uint64_t{1} << 63) | 1u));
}

TEST_CASE("detent accumulator: four counts are a detent, the remainder is kept") {
  input::DetentAccumulator a(0);
  CHECK(a.feed(1) == 0);
  CHECK(a.feed(3) == 0);
  CHECK(a.feed(4) == 1);
  CHECK(a.remainder() == 0);
  CHECK(a.feed(7) == 0);
  CHECK(a.remainder() == 3);
  CHECK(a.feed(8) == 1);
}

TEST_CASE("detent accumulator: every transition, in both directions, one count at a time") {
  input::DetentAccumulator up(0);
  int total = 0;
  for (std::uint16_t c = 1; c <= 40; ++c) {
    total += up.feed(c);
  }
  CHECK(total == 10);
  input::DetentAccumulator down(0);
  total = 0;
  for (std::uint16_t c = 1; c <= 40; ++c) {
    total += down.feed(static_cast<std::uint16_t>(0u - c));
  }
  CHECK(total == -10);
}

TEST_CASE("detent accumulator: bounce around a detent is invisible and a reversal needs a whole detent back") {
  input::DetentAccumulator a(100);
  // the dial leaves its detent by two counts and the spring pushes it back: nothing is reported
  CHECK(a.feed(102) == 0);
  CHECK(a.feed(100) == 0);
  // jitter on the boundary: +3, then +1 (a detent), then -1, +1, -1: only the detent counts
  CHECK(a.feed(103) == 0);
  CHECK(a.feed(104) == 1);
  CHECK(a.feed(103) == 0);
  CHECK(a.feed(104) == 0);
  CHECK(a.feed(103) == 0);
  // a reversal is reported once the dial has come a whole detent (four counts) back from where the last one was reported: 104 -> 100
  CHECK(a.feed(100) == -1);
  CHECK(a.feed(99) == 0);
}

TEST_CASE("detent accumulator: the 16-bit counter wraps both ways") {
  input::DetentAccumulator a(0xFFFE);
  CHECK(a.feed(2) == 1);  // +4 across 65535 -> 0
  input::DetentAccumulator b(2);
  CHECK(b.feed(0xFFFE) == -1);  // -4 across 0 -> 65535
}

TEST_CASE("detent accumulator: a fast spin is many detents in one read, a skipped state is just counts") {
  input::DetentAccumulator a(0);
  CHECK(a.feed(40) == 10);  // ten detents in one pass
  CHECK(a.feed(42) == 0);   // +2: a state skipped, half a detent
  CHECK(a.remainder() == 2);
  CHECK(a.feed(44) == 1);   // another two complete it
  CHECK(a.feed(static_cast<std::uint16_t>(44 - 400)) == -100);  // a hundred detents back in one read
  input::DetentAccumulator big(0);
  CHECK(big.feed(32767) == 8191);  // the most a read can carry: half the counter's range
  CHECK(big.remainder() == 3);
}
