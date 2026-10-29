// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/radio.h>

namespace o3sdk::app {

namespace hc = hal::capture;

void Radio::power(bool on) noexcept {
  if (hw_ != nullptr) {
    hw_->radio_power(on);
  }
}

void Radio::set_region(Region region) noexcept {
  if (static_cast<unsigned>(region) >= hc::kRegionCount) {
    return;
  }
  region_ = region;
  freq_ = static_cast<std::uint16_t>(hc::snap_freq(region_, freq_));
  if (hw_ != nullptr) {
    hw_->radio_region(region_);  // (the driver keeps the station across a band change: it re-tunes to the snapped frequency itself)
  }
}

void Radio::tune(unsigned freq) noexcept {
  freq_ = static_cast<std::uint16_t>(hc::snap_freq(region_, freq));
  if (hw_ != nullptr) {
    hw_->radio_tune(freq_);
  }
}

void Radio::step(int channels) noexcept { tune(hc::step_freq(region_, freq_, channels)); }

void Radio::seek(bool up) noexcept {
  if (hw_ != nullptr) {
    hw_->radio_seek(up);
  }
}

Radio::Status Radio::status() const noexcept {
  if (hw_ == nullptr) {
    Status absent;
    absent.state = Status::State::Absent;
    absent.region = region_;
    absent.freq = freq_;
    return absent;
  }
  return hw_->radio_status();
}

void Radio::poll() noexcept {
  if (hw_ == nullptr) {
    return;
  }
  const Status s = hw_->radio_status();
  if (s.generation == seen_generation_) {
    return;
  }
  seen_generation_ = s.generation;
  // A seek ends on a station of the chip's choosing: from then on the radio is where it says it is (and what the card keeps).
  if (s.state == Status::State::Ready && hc::snap_freq(region_, s.freq) == s.freq) {
    freq_ = s.freq;
  }
}

void Radio::restore(Region region, unsigned freq) noexcept {
  if (static_cast<unsigned>(region) >= hc::kRegionCount) {
    region = Region::Europe;
  }
  region_ = region;
  freq_ = static_cast<std::uint16_t>(hc::snap_freq(region_, freq));
  if (hw_ != nullptr) {
    hw_->radio_region(region_);
    hw_->radio_tune(freq_);
  }
}

}  // namespace o3sdk::app
