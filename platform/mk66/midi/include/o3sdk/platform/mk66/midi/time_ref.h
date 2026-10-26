// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one reference pair that ties the CPU cycle counter to the audio timeline for every interrupt that has to know "what time is it on the
// timeline" (OS-005c2; ADR 0013 D1 and D5). The render publishes it at the start of every block; the DIN emitter's PIT interrupt (priority 96) and
// the USB interrupt (priority 64), both ABOVE the render (128), read it. The audio clock and the cycle counter are one PLL (4,080 cycles a sample
// exactly, core/audio_clock.h), so one pair per block is the whole map: no estimation, no filter.
//
//     timeline cycles of the cycle counter value `now`  =  cycles_from_samples(sample) + (int32)(now - cycles)
//
// where `sample` is the timeline sample whose sound is HEARD when the counter reads `cycles` (the platform adds the render-ahead of the TX ring:
// platform/mk66 `kAudioLeadCycles`; a MIDI byte due at a sample then leaves the wire together with that sample's sound). The unit is the unit of
// midi::OutItem::due and UsbItem::due: CPU cycles on the audio timeline.
//
// CONTEXTS AND PROTOCOL (a documented single-writer protocol, no lock). publish() has exactly one caller, the render context. to_timeline() is called
// by interrupts of HIGHER priority than the writer, which can preempt publish() but are never preempted by it. Two slots and an index: the writer
// fills the slot the index does not name, then flips the index with a release store; a reader that preempts the writer between the fill and the flip
// reads the old slot, which nobody is writing, and one that runs after the flip reads the new one. The writer never touches the slot a reader can be
// reading, because a reader cannot be running while the writer runs. The reader must NOT be of lower priority than the writer (or on another core
// without the barriers here being enough: they are, release/acquire): a main-loop reader would need a retry loop, and none is offered.
//
// The base is precomputed by publish() (render context, from flash): the interrupt side is two loads, an add and a compare, no division, so it can
// live in SRAM_L whatever the sample-rate constant is (the 44,100 Hz retune divides in cycles_from_samples).

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/qtime.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::midi {

class TimeRef {
 public:
  // The render, at the start of every block: the timeline sample `sample` is heard when the cycle counter reads `cycles`.
  void publish(std::uint64_t sample, std::uint32_t cycles) noexcept {
    const std::uint32_t w = 1u - active_.load(std::memory_order_relaxed);  // the slot no reader is using
    slot_[w].base = core::cycles_from_samples(sample);
    slot_[w].cycles = cycles;
    active_.store(w, std::memory_order_release);
    valid_.store(true, std::memory_order_release);
  }

  bool valid() const noexcept { return valid_.load(std::memory_order_acquire); }

  // The timeline time, in CPU cycles, of the cycle counter value `now` (within +-2^31 cycles, 11.9 s, of the last publish: only differences of the
  // counter are meaningful). False before the first publish. Never negative: the start of the timeline is the floor.
  O3SDK_AUDIO_INLINE bool to_timeline(std::uint32_t now, std::uint64_t& out) const noexcept {
    if (!valid_.load(std::memory_order_acquire)) {
      return false;
    }
    const Slot& s = slot_[active_.load(std::memory_order_acquire)];
    const std::int64_t t = static_cast<std::int64_t>(s.base) + static_cast<std::int32_t>(now - s.cycles);
    out = t < 0 ? 0u : static_cast<std::uint64_t>(t);
    return true;
  }

 private:
  struct Slot {
    std::uint64_t base = 0;     // cycles_from_samples(sample)
    std::uint32_t cycles = 0;   // the counter value at which that sample is heard
    std::uint32_t pad = 0;
  };
  Slot slot_[2];
  std::atomic<std::uint32_t> active_{0};
  std::atomic<bool> valid_{false};
};

}  // namespace o3sdk::platform::mk66::midi
