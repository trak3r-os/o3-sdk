// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A card taken out and put back while the unit runs (OS-SDSWAP). Until now the card was mounted once, at boot: a swap left FatFs and the card driver
// holding the first card's state, and every read of the second card failed with an i/o error until the unit was reset. This is the policy that follows
// the slot:
//
//   * PinDebounce  the card-detect level, settled: a level has to hold for kSettleMs before it counts, so a card that is sliding in or out (contact bounce, a
//                  hand that hesitates) gives one Removed or one Inserted, never a burst. Pure arithmetic on a millisecond clock (wraps at 2^32).
//   * CardSwap     who has the card (CardOwner) and what a settled change does. Removed: the volume is released at once (FatStorage::release: every handle closed,
//                  FatFs and the driver forget the card), so every call on the card answers NotMounted instead of failing on a bus that has nothing on it.
//                  Inserted: nothing is touched here; the caller says so on the strip and then calls reacquire(), which blocks like the boot mount does (the card's
//                  own initialisation, bounded, tens of ms for a card that answers). A card that fails to mount is not tried again until it is taken out and put back.
//
// CardOwner is the one answer to "may I touch the card": Tracker (mounted for the OS), NoCard (the slot is empty, or holds a card that did not mount), Host
// (let go on purpose, for the USB disk mode: hand_to_host() and take_back() are the whole hand-over; no USB code is here). While the owner is Host the debounce goes on but
// poll() reports nothing, so the host's own pulling of the card is not a Removed, and NOTHING mounts the volume: reacquire() answers NotMounted and touches neither FatFs nor the
// card until take_back() (OS-USBDISK F1: a mount under the computer would be a second writer on its FAT). The only way to Host is hand_to_host(), which refuses while a claim
// (card_claims.h) says the card is in use. Main loop only, no allocation, no waits except reacquire() and take_back().

#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/card_claims.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/storage/volume_dirty.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage {

enum class CardOwner : std::uint8_t { NoCard, Tracker, Host };

inline constexpr const char* card_owner_name(CardOwner o) noexcept {
  switch (o) {
    case CardOwner::NoCard: return "no card";
    case CardOwner::Tracker: return "tracker";
    case CardOwner::Host: return "host";
  }
  return "?";
}

class PinDebounce {
 public:
  static constexpr std::uint32_t kSettleMs = 250;  // the maintainer's range is 200 to 300 ms: long enough for a card's travel through the slot, short enough to feel immediate
  enum class Edge : std::uint8_t { None, Removed, Inserted };

  // The settled level is `present` from now on (nothing pending).
  void reset(bool present) noexcept {
    stable_ = present;
    raw_ = present;
    since_ = 0;
  }
  // Feeds one reading of the pin. A change is reported once, when the new level has been seen continuously for kSettleMs; a level that goes back before that is forgotten.
  Edge update(bool present, std::uint32_t now_ms) noexcept {
    if (present == stable_) {
      raw_ = stable_;
      return Edge::None;
    }
    if (present != raw_) {
      raw_ = present;
      since_ = now_ms;
      return Edge::None;
    }
    if (static_cast<std::uint32_t>(now_ms - since_) < kSettleMs) {
      return Edge::None;
    }
    stable_ = present;
    return present ? Edge::Inserted : Edge::Removed;
  }
  bool settled() const noexcept { return stable_; }

 private:
  bool stable_ = false;
  bool raw_ = false;
  std::uint32_t since_ = 0;
};

class CardSwap {
 public:
  enum class Event : std::uint8_t { None, Removed, Inserted };

  CardSwap(FatStorage& fat, BlockDevice& device) noexcept : fat_(fat), device_(device) {}

  // Once, right after the boot mount (whatever it answered): the slot's level becomes the settled one and the owner is Tracker when the volume is mounted. A card that was
  // already gone by now is released here.
  void begin() noexcept;
  // Every pass of the main loop: reads the detect level (no bus traffic) and, on a settled change, answers it. Removed: the volume is already released when this returns.
  // Inserted (only when the owner is NoCard): the caller shows it and calls reacquire(). Always None while the owner is Host.
  Event poll(std::uint32_t now_ms) noexcept;

  // Initialises the card and mounts the volume (FatStorage::mount); Tracker on Ok, NoCard otherwise. Blocks for as long as the boot mount does. The boot mount, the insert
  // follow-up and the console's `sd mount` come through here too, so the owner always agrees with the volume. While the owner is Host: NotMounted, nothing touched.
  hal::storage::Status reacquire() noexcept;
  // The card is gone or let go: the volume is released (handles closed, nothing written) and the owner is NoCard.
  void release() noexcept;

  enum class HandOver : std::uint8_t { Ok, NoCard, Busy, AlreadyHost };
  // Tracker to Host. Refused (nothing touched) when the owner is not Tracker (NoCard; AlreadyHost) or when a claim says the card is in use (Busy). Ok: the volume is released
  // (every handle closed, generation() advanced) and host_sessions() counts one more. The caller owns the order of 4.2 of the spec (the layout first, the owner last).
  HandOver hand_to_host() noexcept;
  // The boot disk (OS-USBDISK U1c, mode (b)): the unit started with the chord and mounted nothing, so the owner is NoCard and hand_to_host() (Tracker only) cannot be used. NoCard to Host
  // with no FatFs call and no card traffic (the bridge's open() starts the card); true when it did, false (nothing changed) when the owner is not NoCard. A boot that mounted the card never
  // comes here. The caller does it after the Disk layout is on the bus (4.2: the owner last).
  bool boot_to_host() noexcept;
  // Host to Tracker: the eject path, and nothing else. Mounts the volume like reacquire() (blocking); Tracker on Ok, NoCard otherwise. NotMounted, nothing touched, when the
  // owner is not Host.
  hal::storage::Status take_back() noexcept;
  // OS-USBDISK 6.3 (F5): what the last take_back() found in the volume's dirty flag (volume_dirty.h); Unknown before any take_back and after a mount that failed. A warning only.
  VolumeFlag remount_flag() const noexcept { return remount_flag_; }
  // What hand_to_host() found in the same flag before the card went (three reads, nothing written); Unknown before any hand-over, after a read that failed and after boot_to_host() (nothing was mounted to look at).
  VolumeFlag handover_flag() const noexcept { return handover_flag_; }
  // The warning's condition: the last take_back() found the flag set and the hand-over did not (a card that was already dirty before the share is not the computer's doing, so it does not warn every time).
  // Unknown at the hand-over counts as not dirty: the flag is then set by a session whose start nobody saw.
  bool remount_newly_dirty() const noexcept { return remount_flag_ == VolumeFlag::Dirty && handover_flag_ != VolumeFlag::Dirty; }

  // The users that keep the card across passes register here once (device.cpp does for the image's).
  CardClaims& claims() noexcept { return claims_; }
  bool card_busy() const noexcept { return claims_.busy(); }

  CardOwner owner() const noexcept { return owner_; }
  // Counts the hand-overs to a host since start-up (wraps). A token or a belief taken before a hand-over compares it to know that the card may have been rewritten since.
  std::uint32_t host_sessions() const noexcept { return host_sessions_; }
  std::uint32_t removals() const noexcept { return removals_; }
  std::uint32_t insertions() const noexcept { return insertions_; }

 private:
  FatStorage& fat_;
  BlockDevice& device_;
  PinDebounce pin_;
  hal::storage::Status mount_volume() noexcept;

  CardClaims claims_;
  CardOwner owner_ = CardOwner::NoCard;
  VolumeFlag remount_flag_ = VolumeFlag::Unknown;
  VolumeFlag handover_flag_ = VolumeFlag::Unknown;
  std::uint32_t host_sessions_ = 0;
  std::uint32_t removals_ = 0;
  std::uint32_t insertions_ = 0;
};

}  // namespace o3sdk::platform::mk66::storage
