// SPDX-License-Identifier: GPL-3.0-or-later
//
// M2c slice AP (new): TapMem (audio/tap_mem.h) with the tracker's meter store cut. The object is plain storage that a constructor zeroes (the SRAM_L section the image puts it in is
// not cleared by the reset sequence), it must fit what SRAM_L leaves after the ramfunc budget (mem_map.h), and reset() brings every ring back to the state of a new one.
#include <doctest/doctest.h>

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio/tap_mem.h>
#include <o3sdk/platform/mk66/mem_map.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstring>

namespace {

namespace ma = o3sdk::platform::mk66::audio;
namespace mem = o3sdk::platform::mk66::mem;

// The free half of SRAM_L (from kSramLFreeBase to the end): the stem pool is about 14.5 KB, the two sample rings 2 KB each.
static_assert(sizeof(ma::TapMem) <= mem::kSramLBase + mem::kSramLSize - mem::kSramLFreeBase, "the tap memory fits SRAM_L's free half");
static_assert(alignof(ma::TapMem) >= 4u, "the rings are touched by word loads");

}  // namespace

TEST_CASE("tap mem: it is zero when made, and reset() makes it so again") {
  static o3sdk::test::StaticSlot<ma::TapMem> slot;
  ma::TapMem& m = slot.make();
  CHECK(m.scope.head() == 0u);
  CHECK(m.spectrum.head() == 0u);
  CHECK(m.stem.ring.size() == 0u);

  alignas(4) std::int16_t block[o3sdk::hal::audio::kBlockSamples];
  std::memset(block, 0x55, sizeof block);
  m.scope.write(block, o3sdk::hal::audio::kBlockSamples);
  m.spectrum.write(block, o3sdk::hal::audio::kBlock);
  CHECK(m.scope.head() == o3sdk::hal::audio::kBlockSamples);
  CHECK(m.spectrum.head() == o3sdk::hal::audio::kBlock);

  m.reset();
  CHECK(m.scope.head() == 0u);
  CHECK(m.spectrum.head() == 0u);
  alignas(4) std::int16_t window[2u * ma::kScopeWindowFrames];
  const ma::ScopeRing::Window w = m.scope.read_latest(window, 2u * ma::kScopeWindowFrames);
  CHECK(w.whole);
  for (std::int16_t v : window) {
    CHECK(v == 0);  // a ring that has not filled reads zero in front, and reset() erased the 0x5555s
  }
}

TEST_CASE("tap mem: a StemTap over its pool starts disabled and publishes nothing") {
  static o3sdk::test::StaticSlot<ma::TapMem> slot;
  ma::TapMem& m = slot.make();
  ma::StemTap tap(m.stem);
  CHECK_FALSE(tap.enabled());
  CHECK(tap.published() == 0u);
  CHECK(&tap.ring() == &m.stem.ring);
}
