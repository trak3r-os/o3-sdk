// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: the overwrite ring between two real threads (host only: it needs std::thread; the other ring cases are
// in usb_ring_test.cpp and also run on QEMU). Named usb_ring:* so that `ctest -L tsan` runs it under TSan.
#include <doctest/doctest.h>

#include <o3sdk/usb/ring.h>

#include "usb_test_util.h"

#include <atomic>
#include <cstdint>
#include <thread>

using o3sdk::usb::OverwriteRing;

TEST_CASE("usb_ring: producer and consumer threads never see a torn or reordered item") {
  // Run as a labelled case under ThreadSanitizer too (`ctest -L tsan`, see tests/CMakeLists.txt). The producer
  // writes consecutive numbers in chunks of 1..4 as fast as it can; the consumer is slower on purpose so the
  // ring laps. Invariants: every read is a run of consecutive values, runs are strictly increasing, and at
  // the end read + dropped == written. With a torn copy the first two would break.
  constexpr std::uint32_t kTotal = 600000;
  static OverwriteRing<std::uint32_t, 64, 4> ring;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    usb_test::Rng rng(5);
    std::uint32_t next = 0;
    while (next < kTotal) {
      std::uint32_t chunk[4];
      std::uint32_t n = 1 + rng.below(4);
      if (n > kTotal - next) {
        n = kTotal - next;
      }
      for (std::uint32_t i = 0; i < n; ++i) {
        chunk[i] = next + i;
      }
      ring.write(chunk, n);
      next += n;
      if ((next & 0x3FFu) == 0) {
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });

  usb_test::Rng rng(11);
  std::uint64_t read_total = 0;
  bool have_last = false;
  std::uint32_t last = 0;
  bool bad = false;
  const auto consume = [&]() -> std::uint32_t {
    std::uint32_t out[16];
    const std::uint32_t n = ring.read(out, 1 + rng.below(16));
    for (std::uint32_t i = 0; i < n; ++i) {
      if (i > 0 && out[i] != out[i - 1] + 1u) {
        bad = true;
      }
      if (i == 0 && have_last && out[0] <= last) {
        bad = true;
      }
      if (i + 1 == n) {
        last = out[i];
        have_last = true;
      }
    }
    read_total += n;
    return n;
  };
  while (!done.load(std::memory_order_acquire)) {
    consume();
    if (rng.below(8) == 0) {
      std::this_thread::yield();
    }
  }
  producer.join();
  while (consume() != 0) {
  }
  CHECK_FALSE(bad);
  CHECK(read_total + ring.dropped() == kTotal);
  CHECK(read_total > 0);
  MESSAGE("threads: read " << read_total << ", dropped " << ring.dropped());
}
