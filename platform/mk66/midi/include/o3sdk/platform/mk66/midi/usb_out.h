// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// USB-MIDI out: packets quantised to the 1 ms frame (OS-005c; ADR 0013 D5: "USB MIDI is 1 ms-quantised by the bus"; radar card midi-clock-sync,
// "Master", point 4). Pure; the driver of OS-005c2 calls frame() at each start of frame and writes the events it returns into the bulk-IN buffer
// (usb::MidiRing, OS-006b).
//
// THE MODEL. The device can only hand data to the host in a transfer the host asks for, in practice once a frame. So an event due at time t
// cannot arrive before the first frame that starts at or after t, and is sent in it: NEVER EARLY, late by 0 to 1 ms (mean 0.5 ms, sigma
// 289 us for uniformly distributed due times), which the per-port OUT offset cancels on average. frame() takes the start time of the frame
// (the SOF interrupt's time on the audio timeline: its phase against the audio clock is the platform's to measure, the USB layer already
// estimates it for the feedback value) and returns the events whose time has come, real-time ones first (clock, start, continue, stop), then the
// others in due order, up to the capacity of one bulk packet (16 events, 64 bytes); what does not fit waits for the next frame and is late
// by one more millisecond, counted.
//
// An event is the 32-bit USB-MIDI word of midi::usb_event(), in wire order. The rings are single producer (the render) single consumer (the SOF
// interrupt), as core/ring.h requires.

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/ring.h>
#include <o3sdk/platform/mk66/midi/message.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace o3sdk::platform::mk66::midi {

namespace usb_out {
inline constexpr std::uint32_t kFrameMicros = 1000;
// CPU cycles in one frame: 180,000 at 180 MHz.
inline constexpr std::uint64_t kFrameCycles = std::uint64_t{core::kCpuHz} / 1000u;
inline constexpr std::uint32_t kEventsPerFrame = 16;  // one 64-byte bulk packet of four-byte events
inline constexpr std::uint32_t kRingSize = 256;
inline constexpr std::uint32_t kRtRingSize = 64;
}  // namespace usb_out

struct UsbItem {
  std::uint64_t due;    // CPU cycles on the audio timeline
  std::uint32_t event;  // the USB-MIDI word
  std::uint8_t epoch;
  std::uint8_t pad[3];
};
static_assert(sizeof(UsbItem) == 16 && std::is_trivially_copyable<UsbItem>::value, "UsbItem is two words of eight");

class UsbMidiOut {
 public:
  struct Stats {
    std::uint32_t events = 0;
    std::uint32_t rt_events = 0;
    std::uint32_t deferred = 0;           // frames in which an event that was due did not fit the packet and waited for the next
    std::uint64_t lateness_max = 0;       // cycles after the due time, over all events sent
    std::uint64_t rt_lateness_max = 0;
    std::uint64_t lateness_sum = 0;
  };

  // Producer (the render). Real-time events (clock, start, continue, stop: one byte, 0xF8..0xFF) go in their own ring.
  bool push(std::uint64_t due, std::uint32_t event) noexcept;
  std::uint32_t room() const noexcept { return usb_out::kRingSize - msg_.size(); }

  // Producer: everything pushed before this call is discarded at the next frame (see DinEmitter::request_flush).
  void request_flush() noexcept { epoch_.fetch_add(1u, std::memory_order_acq_rel); }

  // Consumer (the SOF interrupt): the events to put in the packet of the frame that starts at `frame_start` (CPU cycles): those due at or
  // before it, real-time first, then in due order, at most `max` (kEventsPerFrame at most). Returns the count.
  O3SDK_AUDIO_HOT std::uint32_t frame(std::uint64_t frame_start, std::uint32_t* out, std::uint32_t max) noexcept;

  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

 private:
  O3SDK_AUDIO_HOT void fill(std::uint8_t ep) noexcept;

  core::SpscRing<UsbItem, usb_out::kRtRingSize> rt_;
  core::SpscRing<UsbItem, usb_out::kRingSize> msg_;
  std::atomic<std::uint32_t> epoch_{0};
  UsbItem rt_head_{};
  bool rt_head_valid_ = false;
  UsbItem msg_head_{};
  bool msg_head_valid_ = false;
  Stats stats_{};
};

}  // namespace o3sdk::platform::mk66::midi
