// SPDX-License-Identifier: GPL-3.0-or-later
//
// A card taken out and put back while the unit runs (OS-SDSWAP, storage/card_swap.h): the settled card-detect level (a bounce is not a swap), the release on a removal (every call
// answers NotMounted, every handle is closed, the driver forgets the medium), the remount on an insertion (a handle of the first mount stays dead), a card that does not mount
// (not tried again until it is taken out and put back), the hand-over to a host and back (OS-USBDISK U1b: nothing mounts under the host, a claim holds the hand-over), and the two writers: a save or a firmware upload that began on one mount stops with NoCard
// on any other, the SAME card put back included. Runs on the host and on QEMU's Cortex-M4F (the volume is sparse).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/storage/sd_text.h>
#include <o3sdk/platform/mk66/update/fat_firmware_store.h>
#include <o3sdk/platform/mk66/update/fw_service.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3sdk::hal::storage;
namespace st = o3sdk::platform::mk66::storage;
namespace up = o3sdk::platform::mk66::update;
using fakes::Fat32Builder;
using fakes::Medium;
using st::CardOwner;
using Event = st::CardSwap::Event;
using Edge = st::PinDebounce::Edge;

namespace {

std::vector<std::uint8_t> bytes(std::size_t n, unsigned salt) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 31u + 7u + salt) & 0xFFu);
  }
  return v;
}

// README.TXT, Workspace/project.mt, Firmware/ with an image: enough to read, to save into (Settings/ is made) and to upload next to.
struct Card {
  Medium medium{200'000};
  Card() {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", bytes(700, 1));
    const std::uint32_t workspace = fat.make_dir(Fat32Builder::kRootCluster, "Workspace");
    fat.add_file(workspace, "project.mt", bytes(2320, 10));
    const std::uint32_t firmware = fat.make_dir(Fat32Builder::kRootCluster, "Firmware");
    fat.add_file(firmware, "PolyendTracker_1.9.0.ptf", bytes(1500, 20));
  }
};

std::vector<std::uint8_t> read_file(hs::Storage& s, const char* path) {
  std::vector<std::uint8_t> out;
  hs::FileHandle f;
  if (s.open(path, f) != hs::Status::Ok) {
    return out;
  }
  std::uint8_t buf[300];
  std::size_t got = 0;
  while (s.read(f, buf, sizeof buf, got) == hs::Status::Ok && got != 0u) {
    out.insert(out.end(), buf, buf + got);
  }
  s.close(f);
  return out;
}

}  // namespace

TEST_CASE("pin debounce: a level has to hold for the settle time, a bounce is forgotten") {
  st::PinDebounce pin;
  pin.reset(true);
  CHECK(pin.update(true, 0) == Edge::None);
  CHECK(pin.update(false, 1000) == Edge::None);  // the pin opens
  CHECK(pin.update(false, 1249) == Edge::None);
  CHECK(pin.update(false, 1250) == Edge::Removed);  // 250 ms later: settled
  CHECK_FALSE(pin.settled());
  CHECK(pin.update(false, 1300) == Edge::None);  // reported once

  pin.reset(true);  // a bounce: open for 100 ms, closed again, open again: the timer starts over at the last opening
  CHECK(pin.update(false, 0) == Edge::None);
  CHECK(pin.update(true, 100) == Edge::None);
  CHECK(pin.update(false, 150) == Edge::None);
  CHECK(pin.update(false, 399) == Edge::None);
  CHECK(pin.update(false, 400) == Edge::Removed);

  pin.reset(true);  // an opening that goes back in time is nothing at all
  CHECK(pin.update(false, 0) == Edge::None);
  CHECK(pin.update(true, 249) == Edge::None);
  CHECK(pin.update(true, 5000) == Edge::None);
  CHECK(pin.settled());

  pin.reset(false);  // the other way, across the wrap of the millisecond clock
  const std::uint32_t t0 = 0xFFFFFF00u;
  CHECK(pin.update(true, t0) == Edge::None);
  CHECK(pin.update(true, t0 + 249u) == Edge::None);
  CHECK(pin.update(true, t0 + 250u) == Edge::Inserted);  // (t0 + 250 wrapped past zero)
  CHECK(pin.settled());
}

TEST_CASE("card swap: a card taken out is released, every call answers NotMounted, every handle is closed") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(card.medium.ready());

  hs::FileHandle file;
  hs::DirHandle dir;
  NEED(fat.open("README.TXT", file) == hs::Status::Ok);
  NEED(fat.opendir("Workspace", dir) == hs::Status::Ok);
  CHECK(fat.open_files() == 1u);

  card.medium.present_ = false;
  CHECK(fat.exists("README.TXT", nullptr) == hs::Status::NotMounted);  // typed at once, before the detect level has settled
  CHECK(swap.poll(1000) == Event::None);
  CHECK(swap.poll(1249) == Event::None);
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(swap.poll(1250) == Event::Removed);
  CHECK(swap.owner() == CardOwner::NoCard);
  CHECK(swap.removals() == 1u);
  CHECK(card.medium.releases == 1u);
  CHECK_FALSE(card.medium.ready());
  CHECK_FALSE(fat.mounted());
  CHECK(fat.open_files() == 0u);
  CHECK(fat.open_dirs() == 0u);

  std::uint8_t buf[16];
  std::size_t got = 0;
  CHECK(fat.read(file, buf, sizeof buf, got) == hs::Status::BadHandle);  // the handle belonged to the mount that is gone
  hs::DirEntry entry;
  CHECK(fat.readdir(dir, entry) == hs::Status::BadHandle);
  hs::FileHandle other;
  CHECK(fat.open("README.TXT", other) == hs::Status::NotMounted);
  CHECK(fat.opendir("", dir) == hs::Status::NotMounted);
  CHECK(fat.exists("Workspace", nullptr) == hs::Status::NotMounted);
  CHECK(swap.poll(1300) == Event::None);  // said once
  CHECK(swap.removals() == 1u);
}

TEST_CASE("card swap: a card put in is mounted again and a handle of the first mount stays dead") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  const std::vector<std::uint8_t> readme = read_file(fat, "README.TXT");
  NEED(readme.size() == 700u);
  hs::FileHandle old_file;
  NEED(fat.open("README.TXT", old_file) == hs::Status::Ok);

  card.medium.present_ = false;
  NEED(swap.poll(0) == Event::None);
  NEED(swap.poll(300) == Event::Removed);
  card.medium.present_ = true;
  CHECK(swap.poll(1000) == Event::None);
  CHECK(swap.poll(1249) == Event::None);
  CHECK(swap.poll(1250) == Event::Inserted);
  CHECK(swap.insertions() == 1u);
  CHECK(swap.owner() == CardOwner::NoCard);  // the caller mounts: nothing happens by itself
  CHECK_FALSE(fat.mounted());
  CHECK(fat.exists("README.TXT", nullptr) == hs::Status::NotMounted);

  NEED(swap.reacquire() == hs::Status::Ok);
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(fat.mounted());
  CHECK(read_file(fat, "README.TXT") == readme);
  std::uint8_t buf[16];
  std::size_t got = 0;
  CHECK(fat.read(old_file, buf, sizeof buf, got) == hs::Status::BadHandle);
  CHECK(swap.poll(1300) == Event::None);

  // and again, a second swap
  card.medium.present_ = false;
  NEED(swap.poll(2000) == Event::None);
  CHECK(swap.poll(2250) == Event::Removed);
  CHECK(swap.removals() == 2u);
}

TEST_CASE("card swap: a pin that bounces for less than the settle time is not a swap") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  card.medium.present_ = false;
  CHECK(swap.poll(100) == Event::None);
  CHECK(swap.poll(200) == Event::None);
  card.medium.present_ = true;  // back after 150 ms
  CHECK(swap.poll(250) == Event::None);
  CHECK(swap.poll(2000) == Event::None);
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(card.medium.releases == 0u);
  CHECK(fat.mounted());
  CHECK(swap.removals() == 0u);
}

TEST_CASE("card swap: no card at boot, then one is put in; a card that does not mount waits for the next swap") {
  Card card;
  card.medium.present_ = false;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  CHECK(swap.reacquire() == hs::Status::NoCard);  // the boot's own answer, as before
  swap.begin();
  CHECK(swap.owner() == CardOwner::NoCard);
  CHECK(swap.poll(0) == Event::None);
  card.medium.present_ = true;
  CHECK(swap.poll(1000) == Event::None);
  CHECK(swap.poll(1250) == Event::Inserted);
  CHECK(swap.reacquire() == hs::Status::Ok);
  CHECK(swap.owner() == CardOwner::Tracker);

  Medium blank{20'000};  // a card with no file system
  st::FatStorage fat2(blank);
  st::CardSwap swap2(fat2, blank);
  CHECK(swap2.reacquire() == hs::Status::Unsupported);  // in the slot and not readable: the boot fault of main.cpp
  swap2.begin();
  CHECK(swap2.owner() == CardOwner::NoCard);
  CHECK(swap2.poll(0) == Event::None);
  CHECK(swap2.poll(100000) == Event::None);  // no retry, however long it stays
  blank.present_ = false;
  CHECK(swap2.poll(200000) == Event::None);
  CHECK(swap2.poll(200250) == Event::Removed);  // taking it out is news, and it is said
  blank.present_ = true;
  CHECK(swap2.poll(300000) == Event::None);
  CHECK(swap2.poll(300250) == Event::Inserted);
}

TEST_CASE("card swap: a card gone between the mount and begin() is released") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  card.medium.present_ = false;
  swap.begin();
  CHECK(swap.owner() == CardOwner::NoCard);
  CHECK(card.medium.releases == 1u);
  CHECK(swap.poll(1000) == Event::None);  // it was never settled as present: nothing to report
}

TEST_CASE("card swap: a firmware upload ends when the card goes, and does not touch the card that comes back") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  up::FatFirmwareStore store(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  const std::vector<std::uint8_t> data = bytes(4000, 7);
  NEED(store.begin_upload("PolyendTracker_0.7.ptf", 4000) == up::Fault::Ok);
  CHECK(store.append(data.data(), 1024) == up::Fault::Ok);

  card.medium.present_ = false;
  NEED(swap.poll(0) == Event::None);
  NEED(swap.poll(300) == Event::Removed);
  CHECK(store.append(data.data() + 1024, 1024) == up::Fault::NoCard);

  card.medium.present_ = true;
  NEED(swap.poll(400) == Event::None);
  NEED(swap.poll(700) == Event::Inserted);
  NEED(swap.reacquire() == hs::Status::Ok);
  const std::uint64_t blocks = card.medium.blocks_written;
  CHECK(store.append(data.data() + 1024, 1024) == up::Fault::NoCard);  // the file handle is the first mount's: it never writes
  CHECK(store.close_upload() == up::Fault::NoCard);
  CHECK(store.commit_upload() != up::Fault::Ok);  // (the failed close left the file open: refused either way)
  store.abort_upload();
  CHECK(!store.uploading());
  CHECK(card.medium.blocks_written == blocks);  // the abort did not delete anything on the card that came back
  hs::FileHandle f;
  CHECK(fat.open("Firmware/_o3up.tmp", f) == hs::Status::Ok);  // (the next upload's begin_upload removes a leftover)
  fat.close(f);

  // and an upload on the new mount works as ever
  CHECK(store.begin_upload("PolyendTracker_0.7.ptf", 4000) == up::Fault::Ok);
  CHECK(store.append(data.data(), 4000) == up::Fault::Ok);
  CHECK(store.close_upload() == up::Fault::Ok);
  CHECK(store.commit_upload() == up::Fault::Ok);
  CHECK(read_file(fat, "Firmware/PolyendTracker_0.7.ptf") == data);
}

TEST_CASE("sd report: the owner and the swaps are on the `sd` page") {
  st::SdReport r;
  r.owner = CardOwner::Tracker;
  r.removals = 2;
  r.insertions = 3;
  char text[2048];
  const std::size_t n = st::format_sd(r, text, sizeof text);
  const std::string out(text, n);
  CHECK(out.find("owner:   tracker, 2 removed, 3 inserted since boot\n") != std::string::npos);
  r.owner = CardOwner::NoCard;
  const std::size_t m = st::format_sd(r, text, sizeof text);
  CHECK(std::string(text, m).find("owner:   no card, ") != std::string::npos);
}

namespace {
bool g_claim_a = false;
bool g_claim_b = false;
bool claim_a() noexcept { return g_claim_a; }
bool claim_b() noexcept { return g_claim_b; }
}  // namespace

template <int N>
bool claim_n() noexcept { return false; }

TEST_CASE("card claims: busy when any registered user says so; a null or an overflowing claim is refused") {
  st::CardClaims c;
  CHECK_FALSE(c.busy());
  CHECK_FALSE(c.add(nullptr));
  CHECK(c.add(&claim_a));
  CHECK(c.add(&claim_b));
  g_claim_a = g_claim_b = false;
  CHECK_FALSE(c.busy());
  g_claim_b = true;
  CHECK(c.busy());
  g_claim_a = true;
  g_claim_b = false;
  CHECK(c.busy());
  g_claim_a = false;
  CHECK(c.add(&claim_a));  // idempotent: the same claim twice is one entry
  CHECK(c.count() == 2u);
  const st::CardClaims::Claim fillers[] = {&claim_n<0>, &claim_n<1>, &claim_n<2>, &claim_n<3>, &claim_n<4>, &claim_n<5>};
  static_assert(sizeof(fillers) / sizeof(fillers[0]) + 2u == st::CardClaims::kMax, "two claims above plus the fillers fill the table");
  for (const st::CardClaims::Claim f : fillers) {
    CHECK(c.add(f));
  }
  CHECK(c.count() == st::CardClaims::kMax);
  CHECK_FALSE(c.add(&claim_n<6>));  // full: refused, and the caller logs it
  CHECK(c.add(&claim_n<0>));        // (already in the table: still true)
  CHECK(c.count() == st::CardClaims::kMax);
}

TEST_CASE("card swap: nothing mounts under a host (F1): reacquire, an insert event and a second hand-over touch neither FatFs nor the card") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  CHECK(swap.host_sessions() == 0u);
  CHECK(swap.take_back() == hs::Status::NotMounted);  // not Host: nothing to take back, nothing touched
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(fat.mounted());

  g_claim_a = true;  // a user holds the card: refused, the volume stays
  st::CardClaims::Claim claim = &claim_a;
  NEED(swap.claims().add(claim));
  CHECK(swap.card_busy());
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::Busy);
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(fat.mounted());
  CHECK(swap.host_sessions() == 0u);
  g_claim_a = false;
  CHECK_FALSE(swap.card_busy());

  const unsigned releases_before = card.medium.releases;
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK(swap.host_sessions() == 1u);
  CHECK(card.medium.releases == releases_before + 1u);
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::AlreadyHost);
  CHECK(swap.host_sessions() == 1u);

  const unsigned starts = card.medium.starts;
  const std::uint64_t reads = card.medium.blocks_read;
  const std::uint64_t writes = card.medium.blocks_written;
  const std::uint64_t print = card.medium.fingerprint();
  CHECK(swap.reacquire() == hs::Status::NotMounted);  // the boot mount, `sd mount` and the insert follow-up all end here
  CHECK(swap.owner() == CardOwner::Host);
  CHECK_FALSE(fat.mounted());
  card.medium.present_ = false;  // a card pulled and another put in: no event, no mount
  CHECK(swap.poll(0) == Event::None);
  CHECK(swap.poll(300) == Event::None);
  card.medium.present_ = true;
  CHECK(swap.poll(400) == Event::None);
  CHECK(swap.poll(700) == Event::None);
  CHECK(swap.reacquire() == hs::Status::NotMounted);
  CHECK(card.medium.starts == starts);
  CHECK(card.medium.blocks_read == reads);
  CHECK(card.medium.blocks_written == writes);
  CHECK(card.medium.fingerprint() == print);

  NEED(swap.take_back() == hs::Status::Ok);  // the eject path is the one way back
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(fat.mounted());
  CHECK(card.medium.starts == starts + 1u);
  CHECK(fat.generation() >= 2u);
}

// F3: one test for each FwPhase, through the claim the image registers (firmware_has_card = fw_phase_holds_card of the service's phase): the hand-over is refused with
// Busy in every phase that holds the card and allowed in Idle and Done; and `sd mount`, which ends in reacquire(), mounts nothing under a host (the console's own check is image-only).
up::FwPhase g_phase = up::FwPhase::Idle;
bool phase_claim() noexcept { return up::fw_phase_holds_card(g_phase); }

TEST_CASE("card swap: the hand-over is refused or allowed per FwPhase through the claims, and a mount (sd mount) under a host mounts nothing") {
  using P = up::FwPhase;
  const P all[] = {P::Idle, P::Receiving, P::Discarding, P::Finalizing, P::Verifying, P::Arming, P::Rechecking, P::Handover, P::Done};
  for (const P phase : all) {
    Card card;
    st::FatStorage fat(card.medium);
    st::CardSwap swap(fat, card.medium);
    NEED(swap.reacquire() == hs::Status::Ok);
    swap.begin();
    NEED(swap.claims().add(&phase_claim));
    NEED(swap.claims().add(&phase_claim));  // idempotent: a recovery registers again
    CHECK(swap.claims().count() == 1u);
    g_phase = phase;
    const bool holds = up::fw_phase_holds_card(phase);
    CHECK(swap.card_busy() == holds);
    if (holds) {
      CHECK(swap.hand_to_host() == st::CardSwap::HandOver::Busy);
      CHECK(swap.owner() == CardOwner::Tracker);
      CHECK(fat.mounted());
      CHECK(swap.host_sessions() == 0u);
    } else {
      CHECK(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
      CHECK(swap.owner() == CardOwner::Host);
      CHECK(swap.host_sessions() == 1u);
      const unsigned starts = card.medium.starts;
      CHECK(swap.reacquire() == hs::Status::NotMounted);  // what `sd mount` calls
      CHECK(card.medium.starts == starts);
      swap.release();  // a stale release while the computer has the card: a no-op
      CHECK(swap.owner() == CardOwner::Host);
      CHECK(swap.host_sessions() == 1u);
    }
  }
  g_phase = P::Idle;
}

TEST_CASE("card swap: a hand-over without a card is refused, and a card that does not mount comes back as NoCard") {
  Card card;
  card.medium.present_ = false;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  CHECK(swap.reacquire() == hs::Status::NoCard);
  swap.begin();
  CHECK(swap.owner() == CardOwner::NoCard);
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::NoCard);
  CHECK(swap.owner() == CardOwner::NoCard);
  CHECK(swap.host_sessions() == 0u);
}

// ---- OS-USBDISK 6.3 (F5): the volume-dirty flag read after a Host session (storage/volume_dirty.h). The bits are pinned on images built here; what Windows, macOS and Linux leave after a clean
// eject and after a pulled cable is NOT verified (open: fixtures from each).
namespace {

void put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
  put16(p, static_cast<std::uint16_t>(v));
  put16(p + 2, static_cast<std::uint16_t>(v >> 16));
}

// A FAT16 volume at `base`: 512-byte sectors, 4 per cluster, 1 reserved, two FATs of 20 sectors, 512 root entries, 20000 sectors (4981 clusters: FAT16). FAT[1] clean unless asked.
void fat16_at(Medium& m, std::uint32_t base, bool fat_clean, bool boot_dirty) {
  std::uint8_t* b = m.sector(base);
  b[0] = 0xEB;
  put16(b + 11, 512);
  b[13] = 4;
  put16(b + 14, 1);
  b[16] = 2;
  put16(b + 17, 512);
  put16(b + 19, 20000);
  put16(b + 22, 20);
  b[0x25] = boot_dirty ? 1 : 0;
  put16(b + 510, 0xAA55);
  put16(m.sector(base + 1), 0xFFF8);
  put16(m.sector(base + 1) + 2, fat_clean ? 0xFFFF : 0x7FFF);
}

void exfat_at(Medium& m, bool dirty) {
  std::uint8_t* b = m.sector(0);
  b[0] = 0xEB;
  std::memcpy(b + 3, "EXFAT   ", 8);
  put16(b + 0x6A, dirty ? 2u : 0u);
  put16(b + 510, 0xAA55);
}

}  // namespace

TEST_CASE("volume dirty flag: FAT32, FAT16 (bare and behind an MBR), exFAT; anything else is Unknown") {
  using st::VolumeFlag;
  {
    Card card;  // FAT32 as the builder formats it
    (void)card.medium.start();
    CHECK(st::volume_flag(card.medium) == VolumeFlag::Clean);
    std::uint8_t* fat1 = card.medium.sector(Fat32Builder::kReserved) + 4;
    const std::uint32_t was = fat1[0] | (fat1[1] << 8) | (fat1[2] << 16) | (static_cast<std::uint32_t>(fat1[3]) << 24);
    put32(fat1, was & ~0x08000000u);  // ClnShutBit (bit 27) cleared: the volume was not closed
    CHECK(st::volume_flag(card.medium) == VolumeFlag::Dirty);
    put32(fat1, was);
    card.medium.sector(0)[0x41] = 1;  // the boot sector's flag alone
    CHECK(st::volume_flag(card.medium) == VolumeFlag::Dirty);
  }
  {
    Medium m(100'000);
    (void)m.start();
    fat16_at(m, 0, true, false);
    CHECK(st::volume_flag(m) == VolumeFlag::Clean);
    fat16_at(m, 0, false, false);
    CHECK(st::volume_flag(m) == VolumeFlag::Dirty);  // FAT16 ClnShutBit (0x8000 of FAT[1]) cleared
    fat16_at(m, 0, true, true);
    CHECK(st::volume_flag(m) == VolumeFlag::Dirty);  // the boot flag at 0x25
  }
  {
    Medium m(100'000);
    (void)m.start();  // a partitioned card: the first entry is empty, the second holds the volume at LBA 2048
    std::uint8_t* mbr = m.sector(0);
    mbr[446 + 16 + 4] = 0x0E;
    put32(mbr + 446 + 16 + 8, 2048);
    put16(mbr + 510, 0xAA55);
    fat16_at(m, 2048, true, false);
    CHECK(st::volume_flag(m) == VolumeFlag::Clean);
    fat16_at(m, 2048, false, false);
    CHECK(st::volume_flag(m) == VolumeFlag::Dirty);
  }
  {
    Medium m(100'000);
    (void)m.start();
    exfat_at(m, false);
    CHECK(st::volume_flag(m) == VolumeFlag::Clean);
    exfat_at(m, true);
    CHECK(st::volume_flag(m) == VolumeFlag::Dirty);  // VolumeFlags bit 1
  }
  {
    Medium m(100'000);  // nothing that looks like a volume
    (void)m.start();
    CHECK(st::volume_flag(m) == VolumeFlag::Unknown);
  }
}

TEST_CASE("card swap: take_back reads the dirty flag, a warning only (the volume mounts and works either way)") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  CHECK(swap.remount_flag() == st::VolumeFlag::Unknown);  // no Host session yet: nothing was read (the boot mount does not look)
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Clean);
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  std::uint8_t* fat1 = card.medium.sector(Fat32Builder::kReserved) + 4;  // the computer was unplugged without an eject
  fat1[3] = static_cast<std::uint8_t>(fat1[3] & ~0x08u);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK(swap.owner() == CardOwner::Tracker);
  CHECK(read_file(fat, "README.TXT").size() == 700u);  // everything else works
}

TEST_CASE("card swap: the dirty warning is for a flag the session set, not for one the card already had (read at the hand-over, read-only)") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  CHECK(swap.handover_flag() == st::VolumeFlag::Unknown);  // before any hand-over
  CHECK_FALSE(swap.remount_newly_dirty());
  std::uint8_t* fat1 = card.medium.sector(Fat32Builder::kReserved) + 4;

  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);  // a clean card, and the session leaves it dirty: the one case that warns
  CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
  fat1[3] = static_cast<std::uint8_t>(fat1[3] & ~0x08u);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK(swap.remount_newly_dirty());

  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);  // the same card, still dirty from before the share: it is not the computer's doing
  CHECK(swap.handover_flag() == st::VolumeFlag::Dirty);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK_FALSE(swap.remount_newly_dirty());  // (a warning each time was the bug)

  fat1[3] = static_cast<std::uint8_t>(fat1[3] | 0x08u);  // the computer ejected it properly this time: clean before, clean after
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Clean);
  CHECK_FALSE(swap.remount_newly_dirty());
}

TEST_CASE("card swap: reading the flag at the hand-over writes nothing, a refused hand-over reads nothing, and the boot disk has no hand-over flag") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  NEED(swap.claims().add(&claim_a));
  g_claim_a = true;
  const std::uint64_t medium_before = card.medium.fingerprint();
  const auto reads_before = card.medium.read_calls;
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::Busy);
  CHECK(card.medium.read_calls == reads_before);  // refused before the card is looked at
  CHECK(swap.handover_flag() == st::VolumeFlag::Unknown);
  g_claim_a = false;
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK(card.medium.read_calls > reads_before);
  CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
  CHECK(card.medium.fingerprint() == medium_before);  // nothing was written
  CHECK(swap.hand_to_host() == st::CardSwap::HandOver::AlreadyHost);
  CHECK(swap.handover_flag() == st::VolumeFlag::Clean);  // (a second call does not look again)
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Clean);

  swap.release();  // the boot disk: NoCard to Host, nothing mounted, so there was nothing to look at
  NEED(swap.owner() == CardOwner::NoCard);
  NEED(swap.boot_to_host());
  CHECK(swap.handover_flag() == st::VolumeFlag::Unknown);
  CHECK_FALSE(swap.remount_newly_dirty());
}

// OS-USBDISK 6.3 (F5), the three cases of the warning, each with the two places a computer leaves the flag: Windows clears the ClnShutBit of FAT[1] while it has a volume mounted, Linux's vfat sets the boot sector's
// flag byte (0x41, bit 0); both are what volume_flag() looks at, and either one means "not closed properly". Fixtures built by hand (the Windows, macOS and Linux images of the card stay [open], spec 6.3).
TEST_CASE("dirty flag, the warning: a clean card the session left dirty warns once, an already dirty one does not warn again, a dirty one the computer cleaned clears it") {
  struct Style {
    const char* name;
    void (*set)(Card&, bool dirty);
  };
  const Style styles[] = {
      {"FAT[1] ClnShutBit",
       [](Card& c, bool dirty) {
         std::uint8_t* fat1 = c.medium.sector(Fat32Builder::kReserved) + 4;
         fat1[3] = dirty ? static_cast<std::uint8_t>(fat1[3] & ~0x08u) : static_cast<std::uint8_t>(fat1[3] | 0x08u);
       }},
      {"boot sector flag",
       [](Card& c, bool dirty) { c.medium.sector(0)[0x41] = dirty ? 1 : 0; }},
  };
  for (const Style& style : styles) {
    INFO(style.name);
    Card card;
    st::FatStorage fat(card.medium);
    st::CardSwap swap(fat, card.medium);
    NEED(swap.reacquire() == hs::Status::Ok);
    swap.begin();

    // 1. Clean at the hand-over, dirty at the take-back (the computer was unplugged mid-write): the warning, once. The next session over the same card (nothing cleaned it) does not warn again.
    NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
    CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
    style.set(card, true);
    NEED(swap.take_back() == hs::Status::Ok);
    CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
    CHECK(swap.remount_newly_dirty());
    NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
    CHECK(swap.handover_flag() == st::VolumeFlag::Dirty);  // 2. already dirty when the share began
    NEED(swap.take_back() == hs::Status::Ok);
    CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
    CHECK_FALSE(swap.remount_newly_dirty());

    // 3. Dirty at the hand-over, cleaned by the computer during the session (a check disk, a proper eject): clean at the take-back, no warning, and the next dirtying warns again.
    NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
    CHECK(swap.handover_flag() == st::VolumeFlag::Dirty);
    style.set(card, false);
    NEED(swap.take_back() == hs::Status::Ok);
    CHECK(swap.remount_flag() == st::VolumeFlag::Clean);
    CHECK_FALSE(swap.remount_newly_dirty());
    NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
    CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
    style.set(card, true);
    NEED(swap.take_back() == hs::Status::Ok);
    CHECK(swap.remount_newly_dirty());
    CHECK(read_file(fat, "README.TXT").size() == 700u);  // everything works either way
  }
}

// One place dirty and the other clean: the flag is Dirty (volume_flag ORs the boot sector's byte and FAT[1] bit 27), so a computer that cleans only one of the two (a check disk that rewrites FAT[1] and leaves the
// boot byte, or the reverse) does not clear the warning, and the warning is not repeated while the other place stays set. Fixtures by hand, as above; what a real Windows or macOS leaves stays [open] (spec 6.3).
TEST_CASE("dirty flag, one place dirty and the other clean: dirty wins, and only both clean is Clean") {
  const auto fat1_clean = [](Card& c, bool clean) {
    std::uint8_t* fat1 = c.medium.sector(Fat32Builder::kReserved) + 4;
    fat1[3] = clean ? static_cast<std::uint8_t>(fat1[3] | 0x08u) : static_cast<std::uint8_t>(fat1[3] & ~0x08u);
  };
  const auto boot_dirty = [](Card& c, bool dirty) { c.medium.sector(0)[0x41] = dirty ? 1 : 0; };
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();

  fat1_clean(card, false);  // FAT[1] says not closed, the boot byte says closed
  boot_dirty(card, false);
  CHECK(st::volume_flag(card.medium) == st::VolumeFlag::Dirty);
  fat1_clean(card, true);  // the other way round
  boot_dirty(card, true);
  CHECK(st::volume_flag(card.medium) == st::VolumeFlag::Dirty);
  boot_dirty(card, false);
  CHECK(st::volume_flag(card.medium) == st::VolumeFlag::Clean);

  // A session: the computer sets only the boot byte (clean at the hand-over, dirty at the take-back): the warning, once.
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK(swap.handover_flag() == st::VolumeFlag::Clean);
  boot_dirty(card, true);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK(swap.remount_newly_dirty());
  // The next session: the computer clears the boot byte but also sets the FAT[1] mark (it mounted the volume): still Dirty, and not newly so.
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK(swap.handover_flag() == st::VolumeFlag::Dirty);
  boot_dirty(card, false);
  fat1_clean(card, false);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK_FALSE(swap.remount_newly_dirty());
  // The next: FAT[1] is cleaned and the boot byte set again (the one place changed hands, the other stayed): Dirty still, no new warning; only both clean gives Clean.
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  fat1_clean(card, true);
  boot_dirty(card, true);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Dirty);
  CHECK_FALSE(swap.remount_newly_dirty());
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  boot_dirty(card, false);
  NEED(swap.take_back() == hs::Status::Ok);
  CHECK(swap.remount_flag() == st::VolumeFlag::Clean);
  CHECK_FALSE(swap.remount_newly_dirty());
}
