// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/card_swap.h>

namespace o3sdk::platform::mk66::storage {

void CardSwap::begin() noexcept {
  const bool present = device_.present();
  pin_.reset(present);
  if (!present) {
    fat_.release();  // gone between the mount and now: no stale volume is left behind
  }
  owner_ = fat_.mounted() ? CardOwner::Tracker : CardOwner::NoCard;
}

CardSwap::Event CardSwap::poll(std::uint32_t now_ms) noexcept {
  const PinDebounce::Edge edge = pin_.update(device_.present(), now_ms);
  if (owner_ == CardOwner::Host || edge == PinDebounce::Edge::None) {
    return Event::None;
  }
  if (edge == PinDebounce::Edge::Removed) {
    ++removals_;
    release();
    return Event::Removed;
  }
  if (owner_ != CardOwner::NoCard) {
    return Event::None;  // (a card the OS already has: the level came back without a removal being seen, so there is nothing to mount)
  }
  ++insertions_;
  return Event::Inserted;
}

hal::storage::Status CardSwap::mount_volume() noexcept {
  const hal::storage::Status s = fat_.mount();
  owner_ = s == hal::storage::Status::Ok ? CardOwner::Tracker : CardOwner::NoCard;
  return s;
}

hal::storage::Status CardSwap::reacquire() noexcept {
  if (owner_ == CardOwner::Host) {
    return hal::storage::Status::NotMounted;  // the computer has the card: no mount, no card traffic (take_back() is the way back)
  }
  return mount_volume();
}

hal::storage::Status CardSwap::take_back() noexcept {
  if (owner_ != CardOwner::Host) {
    return hal::storage::Status::NotMounted;
  }
  const hal::storage::Status s = mount_volume();
  remount_flag_ = s == hal::storage::Status::Ok ? volume_flag(device_) : VolumeFlag::Unknown;  // (the card is started and mounted: three reads, nothing written)
  return s;
}

void CardSwap::release() noexcept {
  if (owner_ == CardOwner::Host) {
    return;  // the volume is already let go and the card is the computer's: only take_back() ends that
  }
  fat_.release();
  owner_ = CardOwner::NoCard;
}

CardSwap::HandOver CardSwap::hand_to_host() noexcept {
  if (owner_ == CardOwner::Host) {
    return HandOver::AlreadyHost;
  }
  if (owner_ != CardOwner::Tracker) {
    return HandOver::NoCard;
  }
  if (claims_.busy()) {
    return HandOver::Busy;
  }
  handover_flag_ = volume_flag(device_);  // (the card is started and mounted, no claim holds it: three reads, nothing written; the volume is released right after)
  fat_.release();
  owner_ = CardOwner::Host;
  ++host_sessions_;
  return HandOver::Ok;
}

bool CardSwap::boot_to_host() noexcept {
  if (owner_ != CardOwner::NoCard) {
    return false;
  }
  handover_flag_ = VolumeFlag::Unknown;
  owner_ = CardOwner::Host;
  ++host_sessions_;
  return true;
}

}  // namespace o3sdk::platform::mk66::storage
