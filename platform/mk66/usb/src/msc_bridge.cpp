// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/msc_bridge.h>

namespace o3sdk::platform::mk66::usb {
namespace {

using o3sdk::usb::msc::IoResult;

IoResult map(storage::BlockResult r) noexcept {
  switch (r) {
    case storage::BlockResult::Ok:
      return IoResult::Ok;
    case storage::BlockResult::NoMedium:
    case storage::BlockResult::NotReady:
      return IoResult::NoMedium;
    case storage::BlockResult::WriteProtected:
      return IoResult::WriteProtected;
    case storage::BlockResult::Unsupported:
    case storage::BlockResult::OutOfRange:
    case storage::BlockResult::Failed:
      return IoResult::Failed;
  }
  return IoResult::Failed;
}

}  // namespace

bool MscBridge::open() noexcept {
  if (swap_.owner() != storage::CardOwner::Host) {
    return false;
  }
  slot_.reset(device_.present());
  following_ = true;
  opened_ = device_.start() == storage::BlockResult::Ok;
  return opened_;
}

bool MscBridge::follow(std::uint32_t now_ms) noexcept {
  if (!following_ || swap_.owner() != storage::CardOwner::Host) {
    return false;
  }
  const storage::PinDebounce::Edge edge = slot_.update(device_.present(), now_ms);
  if (edge == storage::PinDebounce::Edge::None) {
    return false;
  }
  close();
  if (edge == storage::PinDebounce::Edge::Inserted) {
    if (blocking_ != nullptr) {
      blocking_(true);
    }
    opened_ = device_.start() == storage::BlockResult::Ok;
    if (blocking_ != nullptr) {
      blocking_(false);
    }
  }
  return true;
}

void MscBridge::close() noexcept {
  opened_ = false;
  // Only while the card is the computer's: after the eject path's take_back() the volume is FatFs's again and a release() here would drop the device under its mount.
  if (swap_.owner() == storage::CardOwner::Host) {
    device_.release();
  }
}

bool MscBridge::present() const noexcept {
  return opened_ && swap_.owner() == storage::CardOwner::Host && device_.present() && device_.ready();
}

std::uint32_t MscBridge::block_count() const noexcept { return present() ? device_.block_count() : 0u; }

bool MscBridge::write_protected() const noexcept { return !writable_ || !present() || device_.write_protected(); }

IoResult MscBridge::read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept {
  if (!present()) {
    return IoResult::NoMedium;
  }
  return map(device_.read(lba, count, dst));
}

IoResult MscBridge::write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept {
  if (!present()) {
    return IoResult::NoMedium;
  }
  if (write_protected()) {
    return IoResult::WriteProtected;
  }
  return map(device_.write(lba, count, src));
}

}  // namespace o3sdk::platform::mk66::usb
