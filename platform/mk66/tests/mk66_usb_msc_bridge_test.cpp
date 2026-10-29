// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U1c: the MSC bridge (usb/msc_bridge.h) over a card: NOT READY, MEDIUM NOT PRESENT whenever the owner is not Host (and the device is not touched), read-only by default,
// writes only after set_writable(true), the engine's reads through the bridge. The writable step (end of the file): a computer's WRITE(10) through the engine and the bridge into a FAT32 image,
// the write-protect bit that follows the policy and the card, a write while the owner is not Host (NOT READY, no block), the Tracker's own writers shut out while the card is the computer's, and
// a card swapped in the middle of a WRITE. Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_fakes.h"
#include "mk66_storage_need.h"

#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/usb/msc_bridge.h>
#include <o3sdk/usb/msc.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace st = o3sdk::platform::mk66::storage;
namespace msc = o3sdk::usb::msc;
using fakes::Fat32Builder;
using fakes::Medium;
using o3sdk::platform::mk66::usb::MscBridge;

namespace {
struct Card {
  Medium medium{20'000};
  Card() {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", std::vector<std::uint8_t>(700, 0x5A));
  }
};
}  // namespace

TEST_CASE("msc bridge: NOT READY, MEDIUM NOT PRESENT whenever the owner is not Host, and the device is not touched") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  std::uint8_t buf[512];
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);  // owner Tracker
  swap.begin();
  const std::uint64_t reads = card.medium.blocks_read;
  const unsigned starts = card.medium.starts;
  CHECK_FALSE(bridge.open());  // refused: the card is the Tracker's
  CHECK_FALSE(bridge.present());
  CHECK(bridge.block_count() == 0u);
  CHECK(bridge.read(0, 1, buf) == msc::IoResult::NoMedium);
  CHECK(bridge.write(0, 1, buf) == msc::IoResult::NoMedium);
  CHECK(card.medium.blocks_read == reads);
  CHECK(card.medium.starts == starts);

  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  CHECK_FALSE(bridge.present());  // handed over but not opened: still no medium
  NEED(bridge.open());
  CHECK(bridge.present());
  CHECK(bridge.block_count() == card.medium.block_count());
  CHECK(bridge.read(0, 1, buf) == msc::IoResult::Ok);

  NEED(swap.take_back() == o3sdk::hal::storage::Status::Ok);  // the owner moves first (the eject path): the bridge answers at once, before close()
  CHECK_FALSE(bridge.present());
  CHECK(bridge.block_count() == 0u);
  const std::uint64_t reads_back = card.medium.blocks_read;
  CHECK(bridge.read(0, 1, buf) == msc::IoResult::NoMedium);
  CHECK(card.medium.blocks_read == reads_back);
  bridge.close();
  CHECK_FALSE(bridge.opened());
  // The eject order is take_back() then close(): the volume FatFs mounted must still read (close() must not release the device under it).
  CHECK(fat.mounted());
  o3sdk::hal::storage::FileHandle file{};
  CHECK(fat.open("README.TXT", file) == o3sdk::hal::storage::Status::Ok);
  std::uint8_t file_bytes[16] = {};
  std::size_t got = 0;
  CHECK(fat.read(file, file_bytes, sizeof file_bytes, got) == o3sdk::hal::storage::Status::Ok);
  CHECK(got == sizeof file_bytes);
  CHECK(file_bytes[0] == 0x5Au);
}

TEST_CASE("msc bridge: read-only until set_writable, and a write reaches the card only then") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  swap.begin();
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(bridge.open());
  std::uint8_t out[512];
  for (unsigned i = 0; i < sizeof out; ++i) {
    out[i] = static_cast<std::uint8_t>(i);
  }
  CHECK(bridge.write_protected());
  const std::uint64_t print = card.medium.fingerprint();
  const std::uint64_t writes = card.medium.blocks_written;
  CHECK(bridge.write(100, 1, out) == msc::IoResult::WriteProtected);
  CHECK(card.medium.blocks_written == writes);
  CHECK(card.medium.fingerprint() == print);

  bridge.set_writable(true);
  CHECK_FALSE(bridge.write_protected());
  CHECK(bridge.write(100, 1, out) == msc::IoResult::Ok);
  CHECK(card.medium.blocks_written == writes + 1u);
  std::uint8_t back[512] = {};
  CHECK(bridge.read(100, 1, back) == msc::IoResult::Ok);
  CHECK(back[7] == 7u);
  CHECK(bridge.read(card.medium.block_count(), 1, back) == msc::IoResult::Failed);  // out of range is an error, not a medium change
}

TEST_CASE("msc bridge: a card that does not start leaves the bridge closed, and the engine sees no medium") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  swap.begin();
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  card.medium.present_ = false;
  CHECK_FALSE(bridge.open());
  CHECK_FALSE(bridge.opened());
  msc::Engine eng(bridge);
  CHECK_FALSE(eng.ejected());
  CHECK(bridge.block_count() == 0u);
}

TEST_CASE("msc bridge: follow() lets the pulled card go and starts the one put in, after the debounce, and tells the caller (review fold, spec 3.4 amendment 4)") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  CHECK_FALSE(bridge.follow(0));  // before open(): nothing is watched
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  swap.begin();
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(bridge.open());
  msc::Engine eng(bridge);
  std::uint32_t t = 1000;
  CHECK_FALSE(bridge.follow(t));  // the level did not move

  card.medium.present_ = false;  // pulled: a bounce shorter than the settle time is nothing
  CHECK_FALSE(bridge.follow(t));
  card.medium.present_ = true;
  CHECK_FALSE(bridge.follow(t + 100));
  card.medium.present_ = false;  // pulled for good
  CHECK_FALSE(bridge.follow(t + 200));
  CHECK(bridge.follow(t + 200 + st::PinDebounce::kSettleMs));
  CHECK_FALSE(bridge.opened());
  CHECK_FALSE(bridge.present());
  eng.medium_changed();
  CHECK_FALSE(bridge.follow(t + 1000));  // settled: nothing more to say

  card.medium.present_ = true;  // a card put in: started after the debounce, the caller is told
  t += 2000;
  CHECK_FALSE(bridge.follow(t));
  const unsigned starts = card.medium.starts;
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(bridge.opened());
  CHECK(bridge.present());
  CHECK(card.medium.starts == starts + 1u);
  CHECK(bridge.block_count() == card.medium.block_count());

  NEED(swap.take_back() == o3sdk::hal::storage::Status::Ok);  // under the Tracker again: follow() touches nothing
  card.medium.present_ = false;
  CHECK_FALSE(bridge.follow(t + 5000));
  CHECK_FALSE(bridge.follow(t + 5000 + st::PinDebounce::kSettleMs));
}

TEST_CASE("msc bridge: a card that is in and does not start is not tried again until it is pulled and put back (no retry, review fold)") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  swap.begin();
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(bridge.open());
  std::uint32_t t = 1000;
  CHECK_FALSE(bridge.follow(t));
  card.medium.present_ = false;  // pulled
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  t += 2000;
  card.medium.present_ = true;  // a bad card put in: one attempt, which fails
  card.medium.fail_start = true;
  CHECK_FALSE(bridge.follow(t));
  const unsigned starts = card.medium.starts;
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(card.medium.starts == starts + 1u);
  CHECK_FALSE(bridge.opened());
  for (std::uint32_t i = 1; i <= 50; ++i) {  // every later pass of the main loop: no further start() (each costs seconds on a bad card)
    CHECK_FALSE(bridge.follow(t + st::PinDebounce::kSettleMs + i * 100u));
  }
  CHECK(card.medium.starts == starts + 1u);
  CHECK_FALSE(bridge.opened());
  card.medium.fail_start = false;  // pulled and put back: one more attempt, which works
  t += 10000;
  card.medium.present_ = false;
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  t += 2000;
  card.medium.present_ = true;
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(card.medium.starts == starts + 2u);
  CHECK(bridge.opened());
}

namespace {
// The blocking hook of the review fold (U1c part 2b minor 1): records what it saw of the card's start counter on each call.
struct HookLog {
  static inline unsigned on_calls = 0;
  static inline unsigned off_calls = 0;
  static inline unsigned starts_at_on = 0;
  static inline unsigned starts_at_off = 0;
  static inline const Medium* medium = nullptr;
  static void hook(bool on) noexcept {
    if (on) {
      ++on_calls;
      starts_at_on = medium->starts;
    } else {
      ++off_calls;
      starts_at_off = medium->starts;
    }
  }
};
}  // namespace

TEST_CASE("msc bridge: follow() brackets the start of an inserted card with the blocking hook, and only that (review fold, service_while_blocked)") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  MscBridge bridge(card.medium, swap);
  HookLog::medium = &card.medium;
  HookLog::on_calls = HookLog::off_calls = 0;
  bridge.set_blocking_hook(&HookLog::hook);
  NEED(swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  swap.begin();
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(bridge.open());
  CHECK(HookLog::on_calls == 0u);  // open() is bracketed by its caller (boot_disk_open), not by the bridge
  std::uint32_t t = 1000;
  CHECK_FALSE(bridge.follow(t));
  card.medium.present_ = false;  // a pull starts nothing: no hook
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(HookLog::on_calls == 0u);
  CHECK(HookLog::off_calls == 0u);

  t += 2000;
  card.medium.present_ = true;  // put in: on before the start, off after it
  CHECK_FALSE(bridge.follow(t));
  const unsigned starts = card.medium.starts;
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(HookLog::on_calls == 1u);
  CHECK(HookLog::off_calls == 1u);
  CHECK(HookLog::starts_at_on == starts);
  CHECK(HookLog::starts_at_off == starts + 1u);
  CHECK(bridge.opened());

  card.medium.present_ = false;  // a card that does not start is bracketed too (its start is what blocks)
  t += 2000;
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  t += 2000;
  card.medium.present_ = true;
  card.medium.fail_start = true;
  CHECK_FALSE(bridge.follow(t));
  CHECK(bridge.follow(t + st::PinDebounce::kSettleMs));
  CHECK(HookLog::on_calls == 2u);
  CHECK(HookLog::off_calls == 2u);
  CHECK_FALSE(bridge.opened());
}

// ---- The writable step ---------------------------------------------------------------------------------------------------------------------
namespace {

// The smallest computer: one SCSI command at a time over the engine, packet by packet (usb_msc_test.cpp has the full host simulator over a RAM store; this one drives the real bridge over a FAT32
// image, so that what the computer wrote can be mounted by FatFs afterwards).
constexpr std::uint32_t kMps = 64;
struct Reply {
  bool done = false;           // a CSW came back
  std::uint8_t status = 0xFF;  // bCSWStatus
  std::vector<std::uint8_t> data;
};

class Pc {
 public:
  explicit Pc(msc::Engine& e) : eng_(e) {}

  Reply run(const std::vector<std::uint8_t>& cdb, std::uint32_t hlen, bool host_in, const std::vector<std::uint8_t>& out = {}) {
    Reply r;
    std::uint8_t c[msc::kCbwSize] = {};
    std::memcpy(c, "USBC", 4);
    ++tag_;
    for (unsigned i = 0; i < 4; ++i) {
      c[4 + i] = static_cast<std::uint8_t>(tag_ >> (8 * i));
      c[8 + i] = static_cast<std::uint8_t>(hlen >> (8 * i));
    }
    c[12] = host_in ? 0x80 : 0x00;
    c[14] = static_cast<std::uint8_t>(cdb.size());
    std::memcpy(c + 15, cdb.data(), cdb.size());
    if (!eng_.can_accept_out(kMps)) {
      return r;
    }
    eng_.out_packet(c, msc::kCbwSize);
    eng_.service();  // decodes the CBW
    std::uint8_t buf[msc::kMaxPacket];
    if (hlen != 0 && host_in) {
      for (int guard = 0; guard < 100000 && r.data.size() < hlen; ++guard) {
        const msc::InPacket p = eng_.in_packet(buf, kMps);
        if (p.kind == msc::InPacket::Kind::Stall) {
          break;
        }
        if (p.kind == msc::InPacket::Kind::Nak) {
          eng_.service();
          continue;
        }
        r.data.insert(r.data.end(), buf, buf + p.len);
        if (p.len < kMps) {
          break;
        }
      }
    } else if (hlen != 0) {
      std::uint32_t sent = 0;
      for (int guard = 0; guard < 100000 && sent < hlen && !eng_.out_stalled(); ++guard) {
        if (eng_.can_accept_out(kMps)) {
          const std::uint32_t n = hlen - sent < kMps ? hlen - sent : kMps;
          for (std::uint32_t i = 0; i < n; ++i) {
            buf[i] = sent + i < out.size() ? out[sent + i] : 0xA5;
          }
          eng_.out_packet(buf, n);
          sent += n;
        } else {
          eng_.service();
        }
      }
      for (int i = 0; i < 16; ++i) {
        eng_.service();
      }
    }
    for (int guard = 0; guard < 100000 && !r.done; ++guard) {
      if (eng_.out_stalled()) {
        eng_.clear_halt_out();
      }
      if (eng_.in_stalled()) {
        eng_.clear_halt_in();
      }
      const msc::InPacket p = eng_.in_packet(buf, kMps);
      if (p.kind == msc::InPacket::Kind::Nak) {
        eng_.service();
      } else if (p.kind == msc::InPacket::Kind::Packet) {
        r.done = p.len == msc::kCswSize && buf[0] == 'U' && buf[1] == 'S' && buf[2] == 'B' && buf[3] == 'S';
        r.status = buf[12];
      }
    }
    return r;
  }

  // The first command after a medium change gets the unit attention; this takes it, so that the test starts from a settled unit.
  void settle() { (void)run({0x00, 0, 0, 0, 0, 0}, 0, false); }
  Reply write10(std::uint32_t lba, const std::vector<std::uint8_t>& blocks) {
    const auto n = static_cast<std::uint16_t>(blocks.size() / 512u);
    return run({0x2A, 0, static_cast<std::uint8_t>(lba >> 24), static_cast<std::uint8_t>(lba >> 16), static_cast<std::uint8_t>(lba >> 8), static_cast<std::uint8_t>(lba), 0,
                static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n), 0},
               static_cast<std::uint32_t>(blocks.size()), false, blocks);
  }
  Reply read10(std::uint32_t lba, std::uint16_t n) {
    return run({0x28, 0, static_cast<std::uint8_t>(lba >> 24), static_cast<std::uint8_t>(lba >> 16), static_cast<std::uint8_t>(lba >> 8), static_cast<std::uint8_t>(lba), 0,
                static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n), 0},
               static_cast<std::uint32_t>(n) * 512u, true);
  }
  // MODE SENSE(6), all pages: the device-specific parameter's write-protect bit (0x80).
  bool write_protect_bit() {
    const Reply r = run({0x1A, 0, 0x3F, 0, 4, 0}, 4, true);
    return r.data.size() == 4 && (r.data[2] & 0x80u) != 0;
  }

 private:
  msc::Engine& eng_;
  std::uint32_t tag_ = 0x100;
};

// README.TXT is the first file the builder adds: cluster 3, one sector.
constexpr std::uint32_t kReadmeLba = Fat32Builder::kReserved + 2u * (((65'600u + 2u) * 4u + 511u) / 512u) + 1u;

std::vector<std::uint8_t> pattern(unsigned salt, std::size_t blocks = 1) {
  std::vector<std::uint8_t> v(blocks * 512u);
  for (std::size_t i = 0; i < v.size(); ++i) {
    v[i] = static_cast<std::uint8_t>(i * 3u + salt);
  }
  return v;
}

// A card in the computer's hands: mounted by the Tracker, then handed over and opened by the bridge.
struct Session {
  Card card;
  st::FatStorage fat{card.medium};
  st::CardSwap swap{fat, card.medium};
  MscBridge bridge{card.medium, swap};
  msc::Engine eng{bridge};
  bool up = false;
  bool hand_over(bool writable) {
    if (swap.reacquire() != o3sdk::hal::storage::Status::Ok) {
      return false;
    }
    swap.begin();
    if (swap.hand_to_host() != st::CardSwap::HandOver::Ok) {
      return false;
    }
    bridge.set_writable(writable);
    up = bridge.open();
    eng.medium_changed();
    return up;
  }
};

}  // namespace

TEST_CASE("msc bridge, writable: the computer's WRITE(10) reaches the card, the write-protect bit follows the policy, and the Tracker mounts what the computer wrote after the eject") {
  Session s;
  NEED(s.hand_over(true));
  Pc pc(s.eng);
  pc.settle();
  std::uint8_t sector[512] = {};
  NEED(s.card.medium.read(kReadmeLba, 1, sector) == st::BlockResult::Ok);  // (the layout assumption of kReadmeLba: the file's data sector)
  NEED(sector[0] == 0x5Au);
  CHECK_FALSE(pc.write_protect_bit());  // exported writable

  const std::vector<std::uint8_t> data = pattern(1, 2);
  const std::uint64_t written = s.card.medium.blocks_written;
  const Reply w = pc.write10(kReadmeLba, data);
  CHECK(w.done);
  CHECK(w.status == 0);  // Passed
  CHECK(s.card.medium.blocks_written == written + 2u);
  CHECK(s.card.medium.written_lbas.size() >= 2u);
  CHECK(s.card.medium.written_lbas[s.card.medium.written_lbas.size() - 2u] == kReadmeLba);
  CHECK(s.card.medium.written_lbas.back() == kReadmeLba + 1u);
  const Reply back = pc.read10(kReadmeLba, 2);
  CHECK(back.status == 0);
  CHECK(back.data == data);

  // The policy flips (a READ ONLY row of the running mode, U2a): the bit shows at once and the next WRITE is refused with DATA PROTECT, writing nothing.
  s.bridge.set_writable(false);
  CHECK(pc.write_protect_bit());
  const std::uint64_t print = s.card.medium.fingerprint();
  const std::uint64_t before = s.card.medium.blocks_written;
  const Reply denied = pc.write10(kReadmeLba, pattern(9));
  CHECK(denied.status == 1);
  CHECK(s.eng.sense() == msc::sense::kDataProtect);
  CHECK(s.card.medium.blocks_written == before);
  CHECK(s.card.medium.fingerprint() == print);
  CHECK(pc.read10(kReadmeLba, 1).status == 0);  // a protected disk still reads
  s.bridge.set_writable(true);
  CHECK_FALSE(pc.write_protect_bit());

  // The eject: the owner comes back (take_back() mounts the card again, FatFs forgot the volume at the hand-over) and sees the computer's bytes, not a stale window.
  NEED(s.swap.take_back() == o3sdk::hal::storage::Status::Ok);
  s.bridge.close();
  o3sdk::hal::storage::FileHandle file{};
  NEED(s.fat.open("README.TXT", file) == o3sdk::hal::storage::Status::Ok);
  std::uint8_t got[16] = {};
  std::size_t n = 0;
  CHECK(s.fat.read(file, got, sizeof got, n) == o3sdk::hal::storage::Status::Ok);
  CHECK(n == sizeof got);
  CHECK(std::memcmp(got, data.data(), sizeof got) == 0);
}

TEST_CASE("msc bridge, writable: a card that write-protects itself is exported protected whatever the policy says") {
  Session s;
  s.card.medium.protect = true;
  NEED(s.hand_over(true));
  Pc pc(s.eng);
  pc.settle();
  CHECK(s.bridge.writable());
  CHECK(s.bridge.write_protected());
  CHECK(pc.write_protect_bit());
  const std::uint64_t before = s.card.medium.blocks_written;
  CHECK(pc.write10(kReadmeLba, pattern(2)).status == 1);
  CHECK(s.eng.sense() == msc::sense::kDataProtect);
  CHECK(s.card.medium.blocks_written == before);
}

TEST_CASE("msc bridge, writable: a WRITE while the owner is not Host is NOT READY, MEDIUM NOT PRESENT and touches no block, before the hand-over and after the eject") {
  Session s;
  NEED(s.swap.reacquire() == o3sdk::hal::storage::Status::Ok);  // owner Tracker: the card is FatFs's
  s.swap.begin();
  s.bridge.set_writable(true);
  s.eng.medium_changed();
  Pc pc(s.eng);
  pc.settle();
  const std::uint64_t print = s.card.medium.fingerprint();
  const std::uint64_t written = s.card.medium.blocks_written;
  const std::uint64_t read = s.card.medium.blocks_read;
  const Reply w = pc.write10(kReadmeLba, pattern(4));
  CHECK(w.done);
  CHECK(w.status == 1);
  CHECK(s.eng.sense() == msc::sense::kNoMedium);
  CHECK(s.card.medium.blocks_written == written);
  CHECK(s.card.medium.blocks_read == read);
  CHECK(s.card.medium.fingerprint() == print);

  // A session, then the eject: the computer's next WRITE gets the unit attention and then NOT READY, and the Tracker's volume is not written under.
  NEED(s.swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  NEED(s.bridge.open());
  s.eng.medium_changed();
  pc.settle();
  CHECK(pc.write10(kReadmeLba, pattern(5)).status == 0);
  NEED(s.swap.take_back() == o3sdk::hal::storage::Status::Ok);
  const std::uint64_t after_eject = s.card.medium.blocks_written;
  const std::uint64_t print_back = s.card.medium.fingerprint();
  CHECK(pc.write10(kReadmeLba, pattern(6)).status == 1);  // the unit attention of the medium that went
  CHECK(pc.write10(kReadmeLba, pattern(6)).status == 1);
  CHECK(s.eng.sense() == msc::sense::kNoMedium);
  CHECK(s.card.medium.blocks_written == after_eject);
  CHECK(s.card.medium.fingerprint() == print_back);
}

TEST_CASE("msc bridge, writable: while the card is the computer's the Tracker's own writers (settings, recents, a save) find no card and write no block") {
  Session s;
  namespace sv = o3sdk::platform::mk66::save;
  sv::FatStorageWriter writer(s.fat, s.card.medium);
  NEED(s.swap.reacquire() == o3sdk::hal::storage::Status::Ok);
  s.swap.begin();
  CHECK(writer.gate() == o3sdk::hal::storage::WriteStatus::Ok);  // the Tracker's card: the writer works
  NEED(s.swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  s.bridge.set_writable(true);
  NEED(s.bridge.open());
  const std::uint64_t written = s.card.medium.blocks_written;
  const std::uint64_t read = s.card.medium.blocks_read;
  CHECK(writer.gate() == o3sdk::hal::storage::WriteStatus::NoCard);
  CHECK(writer.make_dir("Projects/FromTheTracker") == o3sdk::hal::storage::WriteStatus::NoCard);
  CHECK(writer.create("Projects/FromTheTracker/_o3save.tmp") == o3sdk::hal::storage::WriteStatus::NoCard);
  CHECK(s.card.medium.blocks_written == written);
  CHECK(s.card.medium.blocks_read == read);  // not even a read: FatFs has no volume to walk
  // And the computer's writes go through the bridge meanwhile.
  msc::Engine eng(s.bridge);
  eng.medium_changed();
  Pc pc(eng);
  pc.settle();
  CHECK(pc.write10(kReadmeLba, pattern(7)).status == 0);
  CHECK(s.card.medium.blocks_written == written + 1u);
}

TEST_CASE("msc bridge, writable: a card pulled in the middle of a WRITE ends the command, and the card put in meanwhile gets none of its blocks") {
  Session s;
  NEED(s.hand_over(true));
  Pc pc(s.eng);
  pc.settle();
  // A WRITE(10) of 4 blocks, 600 bytes in: block 0 is on the card, block 1 half there.
  std::uint8_t c[msc::kCbwSize] = {};
  std::memcpy(c, "USBC", 4);
  c[4] = 0x55;
  c[8] = 0x00;
  c[9] = 0x08;  // 2048 bytes
  c[14] = 10;
  c[15] = 0x2A;
  c[19] = static_cast<std::uint8_t>(kReadmeLba >> 8);  // CDB bytes 2..5: the LBA, big-endian; 7..8: the length in blocks
  c[20] = static_cast<std::uint8_t>(kReadmeLba);
  c[23] = 4;
  NEED(s.eng.can_accept_out(kMps));
  s.eng.out_packet(c, msc::kCbwSize);
  s.eng.service();
  std::uint8_t buf[kMps];
  std::memset(buf, 0x66, sizeof buf);
  for (unsigned sent = 0; sent < 600; sent += kMps) {
    NEED(s.eng.can_accept_out(kMps));
    s.eng.out_packet(buf, kMps);
  }
  s.eng.service();
  const std::uint64_t written = s.card.medium.blocks_written;
  CHECK(written >= 1u);

  // The slot: pulled for good, then another card (here the same image, a different medium to the engine) put in. The platform's disk_pass does exactly this: follow() then medium_changed().
  std::uint32_t t = 1000;
  s.card.medium.present_ = false;
  CHECK_FALSE(s.bridge.follow(t));
  NEED(s.bridge.follow(t + st::PinDebounce::kSettleMs));
  s.eng.medium_changed();
  s.card.medium.present_ = true;
  t += 3000;
  CHECK_FALSE(s.bridge.follow(t));
  NEED(s.bridge.follow(t + st::PinDebounce::kSettleMs));
  s.eng.medium_changed();
  CHECK(s.bridge.opened());
  for (int i = 0; i < 32; ++i) {  // the host goes on sending the rest of the command; whatever the engine does with it, no block of this command reaches the new card
    if (s.eng.out_stalled()) {
      s.eng.clear_halt_out();
    }
    if (s.eng.can_accept_out(kMps)) {
      s.eng.out_packet(buf, kMps);
    }
    s.eng.service();
  }
  CHECK(s.card.medium.blocks_written == written);
}
