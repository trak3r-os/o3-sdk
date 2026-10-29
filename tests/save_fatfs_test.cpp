// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save job (app/atomic_save.h) over the REAL stack of the image: FatStorage and the FatFs-backed StorageWriter on a FAT32 volume built in memory, with a block
// device that loses power at the N-th block written (and one that fails a single block). The same property as atomic_save_test.cpp, now with FatFs's own order of
// writes (data, FAT, directory entries) and its rename (two directory writes, a cross-link in between): after a cut at EVERY block of a replacing save, a fresh mount
// finds the old file or the new one whole, and the next save either succeeds or refuses with "card needs a check", but never loses the user's data and never writes
// outside the guarded folders; on a card that carries no marker file at all (any card is saved to, OS-ANYCARD). Host only (the unit binary): it needs the app layer.
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_storage_fakes.h"
#include "support/need.h"
#include "support/save_source.h"

#include <o3sdk/app/atomic_save.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace app = o3sdk::app;
namespace hs = o3sdk::hal::storage;
namespace st = o3sdk::platform::mk66::storage;
namespace sv = o3sdk::platform::mk66::save;
using fakes::Fat32Builder;
using fakes::Medium;
using o3sdk::test::run;
using o3sdk::test::VecSource;
using Mode = app::AtomicSave::Mode;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes content(std::size_t n, unsigned salt) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<std::uint8_t>((i * 7u + salt * 13u + 1u) & 0xFFu);
  }
  return b;
}

constexpr const char* kDir = "Projects/Song";
constexpr const char* kFile = "Projects/Song/project.o3pj";
constexpr const char* kBak = "Projects/Song/project.o3pj.bak";
constexpr const char* kTmp = "Projects/Song/_o3save.tmp";

// A card of the maintainer's kind: files that are not ours and the OG project (no marker file: any card is saved to).
void build(Medium& medium) {
  Fat32Builder fat(medium);
  fat.add_file(Fat32Builder::kRootCluster, "README.TXT", content(40, 1));
  const std::uint32_t projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
  const std::uint32_t song = fat.make_dir(projects, "Song");
  fat.add_file(song, "project.mt", content(2320, 10));
  const std::uint32_t patterns = fat.make_dir(song, "patterns");
  fat.add_file(patterns, "pattern_01.mtp", content(1000, 11));
  const std::uint32_t samples = fat.make_dir(Fat32Builder::kRootCluster, "Samples");
  fat.add_file(samples, "mine.wav", content(700, 12));
}

Bytes read_file(hs::Storage& s, const char* path) {
  Bytes out;
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

bool user_files_intact(hs::Storage& s) {
  return read_file(s, "README.TXT") == content(40, 1) && read_file(s, "Projects/Song/project.mt") == content(2320, 10) &&
         read_file(s, "Projects/Song/patterns/pattern_01.mtp") == content(1000, 11) && read_file(s, "Samples/mine.wav") == content(700, 12);
}

// One save over a fresh mount of the medium. Returns the job's error through `job`.
struct Stack {
  st::FatStorage fat;
  sv::FatStorageWriter writer;
  app::AtomicSave job;
  explicit Stack(Medium& medium) : fat(medium), writer(fat, medium) {}
  bool mount() { return fat.mount() == hs::Status::Ok; }
  bool save(const Bytes& data, Mode mode = Mode::Replace, const char* dir = kDir, const char* name = "project.o3pj") {
    VecSource source(data);
    if (!job.begin(fat, writer, dir, name, mode, source, "SONG")) {
      return false;
    }
    run(job);
    return job.done();
  }
};

}  // namespace

TEST_CASE("save over FatFs: a first save, a replacing save, the backup, the folders") {
  Medium medium{200'000};
  build(medium);
  Stack stack(medium);
  NEED(stack.mount());
  CHECK_FALSE(exists(stack.fat, ".o3-test-card"));  // no marker file: this card is saved to all the same (OS-ANYCARD)
  const Bytes first = content(20'000, 1);
  CHECK(stack.save(first));
  CHECK(read_file(stack.fat, kFile) == first);
  CHECK_FALSE(exists(stack.fat, kBak));
  CHECK_FALSE(exists(stack.fat, kTmp));
  const Bytes second = content(33'000, 2);
  CHECK(stack.save(second));
  CHECK(read_file(stack.fat, kFile) == second);
  CHECK(read_file(stack.fat, kBak) == first);
  const Bytes third = content(1'000, 3);
  CHECK(stack.save(third));
  CHECK(read_file(stack.fat, kFile) == third);
  CHECK(read_file(stack.fat, kBak) == second);
  CHECK_FALSE(exists(stack.fat, kTmp));
  CHECK(user_files_intact(stack.fat));
  // A new project folder and a WAV, folders made on the way; a name that is taken is refused.
  CHECK(stack.save(first, Mode::NewOnly, "Projects/Track 01", "project.o3pj"));
  CHECK(read_file(stack.fat, "Projects/Track 01/project.o3pj") == first);
  CHECK_FALSE(stack.save(second, Mode::NewOnly, "Projects/Track 01", "project.o3pj"));
  CHECK(stack.job.error() == app::AtomicSave::Error::Exists);
  CHECK(read_file(stack.fat, "Projects/Track 01/project.o3pj") == first);
  CHECK(stack.save(second, Mode::NewOnly, "Samples", "REC_001.wav"));
  CHECK(stack.save(first, Mode::NewOnly, "Settings", "device.o3st"));
  CHECK(read_file(stack.fat, "Samples/REC_001.wav") == second);
  CHECK(read_file(stack.fat, "Samples/mine.wav") == content(700, 12));
  CHECK(user_files_intact(stack.fat));
}

TEST_CASE("save over FatFs: a power cut at every block of a replacing save") {
  const Bytes old_file = content(6'000, 5);
  const Bytes fresh = content(9'000, 6);
  std::uint64_t blocks = 0;
  {  // the blocks of an uninterrupted second save
    Medium medium{200'000};
    build(medium);
    Stack stack(medium);
    NEED(stack.mount());
    NEED(stack.save(old_file));
    const std::uint64_t before = medium.blocks_written;
    NEED(stack.save(fresh));
    blocks = medium.blocks_written - before;
  }
  CHECK(blocks > 20);
  unsigned refused = 0;
  unsigned healed = 0;
  for (std::uint64_t cut = 0; cut <= blocks; ++cut) {
    INFO("power cut at block " << cut << " of " << blocks);
    Medium medium{200'000};
    build(medium);
    {
      Stack stack(medium);
      NEED(stack.mount());
      NEED(stack.save(old_file));
      medium.fail_writes_after = static_cast<std::uint32_t>(cut);
      (void)stack.save(fresh);
    }  // power gone: the FatFs state of that boot is dropped with the objects
    medium.fail_writes_after = 0xFFFFFFFFu;

    // ---- the next boot
    Stack boot(medium);
    NEED(boot.mount());
    CHECK(user_files_intact(boot.fat));
    const Bytes at_f = read_file(boot.fat, kFile);
    const Bytes at_b = read_file(boot.fat, kBak);
    const bool f_ok = at_f == old_file || at_f == fresh;
    const bool b_ok = at_b == old_file;
    CHECK((f_ok || (at_f.empty() && b_ok)));          // never a half file; with no F, B is the old file
    CHECK((at_f == old_file || at_f == fresh || b_ok));  // the user's data is whole in at least one name
    if (exists(boot.fat, kBak)) {
      CHECK((at_b == old_file || at_b == fresh));      // (the same data under two names after a torn rename, never a third thing)
    }

    // ---- the next save: it succeeds, or it refuses because two names share one chain and says so; never a loss
    const Bytes later = content(4'000, 7);
    const bool ok = boot.save(later);
    if (ok) {
      CHECK(read_file(boot.fat, kFile) == later);
      CHECK_FALSE(exists(boot.fat, kTmp));
      healed += boot.job.healed() ? 1u : 0u;
    } else {
      ++refused;
      CHECK(boot.job.error() == app::AtomicSave::Error::Damaged);
      CHECK(std::string(boot.job.detail()) == "CARD NEEDS A CHECK");
      const Bytes now_f = read_file(boot.fat, kFile);
      const Bytes now_b = read_file(boot.fat, kBak);
      CHECK((now_f == old_file || now_f == fresh || now_b == old_file));
    }
    CHECK(user_files_intact(boot.fat));
  }
  // The sweep went through both kinds of cut (a refusal is rare, a heal is not): record what it saw.
  MESSAGE("cuts: " << blocks + 1 << ", refused as cross-linked: " << refused << ", healed: " << healed);
}

TEST_CASE("save over FatFs: a power cut at every block of a new-file save (a WAV) never leaves a partial WAV") {
  const Bytes wav = content(30'000, 8);
  std::uint64_t blocks = 0;
  {
    Medium medium{200'000};
    build(medium);
    Stack stack(medium);
    NEED(stack.mount());
    const std::uint64_t before = medium.blocks_written;
    NEED(stack.save(wav, Mode::NewOnly, "Samples", "REC_001.wav"));
    blocks = medium.blocks_written - before;
  }
  for (std::uint64_t cut = 0; cut <= blocks; cut += 3) {  // (every third block: the WAV is 60 blocks)
    INFO("power cut at block " << cut << " of " << blocks);
    Medium medium{200'000};
    build(medium);
    {
      Stack stack(medium);
      NEED(stack.mount());
      medium.fail_writes_after = static_cast<std::uint32_t>(cut);
      (void)stack.save(wav, Mode::NewOnly, "Samples", "REC_001.wav");
    }
    medium.fail_writes_after = 0xFFFFFFFFu;
    Stack boot(medium);
    NEED(boot.mount());
    const Bytes got = read_file(boot.fat, "Samples/REC_001.wav");
    CHECK((got.empty() ? !exists(boot.fat, "Samples/REC_001.wav") : got == wav));
    CHECK(user_files_intact(boot.fat));
    CHECK(boot.save(wav, Mode::NewOnly, "Samples", "REC_002.wav"));  // the next take takes the next name, whatever was left
    CHECK(read_file(boot.fat, "Samples/REC_002.wav") == wav);
    CHECK(user_files_intact(boot.fat));
  }
}

namespace {

// A medium that fails ONE block write and then works again (a card that hiccups, not one that loses power).
class Hiccup final : public st::BlockDevice {
 public:
  explicit Hiccup(Medium& m) : m_(m) {}
  std::int64_t fail_block = -1;  // the block (counted from now) whose write fails once
  bool present() const noexcept override { return m_.present(); }
  st::BlockResult start() noexcept override { return m_.start(); }
  bool ready() const noexcept override { return m_.ready(); }
  std::uint32_t block_count() const noexcept override { return m_.block_count(); }
  st::BlockResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override { return m_.read(lba, count, dst); }
  st::BlockResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override {
    for (std::uint32_t i = 0; i < count; ++i) {
      if (fail_block == 0) {
        fail_block = -1;
        return st::BlockResult::Failed;
      }
      if (fail_block > 0) {
        --fail_block;
      }
      const st::BlockResult r = m_.write(lba + i, 1, static_cast<const std::uint8_t*>(src) + 512u * i);
      if (r != st::BlockResult::Ok) {
        return r;
      }
    }
    return st::BlockResult::Ok;
  }
  bool write_protected() const noexcept override { return m_.write_protected(); }

 private:
  Medium& m_;
};

}  // namespace

TEST_CASE("save over FatFs: one block that fails once, at every block, leaves the old file whole and no temp") {
  const Bytes old_file = content(6'000, 9);
  const Bytes fresh = content(9'000, 10);
  std::uint64_t blocks = 0;
  {
    Medium medium{200'000};
    build(medium);
    Stack stack(medium);
    NEED(stack.mount());
    NEED(stack.save(old_file));
    const std::uint64_t before = medium.blocks_written;
    NEED(stack.save(fresh));
    blocks = medium.blocks_written - before;
  }
  unsigned failed = 0;
  for (std::uint64_t at = 0; at < blocks; ++at) {
    INFO("block " << at << " of " << blocks << " fails once");
    Medium medium{200'000};
    build(medium);
    Hiccup device(medium);
    st::FatStorage fat(device);
    sv::FatStorageWriter writer(fat, device);
    app::AtomicSave job;
    NEED(fat.mount() == hs::Status::Ok);
    {
      VecSource first(old_file);
      NEED(job.begin(fat, writer, kDir, "project.o3pj", Mode::Replace, first, "SONG"));
      run(job);
      NEED(job.done());
    }
    device.fail_block = static_cast<std::int64_t>(at);
    VecSource source(fresh);
    NEED(job.begin(fat, writer, kDir, "project.o3pj", Mode::Replace, source, "SONG"));
    run(job);
    CHECK_FALSE(writer.writing());
    CHECK(user_files_intact(fat));
    if (job.failed()) {
      ++failed;
      // A write that failed: the old file is whole in F, or in B after the repair (the failure came in the last two renames); or the rename in fact went through and
      // the block that failed was FatFs's bookkeeping after it (the FSInfo sector): F is the new file, B the old one, and the job says failed all the same, the safe way
      // round. Never a third thing, and no temp left behind.
      const Bytes f = read_file(fat, kFile);
      const Bytes b = read_file(fat, kBak);
      CHECK((f == old_file || (f == fresh && b == old_file) || (f.empty() && b == old_file)));
      CHECK_FALSE(exists(fat, kTmp));
    } else {
      CHECK(read_file(fat, kFile) == fresh);  // (the failing block was not on this save's path, e.g. the FAT copy that FatFs repeats)
    }
    // The card works again: the very next save succeeds.
    VecSource again(content(2'000, 11));
    NEED(job.begin(fat, writer, kDir, "project.o3pj", Mode::Replace, again, "SONG"));
    run(job);
    CHECK(job.done());
  }
  CHECK(failed > 10);
}

// ---- OS-FILES part B: RENAME of a project folder and the .deleted marker over the real FatFs ---------------------------------------------------------------------------------------------
//
// OS-FILES part B: the .deleted marker over the real FatFs. (The first version of part B also renamed project folders, first across folders and then inside one; the sweeps below it showed both are unsafe:
// FatFs makes the new directory entry set and then removes the old one, a rename across folders was 3 block writes with both names at 2 of 4 cut points, and inside one folder with 31-character names it was
// 1 to 5 block writes with both names at 9 of 42 cut points over eleven directory alignments. Two entries on one chain corrupt the other when one is deleted on a computer, so the guard allows no folder
// rename at all; docs/specs/OS-FILES-B.md.) The marker is a created-new empty file: one directory entry set, no cluster, nothing to cross-link. The sweeps cut the power at EVERY block and fail a single
// block at every block, with long names (31 characters: three long-name entries and the short one, 4 of a sector's 16 slots) beside it and the directory filled to eleven alignments.

namespace {

constexpr const char* kLongOld = "Song With A Rather Long Name 01";  // 31 characters
static_assert(sizeof("Song With A Rather Long Name 01") - 1 == 31, "the longest name the guard takes");

struct ProjectFiles {
  const char* name;
  unsigned size;
  unsigned salt;
};
constexpr ProjectFiles kSongFiles[] = {{"project.o3pj", 3000, 20}, {"notes.txt", 600, 21}, {"samples/instr01.wav", 1500, 22}, {"patterns/pattern_01.mtp", 1000, 24}};

// A card of the maintainer's kind: `siblings` other projects with 31-character names in Projects/ (so the directory is not empty and the new name's entries land at another place of a sector each time),
// the project under test with `fillers` long-named files beside its own (the marker's entries land at another place too), and Other.
void build_moves(Medium& medium, unsigned siblings = 0, unsigned fillers = 0) {
  Fat32Builder fat(medium);
  fat.add_file(Fat32Builder::kRootCluster, "README.TXT", content(40, 1));
  const std::uint32_t projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
  for (unsigned i = 0; i < siblings; ++i) {
    char name[40];
    std::snprintf(name, sizeof name, "Filler Project Number Eleven %03u", i);
    const std::uint32_t dir = fat.make_dir(projects, name);
    fat.add_file(dir, "project.o3pj", content(100, 40 + i));
  }
  const std::uint32_t song = fat.make_dir(projects, kLongOld);
  fat.add_file(song, "project.o3pj", content(3000, 20));
  fat.add_file(song, "notes.txt", content(600, 21));
  for (unsigned i = 0; i < fillers; ++i) {
    char name[48];
    std::snprintf(name, sizeof name, "a long file name for the filler %03u.dat", i);
    fat.add_file(song, name, content(50, 80 + i));
  }
  const std::uint32_t samples = fat.make_dir(song, "samples");
  fat.add_file(samples, "instr01.wav", content(1500, 22));
  const std::uint32_t patterns = fat.make_dir(song, "patterns");
  fat.add_file(patterns, "pattern_01.mtp", content(1000, 24));
  const std::uint32_t other = fat.make_dir(projects, "Other");
  fat.add_file(other, "project.o3pj", content(700, 25));
}

// Every file of the Song project is there under `root`, byte for byte.
bool song_whole(hs::Storage& s, const std::string& root, unsigned fillers = 0) {
  for (const ProjectFiles& f : kSongFiles) {
    if (read_file(s, (root + "/" + f.name).c_str()) != content(f.size, f.salt)) {
      return false;
    }
  }
  for (unsigned i = 0; i < fillers; ++i) {
    char name[48];
    std::snprintf(name, sizeof name, "/a long file name for the filler %03u.dat", i);
    if (read_file(s, (root + name).c_str()) != content(50, 80 + i)) {
      return false;
    }
  }
  return true;
}

bool others_intact(hs::Storage& s, unsigned siblings = 0) {
  for (unsigned i = 0; i < siblings; ++i) {
    char path[96];
    std::snprintf(path, sizeof path, "Projects/Filler Project Number Eleven %03u/project.o3pj", i);
    if (read_file(s, path) != content(100, 40 + i)) {
      return false;
    }
  }
  return read_file(s, "README.TXT") == content(40, 1) && read_file(s, "Projects/Other/project.o3pj") == content(700, 25);
}

std::string songdir(const char* name) { return std::string("Projects/") + name; }

// The names listed in `dir` (what a File scan would see).
std::vector<std::string> listing(hs::Storage& s, const char* dir) {
  std::vector<std::string> out;
  hs::DirHandle d;
  if (s.opendir(dir, d) != hs::Status::Ok) {
    return out;
  }
  hs::DirEntry e;
  while (s.readdir(d, e) == hs::Status::Ok) {
    out.push_back(e.name);
  }
  s.closedir(d);
  return out;
}

}  // namespace

TEST_CASE("the .deleted marker over FatFs: whole, a fresh mount agrees, nothing else on the card changes, and the guard allows this and no folder rename") {
  Medium medium{200'000};
  build_moves(medium);
  {
    Stack stack(medium);
    NEED(stack.mount());
    // a project folder is not renamed by the layer: not inside Projects/, not by case, not to another folder; not one block is written
    const std::uint64_t none = medium.blocks_written;
    CHECK(stack.writer.rename(songdir(kLongOld).c_str(), "Projects/Renamed") == hs::WriteStatus::BadPath);
    CHECK(stack.writer.rename("Projects/Other", "Projects/other") == hs::WriteStatus::BadPath);
    CHECK(stack.writer.rename("Projects/Other", "Trash/Other") == hs::WriteStatus::BadPath);
    CHECK(medium.blocks_written == none);
    CHECK(song_whole(stack.fat, songdir(kLongOld)));
    // the marker: created empty, once; a second time is Exists, nothing touched
    CHECK(stack.writer.create_marker("Projects/Other/.deleted") == hs::WriteStatus::Ok);
    CHECK(read_file(stack.fat, "Projects/Other/.deleted").empty());
    CHECK(exists(stack.fat, "Projects/Other/.deleted"));
    const std::uint64_t before = medium.blocks_written;
    CHECK(stack.writer.create_marker("Projects/Other/.deleted") == hs::WriteStatus::Exists);
    CHECK(medium.blocks_written == before);
    CHECK(stack.writer.create_marker("Projects/Missing/.deleted") == hs::WriteStatus::NotFound);
    CHECK(read_file(stack.fat, "Projects/Other/project.o3pj") == content(700, 25));  // the project is whole beside it
    // the marker is never written to, replaced or removed by the layer
    CHECK(stack.writer.create("Projects/Other/.deleted") == hs::WriteStatus::BadPath);
    CHECK(stack.writer.remove("Projects/Other/.deleted") == hs::WriteStatus::BadPath);
    CHECK(stack.writer.rename("Projects/Other/.deleted", "Projects/Other/.deleted.bak") == hs::WriteStatus::BadPath);
  }
  {
    Stack boot(medium);  // the next boot
    NEED(boot.mount());
    CHECK(exists(boot.fat, "Projects/Other/.deleted"));
    CHECK(read_file(boot.fat, "Projects/Other/.deleted").empty());
    CHECK(song_whole(boot.fat, songdir(kLongOld)));
    CHECK(read_file(boot.fat, "README.TXT") == content(40, 1));
  }
  // The guard stops what it does not list, before FatFs is asked: not one block is written for any of these.
  Medium medium4{200'000};
  build_moves(medium4);
  Stack guarded(medium4);
  NEED(guarded.mount());
  const std::uint64_t before = medium4.blocks_written;
  for (const auto& pair : {std::pair<const char*, const char*>{"Projects/Other", "Samples/Other"}, {"Projects/Other", "Export/Other"}, {"Projects/Other", "Projects/Other/Other"}, {"Projects/Other", "Projects/../Other"},
                           {"Projects/Other", "Trash/Other"}, {"Projects/Other/samples", "Trash/Song_SAMPLES"}, {"Projects/Other/project.o3pj", "Trash/project.o3pj"},
                           {"Projects/Other/project.o3pj", "Projects/Song/project.o3pj"}, {"README.TXT", "Projects/README.TXT"}, {"Projects", "Trash"}, {"Projects/Other", "Projects/Other"}}) {
    INFO(pair.first << " -> " << pair.second);
    CHECK(guarded.writer.rename(pair.first, pair.second) == hs::WriteStatus::BadPath);
  }
  for (const char* path : {"Projects/.deleted", "Projects/Other/sub/.deleted", "Projects/Other/.deleted.bak", "Samples/.deleted", "Projects/Other/project.o3pj", "Trash/Other/.deleted"}) {
    INFO(path);
    CHECK(guarded.writer.create_marker(path) == hs::WriteStatus::BadPath);
  }
  CHECK(guarded.writer.make_dir("Trash") == hs::WriteStatus::BadPath);
  CHECK(medium4.blocks_written == before);
  CHECK(song_whole(guarded.fat, songdir(kLongOld)));
}

TEST_CASE("the .deleted marker over FatFs: a power cut at every block, long names beside it, eleven directory alignments: present or absent, the project whole, never a half-made entry that a listing shows") {
  const std::string root = songdir(kLongOld);
  const std::string marker = root + "/.deleted";
  unsigned min_blocks = 1000;
  unsigned max_blocks = 0;
  unsigned absent = 0;
  unsigned present = 0;
  unsigned cuts = 0;
  for (unsigned fillers = 0; fillers < 11; ++fillers) {
    std::uint64_t blocks = 0;
    {
      Medium medium{200'000};
      build_moves(medium, 0, fillers);
      Stack stack(medium);
      NEED(stack.mount());
      const std::uint64_t before = medium.blocks_written;
      NEED(stack.writer.create_marker(marker.c_str()) == hs::WriteStatus::Ok);
      blocks = medium.blocks_written - before;
    }
    min_blocks = blocks < min_blocks ? static_cast<unsigned>(blocks) : min_blocks;
    max_blocks = blocks > max_blocks ? static_cast<unsigned>(blocks) : max_blocks;
    for (std::uint64_t cut = 0; cut <= blocks; ++cut) {
      INFO("fillers " << fillers << ", power cut at block " << cut << " of " << blocks);
      Medium medium{200'000};
      build_moves(medium, 0, fillers);
      {
        Stack stack(medium);
        NEED(stack.mount());
        medium.fail_writes_after = static_cast<std::uint32_t>(cut);
        (void)stack.writer.create_marker(marker.c_str());
      }
      medium.fail_writes_after = 0xFFFFFFFFu;
      Stack boot(medium);
      NEED(boot.mount());
      CHECK(others_intact(boot.fat));
      CHECK(song_whole(boot.fat, root, fillers));  // the project is whole whichever way it went
      const bool there = exists(boot.fat, marker.c_str());
      const std::vector<std::string> names = listing(boot.fat, root.c_str());
      unsigned marks = 0;
      for (const std::string& n : names) {
        marks += n == ".deleted" ? 1u : 0u;
      }
      CHECK(marks == (there ? 1u : 0u));  // what a scan lists and what exists agree: no half entry shows
      if (there) {
        CHECK(read_file(boot.fat, marker.c_str()).empty());
      }
      ++cuts;
      present += there ? 1u : 0u;
      absent += there ? 0u : 1u;
      // the next DELETE works from whatever was left
      CHECK(boot.writer.create_marker(marker.c_str()) == (there ? hs::WriteStatus::Exists : hs::WriteStatus::Ok));
      CHECK(exists(boot.fat, marker.c_str()));
      CHECK(song_whole(boot.fat, root, fillers));
    }
  }
  MESSAGE("MARKER: " << cuts << " cuts over 11 alignments, " << min_blocks << ".." << max_blocks << " blocks a marker; absent " << absent << ", present " << present);
  CHECK(absent >= 1u);
  CHECK(present >= 1u);
}

TEST_CASE("the .deleted marker over FatFs: one block that fails once, at every block: Ok or Io, the project whole, and the next call settles it") {
  const std::string root = songdir(kLongOld);
  const std::string marker = root + "/.deleted";
  unsigned ok = 0;
  unsigned io = 0;
  for (unsigned alignment = 0; alignment < 11; ++alignment) {
    std::uint64_t blocks = 0;
    {
      Medium medium{200'000};
      build_moves(medium, 0, alignment);
      Stack stack(medium);
      NEED(stack.mount());
      const std::uint64_t before = medium.blocks_written;
      NEED(stack.writer.create_marker(marker.c_str()) == hs::WriteStatus::Ok);
      blocks = medium.blocks_written - before;
    }
    for (std::uint64_t at = 0; at < blocks; ++at) {
      INFO("alignment " << alignment << ", block " << at << " of " << blocks << " fails once");
      Medium medium{200'000};
      build_moves(medium, 0, alignment);
      Hiccup device(medium);
      st::FatStorage fat(device);
      sv::FatStorageWriter writer(fat, device);
      NEED(fat.mount() == hs::Status::Ok);
      device.fail_block = static_cast<std::int64_t>(at);
      const hs::WriteStatus said = writer.create_marker(marker.c_str());
      CHECK(song_whole(fat, root, alignment));
      CHECK(others_intact(fat));
      CHECK((said == hs::WriteStatus::Ok || said == hs::WriteStatus::Io));
      ok += said == hs::WriteStatus::Ok ? 1u : 0u;
      io += said == hs::WriteStatus::Io ? 1u : 0u;
      if (said == hs::WriteStatus::Ok) {
        CHECK(exists(fat, marker.c_str()));
      }
      const hs::WriteStatus again = writer.create_marker(marker.c_str());
      CHECK((again == hs::WriteStatus::Ok || again == hs::WriteStatus::Exists));
      CHECK(exists(fat, marker.c_str()));
    }
  }
  MESSAGE("MARKER with one block failing: Ok " << ok << ", Io " << io);
}
