// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioStatus under a real concurrent writer and reader (host only: std::thread, linked through Threads::Threads in
// tests/CMakeLists.txt; the build has no ThreadSanitizer target, -DO3SDK_SANITIZE=ON gives AddressSanitizer + UBSan only): the render
// group is a seqlock, so a snapshot that is returned with an even generation is one writer's whole publish, never a mixture of
// two. Each published sample has all its fields derived from one counter, so any tearing is visible as a field that does not match.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/audio/status.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

TEST_CASE("audio status thread: a snapshot with an even generation is one whole publish") {
  o3sdk::platform::mk66::audio::AudioStatus status;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint32_t k = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      o3sdk::platform::mk66::audio::RenderSample s{};
      s.total = k;
      s.latency = k;
      for (unsigned i = 0; i < o3sdk::platform::mk66::audio::kStageCount; ++i) {
        s.stage[i] = k;
      }
      s.input_peak = k;
      status.render_publish(s);
      ++k;
      // A rest every 16 publishes: a reader of a seqlock is starved by a writer that never stops (8 tries inside a publish, and under TSan on a loaded machine the publishes
      // are most of the time), and then fewer than 1000 snapshots come back whole. The bursts in between are the contention this test is about; the render's real period is 2.9 ms.
      if ((k & 15u) == 0u) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  });
  std::uint64_t consistent = 0;
  std::uint64_t torn = 0;
  for (std::uint32_t i = 0; i < 200000; ++i) {
    const o3sdk::platform::mk66::audio::StatusSnapshot snap = status.snapshot();
    if ((snap.generation & 1u) != 0u) {
      continue;  // the bounded retries ran out inside a publish: a diagnostic tool sees the generation and ignores it
    }
    ++consistent;
    bool whole = snap.render_last == snap.latency_last && snap.render_last == snap.input_peak;
    for (unsigned s = 0; s < o3sdk::platform::mk66::audio::kStageCount; ++s) {
      whole = whole && snap.stage_last[s] == snap.render_last;
    }
    // renders counts publishes, last is the counter of the latest: they agree when the snapshot is whole.
    whole = whole && snap.renders == snap.render_last;
    torn += whole ? 0u : 1u;
  }
  stop.store(true);
  writer.join();
  CHECK(consistent > 1000u);
  CHECK(torn == 0u);
}
