// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The image's one SD card (OS-007e): the SDHC bus, the card on it and the FatFs volume on that, as three static objects and the few calls
// main and the `diag` console make on them. Declared everywhere, defined only in the image (src/device.cpp). read only for everything that reads through
// storage(); the firmware writer (update/firmware_store.h, OS-006c) is the one other client. Main loop only.

#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/storage/sd_card.h>
#include <o3sdk/platform/mk66/storage/sd_text.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage::device {

// Once, early (before the first mount): the card-detect pin. The SDHC itself is not touched until the first mount().
void init() noexcept;

// The volume as the layers above read it. Valid at any time; reads answer NotMounted until mount() has succeeded.
hal::storage::Storage& storage() noexcept;

// OS-006c: the same volume and the card under it, for the firmware writer (update/fat_firmware_store.h) and for nothing else.
FatStorage& fat() noexcept;
BlockDevice& block() noexcept;

// Initialises the card and mounts its file system (hal::storage::Storage::mount), remembering what it answered. Blocks for the card's
// initialisation (tens of milliseconds, at most sd::policy::worst_init_us() when the card does not answer). Callers that must keep the
// display serviced around it use appimg::service_while_blocked(). Also the way back after a card was taken out (OS-SDSWAP: the reacquire of release()). While the owner is Host
// it answers NotMounted and touches nothing (OS-USBDISK F1: only take_back() mounts under a hand-over); last_mount_status() is left as it was.
hal::storage::Status mount() noexcept;
hal::storage::Status last_mount_status() noexcept;

// OS-SDSWAP, a card taken out and put back (storage/card_swap.h): watch_begin() once after the boot mount, poll() every pass of the main loop with the millisecond
// clock. Removed: the volume is already released (every call on the card answers NotMounted, nothing is read or written); Inserted: the caller shows it and calls mount().
// release() is the same unmount for a card that is gone (owner NoCard). owner() says who has it.
void watch_begin() noexcept;
CardSwap::Event poll(std::uint32_t now_ms) noexcept;
void release() noexcept;
CardOwner owner() noexcept;

// OS-USBDISK U1b. claim(): a user that keeps the card across passes registers once at start-up (storage/card_claims.h); card_busy(): any of them. hand_to_host() lets the card go to
// a computer (refused when a claim says busy, or the owner is not Tracker); take_back() is the eject path's mount; host_sessions() counts the hand-overs (a `fw arm` token compares it).
bool claim(CardClaims::Claim claim) noexcept;
bool card_busy() noexcept;
CardSwap::HandOver hand_to_host() noexcept;
// OS-USBDISK U1c, the boot disk: NoCard to Host with nothing mounted (CardSwap::boot_to_host); and the swap for the MSC bridge's owner check (read-only).
bool boot_to_host() noexcept;
const CardSwap& swap() noexcept;
hal::storage::Status take_back() noexcept;
std::uint32_t host_sessions() noexcept;

// What `diag sd` prints, as the numbers (the caller adds the loader's summary line).
SdReport report() noexcept;

}  // namespace o3sdk::platform::mk66::storage::device
