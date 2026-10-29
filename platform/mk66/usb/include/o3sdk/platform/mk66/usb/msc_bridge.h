// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U1c: the adapter between the MSC engine's BlockStore (usb/msc.h) and the card's BlockDevice (storage/block_device.h). It is the ONLY place outside the FAT glue that calls
// BlockDevice::write (check_layers rule 9 names this file). The card is the bridge's only while the owner is Host (storage/card_swap.h): whatever the engine asks while the owner is
// Tracker or NoCard, or before open(), answers NoMedium (NOT READY, MEDIUM NOT PRESENT) and touches neither the device nor the bus. Read-only by default (maintainer decision
// 2026-10-16: the disk is exported write-protected until the writable step); set_writable(true) is that step. Main loop only; read() and write() block for the card (the engine's rule).

#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/usb/msc.h>

#include <cstdint>

namespace o3sdk::platform::mk66::usb {

class MscBridge final : public o3sdk::usb::msc::BlockStore {
 public:
  MscBridge(storage::BlockDevice& device, const storage::CardSwap& swap) noexcept : device_(device), swap_(swap) {}

  // Brings the card up for the computer (BlockDevice::start(): blocks, bounded). Only while the owner is Host; false (nothing touched) otherwise or when the card does not start.
  bool open() noexcept;
  // The computer lets go (the eject): the device forgets the medium (BlockDevice::release(), no bus traffic) only while the owner is still Host; after take_back() the card is
  // FatFs's again and close() touches nothing. The owner changes elsewhere (take_back()).
  void close() noexcept;
  bool opened() const noexcept { return opened_; }

  // The slot under the computer (spec 3.4, amendment 4). Armed by open() (a call that found no card or a card that did not start included: the slot's level is the starting one, so a card put in
  // later is noticed). Every pass of the main loop while the owner is Host: the card-detect level is debounced (PinDebounce, 250 ms); on a settled change the card is let go (close()), a card
  // that is in is started (open()), and true is returned so that the caller tells the engine (Engine::medium_changed(): UNIT ATTENTION, capacity read again). A card that is in but did not
  // start is not tried again until it has been taken out and put back (as the OS's own mount). False, nothing touched, before open() and whenever the owner is not Host.
  bool follow(std::uint32_t now_ms) noexcept;
  // U1d (review fold of U1c part 2b, minor 1): a card that follow() starts blocks the main loop for its initialisation (tens of ms; 3 s for a card that does not answer). The hook is called with true just
  // before that start and with false right after it, so that the image can keep the display serviced meanwhile (appimg::service_while_blocked). Not called when nothing is started. Null: none.
  using BlockingHook = void (*)(bool on) noexcept;
  void set_blocking_hook(BlockingHook hook) noexcept { blocking_ = hook; }

  void set_writable(bool on) noexcept { writable_ = on; }
  bool writable() const noexcept { return writable_; }

  // ---- BlockStore ----
  bool present() const noexcept override;
  std::uint32_t block_count() const noexcept override;
  bool write_protected() const noexcept override;
  o3sdk::usb::msc::IoResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override;
  o3sdk::usb::msc::IoResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override;

 private:
  storage::BlockDevice& device_;
  const storage::CardSwap& swap_;
  bool opened_ = false;
  bool writable_ = false;
  bool following_ = false;
  BlockingHook blocking_ = nullptr;
  storage::PinDebounce slot_;
};

}  // namespace o3sdk::platform::mk66::usb
