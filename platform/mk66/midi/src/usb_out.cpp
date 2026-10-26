// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-MIDI out (midi/usb_out.h): the events of one frame.
#include <o3sdk/platform/mk66/midi/usb_out.h>

namespace o3sdk::platform::mk66::midi {

namespace {
// A real-time event: its first message byte is 0xF8..0xFF (the word's byte 1).
constexpr bool word_is_realtime(std::uint32_t event) noexcept { return is_realtime(static_cast<std::uint8_t>((event >> 8) & 0xFFu)); }
}  // namespace

// GCC 13's -Wstringop-overflow misreads the inlined slot store of core::SpscRing::push as a write past the atomic beside the ring (see din.cpp): a false positive.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
bool UsbMidiOut::push(std::uint64_t due, std::uint32_t event) noexcept {
  UsbItem it{};
  it.due = due;
  it.event = event;
  it.epoch = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  return word_is_realtime(event) ? rt_.push(it) : msg_.push(it);
}

#pragma GCC diagnostic pop

void UsbMidiOut::fill(std::uint8_t ep) noexcept {
  if (rt_head_valid_ && rt_head_.epoch != ep) {
    rt_head_valid_ = false;
  }
  while (!rt_head_valid_ && rt_.pop(rt_head_)) {
    rt_head_valid_ = rt_head_.epoch == ep;
  }
  if (msg_head_valid_ && msg_head_.epoch != ep) {
    msg_head_valid_ = false;
  }
  while (!msg_head_valid_ && msg_.pop(msg_head_)) {
    msg_head_valid_ = msg_head_.epoch == ep;
  }
}

std::uint32_t UsbMidiOut::frame(std::uint64_t frame_start, std::uint32_t* out, std::uint32_t max) noexcept {
  const std::uint32_t cap = max < usb_out::kEventsPerFrame ? max : usb_out::kEventsPerFrame;
  const std::uint8_t ep = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  std::uint32_t n = 0;
  // Real-time events first (they are the ones whose lateness is a claim), then the others in due order. Each ring is in due order, because the
  // render's staging queue hands items over sorted.
  for (;;) {
    fill(ep);
    if (n >= cap || !rt_head_valid_ || rt_head_.due > frame_start) {
      break;
    }
    out[n++] = rt_head_.event;
    const std::uint64_t late = frame_start - rt_head_.due;
    ++stats_.events;
    ++stats_.rt_events;
    stats_.lateness_sum += late;
    if (late > stats_.rt_lateness_max) {
      stats_.rt_lateness_max = late;
    }
    if (late > stats_.lateness_max) {
      stats_.lateness_max = late;
    }
    rt_head_valid_ = false;
  }
  for (;;) {
    fill(ep);
    if (!msg_head_valid_ || msg_head_.due > frame_start) {
      break;
    }
    if (n >= cap) {
      ++stats_.deferred;  // due, but the packet is full: it goes in the next frame
      break;
    }
    out[n++] = msg_head_.event;
    const std::uint64_t late = frame_start - msg_head_.due;
    ++stats_.events;
    stats_.lateness_sum += late;
    if (late > stats_.lateness_max) {
      stats_.lateness_max = late;
    }
    msg_head_valid_ = false;
  }
  return n;
}

}  // namespace o3sdk::platform::mk66::midi
