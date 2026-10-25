// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's SD objects (OS-007e): the SDHC bus, the card on it, the FatFs volume on that. Statics with trivial start-up (no allocation, no
// register touched before init() / mount()).
#include <o3sdk/platform/mk66/storage/device.h>

#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/storage/sdhc_bus.h>

namespace o3sdk::platform::mk66::storage::device {
namespace {

SdhcBus g_bus;
sd::Card g_card(g_bus);
FatStorage g_fat(g_card);
CardSwap g_swap(g_fat, g_card);

bool g_tried = false;
hal::storage::Status g_last = hal::storage::Status::NotMounted;

}  // namespace

void init() noexcept { g_bus.init_detect_pin(); }

hal::storage::Storage& storage() noexcept { return g_fat; }

FatStorage& fat() noexcept { return g_fat; }

BlockDevice& block() noexcept { return g_card; }

hal::storage::Status mount() noexcept {
  if (g_swap.owner() == CardOwner::Host) {
    return hal::storage::Status::NotMounted;
  }
  g_last = g_swap.reacquire();
  g_tried = true;
  return g_last;
}

void watch_begin() noexcept { g_swap.begin(); }

CardSwap::Event poll(std::uint32_t now_ms) noexcept { return g_swap.poll(now_ms); }

void release() noexcept {
  if (g_swap.owner() == CardOwner::Host) {
    return;  // (CardSwap::release() ignores it too: `sd` keeps saying what the hand-over left)
  }
  g_swap.release();
  g_last = hal::storage::Status::NoCard;  // (what `sd` says of a card that is not mounted any more)
}

bool claim(CardClaims::Claim c) noexcept { return g_swap.claims().add(c); }

bool card_busy() noexcept { return g_swap.card_busy(); }

CardSwap::HandOver hand_to_host() noexcept {
  const CardSwap::HandOver r = g_swap.hand_to_host();
  if (r == CardSwap::HandOver::Ok) {
    g_last = hal::storage::Status::NotMounted;
  }
  return r;
}

bool boot_to_host() noexcept {
  if (!g_swap.boot_to_host()) {
    return false;
  }
  g_last = hal::storage::Status::NotMounted;
  return true;
}

const CardSwap& swap() noexcept { return g_swap; }

hal::storage::Status take_back() noexcept {
  const bool was_host = g_swap.owner() == CardOwner::Host;
  const hal::storage::Status s = g_swap.take_back();
  if (was_host) {
    g_last = s;  // (a call with no hand-over to end touches nothing, the remembered answer included)
    g_tried = true;
  }
  return s;
}

std::uint32_t host_sessions() noexcept { return g_swap.host_sessions(); }

CardOwner owner() noexcept { return g_swap.owner(); }

hal::storage::Status last_mount_status() noexcept { return g_last; }

SdReport report() noexcept {
  SdReport r;
  r.card_present = g_fat.card_present();
  r.mounted = g_fat.mounted();
  r.mount_status = g_last;
  r.mount_tried = g_tried;
  r.volume = g_fat.volume();
  r.card = &g_card.info();
  r.counters = &g_card.counters();
  r.stage = g_card.stage();
  r.fatfs_error = g_fat.last_fatfs_error();
  r.owner = g_swap.owner();
  r.removals = g_swap.removals();
  r.insertions = g_swap.insertions();
  return r;
}

}  // namespace o3sdk::platform::mk66::storage::device
