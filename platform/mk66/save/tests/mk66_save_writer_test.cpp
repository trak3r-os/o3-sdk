// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FatFs-backed StorageWriter of the save layer (save/fat_storage_writer.h, OS-007n) on a FAT32 volume built in the test (mk66_storage_fakes.h): the gate (not
// mounted, write protected, exFAT; a card with no marker file is written like any other, OS-ANYCARD), every call the path guard refuses writing nothing at all (the medium's
// block counter does not move), folders made and made again, a temp file created, written, flushed and renamed, a name that exists never replaced or truncated, removal
// of our own temp and backup files only, the refusal to remove a file whose first cluster another name uses (a cut inside a rename), and the tree outside the guarded
// folders byte-identical after all of it. Runs on the host and on QEMU's Cortex-M4F (the volume is sparse).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3sdk::hal::storage;
namespace st = o3sdk::platform::mk66::storage;
namespace sv = o3sdk::platform::mk66::save;
using fakes::Fat32Builder;
using fakes::Medium;
using hs::WriteStatus;

namespace {

std::vector<std::uint8_t> bytes(std::size_t n, unsigned salt) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 31u + 7u + salt) & 0xFFu);
  }
  return v;
}

constexpr std::uint32_t kDataStart = Fat32Builder::kReserved + 2u * (((65'600u + 2u) * 4u + 511u) / 512u);  // the builder's first data sector (one-sector clusters)

// The card: a README, a project folder of the OG's with files of the user's, a Samples folder with one WAV, Firmware/ with an image.
struct Card {
  Medium medium{200'000};
  std::uint32_t samples = 0;
  Card() {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", bytes(40, 1));
    const std::uint32_t projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
    const std::uint32_t song = fat.make_dir(projects, "Song");
    fat.add_file(song, "project.mt", bytes(2320, 10));
    const std::uint32_t patterns = fat.make_dir(song, "patterns");
    fat.add_file(patterns, "pattern_01.mtp", bytes(1000, 11));
    samples = fat.make_dir(Fat32Builder::kRootCluster, "Samples");
    fat.add_file(samples, "A.WAV", bytes(700, 12));
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

bool exists(hs::Storage& s, const char* path) { return s.exists(path, nullptr) == hs::Status::Ok; }

// FNV-1a over every file and folder name and every byte of every file, outside the folders `skip1` .. `skip3` of the root.
void hash_tree(hs::Storage& s, const std::string& dir, std::uint64_t& h) {
  hs::DirHandle d;
  if (s.opendir(dir.c_str(), d) != hs::Status::Ok) {
    return;
  }
  std::vector<hs::DirEntry> entries;
  hs::DirEntry e;
  while (entries.size() < 500 && s.readdir(d, e) == hs::Status::Ok) {
    entries.push_back(e);
  }
  s.closedir(d);
  for (const hs::DirEntry& x : entries) {
    if (dir.empty() && (std::strcmp(x.name, "Projects") == 0 || std::strcmp(x.name, "Samples") == 0 || std::strcmp(x.name, "Settings") == 0)) {
      continue;  // the guarded folders: what is inside them is checked file by file
    }
    const std::string path = dir.empty() ? std::string(x.name) : dir + "/" + x.name;
    for (const char* p = path.c_str(); *p != '\0'; ++p) {
      h = (h ^ static_cast<std::uint8_t>(*p)) * 1099511628211ull;
    }
    if (x.is_dir) {
      hash_tree(s, path, h);
    } else {
      for (const std::uint8_t b : read_file(s, path.c_str())) {
        h = (h ^ b) * 1099511628211ull;
      }
    }
  }
}

std::uint64_t outside(hs::Storage& s) {
  std::uint64_t h = 1469598103934665603ull;
  hash_tree(s, "", h);
  return h;
}

// A whole temp file: create, one write, finish.
WriteStatus put_temp(hs::StorageWriter& w, const char* path, const std::vector<std::uint8_t>& data) {
  WriteStatus r = w.create(path);
  if (r != WriteStatus::Ok) {
    return r;
  }
  r = w.write(data.data(), data.size());
  const WriteStatus f = w.finish();
  return r != WriteStatus::Ok ? r : f;
}

}  // namespace

TEST_CASE("save writer: the gate says what is missing, in order, and a refused call writes nothing") {
  {
    Card card;
    st::FatStorage fat(card.medium);
    sv::FatStorageWriter w(fat, card.medium);
    CHECK(w.gate() == WriteStatus::NoCard);  // not mounted
    NEED(fat.mount() == hs::Status::Ok);
    CHECK(w.gate() == WriteStatus::Ok);
    card.medium.protect = true;
    CHECK(w.gate() == WriteStatus::WriteProtected);
    CHECK(w.make_dir("Settings") == WriteStatus::WriteProtected);
    CHECK(card.medium.blocks_written == 0u);
  }
  {  // a card with no marker file is written like any other (OS-ANYCARD)
    Card card;
    st::FatStorage fat(card.medium);
    sv::FatStorageWriter w(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    CHECK_FALSE(exists(fat, ".o3-test-card"));
    CHECK(w.gate() == WriteStatus::Ok);
    CHECK(w.make_dir("Settings") == WriteStatus::Ok);
    CHECK(exists(fat, "Settings"));
  }
  {  // a card pulled between two chunks: the next write says so
    Card card;
    st::FatStorage fat(card.medium);
    sv::FatStorageWriter w(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    NEED(w.make_dir("Settings") == WriteStatus::Ok);
    NEED(w.create("Settings/_o3save.tmp") == WriteStatus::Ok);
    const std::vector<std::uint8_t> chunk = bytes(1000, 4);
    CHECK(w.write(chunk.data(), chunk.size()) == WriteStatus::Ok);
    card.medium.present_ = false;
    CHECK(w.write(chunk.data(), chunk.size()) == WriteStatus::NoCard);
    w.abandon();
    CHECK_FALSE(w.writing());
  }
}

TEST_CASE("save writer: a card swapped in the middle of a save (another volume serial) stops it (OS-004d)") {
  Card card;
  st::FatStorage fat(card.medium);
  sv::FatStorageWriter w(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  NEED(w.make_dir("Settings") == WriteStatus::Ok);  // the save begins: this is its card
  NEED(w.create("Settings/_o3save.tmp") == WriteStatus::Ok);
  const std::vector<std::uint8_t> chunk = bytes(1000, 4);
  CHECK(w.write(chunk.data(), chunk.size()) == WriteStatus::Ok);
  const std::uint32_t serial = fat.volume_serial();
  NEED(serial != 0u);
  // another card, mounted before the next chunk: its boot sector says another serial
  for (std::uint32_t lba = 0; lba < 64; ++lba) {
    std::uint8_t* sec = card.medium.sector(lba);
    const std::uint32_t at67 = static_cast<std::uint32_t>(sec[67]) | (static_cast<std::uint32_t>(sec[68]) << 8) | (static_cast<std::uint32_t>(sec[69]) << 16) | (static_cast<std::uint32_t>(sec[70]) << 24);
    if (at67 == serial && sec[510] == 0x55 && sec[511] == 0xAA) {
      sec[67] ^= 0x5A;
      break;
    }
  }
  NEED(fat.mount() == hs::Status::Ok);
  NEED(fat.volume_serial() != serial);
  CHECK(w.gate() == WriteStatus::Ok);  // the card itself is fine: a new save may begin on it
  CHECK(w.write(chunk.data(), chunk.size()) == WriteStatus::NoCard);
  w.abandon();
  CHECK(w.create("Settings/_o3save.tmp") == WriteStatus::NoCard);
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::NoCard);
  CHECK(w.remove("Settings/_o3save.tmp") == WriteStatus::NoCard);
  // the save ends (the service says so) and the next one begins with a make_dir, which pins the new card (begin_save is the explicit form: the next case)
  CHECK(w.make_dir("Settings") == WriteStatus::NoCard);  // not before: make_dir is a call of the save that pinned the first card
  w.end_save();
  CHECK(w.make_dir("Settings") == WriteStatus::Ok);
  CHECK(w.remove("Settings/_o3save.tmp") == WriteStatus::Ok);  // the cut save's temp, on the card it began on
  CHECK(w.create("Settings/_o3save.tmp") == WriteStatus::Ok);
  w.abandon();
}

// Flips the volume serial in the card's boot sector: another card, mounted again by the caller.
void swap_card(Card& card, std::uint32_t serial) {
  for (std::uint32_t lba = 0; lba < 64; ++lba) {
    std::uint8_t* sec = card.medium.sector(lba);
    const std::uint32_t at67 = static_cast<std::uint32_t>(sec[67]) | (static_cast<std::uint32_t>(sec[68]) << 8) | (static_cast<std::uint32_t>(sec[69]) << 16) | (static_cast<std::uint32_t>(sec[70]) << 24);
    if (at67 == serial && sec[510] == 0x55 && sec[511] == 0xAA) {
      sec[67] ^= 0x5A;
      break;
    }
  }
}

TEST_CASE("save writer: a card swapped between two jobs of one save refuses the whole save, folders included (OS-FU5)") {
  Card card;
  st::FatStorage fat(card.medium);
  sv::FatStorageWriter w(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint32_t serial = fat.volume_serial();
  NEED(serial != 0u);
  CHECK(w.card_id() == serial);
  // the save: one begin, a job per sample, the project file last
  w.begin_save();
  NEED(w.make_dir("Projects") == WriteStatus::Ok);
  NEED(w.make_dir("Projects/Song") == WriteStatus::Ok);
  NEED(w.make_dir("Projects/Song/samples") == WriteStatus::Ok);
  const std::vector<std::uint8_t> data = bytes(600, 5);
  NEED(put_temp(w, "Projects/Song/samples/_o3save.tmp", data) == WriteStatus::Ok);
  NEED(w.rename("Projects/Song/samples/_o3save.tmp", "Projects/Song/samples/A.wav") == WriteStatus::Ok);
  // another card between the jobs: the second job's first call is a make_dir (the AtomicSave of the project file), which used to re-pin the new card
  swap_card(card, serial);
  NEED(fat.mount() == hs::Status::Ok);
  NEED(fat.volume_serial() != serial);
  CHECK(w.card_id() != serial);
  CHECK(w.gate() == WriteStatus::Ok);  // the card itself is fine
  CHECK(w.make_dir("Projects") == WriteStatus::NoCard);
  CHECK(w.make_dir("Projects/Song") == WriteStatus::NoCard);
  CHECK(w.create("Projects/Song/_o3save.tmp") == WriteStatus::NoCard);
  CHECK_FALSE(exists(fat, "Projects/Song/_o3save.tmp"));
  // the save ended (it failed): the next one pins the card now in
  w.end_save();
  w.begin_save();
  CHECK(w.make_dir("Projects/Song") == WriteStatus::Ok);
  CHECK(put_temp(w, "Projects/Song/_o3save.tmp", data) == WriteStatus::Ok);
  w.end_save();
  // with nothing pinned a call is judged by the gate alone (a boot-time recovery)
  CHECK(w.remove("Projects/Song/_o3save.tmp") == WriteStatus::Ok);
}

TEST_CASE("save writer: exFAT is refused") {
  Medium medium{200'000};
  fakes::ExFatBuilder fat(medium);
  fat.add_file(fakes::ExFatBuilder::kRootCluster, "README.TXT", bytes(1, 3), true);
  st::FatStorage storage(medium);
  sv::FatStorageWriter w(storage, medium);
  NEED(storage.mount() == hs::Status::Ok);
  CHECK(storage.volume().kind == hs::FsKind::ExFat);
  const std::uint64_t before = medium.blocks_written;
  CHECK(w.gate() == WriteStatus::Unsupported);
  CHECK(w.make_dir("Settings") == WriteStatus::Unsupported);
  CHECK(w.create("Samples/_o3save.tmp") == WriteStatus::Unsupported);
  CHECK(medium.blocks_written == before);
}

TEST_CASE("save writer: every call the path guard refuses writes nothing") {
  Card card;
  st::FatStorage fat(card.medium);
  sv::FatStorageWriter w(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t before = card.medium.blocks_written;
  const std::uint64_t tree = outside(fat);
  // make_dir
  CHECK(w.make_dir("Firmware") == WriteStatus::BadPath);
  CHECK(w.make_dir("Foo") == WriteStatus::BadPath);
  CHECK(w.make_dir("Projects/Song/patterns") == WriteStatus::BadPath);
  CHECK(w.make_dir("Projects/../Evil") == WriteStatus::BadPath);
  CHECK(w.make_dir("/Projects") == WriteStatus::BadPath);
  CHECK(w.make_dir(nullptr) == WriteStatus::BadPath);
  // create: a Temp only
  CHECK(w.create("README.TXT") == WriteStatus::BadPath);
  CHECK(w.create("Projects/Song/project.o3pj") == WriteStatus::BadPath);
  CHECK(w.create("Samples/A.WAV") == WriteStatus::BadPath);
  CHECK(w.create("Samples/new.wav") == WriteStatus::BadPath);
  CHECK(w.create("Firmware/_o3up.tmp") == WriteStatus::BadPath);
  CHECK(w.create("_o3save.tmp") == WriteStatus::BadPath);
  CHECK_FALSE(w.writing());
  // remove: a Temp or a Backup only
  CHECK(w.remove("README.TXT") == WriteStatus::BadPath);
  CHECK(w.remove("Samples/A.WAV") == WriteStatus::BadPath);
  CHECK(w.remove("Projects/Song/project.mt") == WriteStatus::BadPath);
  CHECK(w.remove("Projects/Song/patterns/pattern_01.mtp") == WriteStatus::BadPath);
  CHECK(w.remove("Projects/Song") == WriteStatus::BadPath);
  CHECK(w.remove("Projects") == WriteStatus::BadPath);
  CHECK(w.remove("Firmware/PolyendTracker_1.9.0.ptf") == WriteStatus::BadPath);
  CHECK(w.remove("README.TXT") == WriteStatus::BadPath);
  // rename: the three pairs only
  CHECK(w.rename("Samples/A.WAV", "Samples/B.WAV") == WriteStatus::BadPath);
  CHECK(w.rename("Samples/A.WAV", "Samples/A.WAV.bak") == WriteStatus::BadPath);
  CHECK(w.rename("Projects/Song/project.mt", "Projects/Song/project.o3pj") == WriteStatus::BadPath);
  CHECK(w.rename("Projects/Song", "Projects/Song2") == WriteStatus::BadPath);  // (OS-FILES part B dropped the folder rename: a folder is never renamed)
  CHECK(w.rename("Projects/Song", "Samples/Song2") == WriteStatus::BadPath);
  CHECK(w.rename("README.TXT", "Samples/README.wav") == WriteStatus::BadPath);
  CHECK(card.medium.blocks_written == before);
  CHECK(outside(fat) == tree);
  CHECK(read_file(fat, "Samples/A.WAV") == bytes(700, 12));
  CHECK(read_file(fat, "Projects/Song/project.mt") == bytes(2320, 10));
}

TEST_CASE("save writer: folders, a temp file, a rename, a removal") {
  Card card;
  st::FatStorage fat(card.medium);
  sv::FatStorageWriter w(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t tree = outside(fat);

  // folders: made, made again (Ok, nothing written the second time), the parent must exist, a file in the way is not touched
  CHECK(w.make_dir("Settings") == WriteStatus::Ok);
  CHECK(exists(fat, "Settings"));
  const std::uint64_t after_first = card.medium.blocks_written;
  CHECK(w.make_dir("Settings") == WriteStatus::Ok);
  CHECK(w.make_dir("Samples") == WriteStatus::Ok);  // the user's own folder, already there
  CHECK(card.medium.blocks_written == after_first);
  CHECK(w.make_dir("Projects/New Song") == WriteStatus::Ok);
  CHECK(w.make_dir("settings") == WriteStatus::Ok);  // FAT folds case: the same folder

  // a temp file: create, write in pieces, finish; it reads back whole
  const std::vector<std::uint8_t> data = bytes(5000, 5);
  NEED(w.create("Settings/_o3save.tmp") == WriteStatus::Ok);
  CHECK(w.writing());
  CHECK(w.create("Samples/_o3save.tmp") == WriteStatus::Busy);  // one file at a time
  CHECK(w.remove("Settings/_o3save.tmp") == WriteStatus::Busy);  // and nothing is removed while it is open
  for (std::size_t at = 0; at < data.size(); at += 1100) {
    CHECK(w.write(data.data() + at, data.size() - at < 1100 ? data.size() - at : 1100) == WriteStatus::Ok);
  }
  CHECK(w.finish() == WriteStatus::Ok);
  CHECK_FALSE(w.writing());
  CHECK(w.write(data.data(), 10) == WriteStatus::NotFound);  // nothing is open
  CHECK(read_file(fat, "Settings/_o3save.tmp") == data);

  // an existing file is never truncated: create says Exists and the bytes are as they were
  CHECK(w.create("Settings/_o3save.tmp") == WriteStatus::Exists);
  CHECK_FALSE(w.writing());
  CHECK(read_file(fat, "Settings/_o3save.tmp") == data);

  // rename: Temp -> Target; a target that exists is not replaced; a source that is not there
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::Ok);
  CHECK_FALSE(exists(fat, "Settings/_o3save.tmp"));
  CHECK(read_file(fat, "Settings/device.o3st") == data);
  const std::vector<std::uint8_t> other = bytes(900, 6);
  NEED(put_temp(w, "Settings/_o3save.tmp", other) == WriteStatus::Ok);
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::Exists);
  CHECK(read_file(fat, "Settings/device.o3st") == data);  // nothing replaced
  CHECK(read_file(fat, "Settings/_o3save.tmp") == other);
  CHECK(w.rename("Settings/device.o3st.bak", "Settings/device.o3st") == WriteStatus::NotFound);  // (no backup yet)
  CHECK(w.rename("Settings/device.o3st", "Settings/device.o3st.bak") == WriteStatus::Ok);
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::Ok);
  CHECK(read_file(fat, "Settings/device.o3st") == other);
  CHECK(read_file(fat, "Settings/device.o3st.bak") == data);
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::NotFound);

  // remove: a missing file is Ok, a backup and a temp go, a target and a folder do not
  CHECK(w.remove("Settings/_o3save.tmp") == WriteStatus::Ok);
  CHECK(w.remove("Settings/device.o3st.bak") == WriteStatus::Ok);
  CHECK_FALSE(exists(fat, "Settings/device.o3st.bak"));
  CHECK(w.remove("Settings/device.o3st") == WriteStatus::BadPath);
  CHECK(w.remove("Settings") == WriteStatus::BadPath);
  CHECK(read_file(fat, "Settings/device.o3st") == other);

  // the user's files and everything outside the guarded folders are what they were
  CHECK(outside(fat) == tree);
  CHECK(read_file(fat, "Samples/A.WAV") == bytes(700, 12));
  CHECK(read_file(fat, "Projects/Song/project.mt") == bytes(2320, 10));
  CHECK(read_file(fat, "Projects/Song/patterns/pattern_01.mtp") == bytes(1000, 11));
}

TEST_CASE("save writer: a folder needs its parent") {
  Medium medium{200'000};
  Fat32Builder builder(medium);
  st::FatStorage fat(medium);
  sv::FatStorageWriter w(fat, medium);
  NEED(fat.mount() == hs::Status::Ok);
  CHECK(w.make_dir("Projects/Song") == WriteStatus::NotFound);
  CHECK(w.create("Samples/_o3save.tmp") == WriteStatus::NotFound);
  CHECK(w.make_dir("Projects") == WriteStatus::Ok);
  CHECK(w.make_dir("Projects/Song") == WriteStatus::Ok);
}

TEST_CASE("save writer: a full volume says NoSpace and the temp can be removed") {
  Medium medium{200'000};
  Fat32Builder builder(medium);
  builder.make_dir(Fat32Builder::kRootCluster, "Samples");
  builder.consume_clusters(20);  // 20 sectors left
  st::FatStorage fat(medium);
  sv::FatStorageWriter w(fat, medium);
  NEED(fat.mount() == hs::Status::Ok);
  NEED(w.create("Samples/_o3save.tmp") == WriteStatus::Ok);
  const std::vector<std::uint8_t> chunk = bytes(4096, 7);
  WriteStatus r = WriteStatus::Ok;
  for (int i = 0; i < 20 && r == WriteStatus::Ok; ++i) {
    r = w.write(chunk.data(), chunk.size());
  }
  CHECK(r == WriteStatus::NoSpace);
  w.abandon();
  CHECK(w.remove("Samples/_o3save.tmp") == WriteStatus::Ok);
  CHECK_FALSE(exists(fat, "Samples/_o3save.tmp"));
}

TEST_CASE("save writer: a file whose first cluster another name uses is not removed (a cut inside a rename)") {
  // (the scopes keep two FatFs stacks at most alive at once: the QEMU suite runs on a small stack)
  Card card;
  {
    st::FatStorage fat(card.medium);
    sv::FatStorageWriter w(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    NEED(put_temp(w, "Samples/_o3save.tmp", bytes(10, 8)) == WriteStatus::Ok);
  }
  // The state a cut inside f_rename leaves: two directory entries on one chain. Made by hand: the entry of "_o3save.tmp" is given the first cluster and the size of A.WAV.
  std::uint8_t* dir = card.medium.sector(kDataStart + card.samples - 2u);
  std::uint8_t* a = nullptr;
  std::uint8_t* t = nullptr;
  for (unsigned i = 0; i < 16; ++i) {
    std::uint8_t* e = dir + 32u * i;
    if (std::memcmp(e, "A       WAV", 11) == 0) {
      a = e;
    }
    if (std::memcmp(e, "_O3SAVE TMP", 11) == 0) {
      t = e;
    }
  }
  NEED(a != nullptr);
  NEED(t != nullptr);
  std::memcpy(t + 20, a + 20, 2);  // first cluster, high word
  std::memcpy(t + 26, a + 26, 2);  // low word
  std::memcpy(t + 28, a + 28, 4);  // size
  {  // (a fresh mount reads the sector from the medium)
    st::FatStorage fat(card.medium);
    sv::FatStorageWriter w(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    CHECK(read_file(fat, "Samples/_o3save.tmp") == bytes(700, 12));  // both names read the same chain
    const std::uint64_t before = card.medium.blocks_written;
    CHECK(w.remove("Samples/_o3save.tmp") == WriteStatus::Damaged);
    CHECK(card.medium.blocks_written == before);  // nothing was freed or changed
    CHECK(read_file(fat, "Samples/A.WAV") == bytes(700, 12));
    CHECK(read_file(fat, "Samples/_o3save.tmp") == bytes(700, 12));
  }
  // The ordinary case, a temp on a chain of its own, is removed.
  Card ok;
  st::FatStorage fat(ok.medium);
  sv::FatStorageWriter w(fat, ok.medium);
  NEED(fat.mount() == hs::Status::Ok);
  NEED(put_temp(w, "Samples/_o3save.tmp", bytes(3000, 9)) == WriteStatus::Ok);
  CHECK(w.remove("Samples/_o3save.tmp") == WriteStatus::Ok);
  CHECK_FALSE(exists(fat, "Samples/_o3save.tmp"));
  CHECK(read_file(fat, "Samples/A.WAV") == bytes(700, 12));
}

// OS-USBDISK 4.3 (U2a-3): while the computer has the card (CardOwner::Host) every mutating call of the writer answers Host and the medium's block counter does not move.
TEST_CASE("save writer: Host gate, every card path of the writer is refused while the card is the computer's, nothing is written") {
  Card card;
  st::FatStorage fat(card.medium);
  st::CardSwap swap(fat, card.medium);
  sv::FatStorageWriter w(fat, card.medium);
  w.attach_owner(swap);
  NEED(swap.reacquire() == hs::Status::Ok);
  swap.begin();
  CHECK(w.gate() == WriteStatus::Ok);
  NEED(swap.hand_to_host() == st::CardSwap::HandOver::Ok);
  const auto before = card.medium.blocks_written;
  CHECK(w.gate() == WriteStatus::Host);
  CHECK(w.make_dir("Settings") == WriteStatus::Host);
  CHECK(w.create("Settings/_o3save.tmp") == WriteStatus::Host);
  CHECK(w.create_marker("Projects/Song/.deleted") == WriteStatus::Host);
  CHECK(w.rename("Settings/_o3save.tmp", "Settings/device.o3st") == WriteStatus::Host);
  CHECK(w.remove("Settings/_o3save.tmp") == WriteStatus::Host);
  CHECK_FALSE(w.writing());
  CHECK(card.medium.blocks_written == before);
  NEED(swap.take_back() == hs::Status::Ok);  // the eject: the writer works again
  CHECK(w.gate() == WriteStatus::Ok);
  CHECK(w.make_dir("Settings") == WriteStatus::Ok);
}
