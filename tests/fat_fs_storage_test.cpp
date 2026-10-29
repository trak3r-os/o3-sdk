// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET D1: FatFsStorage, the simulator's card on a FAT32 image through the unit's own FatFs (platform/host/fat), and FatImage, the writer of the image. What these tests hold down: a folder with long names,
// spaces, non-ASCII letters and a folder of 600 entries is written into an image that the unit's FatFs mounts as FAT32 and reads back byte for byte, in the order it was written, matched without regard to case; the
// image file is sparse and is never changed by a run (a private mapping); the write side is the unit's FatStorageWriter behind the real guard (a save is a Temp, then a rename; nothing replaces); a pulled card
// answers NoCard and reads again when it is back; `card copy` on an image (the computer's copy) rebuilds the volume with the copy in it; an image that is too small or too full is refused with the reason.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fat_fs_storage.h>
#include <o3sdk/platform/host/fat_image.h>

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace hs = o3sdk::hal::storage;
using o3sdk::platform::host::FatFsStorage;
using o3sdk::platform::host::FatImage;
using o3sdk::platform::host::ScratchDir;

namespace {

std::string content(std::size_t n, unsigned salt) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 7u + salt * 13u + 1u) & 0xFFu);
  }
  return s;
}

void put(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A card of a user's kind: long names, spaces, accents, a plain 8.3 name, a lower-case 8.3 name, a hidden file, a folder of 600 files.
void fill(const fs::path& root) {
  put(root / "Projects" / "Small Song" / "project.o3pj", content(1500, 1));
  put(root / "Projects" / "Small Song" / "samples" / "kick drum 01.wav", content(5000, 2));
  put(root / "Samples" / "Café Niño über.wav", content(3000, 3));
  put(root / "Samples" / "UPPER.WAV", content(700, 4));
  put(root / "Samples" / "pattern_01.mtp", content(60, 5));
  put(root / "Samples" / "._hidden.wav", content(10, 6));
  put(root / "readme.TXT", content(40, 7));
  for (int i = 0; i < 600; ++i) {
    char name[64];
    std::snprintf(name, sizeof name, "Big Folder/break %04d long name.wav", i);
    put(root / name, content(20 + static_cast<std::size_t>(i % 7), 10u + static_cast<unsigned>(i)));
  }
}

struct Card {
  ScratchDir tree{"fatfs-tree"};
  ScratchDir out{"fatfs-img"};
  std::string image;
  FatFsStorage storage;
  bool good = false;  // (no REQUIRE in a constructor: the tests REQUIRE this)
  explicit Card(bool mount = true) {
    if (!tree.ok() || !out.ok()) {
      return;
    }
    fill(tree.path());
    FatImage img;
    std::string error;
    image = (fs::path(out.path()) / "card.img").string();
    good = img.add_tree(tree.path()) && img.write_file(image, error) && storage.open_image(image, error) && (!mount || storage.mount() == hs::Status::Ok);
    CHECK_MESSAGE(good, img.error() << " " << error);
  }
};

bool read_all(hs::Storage& card, const char* path, std::string& out) {
  hs::FileHandle f;
  if (card.open(path, f) != hs::Status::Ok) {
    return false;
  }
  out.clear();
  for (;;) {
    char buf[300];
    std::size_t got = 0;
    if (card.read(f, buf, sizeof buf, got) != hs::Status::Ok) {
      card.close(f);
      return false;
    }
    if (got == 0u) {
      break;
    }
    out.append(buf, got);
  }
  card.close(f);
  return true;
}

std::vector<std::string> list(hs::Storage& card, const char* path) {
  std::vector<std::string> names;
  hs::DirHandle d;
  if (card.opendir(path, d) != hs::Status::Ok) {
    CHECK_MESSAGE(false, "opendir " << path);
    return names;
  }
  hs::DirEntry e{};
  while (card.readdir(d, e) == hs::Status::Ok) {
    names.push_back(e.name);
  }
  card.closedir(d);
  return names;
}

}  // namespace

TEST_CASE("fat image: a card of long names mounts as FAT32 through the unit's FatFs and reads back byte for byte") {
  Card c;
  NEED(c.good);
  const hs::VolumeInfo v = c.storage.volume();
  CHECK(v.kind == hs::FsKind::Fat32);
  CHECK(v.capacity_mb >= 60u);
  CHECK(v.cluster_bytes == 512u);
  CHECK(c.storage.card_id() == 0x1234ABCDu);

  std::string got;
  NEED(read_all(c.storage, "Projects/Small Song/project.o3pj", got));
  CHECK(got == content(1500, 1));
  NEED(read_all(c.storage, "Samples/Café Niño über.wav", got));  // UTF-8 at the API, UTF-16 long-name entries on the volume
  CHECK(got == content(3000, 3));
  NEED(read_all(c.storage, "samples/UPPER.wav", got));  // matched without regard to case
  CHECK(got == content(700, 4));
  NEED(read_all(c.storage, "READme.txt", got));
  CHECK(got == content(40, 7));
  bool dir = false;
  CHECK(c.storage.exists("projects/SMALL SONG/samples", &dir) == hs::Status::Ok);
  CHECK(dir);
  CHECK(c.storage.exists("Projects/Small Song/nothing.wav", nullptr) == hs::Status::NotFound);

  hs::FileStamp stamp;
  NEED(c.storage.stat("Samples/pattern_01.mtp", stamp) == hs::Status::Ok);
  CHECK(stamp.size == 60u);
  CHECK(stamp.stamp == ((((2024u - 1980u) << 9) | (11u << 5) | 1u) << 16));  // 2024-11-01 00:00:00, the unit's FF_NORTC_*
}

TEST_CASE("fat image: a folder lists in the order it was written, 600 entries, and the root and a hidden file are there") {
  Card c;
  NEED(c.good);
  const std::vector<std::string> big = list(c.storage, "Big Folder");
  NEED(big.size() == 600u);
  CHECK(big.front() == "break 0000 long name.wav");
  CHECK(big.back() == "break 0599 long name.wav");
  for (std::size_t i = 1; i < big.size(); ++i) {
    CHECK(big[i - 1] < big[i]);
  }
  const std::vector<std::string> root = list(c.storage, "");
  NEED(root.size() == 4u);  // sorted by byte order: Big Folder, Projects, Samples, readme.TXT
  CHECK(root[0] == "Big Folder");
  CHECK(root[3] == "readme.TXT");
  const std::vector<std::string> samples = list(c.storage, "Samples");
  NEED(samples.size() == 4u);
  CHECK(samples[0] == "._hidden.wav");  // byte order of the names: '.' before the capitals before the lower case
  CHECK(samples[1] == "Café Niño über.wav");  // the long name as written, not its alias
  CHECK(samples[2] == "UPPER.WAV");
  CHECK(samples[3] == "pattern_01.mtp");
}

TEST_CASE("fat image: the image file is sparse, and a run never changes it") {
  Card c;
  NEED(c.good);
  struct stat info {};
  NEED(::stat(c.image.c_str(), &info) == 0);
  CHECK(info.st_size == 64 * 1024 * 1024);
  CHECK(static_cast<long long>(info.st_blocks) * 512 < info.st_size / 8);  // holes, not zeros
  const std::string before = slurp(c.image);

  CHECK(c.storage.make_dir("Projects/Fresh") == hs::WriteStatus::Ok);
  CHECK(c.storage.create("Projects/Fresh/_o3save.tmp") == hs::WriteStatus::Ok);
  const std::string payload = content(4000, 9);
  CHECK(c.storage.write(payload.data(), payload.size()) == hs::WriteStatus::Ok);
  CHECK(c.storage.finish() == hs::WriteStatus::Ok);
  CHECK(c.storage.rename("Projects/Fresh/_o3save.tmp", "Projects/Fresh/project.o3pj") == hs::WriteStatus::Ok);
  std::string got;
  NEED(read_all(c.storage, "Projects/Fresh/project.o3pj", got));
  CHECK(got == payload);
  CHECK(c.storage.create("Projects/Fresh/_o3save.tmp") == hs::WriteStatus::Ok);
  c.storage.abandon();
  CHECK(c.storage.rename("Projects/Fresh/_o3save.tmp", "Projects/Fresh/project.o3pj") == hs::WriteStatus::Exists);  // nothing replaces
  CHECK(c.storage.remove("Projects/Fresh/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK(c.storage.make_dir("Elsewhere") == hs::WriteStatus::BadPath);  // the real guard
  CHECK(c.storage.create("Projects/Small Song/project.o3pj") == hs::WriteStatus::BadPath);

  CHECK(slurp(c.image) == before);  // the file is the fixture: what the run wrote went to private memory
}

TEST_CASE("fat image: a pulled card answers NoCard and NotMounted, and reads again once it is put back and mounted") {
  Card c;
  NEED(c.good);
  std::string got;
  NEED(read_all(c.storage, "readme.TXT", got));
  c.storage.set_present(false);
  CHECK_FALSE(c.storage.card_present());
  CHECK_FALSE(c.storage.mounted());
  CHECK(c.storage.exists("readme.TXT", nullptr) == hs::Status::NotMounted);
  CHECK(c.storage.gate() == hs::WriteStatus::NoCard);
  CHECK(c.storage.mount() == hs::Status::NoCard);
  c.storage.set_present(true);
  CHECK_FALSE(c.storage.mounted());  // put back, not mounted until the platform says
  CHECK(c.storage.mount() == hs::Status::Ok);
  NEED(read_all(c.storage, "readme.TXT", got));
  CHECK(got == content(40, 7));
  CHECK(c.storage.gate() == hs::WriteStatus::Ok);
}

TEST_CASE("fat image: `card copy` on an image is the computer's copy of a folder, written into the volume again") {
  Card c;
  NEED(c.good);
  std::string error;
  CHECK_FALSE(c.storage.computer_copy("Samples", "Samples2", error));  // the card is in the slot
  c.storage.set_present(false);
  CHECK(c.storage.computer_copy("Samples", "Projects/Samples Copy", error));
  CHECK(error.empty());
  CHECK_FALSE(c.storage.computer_copy("Nothing", "Projects/Else", error));
  CHECK_FALSE(c.storage.computer_copy("Samples", "Projects/Samples Copy", error));  // exists
  CHECK_FALSE(c.storage.computer_copy("Samples", "Nowhere/Deeper", error));          // no parent
  CHECK(c.storage.mount() == hs::Status::NoCard);
  c.storage.set_present(true);
  NEED(c.storage.mount() == hs::Status::Ok);
  std::string got;
  NEED(read_all(c.storage, "Projects/Samples Copy/Café Niño über.wav", got));
  CHECK(got == content(3000, 3));
  NEED(read_all(c.storage, "Samples/Café Niño über.wav", got));  // the original is where it was
  CHECK(got == content(3000, 3));
  CHECK(list(c.storage, "Big Folder").size() == 600u);
  CHECK(c.storage.volume().kind == hs::FsKind::Fat32);
}

TEST_CASE("fat image: an image that is too small, too full, or not an image says why") {
  FatImage small(1000);
  CHECK_FALSE(small.ok());
  CHECK(small.error().find("MiB") != std::string::npos);
  ScratchDir tree("fatfs-full");
  NEED(tree.ok());
  put(fs::path(tree.path()) / "a.bin", content(70 * 1024 * 1024, 1));  // more than the 64 MiB image holds
  FatImage full;
  CHECK_FALSE(full.add_tree(tree.path()));
  CHECK(full.error().find("full") != std::string::npos);

  ScratchDir out("fatfs-bad");
  NEED(out.ok());
  put(fs::path(out.path()) / "odd.img", content(1000, 1));  // not a multiple of 512
  put(fs::path(out.path()) / "zeros.img", std::string(512 * 100, '\0'));  // the right size, no file system
  FatFsStorage storage;
  std::string error;
  CHECK_FALSE(storage.open_image((fs::path(out.path()) / "odd.img").string(), error));
  CHECK(error.find("512") != std::string::npos);
  CHECK_FALSE(storage.open_image((fs::path(out.path()) / "missing.img").string(), error));
  CHECK_FALSE(storage.open_image((fs::path(out.path()) / "zeros.img").string(), error));  // no FAT boot sector: refused at once, by the image's name
  CHECK(error.find("zeros.img") != std::string::npos);
  CHECK(error.find("FAT") != std::string::npos);
}

TEST_CASE("fat image: a file that cannot be read fails the image, and the image is written whole or not at all") {
  ScratchDir tree("fatfs-unreadable");
  NEED(tree.ok());
  const fs::path secret = fs::path(tree.path()) / "locked.bin";
  put(secret, content(2000, 3));
  put(fs::path(tree.path()) / "ok.bin", content(100, 4));
  fs::permissions(secret, fs::perms::none);
  if (std::ifstream(secret).is_open()) {
    fs::permissions(secret, fs::perms::owner_all);  // a user that reads anything (root): the case cannot be made
  } else {
    FatImage image;
    CHECK_FALSE(image.add_tree(tree.path()));
    CHECK(image.error().find("locked.bin") != std::string::npos);
    fs::permissions(secret, fs::perms::owner_all);
  }

  FatImage good;
  CHECK(good.add_tree(tree.path()));
  const std::string target = (fs::path(tree.path()).parent_path() / (fs::path(tree.path()).filename().string() + ".img")).string();
  std::string error;
  NEED(good.write_file(target, error));
  CHECK(fs::exists(target));
  CHECK_FALSE(fs::exists(target + ".tmp"));  // written beside, renamed in
  CHECK(good.write_file(target, error));     // a second write replaces it
  CHECK_FALSE(fs::exists(target + ".tmp"));
  FatImage bad(1000);  // not ok(): nothing is written, the old image stays
  const auto before = fs::file_size(target);
  CHECK_FALSE(bad.write_file(target, error));
  CHECK(fs::file_size(target) == before);
  fs::remove(target);
}
