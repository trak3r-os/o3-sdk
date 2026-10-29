// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/audio_in.h>

namespace o3sdk::app {

namespace {

constexpr unsigned kMaxPercent = 100;
constexpr float kMeterFallMs = 300.0f;  // the AUDIO section's meter falls to nothing in about this long

unsigned clamped(unsigned percent) noexcept { return percent > kMaxPercent ? kMaxPercent : percent; }

}  // namespace

std::uint8_t AudioIn::map_for(Source s) noexcept {
  switch (s) {
    case Source::LineMix: return kMapMix;
    case Source::LineLeft: return kMapLeft;
    case Source::LineRight: return kMapRight;
    case Source::MicLow:
    case Source::MicHigh:
    case Source::Radio:
    case Source::Usb:
    case Source::Count: break;
  }
  return kMapAsIs;
}

// One write to the device for what is pending (the choice as it stands now); true: acknowledged.
bool AudioIn::send(Pending what) noexcept {
  return what == Pending::Select ? hw_->select_source(source_, gain_) : hw_->set_gain(source_, gain_);
}

bool AudioIn::radio_is_dead() const noexcept {
  const hal::capture::RadioStatus::State s = hw_->radio_status().state;
  return s == hal::capture::RadioStatus::State::Absent || s == hal::capture::RadioStatus::State::Failed;
}

bool AudioIn::select(Source source, unsigned gain_percent) noexcept {
  if (hw_ == nullptr || static_cast<unsigned>(source) >= hal::capture::kSourceCount) {
    return false;
  }
  source_ = source;
  gain_ = static_cast<std::uint8_t>(clamped(gain_percent));
  selected_ = true;
  // The planes go silent first (a one-block fade), so that nothing of the switch reaches the units or the monitor; the new channel map joins them only once the fade is done
  // (poll(), kMapDelayMs on), or the first block of the fade would carry it at full level.
  engine_->set_input_gate(false);
  map_next_ = map_for(source);
  map_pending_ = true;
  map_at_ = now_ms_ + kMapDelayMs;
  settling_ = true;
  settle_until_ = now_ms_ + kSettleMs;
  if (send(Pending::Select)) {
    pending_ = Pending::None;
    return true;
  }
  pending_ = Pending::Select;
  retry_at_ = now_ms_ + kRetryMs;
  return false;
}

bool AudioIn::set_gain(unsigned gain_percent) noexcept {
  if (hw_ == nullptr) {
    return false;
  }
  gain_ = static_cast<std::uint8_t>(clamped(gain_percent));
  if (pending_ == Pending::Select) {
    return false;  // the switch itself has not gone through yet: the retry carries this gain
  }
  if (send(Pending::Gain)) {
    pending_ = Pending::None;
    return true;
  }
  pending_ = Pending::Gain;
  retry_at_ = now_ms_ + kRetryMs;
  return false;
}

void AudioIn::monitor(unsigned level_percent) noexcept {
  monitor_ = static_cast<std::uint8_t>(clamped(level_percent));
  if (engine_ != nullptr) {
    engine_->set_input_monitor(static_cast<float>(monitor_) * 0.01f);
  }
}

dsp::InputPeaks AudioIn::peaks() noexcept {
  if (hw_ == nullptr) {
    return dsp::InputPeaks{};
  }
  peaks_until_ = now_ms_ + kPeaksHoldMs;
  const dsp::InputPeaks p{held_l_, held_r_};
  held_l_ = 0.0f;
  held_r_ = 0.0f;
  return p;
}

void AudioIn::restore(Source source, unsigned gain_percent, unsigned monitor_percent) noexcept {
  if (static_cast<unsigned>(source) >= hal::capture::kSourceCount) {
    source = Source::LineMix;
  }
  if (hw_ == nullptr) {
    source_ = source;
    gain_ = static_cast<std::uint8_t>(clamped(gain_percent));
    monitor_ = static_cast<std::uint8_t>(clamped(monitor_percent));
    return;
  }
  if (source != source_ || clamped(gain_percent) != gain_) {  // (a monitor-only change costs the codec nothing and the gate nothing)
    (void)select(source, gain_percent);
  }
  monitor(monitor_percent);
}

void AudioIn::poll(std::uint64_t now_ms) noexcept {
  now_ms_ = now_ms;
  if (hw_ == nullptr) {
    return;
  }
  if (map_pending_ && now_ms >= map_at_) {
    map_.store(map_next_, std::memory_order_relaxed);
    map_pending_ = false;
  }
  // A refused write: again every kRetryMs, one write a pass. The gate stays shut until it went through, then a new settling time starts. The platform's select of Radio powers the
  // receiver (from Absent or Failed that is a full probe: a reset and three bus reads), so a retry of it waits while the radio is dead; the first call and a person's
  // radio().power(true) are what probe it, and the retry goes on once that has started it again.
  const bool radio_dead = source_ == Source::Radio && pending_ == Pending::Select && radio_is_dead();
  if (pending_ != Pending::None && now_ms >= retry_at_ && !radio_dead) {
    if (send(pending_)) {
      if (pending_ == Pending::Select) {
        settling_ = true;
        settle_until_ = now_ms + kSettleMs;
      }
      pending_ = Pending::None;
    } else {
      retry_at_ = now_ms + kRetryMs;
    }
  }
  if (settling_ && pending_ == Pending::None && now_ms >= settle_until_) {
    settling_ = false;
    engine_->set_input_gate(true);
  }

  // The peaks, only while somebody wants them.
  const bool want = watching_ || (peaks_until_ != 0u && now_ms < peaks_until_);
  if (want != measuring_) {
    measuring_ = want;
    engine_->set_input_peaks(want);
    if (!want) {
      meter_l_ = 0.0f;
      meter_r_ = 0.0f;
      held_l_ = 0.0f;
      held_r_ = 0.0f;
      peaks_until_ = 0;
    }
  }
  if (measuring_) {
    const dsp::InputPeaks p = engine_->input_peaks();
    held_l_ = p.l > held_l_ ? p.l : held_l_;
    held_r_ = p.r > held_r_ ? p.r : held_r_;
    float dt = static_cast<float>(now_ms - last_poll_ms_);
    dt = dt > kMeterFallMs ? kMeterFallMs : dt;
    const float keep = 1.0f - dt / kMeterFallMs;
    meter_l_ = p.l > meter_l_ * keep ? p.l : meter_l_ * keep;
    meter_r_ = p.r > meter_r_ * keep ? p.r : meter_r_ * keep;
  }
  last_poll_ms_ = now_ms;
}

}  // namespace o3sdk::app
