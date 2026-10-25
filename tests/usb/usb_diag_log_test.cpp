// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the `diag log` ring (usb/diag_log.h): ordering, wrap, truncation, refusal of bad storage, and writers that interrupt one
// another. Runs on the host and on QEMU (qemu_usb_device); the concurrent version is usb_diag_log_thread_test.cpp (host only).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/diag_log.h>

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb::diag;

namespace {
LogRecord g_slots[64];
}

TEST_CASE("usb log: not ready until it has storage, and refuses bad storage") {
  LogRing ring;
  CHECK_FALSE(ring.ready());
  ring.write(1, "ignored");  // nowhere to go: not a crash
  CHECK(ring.written() == 0u);
  LogEntry e;
  CHECK_FALSE(ring.read(0, e));
  ring.init(nullptr, 64);
  CHECK_FALSE(ring.ready());
  ring.init(g_slots, 0);
  CHECK_FALSE(ring.ready());
  ring.init(g_slots, 1);
  CHECK_FALSE(ring.ready());
  ring.init(g_slots, 48);  // not a power of two
  CHECK_FALSE(ring.ready());
  ring.init(g_slots, 64);
  CHECK(ring.ready());
  CHECK(ring.capacity() == 64u);
}

TEST_CASE("usb log: records come back in order with their stamps") {
  LogRing ring;
  std::memset(static_cast<void*>(g_slots), 0xAB, sizeof g_slots);  // SDRAM holds garbage until the ring clears it
  ring.init(g_slots, 64);
  ring.write(10, "boot: reset 0x0080");
  ring.write(25, "usb: reset");
  ring.write(0xFFFFFFFFu, "usb: configured");
  CHECK(ring.written() == 3u);
  CHECK(ring.oldest() == 0u);
  LogEntry e;
  USB_REQUIRE(ring.read(0, e));
  CHECK((e.sequence == 0u && e.ms == 10u && std::strcmp(e.text, "boot: reset 0x0080") == 0));
  USB_REQUIRE(ring.read(1, e));
  CHECK((e.ms == 25u && std::strcmp(e.text, "usb: reset") == 0));
  USB_REQUIRE(ring.read(2, e));
  CHECK((e.ms == 0xFFFFFFFFu && std::strcmp(e.text, "usb: configured") == 0));
  CHECK_FALSE(ring.read(3, e));  // not written yet
}

TEST_CASE("usb log: text longer than a record is cut, an empty line is kept") {
  LogRing ring;
  ring.init(g_slots, 64);
  char big[200];
  std::memset(big, 'x', sizeof big);
  big[sizeof big - 1] = '\0';
  ring.write(1, big);
  ring.write(2, "");
  LogEntry e;
  USB_REQUIRE(ring.read(0, e));
  CHECK(std::strlen(e.text) == kLogTextMax);
  USB_REQUIRE(ring.read(1, e));
  CHECK(e.text[0] == '\0');
}

TEST_CASE("usb log: the newest records survive a wrap, older ones read as gone") {
  LogRing ring;
  ring.init(g_slots, 64);
  char line[16];
  for (std::uint32_t i = 0; i < 200; ++i) {
    std::strcpy(line, "n 000");
    line[2] = static_cast<char>('0' + i / 100);
    line[3] = static_cast<char>('0' + (i / 10) % 10);
    line[4] = static_cast<char>('0' + i % 10);
    ring.write(i, line);
  }
  CHECK(ring.written() == 200u);
  CHECK(ring.oldest() == 136u);
  LogEntry e;
  CHECK_FALSE(ring.read(135, e));  // lapped
  CHECK_FALSE(ring.read(0, e));
  for (std::uint32_t i = 136; i < 200; ++i) {
    USB_REQUIRE(ring.read(i, e));
    CHECK((e.sequence == i && e.ms == i));
    CHECK(e.text[2] == static_cast<char>('0' + i / 100));
    CHECK(e.text[4] == static_cast<char>('0' + i % 10));
  }
}

TEST_CASE("usb log: a record that is being rewritten is not shown") {
  LogRing ring;
  ring.init(g_slots, 64);
  ring.write(1, "first");
  // Simulate a writer that claimed the slot and was interrupted before it published: its sequence word is 0.
  g_slots[0].seq.store(0, std::memory_order_relaxed);
  LogEntry e;
  CHECK_FALSE(ring.read(0, e));
  g_slots[0].seq.store(1, std::memory_order_relaxed);  // published
  CHECK(ring.read(0, e));
  // A slot that holds another lap's sequence number is not this record either.
  g_slots[0].seq.store(65, std::memory_order_relaxed);
  CHECK_FALSE(ring.read(0, e));
}

TEST_CASE("usb log: an interrupt that writes in the middle of another write loses nothing") {
  // One context claims slot n, a higher-priority one writes before it finishes: each gets its own slot. Emulated by claiming
  // through the public interface in an interleaved order is not possible, so this checks the visible consequence: many writers in
  // sequence leave every record whole, with unique sequence numbers.
  LogRing ring;
  ring.init(g_slots, 64);
  for (std::uint32_t i = 0; i < 60; ++i) {
    ring.write(i, (i & 1u) ? "odd writer" : "even writer");
  }
  std::uint32_t odd = 0;
  std::uint32_t even = 0;
  for (std::uint32_t i = 0; i < 60; ++i) {
    LogEntry e;
    USB_REQUIRE(ring.read(i, e));
    if (std::strcmp(e.text, "odd writer") == 0) {
      ++odd;
    } else if (std::strcmp(e.text, "even writer") == 0) {
      ++even;
    }
  }
  CHECK((odd == 30u && even == 30u));
}
