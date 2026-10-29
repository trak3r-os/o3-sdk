// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/usb_audio_service.h>

#include <o3sdk/dsp/engine.h>

namespace o3sdk::platform::mk66::audio {

// The engine's stem sources are the tap's (docs/M2C-PLAN.md D12): the bit order of Engine::set_stem_slots() and the index of stem_block() is stem_src of taps.h.
static_assert(dsp::kStemSources == usb::kSlotSources, "the engine's stem table is the USB slot sources");
static_assert(dsp::kStemMasterL == stem_src::kMasterL && dsp::kStemMasterR == stem_src::kMasterR && dsp::kStemSendA == stem_src::kSendA && dsp::kStemSendB == stem_src::kSendB,
              "the engine's master and send sources are the tap's");
static_assert(dsp::kMaxSlots <= stem_src::kTracks, "the engine's slots are the tap's T1..T8 (T9..T12 are never staged)");
static_assert(dsp::kMaxBlockFrames == hal::audio::kBlock, "one engine block is one render block");
static_assert(hal::usb_audio::kStemSources == usb::kSlotSources, "the HAL's sources are the USB slot sources");
static_assert(hal::usb_audio::kSourceMasterL == (1u << stem_src::kMasterL) && hal::usb_audio::kSourceMasterR == (1u << stem_src::kMasterR) &&
                  hal::usb_audio::kSourceSendA == (1u << stem_src::kSendA) && hal::usb_audio::kSourceSendB == (1u << stem_src::kSendB),
              "the HAL's master and send bits are the tap's");

namespace {

// A table of no sources: a stem block published while the engine staged nothing is silence, so the stream's ring keeps its pace.
const std::int16_t* const kNoSources[usb::kSlotSources] = {};

std::int16_t saturate(std::int32_t v) noexcept { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

}  // namespace

bool factory_map(StemMap& map, usb::Profile p) noexcept {
  if (!map.reset(p)) {
    return false;
  }
  if (p == usb::Profile::P8) {
    for (unsigned c = 0; c + 2u < usb::profile_channels(p); ++c) {
      (void)map.set_slot(c, 0u);  // o3-sdk's boot profile streams the master and silence (header)
    }
  }
  return true;
}

// ---- Main loop --------------------------------------------------------------------------------------------------------------------------

bool UsbAudioService::set_stem(unsigned channel, std::uint32_t sources) noexcept {
  if (!map_.set_slot(channel, sources & 0xFFFFu)) {
    return false;
  }
  map_.apply(tap_);
  refresh_stems();
  return true;
}

void UsbAudioService::reset_stems() noexcept {
  if (factory_map(map_, map_.profile())) {
    map_.apply(tap_);
    refresh_stems();
  }
}

void UsbAudioService::set_return_level(float level) noexcept {
  const float v = level != level ? 0.0f : (level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level));
  gain_q16_.store(static_cast<std::int32_t>(v * 65536.0f + 0.5f), std::memory_order_relaxed);
}

// ---- The driver's side ------------------------------------------------------------------------------------------------------------------

void UsbAudioService::refresh_stems() noexcept {
  dsp::Engine* const engine = engine_.load(std::memory_order_acquire);
  if (engine == nullptr) {
    return;
  }
  std::uint32_t mask = 0;
  if (tap_.enabled()) {
    const unsigned channels = static_cast<unsigned>(tap_.channels());
    for (unsigned c = 0; c < channels; ++c) {
      mask |= tap_.slot(c);
    }
  }
  engine->set_stem_slots(mask);  // (bits the engine never stages are dropped there)
}

void UsbAudioService::streams_changed(bool in_active, bool out_active) noexcept {
  tap_.set_enabled(in_active);
  refresh_stems();
  return_on_.store(out_active, std::memory_order_relaxed);
}

// ---- Render -----------------------------------------------------------------------------------------------------------------------------

hal::audio::BlockCallback UsbAudioService::wrap(hal::audio::BlockCallback inner, dsp::Engine& engine) noexcept {
  inner_ = inner;
  engine_.store(&engine, std::memory_order_release);
  return hal::audio::BlockCallback{&UsbAudioService::block, this};
}

void UsbAudioService::block(void* user, const hal::audio::BlockIo& io) noexcept {
  UsbAudioService& self = *static_cast<UsbAudioService*>(user);
  const bool stems = self.tap_.enabled();
  const bool ret = self.return_on_.load(std::memory_order_relaxed) && self.reader_.load(std::memory_order_acquire) != nullptr;
  const bool usb_in = self.input_usb_.load(std::memory_order_relaxed);
  if (!stems && !ret && !usb_in) {
    if (self.inner_.fn != nullptr) {
      self.inner_.fn(self.inner_.user, io);
    }
    // The stream closed between the interrupt's two stores (or an edit raced it): the engine's mask is put back so that nothing stages for a host that is not there.
    dsp::Engine* const engine = self.engine_.load(std::memory_order_relaxed);
    if (engine != nullptr && engine->stem_slots() != 0u) {
      engine->set_stem_slots(0u);
    }
    return;
  }
  self.process(io);
}

void UsbAudioService::process(const hal::audio::BlockIo& io) noexcept {
  constexpr std::uint32_t n = hal::audio::kBlock;
  active_.store(active_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  usb::ReturnReader* const reader = reader_.load(std::memory_order_acquire);
  const bool ret = return_on_.load(std::memory_order_relaxed) && reader != nullptr;
  if (ret) {
    (void)reader->pull(ret_[0], ret_[1], n);  // silence for what the ring could not supply (counted by the reader); pulled whatever the source and the level, so the ring never overruns
    pulled_.store(pulled_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
  hal::audio::BlockIo inner_io = io;
  if (input_usb_.load(std::memory_order_relaxed)) {  // the engine hears the return, not the codec (silence while the computer plays nothing)
    for (std::uint32_t i = 0; i < n; ++i) {
      in_[2u * i] = ret ? ret_[0][i] : std::int16_t{0};
      in_[2u * i + 1u] = ret ? ret_[1][i] : std::int16_t{0};
    }
    inner_io.in = in_;
  }
  if (inner_.fn != nullptr) {
    inner_.fn(inner_.user, inner_io);
  }
  dsp::Engine* const engine = engine_.load(std::memory_order_relaxed);
  if (tap_.enabled() && engine != nullptr) {
    if (engine->stem_slots() == 0u) {
      refresh_stems();  // the stream opened before the service had the engine (or the mask was put back by a race): the sources are selected now
    }
    const std::int16_t* const* table = engine->stem_block();  // after the render, before the return is added: no stem hears it
    tap_.push(table != nullptr ? table : kNoSources);
  }
  if (ret && (io.flags & hal::audio::kFlagSilence) == 0u) {
    mix_return(io.out);
  } else {
    gain_now_q16_ = gain_q16_.load(std::memory_order_relaxed);  // nothing mixed: the next mix starts at the level, not at an old ramp
  }
}

// out += ret * gain, saturating, the gain ramped linearly from where the last block ended to the target across the block.
void UsbAudioService::mix_return(std::int16_t* out) noexcept {
  constexpr std::int32_t n = static_cast<std::int32_t>(hal::audio::kBlock);
  const std::int32_t target = gain_q16_.load(std::memory_order_relaxed);
  const std::int32_t step = (target - gain_now_q16_) / n;
  std::int32_t g = gain_now_q16_;
  for (std::int32_t i = 0; i < n; ++i) {
    g += step;
    const std::int32_t l = (static_cast<std::int32_t>(ret_[0][i]) * g) >> 16;
    const std::int32_t r = (static_cast<std::int32_t>(ret_[1][i]) * g) >> 16;
    out[2 * i] = saturate(static_cast<std::int32_t>(out[2 * i]) + l);
    out[2 * i + 1] = saturate(static_cast<std::int32_t>(out[2 * i + 1]) + r);
  }
  gain_now_q16_ = target;
}

}  // namespace o3sdk::platform::mk66::audio
