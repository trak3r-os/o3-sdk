// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/usb_audio.h>

#include <o3sdk/app/settings.h>

namespace o3sdk::app {

namespace hu = hal::usb_audio;

bool UsbAudio::request(bool audio, Profile profile, bool duplex) noexcept {
  if (dev_ == nullptr || !dev_->request(audio, profile, duplex)) {
    return false;
  }
  if (profile != profile_) {
    map_set_ = false;  // the slot masks mean something else on another profile: the device gave the factory slots
  }
  audio_ = audio;
  profile_ = profile;
  duplex_ = duplex;
  return true;
}

void UsbAudio::snapshot_stems() noexcept {
  for (unsigned c = 0; c < kMaxChannels; ++c) {
    stems_[c] = static_cast<std::uint16_t>(c < hu::profile_channels(profile_) ? dev_->stem(c) : 0u);
  }
}

bool UsbAudio::set_stem(unsigned channel, std::uint32_t sources) noexcept {
  if (dev_ == nullptr || !dev_->set_stem(channel, sources)) {
    return false;
  }
  map_set_ = true;
  snapshot_stems();
  return true;
}

void UsbAudio::reset_stems() noexcept {
  if (dev_ != nullptr) {
    dev_->reset_stems();
  }
  map_set_ = false;
}

void UsbAudio::set_return_level(unsigned percent) noexcept {
  level_ = static_cast<std::uint8_t>(percent > 100u ? 100u : percent);
  if (dev_ != nullptr) {
    dev_->set_return_level(static_cast<float>(level_) * 0.01f);
  }
}

bool UsbAudio::set_read_only(bool on) noexcept {
  if (dev_ != nullptr && dev_->disk().phase != hu::DiskPhase::Idle) {
    return false;  // the policy is fixed when the card is handed over
  }
  read_only_ = on;
  return true;
}

hu::DiskRefusal UsbAudio::share_disk() noexcept {
  if (dev_ == nullptr) {
    return hu::DiskRefusal::Unavailable;
  }
  if (files_ != nullptr && files_->busy()) {
    return hu::DiskRefusal::Busy;  // a load, a save or a listing runs: the card is in use
  }
  return dev_->disk_share(read_only_);
}

void UsbAudio::poll() noexcept {
  if (dev_ == nullptr || !dev_->ready()) {
    return;
  }
  if (!applied_) {
    // The driver runs: what the settings kept goes to it. The layout is the one the port started with (main took it from boot_layout()), so only the level and the edited slots are left.
    dev_->set_return_level(static_cast<float>(level_) * 0.01f);
    if (map_set_) {
      const unsigned channels = hu::profile_channels(dev_->profile());
      for (unsigned c = 0; c < channels && c < kMaxChannels; ++c) {
        (void)dev_->set_stem(c, stems_[c]);
      }
    }
    applied_ = true;
  }
  if (!dev_->changing()) {  // the kit's copy follows the port: a change the console made is kept too
    const Profile now = dev_->profile();
    if (now != profile_) {
      map_set_ = false;
    }
    audio_ = dev_->audio();
    profile_ = now;
    duplex_ = dev_->duplex();
  }
}

void UsbAudio::restore(const KitValues& v) noexcept {
  audio_ = v.usb_audio;
  profile_ = static_cast<Profile>(v.usb_profile < hu::kProfileCount ? v.usb_profile : static_cast<std::uint8_t>(Profile::P8));
  duplex_ = v.usb_duplex;
  level_ = v.usb_level > 100u ? std::uint8_t{100} : v.usb_level;
  map_set_ = v.usb_map_set;
  for (unsigned c = 0; c < kMaxChannels; ++c) {
    stems_[c] = v.usb_stem[c];
  }
  if (dev_ != nullptr && applied_) {  // a card put in after the boot: the level and the slots follow it (the layout does not: the port is not re-enumerated behind the user's back)
    dev_->set_return_level(static_cast<float>(level_) * 0.01f);
    if (map_set_) {
      const unsigned channels = hu::profile_channels(profile_);
      for (unsigned c = 0; c < channels && c < kMaxChannels; ++c) {
        (void)dev_->set_stem(c, stems_[c]);
      }
    }
  }
}

void UsbAudio::fill(KitValues& v) const noexcept {
  v.usb_audio = audio_;
  v.usb_profile = static_cast<std::uint8_t>(profile_);
  v.usb_duplex = duplex_;
  v.usb_level = level_;
  v.usb_map_set = map_set_;
  for (unsigned c = 0; c < kMaxChannels; ++c) {
    v.usb_stem[c] = map_set_ ? stems_[c] : std::uint16_t{0};
  }
}

}  // namespace o3sdk::app
