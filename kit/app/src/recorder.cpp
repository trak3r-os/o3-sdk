// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/recorder.h>

#include <o3sdk/hal/audio.h>

namespace o3sdk::app {

bool Recorder::arm(const Take& take) noexcept {
  if (hw_ == nullptr || !hw_->arm(take)) {
    return false;
  }
  buf_ = take.buf;
  channels_ = static_cast<std::uint8_t>(hal::record::channel_count(take.channels));
  return true;
}

void Recorder::stop_at(Align boundary) noexcept {
  if (hw_ != nullptr) {
    hw_->stop_at(boundary);
  }
}

void Recorder::disarm() noexcept {
  if (hw_ != nullptr) {
    hw_->disarm();
  }
}

Recorder::State Recorder::state() const noexcept { return hw_ != nullptr ? hw_->state() : State::Idle; }
std::uint32_t Recorder::frames() const noexcept { return hw_ != nullptr ? hw_->frames() : 0u; }
std::uint32_t Recorder::overrun() const noexcept { return hw_ != nullptr ? hw_->overrun() : 0u; }
std::uint32_t Recorder::settle() noexcept { return hw_ != nullptr ? hw_->settle() : 0u; }

void Recorder::release() noexcept {
  if (hw_ != nullptr) {
    hw_->release();
  }
}

void Recorder::set_tap(Tap tap) noexcept {
  if (hw_ != nullptr) {
    hw_->set_tap(tap);
  }
}

Recorder::Tap Recorder::tap() const noexcept { return hw_ != nullptr ? hw_->tap() : Tap::Input; }

void Recorder::set_pattern_steps(std::uint32_t steps) noexcept {
  if (hw_ != nullptr) {
    hw_->set_pattern_steps(steps);
  }
}

Recorder::Clip Recorder::clip() noexcept {
  if (hw_ == nullptr || buf_ == nullptr || hw_->state() != State::Done) {
    return Clip{};
  }
  const std::uint32_t lead = hw_->settle();
  const std::uint32_t total = hw_->frames();
  if (total <= lead) {
    return Clip{};
  }
  return Clip{buf_ + static_cast<std::size_t>(lead) * channels_, total - lead, channels_};
}

bool Recorder::save(Files& files, const char* folder, const char* stem) noexcept {
  const Clip c = clip();
  if (c.frames == 0u) {
    return false;
  }
  // The engine's rate (44,100 Hz; 44,118 with the 4080-cycle clock): the WAV plays at the speed it was heard.
  const std::uint32_t rate = (hal::audio::sample_rate_mhz() + 500u) / 1000u;
  return files.save_wav(folder, c.pcm, c.frames, c.channels, rate, stem);
}

}  // namespace o3sdk::app
