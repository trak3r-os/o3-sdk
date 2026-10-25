// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/usb_audio.h>

namespace o3sdk::platform::mk66::usb_audio {

void UsbAudioTap::push(const std::int16_t* lr, std::uint32_t frames) noexcept {
  const bool on = enabled_.load(std::memory_order_relaxed) && lr != nullptr;
  if (!on) {
    was_enabled_ = false;
    return;
  }
  if (!was_enabled_) {  // a stream starts on a stem-block boundary: the reader flushes its side at the stream start (InStream::start)
    was_enabled_ = true;
    filled_ = 0;
  }
  if (frames > o3sdk::usb::kStemBlockFrames) {  // (a render block is 64 frames: this bounds the loop below to two passes)
    frames = o3sdk::usb::kStemBlockFrames;
  }
  while (frames != 0u) {
    const std::uint32_t room = o3sdk::usb::kStemBlockFrames - filled_;
    const std::uint32_t n = frames < room ? frames : room;
    std::int16_t* const left = &pool_[0][slot_][filled_];
    std::int16_t* const right = &pool_[1][slot_][filled_];
    for (std::uint32_t i = 0; i < n; ++i) {
      left[i] = lr[2u * i];
      right[i] = lr[2u * i + 1u];
    }
    lr += 2u * n;
    frames -= n;
    filled_ += n;
    if (filled_ < o3sdk::usb::kStemBlockFrames) {
      continue;
    }
    // The slot is full: publish it. The master pair is the last two channels of the stream, every other channel the block of zeros.
    const std::uint32_t channels = this->channels();
    const std::uint32_t first = channels >= 2u ? channels - 2u : 0u;
    o3sdk::usb::StemBlock block;
    for (std::uint32_t c = 0; c < o3sdk::usb::kMaxChannels; ++c) {
      block.ch[c] = kSilence;
    }
    block.ch[first] = pool_[0][slot_];
    block.ch[first + 1u] = pool_[1][slot_];
    ring_.write(&block, 1);
    filled_ = 0;
    slot_ = slot_ + 1u < kPool ? slot_ + 1u : 0u;
    published_.store(published_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
}

void UsbAudioTap::drain_return() noexcept {
  if (!return_on_.load(std::memory_order_relaxed)) {
    return;
  }
  o3sdk::usb::ReturnReader* const reader = reader_.load(std::memory_order_acquire);
  if (reader == nullptr) {
    return;
  }
  (void)reader->pull(drop_[0], drop_[1], hal::audio::kBlock);  // silence for what the ring could not supply (counted by the reader); dropped either way
  drained_.store(drained_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

void UsbAudioTap::reset() noexcept {
  enabled_.store(false, std::memory_order_relaxed);
  return_on_.store(false, std::memory_order_relaxed);
  channels_xor_.store(0, std::memory_order_relaxed);
  reader_.store(nullptr, std::memory_order_relaxed);
  ring_.reset();
  for (unsigned p = 0; p < kPlanes; ++p) {
    for (unsigned b = 0; b < kPool; ++b) {
      for (unsigned i = 0; i < o3sdk::usb::kStemBlockFrames; ++i) {
        pool_[p][b][i] = 0;
      }
    }
    for (unsigned i = 0; i < hal::audio::kBlock; ++i) {
      drop_[p][i] = 0;
    }
  }
  was_enabled_ = false;
  filled_ = 0;
  slot_ = 0;
  published_.store(0, std::memory_order_relaxed);
  drained_.store(0, std::memory_order_relaxed);
}

namespace {

UsbAudioTap g_tap;  // .bss of SRAM_U (about 3 KiB); the object the USB interrupt reads through its ring and the render writes

struct Wrapped {
  hal::audio::BlockCallback inner;
  UsbAudioTap* tap;
};
Wrapped g_wrapped = {{nullptr, nullptr}, &g_tap};

void wrapped_block(void* user, const hal::audio::BlockIo& io) noexcept {
  const Wrapped& w = *static_cast<const Wrapped*>(user);
  if (w.inner.fn != nullptr) {
    w.inner.fn(w.inner.user, io);
  }
  w.tap->push(io.out, hal::audio::kBlock);  // the block the main output plays, as the app made it (io.out is untouched)
  w.tap->drain_return();
}

}  // namespace

UsbAudioTap& tap() noexcept { return g_tap; }

hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner) noexcept {
  g_wrapped.inner = inner;
  return hal::audio::BlockCallback{&wrapped_block, &g_wrapped};
}

}  // namespace o3sdk::platform::mk66::usb_audio
