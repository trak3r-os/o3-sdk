// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/ring.h>

#include <cstdint>
#include <thread>

using o3sdk::core::SpscRing;

static_assert(SpscRing<std::uint8_t, 2>::capacity() == 2, "smallest ring");
static_assert(SpscRing<std::uint32_t, 64>::capacity() == 64, "capacity is the template argument");

TEST_CASE("ring: empty ring pops nothing and leaves the output alone") {
  SpscRing<std::uint32_t, 4> ring;
  std::uint32_t out = 99;
  CHECK(ring.empty());
  CHECK(ring.size() == 0u);
  CHECK_FALSE(ring.pop(out));
  CHECK(out == 99u);
}

TEST_CASE("ring: items come out in FIFO order") {
  SpscRing<std::uint32_t, 4> ring;
  CHECK(ring.push(10));
  CHECK(ring.push(20));
  CHECK(ring.push(30));
  CHECK(ring.size() == 3u);
  std::uint32_t out = 0;
  CHECK(ring.pop(out));
  CHECK(out == 10u);
  CHECK(ring.pop(out));
  CHECK(out == 20u);
  CHECK(ring.pop(out));
  CHECK(out == 30u);
  CHECK_FALSE(ring.pop(out));
  CHECK(ring.empty());
}

TEST_CASE("ring: all N slots are usable, a full ring refuses and keeps the oldest items") {
  SpscRing<std::uint32_t, 4> ring;
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(ring.push(i + 1));
  }
  CHECK(ring.size() == 4u);
  CHECK_FALSE(ring.push(5));  // dropped, not overwriting
  CHECK(ring.size() == 4u);
  std::uint32_t out = 0;
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(ring.pop(out));
    CHECK(out == i + 1);
  }
  CHECK_FALSE(ring.pop(out));
  CHECK(ring.push(6));  // space again after draining
  CHECK(ring.pop(out));
  CHECK(out == 6u);
}

TEST_CASE("ring: slot index wraps around the buffer many times") {
  SpscRing<std::uint32_t, 8> ring;
  std::uint32_t next_in = 0;
  std::uint32_t next_out = 0;
  // Fill levels cycle 0..8 so head and tail cross the end of the buffer at every possible offset.
  for (std::uint32_t round = 0; round < 500; ++round) {
    const std::uint32_t burst = (round % 8u) + 1u;
    for (std::uint32_t i = 0; i < burst; ++i) {
      if (ring.size() < 8u) {
        CHECK(ring.push(next_in));
        ++next_in;
      }
    }
    const std::uint32_t drain = (round % 5u) + 1u;
    for (std::uint32_t i = 0; i < drain; ++i) {
      std::uint32_t out = 0;
      if (ring.pop(out)) {
        CHECK(out == next_out);
        ++next_out;
      }
    }
    CHECK(ring.size() == next_in - next_out);
  }
  std::uint32_t out = 0;
  while (ring.pop(out)) {
    CHECK(out == next_out);
    ++next_out;
  }
  CHECK(next_in == next_out);
  CHECK(next_in > 8u * 50u);  // it really did wrap the 8-slot buffer many times
}

TEST_CASE("ring: the free-running counters wrap at 2^32 without losing or reordering items") {
  // Start 3 below 2^32: the 8-item fill straddles the counter wrap.
  SpscRing<std::uint32_t, 8> ring(0xFFFFFFFDu);
  CHECK(ring.empty());
  for (std::uint32_t i = 0; i < 8; ++i) {
    CHECK(ring.push(100 + i));
  }
  CHECK(ring.size() == 8u);
  CHECK_FALSE(ring.push(999));  // full is detected across the wrap
  std::uint32_t out = 0;
  for (std::uint32_t i = 0; i < 8; ++i) {
    CHECK(ring.pop(out));
    CHECK(out == 100 + i);
  }
  CHECK(ring.empty());
  CHECK_FALSE(ring.pop(out));
  // And it keeps working afterwards.
  for (std::uint32_t i = 0; i < 40; ++i) {
    CHECK(ring.push(i));
    CHECK(ring.pop(out));
    CHECK(out == i);
  }
}

TEST_CASE("ring: structs are copied whole") {
  struct Item {
    std::uint32_t a;
    std::uint16_t b;
    std::uint8_t c;
  };
  SpscRing<Item, 2> ring;
  CHECK(ring.push(Item{0xDEADBEEFu, 0x1234, 0x56}));
  Item out{};
  CHECK(ring.pop(out));
  CHECK(out.a == 0xDEADBEEFu);
  CHECK(out.b == 0x1234);
  CHECK(out.c == 0x56);
}

namespace {
// 8 bytes whose halves must always agree: a torn or stale read breaks the pairing.
struct Pair {
  std::uint32_t seq;
  std::uint32_t inverse;
};
}  // namespace

TEST_CASE("ring: producer and consumer threads exchange every item in order") {
  // The smallest ring maximises contention on the full/empty edges. Run under ThreadSanitizer
  // (-DO3SDK_TSAN=ON) this also proves the acquire/release pairing: a missing edge is a reported race.
  constexpr std::uint32_t kCount = 200000;
  SpscRing<Pair, 2> ring;

  std::thread producer([&ring]() {
    for (std::uint32_t seq = 0; seq < kCount; ++seq) {
      while (!ring.push(Pair{seq, ~seq})) {
        std::this_thread::yield();
      }
    }
  });

  std::uint32_t expected = 0;
  std::uint32_t order_errors = 0;
  std::uint32_t torn_reads = 0;
  while (expected < kCount) {
    Pair item{};
    if (!ring.pop(item)) {
      std::this_thread::yield();
      continue;
    }
    order_errors += (item.seq != expected) ? 1u : 0u;
    torn_reads += (item.inverse != ~item.seq) ? 1u : 0u;
    ++expected;
  }
  producer.join();
  CHECK(order_errors == 0u);
  CHECK(torn_reads == 0u);
  CHECK(ring.empty());
}
