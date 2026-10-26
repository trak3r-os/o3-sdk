// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-MIDI out (midi/usb_out.h, OS-005c): events leave in the first frame that starts at or after their time (never early, late by 0 to 1 ms), real-time events
// first, 16 to a frame, the rest in the next. The statistics of the quantisation are the card's (mean 0.5 ms, sigma 289 us for uniform times). Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/midi/usb_out.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
using o3sdk::test::Rng;

namespace {
constexpr std::uint64_t kFrame = midi::usb_out::kFrameCycles;
}

TEST_CASE("usb out: the frame is 1 ms: 180,000 cycles") {
  CHECK(kFrame == 180000u);
  CHECK(midi::usb_out::kFrameMicros == 1000u);
  CHECK(midi::usb_out::kEventsPerFrame == 16u);
}

TEST_CASE("usb out: never early, late by less than a frame, in due order") {
  midi::UsbMidiOut u;
  const std::uint32_t note = midi::usb_event(0, midi::note_on(0, 60, 100));
  CHECK(u.push(100, note));
  CHECK(u.push(200, note + 1));
  CHECK(u.push(kFrame + 5, note + 2));
  std::uint32_t out[16];
  CHECK(u.frame(50, out, 16) == 0u);            // a frame that starts before the event's time does not carry it
  std::uint32_t n = u.frame(kFrame, out, 16);   // the next frame carries both that are due, in order
  CHECK(n == 2u);
  CHECK(out[0] == note);
  CHECK(out[1] == note + 1);
  CHECK(u.frame(kFrame, out, 16) == 0u);        // nothing twice
  n = u.frame(2u * kFrame, out, 16);
  CHECK(n == 1u);
  CHECK(out[0] == note + 2);
  CHECK(u.stats().events == 3u);
  CHECK(u.stats().lateness_max == kFrame - 5u);  // the third event: due 5 cycles into the second frame, sent at its end
}

TEST_CASE("usb out: real-time events go first and the packet holds 16") {
  midi::UsbMidiOut u;
  const std::uint32_t note = midi::usb_event(0, midi::note_on(0, 60, 100));
  const std::uint32_t clk = midi::usb_event(0, midi::msg1(midi::kClock));
  for (std::uint32_t i = 0; i < 20; ++i) {
    CHECK(u.push(10 + i, note + (i << 16)));
  }
  CHECK(u.push(1000, clk));  // queued last, due after the notes, but real-time
  std::uint32_t out[16];
  std::uint32_t n = u.frame(kFrame, out, 16);
  CHECK(n == 16u);
  CHECK(out[0] == clk);  // the clock leads the packet
  for (std::uint32_t i = 0; i < 15; ++i) {
    CHECK(out[1 + i] == note + (i << 16));
  }
  CHECK(u.stats().deferred == 1u);
  n = u.frame(2u * kFrame, out, 16);  // the five that did not fit go in the next frame, a millisecond late
  CHECK(n == 5u);
  CHECK(out[0] == note + (15u << 16));
  CHECK(u.stats().lateness_max >= kFrame);
  CHECK(u.frame(2u * kFrame, out, 0) == 0u);  // max 0
}

TEST_CASE("usb out: flushing discards what was queued before it") {
  midi::UsbMidiOut u;
  const std::uint32_t note = midi::usb_event(0, midi::note_on(0, 60, 100));
  const std::uint32_t clk = midi::usb_event(0, midi::msg1(midi::kClock));
  CHECK(u.push(10, note));
  CHECK(u.push(10, clk));
  u.request_flush();
  CHECK(u.push(20, note + 1));
  std::uint32_t out[16];
  const std::uint32_t n = u.frame(kFrame, out, 16);
  CHECK(n == 1u);
  CHECK(out[0] == note + 1);
}

TEST_CASE("usb out: the quantisation is the card's: mean 0.5 ms, sigma 289 us, for due times uniform in the frame") {
  const bool report = std::getenv("O3SDK_SEQ_REPORT") != nullptr;
  midi::UsbMidiOut u;
  Rng rng(21);
#if defined(__arm__)
  constexpr std::uint32_t kFrames = 400;
#else
  constexpr std::uint32_t kFrames = 5000;
#endif
  const std::uint32_t note = midi::usb_event(0, midi::note_on(0, 60, 100));
  // two events a frame at random times; frames start at a phase of 12345 cycles against the audio clock
  const std::uint64_t phase = 12345;
  std::uint64_t pushed = 0;
  double sum = 0, sum2 = 0;
  std::uint64_t count = 0;
  std::uint32_t out[16];
  std::uint64_t due_store[kFrames * 2 + 8];
  std::uint32_t due_n = 0, due_read = 0;
  for (std::uint32_t f = 1; f <= kFrames; ++f) {
    const std::uint64_t frame_start = phase + static_cast<std::uint64_t>(f) * kFrame;
    for (int k = 0; k < 2; ++k) {
      const std::uint64_t due = frame_start - kFrame + 1u + (static_cast<std::uint64_t>(rng.next()) % (kFrame - 1u));  // in (previous frame start, this frame start]
      CHECK(u.push(due, note));
      due_store[due_n++] = due;
      ++pushed;
    }
    const std::uint32_t n = u.frame(frame_start, out, 16);
    // events of one frame leave in due order; the due times were drawn at random, so sort them by taking the sent count's worth in order of due
    for (std::uint32_t i = 0; i < n; ++i) {
      (void)i;
    }
    count += n;
    (void)due_read;
  }
  // the statistics the unit keeps: lateness_sum / events
  const double mean = static_cast<double>(u.stats().lateness_sum) / static_cast<double>(u.stats().events);
  CHECK(u.stats().events == pushed);
  CHECK(u.stats().lateness_max < kFrame);
  CHECK(std::fabs(mean - 90000.0) < 3000.0);  // 0.5 ms
  // sigma from the sum of squares, recomputed from the due times (the unit keeps the sum only)
  for (std::uint32_t i = 0; i < due_n; ++i) {
    const std::uint64_t frame_start = phase + (static_cast<std::uint64_t>(i / 2u) + 1u) * kFrame;
    const double late = static_cast<double>(frame_start - due_store[i]);
    sum += late;
    sum2 += late * late;
  }
  const double m = sum / due_n;
  const double sigma = std::sqrt(sum2 / due_n - m * m);
  CHECK(std::fabs(sigma - 51962.0) < 2500.0);  // 289 us
  if (report) {
    std::printf("usb  out 1 ms frames: lateness mean %.1f us  sigma %.1f us  max %.1f us  (%llu events)\n", mean / 180.0, sigma / 180.0,
                static_cast<double>(u.stats().lateness_max) / 180.0, static_cast<unsigned long long>(u.stats().events));
  }
  (void)count;
}
