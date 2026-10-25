// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: the overwrite ring behind the stem and return paths (ADR 0012: overrun drops the oldest and counts it).
#include <doctest/doctest.h>

#include <o3sdk/usb/ring.h>
#include <o3sdk/usb/stream.h>

#include "usb_test_util.h"

#include <cstdint>

using o3sdk::usb::OverwriteRing;

namespace {

std::uint32_t read_all(OverwriteRing<std::uint32_t, 8, 1>& r, std::uint32_t* out, std::uint32_t max_n = 100) {
  return r.read(out, max_n);
}

}  // namespace

static_assert(OverwriteRing<std::uint32_t, 8, 1>::capacity() == 7, "one chunk of slack");
static_assert(OverwriteRing<std::uint32_t, 512, 48>::capacity() == 464, "the return ring keeps 48 frames of slack");
static_assert(OverwriteRing<std::uint32_t, 4, 1>::max_chunk() == 1, "max chunk is the template argument");

TEST_CASE("usb_ring: empty ring reads nothing") {
  OverwriteRing<std::uint32_t, 8, 1> ring;
  std::uint32_t out[8] = {99, 99, 99, 99, 99, 99, 99, 99};
  CHECK(ring.size() == 0);
  CHECK(ring.size_from_producer() == 0);
  CHECK(read_all(ring, out) == 0);
  CHECK(out[0] == 99u);
  CHECK(ring.dropped() == 0);
}

TEST_CASE("usb_ring: items come out in order, partial reads continue where they stopped") {
  OverwriteRing<std::uint32_t, 8, 1> ring;
  for (std::uint32_t i = 1; i <= 5; ++i) {
    ring.write(&i, 1);
  }
  CHECK(ring.size() == 5);
  CHECK(ring.size_from_producer() == 5);
  std::uint32_t out[8] = {};
  CHECK(read_all(ring, out, 2) == 2);
  CHECK((out[0] == 1u && out[1] == 2u));
  CHECK(ring.size() == 3);
  CHECK(read_all(ring, out) == 3);
  CHECK((out[0] == 3u && out[1] == 4u && out[2] == 5u));
  CHECK(read_all(ring, out) == 0);
  CHECK(ring.dropped() == 0);
}

TEST_CASE("usb_ring: overrun overwrites the oldest and counts it") {
  OverwriteRing<std::uint32_t, 8, 1> ring;  // capacity 7
  for (std::uint32_t i = 1; i <= 10; ++i) {
    ring.write(&i, 1);
  }
  CHECK(ring.size() == 7);                  // never reports more than it can deliver
  CHECK(ring.size_from_producer() == 7);
  CHECK(ring.dropped() == 0);               // counted when the consumer notices
  std::uint32_t out[8] = {};
  CHECK(read_all(ring, out) == 7);
  for (std::uint32_t i = 0; i < 7; ++i) {
    CHECK(out[i] == 4 + i);                 // items 1..3 are gone, the newest seven remain in order
  }
  CHECK(ring.dropped() == 3);
  // A second lap with a partial read: the skip is counted once, the rest stays readable.
  for (std::uint32_t i = 11; i <= 30; ++i) {
    ring.write(&i, 1);
  }
  CHECK(read_all(ring, out, 2) == 2);
  CHECK((out[0] == 24u && out[1] == 25u));
  CHECK(ring.dropped() == 3 + 13);
  CHECK(read_all(ring, out) == 5);
  CHECK(out[4] == 30u);
  CHECK(ring.dropped() == 16);
  // max_n == 0 still commits a lap.
  for (std::uint32_t i = 31; i <= 50; ++i) {
    ring.write(&i, 1);
  }
  CHECK(read_all(ring, out, 0) == 0);
  CHECK(ring.dropped() == 16 + 13);
}

TEST_CASE("usb_ring: bulk writes keep a chunk of slack and are cut at MaxChunk") {
  OverwriteRing<std::uint32_t, 16, 4> ring;  // capacity 12
  std::uint32_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ring.write(in, 8);                         // cut to 4
  CHECK(ring.size() == 4);
  std::uint32_t out[16] = {};
  CHECK(ring.read(out, 16) == 4);
  CHECK((out[0] == 1u && out[3] == 4u));
  std::uint32_t next = 100;
  for (int i = 0; i < 5; ++i) {              // 20 items through a ring of capacity 12
    std::uint32_t chunk[4] = {next, next + 1, next + 2, next + 3};
    next += 4;
    ring.write(chunk, 4);
  }
  CHECK(ring.size() == 12);
  CHECK(ring.read(out, 16) == 12);
  CHECK(out[0] == 108u);                     // the oldest eight (100..107) were overwritten
  CHECK(out[11] == 119u);
  CHECK(ring.dropped() == 8);
}

TEST_CASE("usb_ring: flush discards without counting") {
  OverwriteRing<std::uint32_t, 8, 1> ring;
  for (std::uint32_t i = 1; i <= 5; ++i) {
    ring.write(&i, 1);
  }
  ring.flush();
  CHECK(ring.size() == 0);
  std::uint32_t out[8];
  CHECK(read_all(ring, out) == 0);
  CHECK(ring.dropped() == 0);
  std::uint32_t v = 77;
  ring.write(&v, 1);
  CHECK(read_all(ring, out) == 1);
  CHECK(out[0] == 77u);
}

TEST_CASE("usb_ring: reset (both sides stopped) is the state of a new ring: empty, nothing dropped") {
  OverwriteRing<std::uint32_t, 8, 1> ring;
  std::uint32_t out[8];
  for (std::uint32_t i = 1; i <= 20; ++i) {  // laps a ring that nobody reads
    ring.write(&i, 1);
  }
  CHECK(read_all(ring, out) > 0);
  CHECK(ring.dropped() > 0);
  for (std::uint32_t i = 1; i <= 20; ++i) {
    ring.write(&i, 1);
  }
  ring.reset();
  CHECK(ring.size() == 0);
  CHECK(ring.dropped() == 0);
  CHECK(read_all(ring, out) == 0);
  std::uint32_t v = 5;
  ring.write(&v, 1);
  CHECK(read_all(ring, out) == 1);
  CHECK(out[0] == 5u);
  CHECK(ring.dropped() == 0);
}

TEST_CASE("usb_ring: free-running counters cross 2^32") {
  OverwriteRing<std::uint32_t, 8, 1> ring(0xFFFFFFF0u);
  std::uint32_t expect = 1;
  std::uint32_t out[8];
  for (std::uint32_t i = 1; i <= 40; ++i) {
    ring.write(&i, 1);
    if (i % 3 == 0) {
      const std::uint32_t n = read_all(ring, out, 4);
      for (std::uint32_t k = 0; k < n; ++k) {
        CHECK(out[k] == expect++);
      }
    }
  }
  // Drain: everything not yet read arrives in order, none lost (the ring never lapped: <= 3 behind).
  for (std::uint32_t n = read_all(ring, out); n != 0; n = read_all(ring, out)) {
    for (std::uint32_t k = 0; k < n; ++k) {
      CHECK(out[k] == expect++);
    }
  }
  CHECK(expect == 41u);
  CHECK(ring.dropped() == 0);
  // And an overrun that straddles the wrap.
  OverwriteRing<std::uint32_t, 8, 1> lap(0xFFFFFFFCu);
  for (std::uint32_t i = 1; i <= 20; ++i) {
    lap.write(&i, 1);
  }
  CHECK(lap.read(out, 8) == 7);
  CHECK((out[0] == 14u && out[6] == 20u));
  CHECK(lap.dropped() == 13);
}

TEST_CASE("usb_ring: carries a stem block (a struct of pointers) intact") {
  OverwriteRing<o3sdk::usb::StemBlock, 4, 1> ring;
  static std::int16_t samples[3][8];
  for (int b = 0; b < 3; ++b) {
    o3sdk::usb::StemBlock blk{};
    for (std::uint32_t c = 0; c < o3sdk::usb::kMaxChannels; ++c) {
      blk.ch[c] = samples[b] + (c % 8);
    }
    ring.write(&blk, 1);
  }
  o3sdk::usb::StemBlock got{};
  for (int b = 0; b < 3; ++b) {
    CHECK(ring.read(&got, 1) == 1);
    for (std::uint32_t c = 0; c < o3sdk::usb::kMaxChannels; ++c) {
      CHECK(got.ch[c] == samples[b] + (c % 8));
    }
  }
  CHECK(ring.read(&got, 1) == 0);
}
