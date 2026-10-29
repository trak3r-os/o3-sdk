// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/record_service.h>

#include <o3sdk/dsp/engine.h>
#include <o3sdk/dsp/transport.h>

namespace o3sdk::platform::mk66::audio {

namespace hr = hal::record;

hr::State RecordService::state() const noexcept {
  switch (tap_.state()) {
    case RecTap::State::Idle: return hr::State::Idle;
    case RecTap::State::Waiting: return hr::State::Waiting;
    case RecTap::State::Listening: return hr::State::Listening;
    case RecTap::State::Armed: return hr::State::Recording;
    case RecTap::State::Done: return hr::State::Done;
  }
  return hr::State::Idle;
}

bool RecordService::arm(const hr::Take& take) noexcept {
  if (tap_.state() != RecTap::State::Idle) {
    return false;
  }
  const unsigned channels = hr::channel_count(take.channels);
  // One channel of the pair, or the mix of both, for a mono take (RecTap::set_channel); the stereo take ignores it.
  tap_.set_channel(take.channels == hr::Channels::Left ? RecTap::Channel::Left : (take.channels == hr::Channels::Right ? RecTap::Channel::Right : RecTap::Channel::Mix));
  armed_tap_ = tap();
  armed_align_ = take.align;
  stop_rule_.store(static_cast<std::uint8_t>(hr::Align::Free), std::memory_order_relaxed);
  if (take.align == hr::Align::Level) {
    return tap_.arm_level(take.buf, take.capacity_frames, channels, take.threshold, take.preroll_frames, take.auto_steps);
  }
  return tap_.arm(take.buf, take.capacity_frames, channels, rec_align(take.align), take.auto_steps);
}

hal::audio::BlockCallback RecordService::wrap(hal::audio::BlockCallback inner, const dsp::Engine& engine) noexcept {
  inner_ = inner;
  engine_ = &engine;
  return hal::audio::BlockCallback{&RecordService::block, this};
}

void RecordService::block(void* user, const hal::audio::BlockIo& io) noexcept {
  RecordService& self = *static_cast<RecordService*>(user);
  if (self.inner_.fn != nullptr) {
    self.inner_.fn(self.inner_.user, io);
  }
  // Idle, or a take that is Done and waiting for the app to take it: one load and a branch, and nothing else.
  const RecTap::State s = self.tap_.state();
  if (s == RecTap::State::Idle || s == RecTap::State::Done) {
    return;
  }
  self.record_block(io, s);
}

void RecordService::record_block(const hal::audio::BlockIo& io, RecTap::State s) noexcept {
  constexpr std::uint32_t n = hal::audio::kBlock;
  active_.store(active_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  // The events of this block come before its push (taps.h): the step a take starts or ends on, and the stop. A Listening take counts none (its trigger block's steps are not counted).
  if (s != RecTap::State::Listening && engine_ != nullptr) {
    const core::ClockFrame& f = engine_->block_frame();
    dsp::Transport t;
    t.playing = f.playing;
    t.started = f.started;
    t.stopped = f.stopped;
    t.tempo_mbpm = f.tempo_mbpm;
    t.pos = f.pos;
    t.inc = f.inc;
    t.frames = n;
    t.following = f.follow;
    const std::uint32_t pattern = pattern_steps_.load(std::memory_order_relaxed);
    // Which boundary RecTap asks about: the take's start alignment while it waits, the pending stop_at() once it records (tap_step: 0 at a boundary of that rule, 1 elsewhere).
    dsp::for_each_step(t, dsp::kTicksPerSixteenth, 0u, 0u, [&](std::uint32_t frame, std::uint64_t k) {
      const hr::Align rule = tap_.state() == RecTap::State::Waiting ? armed_align_ : static_cast<hr::Align>(stop_rule_.load(std::memory_order_relaxed));
      tap_.on_step(tap_step(k, pattern, rule), frame);
    });
    if (f.stopped) {
      tap_.on_stopped(0u);  // (the engine applies a stop at frame 0 of the block)
    }
  }
  const RecTap::State now = tap_.state();
  if (now != RecTap::State::Armed && now != RecTap::State::Listening) {
    // Still Waiting for its step: nothing is written, so no block is split. push() is what ends a Waiting take that disarm() cancelled; it reads no plane in this state.
    alignas(4) static const std::int16_t kNone[n] = {};
    tap_.push(kNone, kNone, n);
    return;
  }
  const std::int16_t* src = armed_tap_ == hr::Tap::Master ? io.out : io.in;
  // The Input tap hears the line's channel map, as the engine's own input does (the mean of the two channels, or one of them, in both planes).
  const std::uint8_t map = armed_tap_ == hr::Tap::Input ? line_map_.load(std::memory_order_relaxed) : 0u;
  alignas(4) std::int16_t l[n];
  alignas(4) std::int16_t r[n];
  static_assert(n % 2u == 0u, "the planes are whole words");
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t a = src[2u * i];
    const std::int32_t b = src[2u * i + 1u];
    const std::int32_t v = map == static_cast<std::uint8_t>(hr::LineMap::Mix) ? (a + b + 1) >> 1 : (map == static_cast<std::uint8_t>(hr::LineMap::Left) ? a : b);
    l[i] = static_cast<std::int16_t>(map == 0u ? a : v);
    r[i] = static_cast<std::int16_t>(map == 0u ? b : v);
  }
  tap_.push(l, r, n);
}

namespace {
RecordService g_service;  // SRAM_U .bss, about 150 bytes (built by the static constructors; the take's buffer is the app's, in the SDRAM)
}

namespace record_service {
RecordService& service() noexcept { return g_service; }
}  // namespace record_service

}  // namespace o3sdk::platform::mk66::audio
