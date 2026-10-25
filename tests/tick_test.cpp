// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/tick.h>

#include <cstdint>

using namespace o3sdk::core;

static_assert(ticks_from_ms(1) == 1000u, "1 tick = 1 us");
static_assert(tick_elapsed(10u, 4u) == 6u, "plain difference");
static_assert(tick_reached(5u, 5u), "a deadline is reached at the tick itself");

TEST_CASE("tick: ticks_from_ms converts and wraps modulo 2^32") {
  CHECK(ticks_from_ms(0) == 0u);
  CHECK(ticks_from_ms(250) == 250000u);
  CHECK(ticks_from_ms(4294967u) == 4294967000u);  // the largest millisecond count that does not wrap
  CHECK(ticks_from_ms(4294968u) == 704u);  // 4294968000 - 2^32
}

TEST_CASE("tick: tick_elapsed is correct across the 2^32 wrap") {
  CHECK(tick_elapsed(100u, 100u) == 0u);
  CHECK(tick_elapsed(0u, 0xFFFFFF00u) == 0x100u);           // now wrapped past `since`
  CHECK(tick_elapsed(0x10u, 0xFFFFFFF0u) == 0x20u);
  CHECK(tick_elapsed(0xFFFFFFFFu, 0u) == 0xFFFFFFFFu);      // the longest span that can be told apart
}

TEST_CASE("tick: tick_reached across the wrap and at the 2^31 limit") {
  CHECK(tick_reached(1000u, 999u));
  CHECK(tick_reached(1000u, 1000u));
  CHECK_FALSE(tick_reached(999u, 1000u));
  // Deadline set shortly before the wrap, `now` shortly after it.
  CHECK_FALSE(tick_reached(0xFFFFFFF0u, 0x10u));  // deadline is 0x20 ticks in the future
  CHECK(tick_reached(0x10u, 0xFFFFFFF0u));        // now is 0x20 ticks past the deadline
  // Limit of validity: a deadline up to 2^31 ticks ahead still reads "not reached", and one up to 2^31 - 1
  // ticks behind still reads "reached". Further out the two are indistinguishable (documented in tick.h).
  CHECK_FALSE(tick_reached(0u, 0x80000000u));
  CHECK(tick_reached(0x7FFFFFFFu, 0u));
  CHECK(tick_reached(0u, 0x80000001u));  // 2^31 + 1 ahead is misread as past: out of contract
}

TEST_CASE("tick: tick_clamp_to_now leaves in-range stamps alone and pulls a stamp ahead of the clock back to it") {
  CHECK(tick_clamp_to_now(1000u, 1000u) == 1000u);
  CHECK(tick_clamp_to_now(999u, 1000u) == 999u);
  CHECK(tick_clamp_to_now(0u, 5000u) == 0u);
  CHECK(tick_clamp_to_now(1001u, 1000u) == 1000u);                    // 1 us ahead
  CHECK(tick_clamp_to_now(1000u + 0x40000000u, 1000u) == 1000u);      // 2^30 us ahead
  CHECK(tick_clamp_to_now(1000u + 0x7FFFFFFFu, 1000u) == 1000u);      // the farthest that is "ahead"
  CHECK(tick_clamp_to_now(0x00000010u, 0xFFFFFFF0u) == 0xFFFFFFF0u);  // ahead across the wrap
  CHECK(tick_clamp_to_now(0xFFFFFFF0u, 0x00000010u) == 0xFFFFFFF0u);  // behind across the wrap: untouched
  CHECK(tick_clamp_to_now(0xFFFFFF00u, 0x00000100u) == 0xFFFFFF00u);
}
