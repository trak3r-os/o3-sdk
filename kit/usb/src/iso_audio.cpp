// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/iso_audio.h>

namespace o3sdk::usb {

void IsoAudio::reset_session(Speed speed) noexcept {
  speed_ = speed;
  rate_.reset(speed);
  fb_rate_q16_ = nominal_frames_q16(speed);
  const bool was_active = in_.active() || out_active_;
  in_.stop();
  in_channels_ = 0;
  out_active_ = false;
  if (was_active) {
    notify();
  }
}

void IsoAudio::start_in(std::uint32_t channels) noexcept {
  in_channels_ = channels < kMaxChannels ? channels : kMaxChannels;
  in_.start(speed_, in_channels_);
  notify();
}

void IsoAudio::stop_in() noexcept {
  const bool was = in_.active();
  in_.stop();
  if (was) {
    notify();
  }
}

void IsoAudio::start_out() noexcept {
  fb_.reset();
  fb_rate_q16_ = rate_.frames_per_sof_q16();
  reader_.request_restart();  // the engine's pull discards stale frames and waits for the prime fill again
  out_active_ = true;
  notify();
}

void IsoAudio::stop_out() noexcept {
  const bool was = out_active_;
  out_active_ = false;
  if (was) {
    notify();
  }
}

void IsoAudio::on_sof(std::uint32_t cyccnt, std::uint32_t frame) noexcept {
  rate_.on_sof(cyccnt, frame);
  if (out_active_) {
    fb_rate_q16_ = fb_.update(rate_.frames_per_sof_q16(), out_.fill());
  }
}

std::uint32_t IsoAudio::build_in(std::int16_t* out) noexcept {
  const std::uint32_t frames = in_.build_packet(rate_.frames_per_sof_q16(), out);
  return frames * in_channels_ * kBytesPerSample;
}

std::uint32_t IsoAudio::feedback_packet(std::uint8_t* out) noexcept {
  const std::uint32_t value = feedback_value(speed_, fb_rate_q16_);
  counters_.note_feedback(value);
  return pack_feedback(speed_, value, out);
}

}  // namespace o3sdk::usb
