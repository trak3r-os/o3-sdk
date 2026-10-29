// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/taps.h>

#include <o3sdk/platform/mk66/audio/kernels.h>

#include <algorithm>
#include <cstring>

namespace o3sdk::platform::mk66::audio {

// ---- StemTap ----------------------------------------------------------------------------------------------------------------

void StemPool::reset() noexcept {
  ring.reset();  // a ring in use is never reset: the render and the USB reader are stopped, like everything here
  for (unsigned c = 0; c < kChannels; ++c) {
    for (unsigned b = 0; b < kPool; ++b) {
      for (unsigned i = 0; i < usb::kStemBlockFrames; ++i) {
        pool[c][b][i] = 0;
      }
    }
  }
  for (unsigned t = 0; t < kStaged; ++t) {
    for (unsigned i = 0; i < hal::audio::kBlock; ++i) {
      stage[t][i] = 0;
    }
  }
}

StemTap::StemTap(StemPool& pool) noexcept : pool_(pool), sum_(&usb::sum_slots) {
  reset();
}

void StemTap::reset() noexcept {
  enabled_.store(false, std::memory_order_relaxed);
  channels_.store(kDefaultChannels, std::memory_order_relaxed);
  // The default slots are profile P10 (ADR 0012): T1..T8, then Master L and Master R.
  for (unsigned c = 0; c < kChannels; ++c) {
    mask_[c].store(0, std::memory_order_relaxed);
  }
  for (unsigned c = 0; c < 8u; ++c) {
    mask_[c].store(1u << c, std::memory_order_relaxed);
  }
  mask_[8].store(1u << stem_src::kMasterL, std::memory_order_relaxed);
  mask_[9].store(1u << stem_src::kMasterR, std::memory_order_relaxed);
  was_enabled_ = false;
  half_ = 0;
  slot_ = 0;
  published_.store(0, std::memory_order_relaxed);
  pool_.reset();
}

void StemTap::push(const std::int16_t* const* sources) noexcept {
  // `sources` may be null (the graph passes null while the tap is disabled): a switch-on that lands between the graph's look at the flag and
  // this one (the USB interrupt preempts the render) then skips one block instead of reading a null table.
  const bool on = enabled_.load(std::memory_order_relaxed) && sources != nullptr;
  if (!on) {
    was_enabled_ = false;
    return;
  }
  if (!was_enabled_) {  // a stream starts on a stem-block boundary: the reader flushes its side at the stream start (InStream::start)
    was_enabled_ = true;
    half_ = 0;
  }
  const std::uint32_t channels = channels_.load(std::memory_order_relaxed);
  for (std::uint32_t c = 0; c < channels && c < kChannels; ++c) {
    sum_(sources, mask_[c].load(std::memory_order_relaxed), hal::audio::kBlock, &pool_.pool[c][slot_][half_ * hal::audio::kBlock]);
  }
  if (++half_ < kHalves) {
    return;
  }
  half_ = 0;
  usb::StemBlock block;
  for (unsigned c = 0; c < kChannels; ++c) {
    block.ch[c] = pool_.pool[c][slot_];
  }
  pool_.ring.write(&block, 1);
  slot_ = slot_ + 1u < kPool ? slot_ + 1u : 0u;
  published_.store(published_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

// ---- RecTap -----------------------------------------------------------------------------------------------------------------

void RecTap::reset() noexcept {
  interleave_ = &usb::interleave;
  state_.store(static_cast<std::uint32_t>(State::Idle), std::memory_order_relaxed);
  stop_.store(false, std::memory_order_relaxed);
  frames_.store(0, std::memory_order_relaxed);
  overrun_.store(0, std::memory_order_relaxed);
  source_.store(0, std::memory_order_relaxed);
  channel_.store(0, std::memory_order_relaxed);
  dst_ = nullptr;
  capacity_ = 0;
  channels_ = 0;
  align_ = Align::Free;
  skip_ = 0;
  stop_align_.store(0, std::memory_order_relaxed);
  auto_steps_ = 0;
  steps_ = 0;
  end_ = kNoEnd;
  thresh_ = 0;
  ring_ = 0;
  wpos_ = 0;
  heard_ = 0;
  kept_ = 0;
  floor_ = kUnsettled;
}

bool RecTap::arm(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, Align align, std::uint32_t auto_steps) noexcept {
  if (align == Align::Level) {  // (a level take is armed by arm_level())
    return false;
  }
  return arm_as(dst, capacity_frames, channels, align, auto_steps, align == Align::Free ? State::Armed : State::Waiting, 0u, 0);
}

bool RecTap::arm_level(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, std::int32_t threshold, std::uint32_t preroll_frames, std::uint32_t auto_steps) noexcept {
  const std::uint32_t ring = ring_frames(preroll_frames);
  if (threshold < 1 || threshold > 32767 || capacity_frames <= ring) {
    return false;
  }
  return arm_as(dst, capacity_frames, channels, Align::Level, auto_steps, State::Listening, ring, threshold);
}

bool RecTap::arm_as(std::int16_t* dst, std::uint32_t capacity_frames, unsigned channels, Align align, std::uint32_t auto_steps, State first, std::uint32_t ring, std::int32_t threshold) noexcept {
  if (state() != State::Idle || dst == nullptr || capacity_frames == 0u || (channels != 1u && channels != 2u) || !kernel::detail::aligned4(dst)) {
    return false;
  }
  dst_ = dst;
  capacity_ = capacity_frames;
  channels_ = channels;
  align_ = align;
  skip_ = 0;
  auto_steps_ = auto_steps;
  steps_ = 0;
  end_ = kNoEnd;
  thresh_ = threshold;
  ring_ = ring;
  wpos_ = 0;
  heard_ = 0;
  kept_ = 0;
  floor_ = kUnsettled;
  stop_align_.store(0, std::memory_order_relaxed);
  frames_.store(0, std::memory_order_relaxed);
  overrun_.store(0, std::memory_order_relaxed);
  stop_.store(false, std::memory_order_relaxed);
  state_.store(static_cast<std::uint32_t>(first), std::memory_order_release);
  return true;
}

// OS-SREC-LEVEL. Render, state Listening, one call a block (flash). The block goes into the ring in any case (the pre-roll of a trigger in the next block is made of it), then it is scanned for the
// first frame at or above the threshold: |x| is compared as x >= t || x <= -t in int32 (no negation of an int16: -32768), at most kBlock frames, no float.
void RecTap::listen(const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  if (stop_.load(std::memory_order_relaxed)) {
    state_.store(static_cast<std::uint32_t>(State::Done), std::memory_order_release);  // cancelled while listening: nothing was recorded
    return;
  }
  if (n > hal::audio::kBlock) {
    n = hal::audio::kBlock;  // (the ring holds the pre-roll plus one render block)
  }
  const Channel c = channel();
  const auto put = [&](std::uint32_t at, const std::int16_t* pl, const std::int16_t* pr, std::uint32_t count) {
    if (channels_ == 1u) {
      kernel::mono_mix(dst_ + at, c == Channel::Right ? pr : pl, c == Channel::Left ? pl : pr, count);
    } else {
      const std::int16_t* planes[2] = {pl, pr};
      interleave_(planes, 2, 0, count, dst_ + 2u * at);
    }
  };
  const std::uint32_t first = ring_ - wpos_ < n ? ring_ - wpos_ : n;
  put(wpos_, l, r, first);
  if (first < n) {
    put(0, l + first, r + first, n - first);
  }
  const std::int16_t* a = c == Channel::Right ? r : l;
  const std::int16_t* b = c == Channel::Left ? l : r;
  const std::int32_t t = thresh_;
  std::uint32_t k = 0;
  for (; k < n; ++k) {
    const std::int32_t x = a[k];
    const std::int32_t y = b[k];
    if (x >= t || x <= -t || y >= t || y <= -t) {
      break;
    }
  }
  const std::uint32_t before = heard_;
  wpos_ += n;
  if (wpos_ >= ring_) {
    wpos_ -= ring_;
  }
  heard_ = before + n < ring_ ? before + n : ring_;
  if (k == n) {
    return;
  }
  const std::uint32_t pre = ring_ - hal::audio::kBlock;
  const std::uint32_t heard_before = before + k;  // frames of the signal that precede the trigger frame
  std::uint32_t kept = (heard_before < pre ? heard_before : pre) + (n - k);
  if (((ring_ - kept) & 1u) != 0u) {  // the take starts on an even frame of the region (the sample bank's alignment): one frame more of pre-roll if it was heard, else one less
    kept = heard_ > kept ? kept + 1u : (kept > 1u ? kept - 1u : kept);
  }
  kept_ = kept;
  frames_.store(ring_, std::memory_order_release);  // the take so far is the ring; later blocks write behind it
  state_.store(static_cast<std::uint32_t>(State::Armed), std::memory_order_release);
}

std::uint32_t RecTap::settle_ring() noexcept {
  if (floor_ != kUnsettled) {
    return floor_;
  }
  const State s = state();
  if (ring_ == 0u || (s != State::Armed && s != State::Done) || kept_ == 0u) {
    return 0;
  }
  const std::size_t ch = channels_;
  const std::size_t ring = ring_;
  const std::size_t kept = kept_;
  if (heard_ < ring_) {  // the ring never wrapped: [0, heard_) is in time order, the trigger block ended at heard_; the kept frames go to the end of the ring
    std::memmove(dst_ + (ring - kept) * ch, dst_ + (heard_ - kept) * ch, kept * ch * sizeof(std::int16_t));
  } else {  // the oldest frame is at wpos_ (0 when the ring has just filled: nothing to turn)
    std::rotate(dst_, dst_ + static_cast<std::size_t>(wpos_) * ch, dst_ + ring * ch);
  }
  std::memset(dst_, 0, (ring - kept) * ch * sizeof(std::int16_t));  // what lies before the kept frames is no audio of this take
  floor_ = ring_ - kept_;
  return floor_;
}

void RecTap::on_step(std::uint32_t step, std::uint32_t offset) noexcept {
  const std::uint32_t s = state_.load(std::memory_order_acquire);
  if (s == static_cast<std::uint32_t>(State::Armed)) {
    // OS-REC2: the take is running; a Step event may be where it ends (the first such event of the block, they come in order of their offsets).
    if (end_ == kNoEnd) {
      ++steps_;
      const Align a = static_cast<Align>(stop_align_.load(std::memory_order_relaxed));
      const bool boundary = a == Align::Step || (a == Align::Bar && (step & 15u) == 0u) || (a == Align::Pattern && step == 0u);
      if (boundary || (auto_steps_ != 0u && steps_ >= auto_steps_)) {
        end_ = offset;  // consumed by the push of this very block
      }
    }
    return;
  }
  if (s != static_cast<std::uint32_t>(State::Waiting)) {
    return;
  }
  if ((align_ == Align::Bar && (step & 15u) != 0u) || (align_ == Align::Pattern && step != 0u)) {
    return;
  }
  skip_ = offset;  // consumed by the push of this very block (Engine::play_block comes before Graph::render)
  steps_ = 0;      // the step the take begins at is not counted
  state_.store(static_cast<std::uint32_t>(State::Armed), std::memory_order_release);
}

void RecTap::on_stopped(std::uint32_t offset) noexcept {
  if (state_.load(std::memory_order_acquire) == static_cast<std::uint32_t>(State::Armed) && end_ == kNoEnd && stop_align_.load(std::memory_order_relaxed) != 0u) {
    end_ = offset;
  }
}

void RecTap::push(const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  const std::uint32_t s = state_.load(std::memory_order_acquire);
  if (s != static_cast<std::uint32_t>(State::Armed)) {
    if (s == static_cast<std::uint32_t>(State::Waiting) && stop_.load(std::memory_order_relaxed)) {
      state_.store(static_cast<std::uint32_t>(State::Done), std::memory_order_release);  // stopped before the step came: nothing was recorded
    } else if (s == static_cast<std::uint32_t>(State::Listening)) {
      listen(l, r, n);  // OS-SREC-LEVEL: flash, one call a block while the tap waits for a level
    }
    return;
  }
  const std::uint32_t done = frames_.load(std::memory_order_relaxed);
  if (stop_.load(std::memory_order_relaxed)) {
    state_.store(static_cast<std::uint32_t>(State::Done), std::memory_order_release);
    return;
  }
  std::uint32_t limit = end_ == kNoEnd || end_ > n ? n : end_;  // OS-REC2: the frames of this block that come before the take's end
  if (skip_ != 0u) {  // the first block of an aligned take: from the step's own sample on (skip_ < kBlock: an event's offset is inside its block)
    const std::uint32_t k = skip_ < n ? skip_ : n;
    skip_ = 0;
    l += k;
    r += k;
    n -= k;
    limit = limit > k ? limit - k : 0u;
  }
  const std::uint32_t room = capacity_ - done;
  const std::uint32_t take = limit < room ? limit : room;
  if (take != 0u) {
    if (channels_ == 1u) {
      const Channel c = channel();
      // One channel is the mix of it with itself: (x + x + 1) >> 1 == x, so the same kernel serves the three (and never a memcpy call out of SRAM_L).
      kernel::mono_mix(dst_ + done, c == Channel::Right ? r : l, c == Channel::Left ? l : r, take);
    } else {
      const std::int16_t* planes[2] = {l, r};
      interleave_(planes, 2, 0, take, dst_ + 2u * done);
    }
    frames_.store(done + take, std::memory_order_release);  // the frames are in memory before the count says so
  }
  if (take < limit) {
    overrun_.store(overrun_.load(std::memory_order_relaxed) + (limit - take), std::memory_order_relaxed);
  }
  // A take with a step count (a loop's, auto_steps_) that fills the memory exactly waits for its end step: it may fall on the very first sample of the next block (a loop of exactly the capacity),
  // and the frames that do not fit by then are counted as overrun by the next push (take < limit), which ends it.
  if (end_ != kNoEnd || (done + take >= capacity_ && (auto_steps_ == 0u || take < limit))) {
    state_.store(static_cast<std::uint32_t>(State::Done), std::memory_order_release);
  }
}

}  // namespace o3sdk::platform::mk66::audio
