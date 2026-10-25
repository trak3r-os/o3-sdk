// SPDX-License-Identifier: GPL-3.0-or-later
//
// A FAT32 volume read through the whole read-only stack (OS-007e): an image built in the test from byte maps (mk66_storage_fakes.h Fat32Builder:
// long names, one-sector clusters so that every file is a chain, a directory that spans clusters), FatFs R0.15a in its read-only configuration,
// the FatStorage glue and, in the last cases, the SD card logic on a software card under it and the `diag` commands on top. Runs on the host and on
// QEMU's Cortex-M4F (the image is sparse: about 140 sectors are materialised).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/storage/ls_command.h>
#include <o3sdk/platform/mk66/storage/sd_card.h>
#include <o3sdk/platform/mk66/storage/sd_text.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3sdk::hal::storage;
namespace st = o3sdk::platform::mk66::storage;
namespace sd = o3sdk::platform::mk66::storage::sd;
namespace con = o3sdk::platform::mk66::console;
using fakes::Fat32Builder;
using fakes::Medium;

namespace {

std::vector<std::uint8_t> bytes(std::size_t n, unsigned salt) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 31u + 7u + salt) & 0xFFu);
  }
  return v;
}

// The card of these tests:
//   README.TXT  Caf\u00e9.txt  Workspace/{project.mt, patterns/{pattern_01.mtp, pattern_02.mtp, patternsMetadata}, instruments/{instrument_01.mti, 02 snare.pti}}
//   Projects/My Song/project.mt   many/file_00.dat .. file_39.dat
struct TestCard {
  Medium medium{200'000};
  std::uint32_t workspace = 0, patterns = 0, instruments = 0, projects = 0, song = 0, many = 0;
  TestCard() {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", bytes(40, 1));
    fat.add_file(Fat32Builder::kRootCluster, "Caf\xC3\xA9.txt", bytes(3, 2));  // UTF-8 in the test, UTF-16 on the volume, UTF-8 again from FatFs
    workspace = fat.make_dir(Fat32Builder::kRootCluster, "Workspace");
    fat.add_file(workspace, "project.mt", bytes(2320, 10));
    patterns = fat.make_dir(workspace, "patterns");
    fat.add_file(patterns, "pattern_01.mtp", bytes(9260, 11));
    fat.add_file(patterns, "pattern_02.mtp", bytes(6184, 12));
    fat.add_file(patterns, "patternsMetadata", bytes(100, 13));
    instruments = fat.make_dir(workspace, "instruments");
    fat.add_file(instruments, "instrument_01.mti", bytes(392, 14));
    fat.add_file(instruments, "02 snare.pti", bytes(592, 15));
    projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
    song = fat.make_dir(projects, "My Song");
    fat.add_file(song, "project.mt", bytes(2320, 16));
    many = fat.make_dir(Fat32Builder::kRootCluster, "many");
    for (unsigned i = 0; i < 40; ++i) {
      fat.add_file(many, std::string("file_") + static_cast<char>('0' + i / 10) + static_cast<char>('0' + i % 10) + ".dat", bytes(i + 1u, i));
    }
  }
};

std::string joined(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) {
    out += "[" + s + "]";
  }
  return out;
}

std::vector<std::string> list(hs::Storage& s, const char* path, std::vector<hs::DirEntry>* entries = nullptr) {
  std::vector<std::string> names;
  hs::DirHandle d;
  const hs::Status opened = s.opendir(path, d);
  CHECK_MESSAGE(opened == hs::Status::Ok, path);
  if (opened != hs::Status::Ok) {
    return names;
  }
  hs::DirEntry e;
  for (unsigned guard = 0; guard < 1000; ++guard) {
    const hs::Status r = s.readdir(d, e);
    if (r == hs::Status::End) {
      break;
    }
    CHECK(r == hs::Status::Ok);
    if (r != hs::Status::Ok) {
      break;
    }
    names.push_back(e.name);
    if (entries != nullptr) {
      entries->push_back(e);
    }
  }
  s.closedir(d);
  return names;
}

std::vector<std::uint8_t> slurp(hs::Storage& s, const char* path, std::size_t chunk) {
  std::vector<std::uint8_t> out;
  hs::FileHandle f;
  const hs::Status opened = s.open(path, f);
  CHECK_MESSAGE(opened == hs::Status::Ok, path);
  if (opened != hs::Status::Ok) {
    return out;
  }
  std::vector<std::uint8_t> buf(chunk);
  for (unsigned guard = 0; guard < 100000; ++guard) {
    std::size_t got = 0;
    const hs::Status r = s.read(f, buf.data(), chunk, got);
    CHECK(r == hs::Status::Ok);
    if (r != hs::Status::Ok || got == 0) {
      break;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
  }
  s.close(f);
  return out;
}

}  // namespace

TEST_CASE("fat storage: a FAT32 volume mounts and says what it is") {
  TestCard card;
  st::FatStorage fs(card.medium);
  CHECK(!fs.mounted());
  CHECK(fs.card_present());
  NEED(fs.mount() == hs::Status::Ok);
  CHECK(fs.mounted());
  const hs::VolumeInfo v = fs.volume();
  CHECK(v.kind == hs::FsKind::Fat32);
  CHECK(v.cluster_bytes == 512u);
  CHECK(v.capacity_mb == 32u);  // 65,600 clusters of 512 bytes
  CHECK(std::strcmp(hs::fs_kind_name(v.kind), "FAT32") == 0);
  CHECK(card.medium.starts == 1u);
}

TEST_CASE("fat storage: directories list long names in the volume's order, without . and ..") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);

  const std::vector<std::string> root = list(fs, "/");
  const std::vector<std::string> expected_root = {"README.TXT", "Caf\xC3\xA9.txt", "Workspace", "Projects", "many"};
  CHECK_MESSAGE(root == expected_root, joined(root));
  CHECK_MESSAGE(list(fs, "") == expected_root, "the empty path is the root too");

  std::vector<hs::DirEntry> entries;
  const std::vector<std::string> patterns = list(fs, "Workspace/patterns", &entries);
  const std::vector<std::string> expected_patterns = {"pattern_01.mtp", "pattern_02.mtp", "patternsMetadata"};
  CHECK_MESSAGE(patterns == expected_patterns, joined(patterns));
  NEED(entries.size() == 3u);
  CHECK(entries[0].size == 9260u);
  CHECK(entries[1].size == 6184u);
  CHECK(entries[2].size == 100u);
  CHECK(!entries[0].is_dir);

  const std::vector<std::string> expected_ws = {"project.mt", "patterns", "instruments"};
  entries.clear();
  CHECK(list(fs, "/Workspace/", &entries) == expected_ws);
  CHECK(!entries[0].is_dir);
  CHECK(entries[1].is_dir);
  CHECK(entries[1].size == 0u);
  CHECK(entries[2].is_dir);

  const std::vector<std::string> expected_instr = {"instrument_01.mti", "02 snare.pti"};
  CHECK(list(fs, "Workspace/instruments") == expected_instr);
  const std::vector<std::string> expected_song = {"project.mt"};
  CHECK(list(fs, "Projects/My Song") == expected_song);  // a name with a space
}

TEST_CASE("fat storage: a directory that spans clusters is listed whole, once each") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  std::vector<hs::DirEntry> entries;
  const std::vector<std::string> names = list(fs, "many", &entries);
  NEED(names.size() == 40u);  // 40 files, two 32-byte entries each (one long-name part and the alias) plus . and ..: 82 entries over six clusters of sixteen
  for (unsigned i = 0; i < 40; ++i) {
    const std::string want = std::string("file_") + static_cast<char>('0' + i / 10) + static_cast<char>('0' + i % 10) + ".dat";
    CHECK(names[i] == want);
    CHECK(entries[i].size == i + 1u);
  }
}

TEST_CASE("fat storage: files read back byte for byte, in any chunk size, across their cluster chain") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  const std::vector<std::uint8_t> want = bytes(9260, 11);  // 19 clusters
  for (const std::size_t chunk : {1u, 7u, 100u, 511u, 512u, 513u, 4096u, 20000u}) {
    CHECK(slurp(fs, "Workspace/patterns/pattern_01.mtp", chunk) == want);
  }
  CHECK(slurp(fs, "README.TXT", 64) == bytes(40, 1));
  CHECK(slurp(fs, "Caf\xC3\xA9.txt", 64) == bytes(3, 2));
  CHECK(slurp(fs, "Projects/My Song/project.mt", 4096) == bytes(2320, 16));
  CHECK(slurp(fs, "Workspace/instruments/02 snare.pti", 300) == bytes(592, 15));
}

TEST_CASE("fat storage: size, seek (clamped at the end), end of file, and a read after the end") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  hs::FileHandle f;
  NEED(fs.open("Workspace/project.mt", f) == hs::Status::Ok);
  std::uint32_t size = 0;
  NEED(fs.size(f, size) == hs::Status::Ok);
  CHECK(size == 2320u);
  const std::vector<std::uint8_t> want = bytes(2320, 10);
  std::uint8_t buf[64];
  std::size_t got = 0;
  NEED(fs.seek(f, 1000) == hs::Status::Ok);
  NEED(fs.read(f, buf, sizeof buf, got) == hs::Status::Ok);
  CHECK(got == 64u);
  CHECK(std::memcmp(buf, want.data() + 1000, 64) == 0);
  NEED(fs.seek(f, 2310) == hs::Status::Ok);
  NEED(fs.read(f, buf, sizeof buf, got) == hs::Status::Ok);
  CHECK(got == 10u);  // fewer than asked only at the end
  CHECK(std::memcmp(buf, want.data() + 2310, 10) == 0);
  NEED(fs.read(f, buf, sizeof buf, got) == hs::Status::Ok);
  CHECK(got == 0u);  // the end of the file, again
  NEED(fs.seek(f, 0xFFFFFFFFu) == hs::Status::Ok);  // past the end of a read-only file: the end
  NEED(fs.read(f, buf, sizeof buf, got) == hs::Status::Ok);
  CHECK(got == 0u);
  NEED(fs.seek(f, 0) == hs::Status::Ok);
  NEED(fs.read(f, buf, 16, got) == hs::Status::Ok);
  CHECK(std::memcmp(buf, want.data(), 16) == 0);
  fs.close(f);
  CHECK(!f.is_open());
  CHECK(fs.read(f, buf, 1, got) == hs::Status::BadHandle);
  CHECK(fs.seek(f, 0) == hs::Status::BadHandle);
  CHECK(fs.size(f, size) == hs::Status::BadHandle);
}

TEST_CASE("fat storage: names are matched without regard to case, and exists tells files from directories") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  bool dir = false;
  CHECK(fs.exists("workspace", &dir) == hs::Status::Ok);
  CHECK(dir);
  CHECK(fs.exists("WORKSPACE/PATTERNS/PATTERN_01.MTP", &dir) == hs::Status::Ok);
  CHECK(!dir);
  CHECK(fs.exists("/Workspace/Patterns/patternsmetadata", nullptr) == hs::Status::Ok);
  CHECK(fs.exists("readme.txt", &dir) == hs::Status::Ok);
  CHECK(fs.exists("/", &dir) == hs::Status::Ok);
  CHECK(dir);
  CHECK(fs.exists("", &dir) == hs::Status::Ok);
  CHECK(fs.exists("Workspace/patterns/pattern_03.mtp", &dir) == hs::Status::NotFound);
  CHECK(fs.exists("Nothing/here", &dir) == hs::Status::NotFound);
  hs::FileHandle f;
  CHECK(fs.open("WORKSPACE/PROJECT.MT", f) == hs::Status::Ok);
  fs.close(f);
}

TEST_CASE("fat storage: every error is its own answer, and a failed call leaves nothing open") {
  TestCard card;
  st::FatStorage fs(card.medium);
  hs::FileHandle f;
  hs::DirHandle d;
  hs::DirEntry e;
  // Before the mount.
  CHECK(fs.open("README.TXT", f) == hs::Status::NotMounted);
  CHECK(fs.opendir("/", d) == hs::Status::NotMounted);
  CHECK(fs.exists("README.TXT", nullptr) == hs::Status::NotMounted);
  CHECK(fs.readdir(d, e) == hs::Status::BadHandle);
  NEED(fs.mount() == hs::Status::Ok);
  CHECK(fs.open("Workspace/nothing.mt", f) == hs::Status::NotFound);
  CHECK(fs.open("Workspace", f) == hs::Status::NotAFile);     // a directory
  CHECK(fs.open("Nothing/x.mt", f) == hs::Status::NotFound);
  CHECK(fs.opendir("README.TXT", d) == hs::Status::NotADirectory);  // a file
  CHECK(fs.opendir("Nothing", d) == hs::Status::NotFound);
  CHECK(fs.open("README.TXT/x", f) == hs::Status::NotFound);  // a file as a component: FatFs says "no path"
  CHECK(fs.open("../README.TXT", f) == hs::Status::BadPath);
  CHECK(fs.open("Workspace/./project.mt", f) == hs::Status::BadPath);
  CHECK(fs.open("a|b", f) == hs::Status::BadPath);
  CHECK(fs.open("a\\b", f) == hs::Status::BadPath);
  CHECK(fs.open("C:/x", f) == hs::Status::BadPath);
  CHECK(fs.open("", f) == hs::Status::NotAFile);   // the root is a directory
  CHECK(fs.open("/", f) == hs::Status::NotAFile);
  CHECK(fs.open(nullptr, f) == hs::Status::BadPath);
  CHECK(fs.opendir("../x", d) == hs::Status::BadPath);
  CHECK(fs.exists("../x", nullptr) == hs::Status::BadPath);
  CHECK(fs.open_files() == 0u);
  CHECK(fs.open_dirs() == 0u);
  CHECK(!f.is_open());
  CHECK(!d.is_open());
  CHECK(fs.readdir(hs::DirHandle{}, e) == hs::Status::BadHandle);
  CHECK(fs.readdir(hs::DirHandle{5}, e) == hs::Status::BadHandle);
}

TEST_CASE("fat storage: the handle pools are the contract's size, and closing frees a slot") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  hs::FileHandle a, b, c;
  NEED(fs.open("README.TXT", a) == hs::Status::Ok);
  NEED(fs.open("Workspace/project.mt", b) == hs::Status::Ok);
  CHECK(a.slot != b.slot);
  CHECK(fs.open_files() == hs::kMaxOpenFiles);
  CHECK(fs.open("Projects/My Song/project.mt", c) == hs::Status::NoHandle);
  CHECK(!c.is_open());
  // The two files are independent: each keeps its own position.
  std::uint8_t x[8];
  std::uint8_t y[8];
  std::size_t got = 0;
  NEED(fs.read(a, x, 8, got) == hs::Status::Ok);
  NEED(fs.read(b, y, 8, got) == hs::Status::Ok);
  const std::vector<std::uint8_t> wa = bytes(40, 1);
  const std::vector<std::uint8_t> wb = bytes(2320, 10);
  CHECK(std::memcmp(x, wa.data(), 8) == 0);
  CHECK(std::memcmp(y, wb.data(), 8) == 0);
  fs.close(a);
  NEED(fs.open("Projects/My Song/project.mt", c) == hs::Status::Ok);
  fs.close(b);
  fs.close(c);
  CHECK(fs.open_files() == 0u);

  hs::DirHandle d1, d2, d3;
  NEED(fs.opendir("/", d1) == hs::Status::Ok);
  NEED(fs.opendir("many", d2) == hs::Status::Ok);
  CHECK(fs.opendir("Workspace", d3) == hs::Status::NoHandle);
  // A file can be open beside two directories being listed (what the project loader does).
  NEED(fs.open("README.TXT", a) == hs::Status::Ok);
  hs::DirEntry e1, e2;
  NEED(fs.readdir(d1, e1) == hs::Status::Ok);
  NEED(fs.readdir(d2, e2) == hs::Status::Ok);
  CHECK(std::strcmp(e1.name, "README.TXT") == 0);
  CHECK(std::strcmp(e2.name, "file_00.dat") == 0);
  fs.close(a);
  fs.closedir(d1);
  fs.closedir(d2);
  CHECK(fs.open_dirs() == 0u);
}

TEST_CASE("fat storage: a medium that is absent, blank, unreadable or taken out") {
  {
    TestCard card;
    card.medium.present_ = false;
    st::FatStorage fs(card.medium);
    CHECK(fs.mount() == hs::Status::NoCard);
    CHECK(!fs.mounted());
    CHECK(!fs.card_present());
  }
  {
    Medium blank(200'000);  // no file system: the sectors are zeros
    st::FatStorage fs(blank);
    CHECK(fs.mount() == hs::Status::Unsupported);
    CHECK(!fs.mounted());
  }
  {
    TestCard card;
    card.medium.fail_reads = true;
    st::FatStorage fs(card.medium);
    CHECK(fs.mount() == hs::Status::Io);
  }
  {
    TestCard card;
    st::FatStorage fs(card.medium);
    NEED(fs.mount() == hs::Status::Ok);
    hs::FileHandle f;
    NEED(fs.open("Workspace/project.mt", f) == hs::Status::Ok);
    card.medium.present_ = false;  // the card is pulled out
    CHECK(!fs.mounted());
    std::uint8_t buf[16];
    std::size_t got = 0;
    CHECK(fs.read(f, buf, 16, got) == hs::Status::NotMounted);
    CHECK(fs.open("README.TXT", f) == hs::Status::NotMounted);
    card.medium.present_ = true;
    CHECK(fs.mount() == hs::Status::Ok);  // back in: a clean mount, the old handle is gone
    CHECK(fs.open_files() == 0u);
    hs::FileHandle again;
    CHECK(fs.open("Workspace/project.mt", again) == hs::Status::Ok);
  }
  {
    TestCard card;
    st::FatStorage fs(card.medium);
    NEED(fs.mount() == hs::Status::Ok);
    hs::FileHandle f;
    NEED(fs.open("Workspace/patterns/pattern_01.mtp", f) == hs::Status::Ok);
    card.medium.fail_reads = true;  // a read that the card refuses mid-file
    std::vector<std::uint8_t> big(9260);
    std::size_t got = 0;
    CHECK(fs.read(f, big.data(), big.size(), got) == hs::Status::Io);
  }
}

TEST_CASE("fat storage: an exFAT volume mounts, lists and reads, contiguous and fragmented files") {
  Medium medium(20'000);
  fakes::ExFatBuilder fat(medium);
  const std::uint32_t docs = fat.make_dir(fakes::ExFatBuilder::kRootCluster, "Projects");
  fat.add_file(fakes::ExFatBuilder::kRootCluster, "README.TXT", bytes(40, 1), true);
  fat.add_file(fakes::ExFatBuilder::kRootCluster, "a long name for a file in an exFAT root.bin", bytes(1300, 2), false);  // fragmented: a chain
  fat.add_file(docs, "project.mt", bytes(2320, 3), true);                                                                // contiguous, no chain
  fat.add_file(docs, "pattern_01.mtp", bytes(9260, 4), false);
  for (unsigned i = 0; i < 20; ++i) {  // enough entry sets to cross cluster boundaries of the directory
    fat.add_file(docs, "extra_" + std::to_string(i) + ".dat", bytes(i, i), true);
  }
  st::FatStorage fs(medium);
  NEED(fs.mount() == hs::Status::Ok);
  CHECK(fs.volume().kind == hs::FsKind::ExFat);
  CHECK(std::strcmp(hs::fs_kind_name(fs.volume().kind), "exFAT") == 0);
  CHECK(fs.volume().cluster_bytes == 512u);

  const std::vector<std::string> root = list(fs, "/");
  const std::vector<std::string> expected_root = {"Projects", "README.TXT", "a long name for a file in an exFAT root.bin"};
  CHECK_MESSAGE(root == expected_root, joined(root));
  std::vector<hs::DirEntry> entries;
  const std::vector<std::string> in_projects = list(fs, "projects", &entries);  // case-insensitive, as on FAT
  NEED(in_projects.size() == 22u);
  CHECK(in_projects[0] == "project.mt");
  CHECK(in_projects[1] == "pattern_01.mtp");
  CHECK(in_projects[21] == "extra_19.dat");
  CHECK(entries[0].size == 2320u);
  CHECK(entries[1].size == 9260u);

  CHECK(slurp(fs, "README.TXT", 100) == bytes(40, 1));
  CHECK(slurp(fs, "a long name for a file in an exFAT root.bin", 77) == bytes(1300, 2));
  CHECK(slurp(fs, "Projects/project.mt", 4096) == bytes(2320, 3));
  CHECK(slurp(fs, "PROJECTS/PATTERN_01.MTP", 513) == bytes(9260, 4));
  CHECK(slurp(fs, "Projects/extra_7.dat", 10) == bytes(7, 7));
  hs::FileHandle f;
  CHECK(fs.open("Projects/missing.mt", f) == hs::Status::NotFound);
  CHECK(fs.open("Projects", f) == hs::Status::NotAFile);
  bool dir = false;
  CHECK(fs.exists("projects", &dir) == hs::Status::Ok);
  CHECK(dir);
}

TEST_CASE("fat storage: the valid_path rules both implementations of the contract share") {
  CHECK(hs::valid_path("/"));
  CHECK(hs::valid_path(""));
  CHECK(hs::valid_path("a"));
  CHECK(hs::valid_path("/a/b/"));
  CHECK(hs::valid_path("Projects/My Song/project.mt"));
  CHECK(hs::valid_path("Caf\xC3\xA9.txt"));
  CHECK(hs::valid_path("a.b"));
  CHECK(hs::valid_path(".hidden"));
  CHECK(hs::valid_path("..."));  // not "." or "..": a name of three dots
  CHECK(!hs::valid_path(nullptr));
  CHECK(!hs::valid_path("."));
  CHECK(!hs::valid_path(".."));
  CHECK(!hs::valid_path("a/./b"));
  CHECK(!hs::valid_path("a/../b"));
  CHECK(!hs::valid_path("a/.."));
  CHECK(!hs::valid_path("../a"));
  CHECK(!hs::valid_path("a/b\\c"));
  CHECK(!hs::valid_path("a:b"));
  CHECK(!hs::valid_path("a*b"));
  CHECK(!hs::valid_path("a?b"));
  CHECK(!hs::valid_path("a\"b"));
  CHECK(!hs::valid_path("a<b"));
  CHECK(!hs::valid_path("a>b"));
  CHECK(!hs::valid_path("a|b"));
  CHECK(!hs::valid_path("a\tb"));
  CHECK(!hs::valid_path("a\x7F"));
  const std::string longest(hs::kPathMax, 'x');
  CHECK(hs::valid_path(longest.c_str()));
  CHECK(!hs::valid_path((longest + "x").c_str()));
}

// ---- The whole stack: the software card under FatFs -------------------------------------------------------------------------------------
TEST_CASE("sd stack: a card mounted and read through the SD logic, FatFs and the glue; the counters and `diag sd` say so") {
  TestCard card;
  // The software card serves the same sectors the FAT image holds, as an 8-"GB" card (the Medium of the image is smaller than the CSD's capacity;
  // the fake card's capacity is the Medium's, so the volume is as big as the image is).
  fakes::FakeCardSpec spec;
  fakes::FakeSdBus bus(card.medium, spec);
  sd::Card sdcard(bus);
  st::FatStorage fs(sdcard);
  NEED(fs.mount() == hs::Status::Ok);
  CHECK(fs.volume().kind == hs::FsKind::Fat32);
  CHECK(sdcard.info().ready);
  CHECK(sdcard.info().high_speed);
  CHECK(slurp(fs, "Workspace/patterns/pattern_02.mtp", 4096) == bytes(6184, 12));
  CHECK(sdcard.counters().blocks > 12u);
  CHECK(sdcard.counters().failures == 0u);
  CHECK(bus.reads_multi + bus.reads_single > 0u);

  st::SdReport r;
  r.card_present = fs.card_present();
  r.mounted = fs.mounted();
  r.mount_status = hs::Status::Ok;
  r.mount_tried = true;
  r.volume = fs.volume();
  r.card = &sdcard.info();
  r.counters = &sdcard.counters();
  r.stage = sdcard.stage();
  r.load_summary = "Workspace: v16, 2 patterns, 2 instruments";
  r.sample_summary = "samples 2 loaded, 0 missing, 1 KB";
  char text[1500];
  const std::size_t n = st::format_sd(r, text, sizeof text);
  NEED(n > 0u);
  const std::string out(text, n);
  CHECK(out.find("a card is in") != std::string::npos);
  CHECK(out.find("mounted, FAT32, 32 MB, cluster 512 B") != std::string::npos);
  CHECK(out.find("SDHC") != std::string::npos);
  CHECK(out.find("4 data lines, 45000 kHz (high speed)") != std::string::npos);
  CHECK(out.find("product 'SC16G'") != std::string::npos);
  CHECK(out.find("made 2024-07") != std::string::npos);
  // The raw registers, in the form Linux prints them: the CID the fake card was told to have (manufacturer 03, "SD", "SC16G", revision 80, serial
  // 1234ABCD, 2024-07; the last byte, the CRC7, is zero here) and a CSD of version 2 (the structure bits 01 in the top of byte 0).
  CHECK(out.find("cid:     0353445343313647801234abcd018700\n") != std::string::npos);
  CHECK(out.find("csd:     40") != std::string::npos);
  CHECK(out.find("failed reads") != std::string::npos);
  CHECK(out.find("load:    Workspace: v16, 2 patterns, 2 instruments") != std::string::npos);
  CHECK(out.find("pcm:     samples 2 loaded, 0 missing, 1 KB\n") != std::string::npos);
  CHECK(out.find("read only except the firmware writer") != std::string::npos);
  CHECK(out.back() == '\n');
  CHECK(n < 1500u);  // fits the console's text buffer (1,536 bytes) with room
}

TEST_CASE("sd stack: `diag sd` for the cases that are not a mounted card") {
  char text[1500];
  st::SdReport none;
  none.card_present = false;
  std::size_t n = st::format_sd(none, text, sizeof text);
  std::string out(text, n);
  CHECK(out.find("no card (detect switch open)") != std::string::npos);
  CHECK(out.find("not tried") != std::string::npos);
  CHECK(out.find("not initialised") != std::string::npos);

  TestCard card;
  fakes::FakeCardSpec spec;
  spec.never_ready = true;
  fakes::FakeSdBus bus(card.medium, spec);
  sd::Card sdcard(bus);
  st::FatStorage fs(sdcard);
  const hs::Status s = fs.mount();
  CHECK(s == hs::Status::Io);  // a card that never gets ready is a failed start, not an absent one
  st::SdReport r;
  r.card_present = true;
  r.mount_status = s;
  r.mount_tried = true;
  r.card = &sdcard.info();
  r.counters = &sdcard.counters();
  n = st::format_sd(r, text, sizeof text);
  out.assign(text, n);
  CHECK(out.find("not mounted: i/o error") != std::string::npos);
  CHECK(out.find("init stopped at ACMD41, card not ready") != std::string::npos);
  CHECK(out.find("read only except the firmware writer") != std::string::npos);

  // A cut buffer never overruns.
  char tiny[40];
  n = st::format_sd(r, tiny, sizeof tiny);
  CHECK(n < sizeof tiny);
  CHECK(tiny[n] == '\0');
}

// ---- `ls` -----------------------------------------------------------------------------------------------------------------------------------
namespace {

// What format_ls_entry makes: a file is "  " + the size right-aligned in ten columns + "  " + the name; a directory "d" and thirteen spaces, the
// name and a "/", so that the names line up.
std::string file_line(std::uint32_t size, const char* name) {
  const std::string digits = std::to_string(size);
  return "  " + std::string(10 - digits.size(), ' ') + digits + "  " + name;
}
std::string dir_line(const char* name) { return "d" + std::string(13, ' ') + name + "/"; }

struct Capture {
  std::string text;
  static void write(void* self, const char* data, std::size_t n) { static_cast<Capture*>(self)->text.append(data, n); }
  static void flush(void*) {}
  o3sdk::usb::diag::Sink sink() { return o3sdk::usb::diag::Sink{&Capture::write, &Capture::flush, this, nullptr}; }
};

hs::Storage* g_ls_storage = nullptr;
void cmd_ls(o3sdk::usb::diag::Shell& sh, const o3sdk::usb::diag::Args& a) { con::run_ls_command(sh, *g_ls_storage, a); }

std::string run_line(o3sdk::usb::diag::Shell& sh, Capture& cap, const char* line) {
  cap.text.clear();
  for (const char* p = line; *p != '\0'; ++p) {
    const std::uint8_t b = static_cast<std::uint8_t>(*p);
    (void)sh.feed(&b, 1);
  }
  const std::uint8_t cr = '\r';
  (void)sh.feed(&cr, 1);
  for (unsigned i = 0; i < 200 && (sh.streaming() || i == 0); ++i) {
    sh.tick();
  }
  return cap.text;
}

}  // namespace

TEST_CASE("ls: a directory is listed a few lines a pass, with sizes, directories marked, a count at the end") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  g_ls_storage = &fs;
  Capture cap;
  const o3sdk::usb::diag::Command table[] = {o3sdk::usb::diag::command("ls", "ls [path]", &cmd_ls)};
  o3sdk::usb::diag::Shell sh(table, 1, o3sdk::usb::diag::Output(cap.sink()));

  std::string out = run_line(sh, cap, "ls");
  CHECK(out.find("ls /:") != std::string::npos);
  CHECK(out.find("README.TXT") != std::string::npos);
  CHECK(out.find("Workspace/") != std::string::npos);
  CHECK(out.find(dir_line("Projects")) != std::string::npos);
  CHECK(out.find("5 entries") != std::string::npos);
  CHECK(!sh.streaming());
  CHECK(fs.open_dirs() == 0u);  // the pager closed its directory

  out = run_line(sh, cap, "ls Workspace/patterns");
  CHECK(out.find("ls Workspace/patterns:") != std::string::npos);
  CHECK(out.find(file_line(9260, "pattern_01.mtp")) != std::string::npos);
  CHECK(out.find(file_line(6184, "pattern_02.mtp")) != std::string::npos);
  CHECK(out.find(file_line(100, "patternsMetadata")) != std::string::npos);
  CHECK(out.find("3 entries") != std::string::npos);

  out = run_line(sh, cap, "ls Projects/My Song");  // the line is split on spaces; the path is joined again
  CHECK(out.find("ls Projects/My Song:") != std::string::npos);
  CHECK(out.find("project.mt") != std::string::npos);
  CHECK(out.find("1 entry") != std::string::npos);

  out = run_line(sh, cap, "ls many");
  CHECK(out.find("file_00.dat") != std::string::npos);
  CHECK(out.find("file_39.dat") != std::string::npos);
  CHECK(out.find("40 entries") != std::string::npos);

  out = run_line(sh, cap, "ls Nothing");
  CHECK(out.find("ls: Nothing: not found") != std::string::npos);
  out = run_line(sh, cap, "ls README.TXT");
  CHECK(out.find("ls: README.TXT: not a directory") != std::string::npos);
  out = run_line(sh, cap, "ls ../x");
  CHECK(out.find("ls: ../x: bad path") != std::string::npos);
  CHECK(fs.open_dirs() == 0u);

  // Not mounted: said plainly.
  st::FatStorage idle(card.medium);
  g_ls_storage = &idle;
  out = run_line(sh, cap, "ls");
  CHECK(out.find("the card is not mounted") != std::string::npos);
}

TEST_CASE("ls: a listing that a key press ended does not leak its directory, and the path arguments join as typed") {
  TestCard card;
  st::FatStorage fs(card.medium);
  NEED(fs.mount() == hs::Status::Ok);
  g_ls_storage = &fs;
  Capture cap;
  const o3sdk::usb::diag::Command table[] = {o3sdk::usb::diag::command("ls", "ls [path]", &cmd_ls)};
  o3sdk::usb::diag::Shell sh(table, 1, o3sdk::usb::diag::Output(cap.sink()));
  // Start a listing and stop it after one pass (the shell's stop_stream, as a key press does): the directory stays open until the next `ls` ...
  cap.text.clear();
  for (const char* p = "ls many\r"; *p != '\0'; ++p) {
    const std::uint8_t b = static_cast<std::uint8_t>(*p);
    (void)sh.feed(&b, 1);
  }
  sh.tick();
  sh.stop_stream();
  CHECK(fs.open_dirs() == 1u);
  // ... which closes it before it opens its own.
  const std::string out = run_line(sh, cap, "ls Workspace");
  CHECK(out.find("3 entries") != std::string::npos);
  CHECK(fs.open_dirs() == 0u);

  o3sdk::usb::diag::Args args;
  args.argc = 4;
  args.argv[0] = "ls";
  args.argv[1] = "Projects/My";
  args.argv[2] = "Song";
  args.argv[3] = "x";
  char path[64];
  CHECK(con::join_path_args(args, 1, path, sizeof path));
  CHECK(std::strcmp(path, "Projects/My Song x") == 0);
  CHECK(!con::join_path_args(args, 1, path, 12));  // does not fit
  CHECK(!con::join_path_args(args, 4, path, sizeof path));  // no arguments
}

TEST_CASE("ls: one line of a listing, directories and files") {
  hs::DirEntry e{};
  std::strcpy(e.name, "KICK.WAV");
  e.size = 123456;
  e.is_dir = false;
  char buf[300];
  std::size_t n = st::format_ls_entry(e, buf, sizeof buf);
  CHECK(std::string(buf, n) == file_line(123456, "KICK.WAV") + "\n");
  e.size = 0;
  n = st::format_ls_entry(e, buf, sizeof buf);
  CHECK(std::string(buf, n) == file_line(0, "KICK.WAV") + "\n");
  CHECK(file_line(0, "KICK.WAV") == "           0  KICK.WAV");  // 2 + 9 spaces, the digit, 2 spaces
  e.size = 0xFFFFFFFFu;
  n = st::format_ls_entry(e, buf, sizeof buf);
  CHECK(std::string(buf, n) == "  4294967295  KICK.WAV\n");  // ten digits: the column is full
  std::strcpy(e.name, "Projects");
  e.is_dir = true;
  n = st::format_ls_entry(e, buf, sizeof buf);
  CHECK(std::string(buf, n) == dir_line("Projects") + "\n");
  CHECK(dir_line("Projects") == "d             Projects/");  // the name starts in the same column as a file's: 14
  // The longest name still fits the line buffer the pager uses.
  std::memset(e.name, 'n', hs::kNameMax);
  e.name[hs::kNameMax] = '\0';
  e.is_dir = false;
  char line[hs::kNameMax + 32];
  n = st::format_ls_entry(e, line, sizeof line);
  CHECK(n == 14u + hs::kNameMax + 1u);
  CHECK(line[n - 1] == '\n');
}
