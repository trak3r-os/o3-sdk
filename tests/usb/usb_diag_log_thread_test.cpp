// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the `diag log` ring with real concurrent writers (host only: it needs std::thread, so it is not in the QEMU suite).
// Several threads write at once while a reader reads; every record the reader accepts must be whole and every sequence number
// must be handed out exactly once. Also runs under ThreadSanitizer when the host build enables it.
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/diag_log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace o3sdk::usb::diag;

namespace {
LogRecord g_thread_slots[1024];
}

TEST_CASE("usb log: concurrent writers get unique sequence numbers and the reader never sees a torn record") {
  LogRing ring;
  ring.init(g_thread_slots, 1024);
  constexpr unsigned kWriters = 4;
  constexpr unsigned kEach = 5000;
  std::atomic<bool> go{false};
  std::atomic<unsigned> done{0};
  std::thread writers[kWriters];
  for (unsigned w = 0; w < kWriters; ++w) {
    writers[w] = std::thread([&, w] {
      while (!go.load(std::memory_order_acquire)) {
      }
      char text[40];
      for (unsigned i = 0; i < kEach; ++i) {
        // The text names its writer and a counter twice: a torn record would disagree with itself.
        std::snprintf(text, sizeof text, "w%u n%u w%u n%u", w, i, w, i);
        ring.write(w * 100000u + i, text);
      }
      done.fetch_add(1, std::memory_order_release);
    });
  }
  std::uint32_t whole = 0;
  std::uint32_t torn = 0;
  go.store(true, std::memory_order_release);
  std::uint32_t next = 0;
  while (done.load(std::memory_order_acquire) < kWriters || next < ring.written()) {
    LogEntry e;
    if (next < ring.written() && next >= ring.oldest() && ring.read(next, e)) {
      unsigned w1, n1, w2, n2;
      if (std::sscanf(e.text, "w%u n%u w%u n%u", &w1, &n1, &w2, &n2) == 4 && w1 == w2 && n1 == n2 && e.ms == w1 * 100000u + n1) {
        ++whole;
      } else {
        ++torn;
      }
    }
    if (next < ring.written()) {
      ++next;
    }
  }
  for (std::thread& t : writers) {
    t.join();
  }
  CHECK(ring.written() == kWriters * kEach);
  CHECK(torn == 0u);
  CHECK(whole > 0u);
}
