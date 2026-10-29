// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/atomic_save.h (OS-007n): the save job against an in-memory card (support/mem_card.h) that applies the real path guard, with the faults of a card. The sequence,
// the refusals (the gate, an existing name, a source that fails, no space, a read-back that differs, a rename that is refused after the old file moved), the repairs, and
// the property that matters: a power cut at EVERY mutating call of a replacing save and of a new-file save leaves the old file or the new one whole, never a half-written
// final file, nothing deleted but our own temp and backup files, no unguarded call, and a later save heals and succeeds.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/save_source.h"

#include <o3sdk/app/atomic_save.h>

#include <cstdint>
#include <string>
#include <vector>

namespace app = o3sdk::app;
namespace hs = o3sdk::hal::storage;
using o3sdk::test::MemCard;
using o3sdk::test::VecSource;
using o3sdk::test::run;
using Bytes = MemCard::Bytes;
using Mode = app::AtomicSave::Mode;
using Error = app::AtomicSave::Error;

namespace {

constexpr const char* kDir = "Projects/Track 01";
constexpr const char* kFile = "Projects/Track 01/project.o3pj";
constexpr const char* kBak = "Projects/Track 01/project.o3pj.bak";
constexpr const char* kTmp = "Projects/Track 01/_o3save.tmp";

Bytes content(std::size_t n, unsigned salt) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<std::uint8_t>((i * 7u + salt * 13u + 1u) & 0xFFu);
  }
  return b;
}

struct Rig {
  MemCard card;
  app::AtomicSave job;
  bool begin(VecSource& source, Mode mode, const char* dir = kDir, const char* name = "project.o3pj") {
    return job.begin(card, card, dir, name, mode, source, "TRACK 01");
  }
};

// The card the replacing tests start from: an old project file, an older backup of it, and files of the user's that must never change.
void populate(MemCard& card, const Bytes& old_file, const Bytes* old_bak) {
  card.make_dir_now("Projects");
  card.make_dir_now(kDir);
  card.put(kFile, old_file);
  if (old_bak != nullptr) {
    card.put(kBak, *old_bak);
  }
  card.put("Projects/Track 01/project.mt", content(100, 90));
  card.put("Projects/Track 01/notes.txt", std::string("hello"));
  card.put("README.TXT", std::string("readme"));
  card.make_dir_now("Samples");
  card.put("Samples/mine.wav", content(300, 91));
}

void check_user_files_intact(const MemCard& card) {
  CHECK(card.same("Projects/Track 01/project.mt", content(100, 90)));
  CHECK(card.same("Projects/Track 01/notes.txt", Bytes{'h', 'e', 'l', 'l', 'o'}));
  CHECK(card.same("README.TXT", Bytes{'r', 'e', 'a', 'd', 'm', 'e'}));
  CHECK(card.same("Samples/mine.wav", content(300, 91)));
}

}  // namespace

TEST_CASE("atomic save: a new file on an empty card (NewOnly)") {
  Rig rig;
  const Bytes data = content(20000, 1);
  VecSource source(data);
  NEED(rig.begin(source, Mode::NewOnly));
  CHECK(rig.job.running());
  const unsigned steps = run(rig.job);
  CHECK(steps > 5);
  CHECK(rig.job.done());
  CHECK(rig.job.error() == Error::None);
  CHECK(rig.job.progress() == o3sdk::core::kJobProgressMax);
  CHECK(rig.card.same(kFile, data));
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK_FALSE(rig.card.has(kBak));
  CHECK(rig.card.dirs.count("projects") == 1);
  CHECK(rig.card.dirs.count("projects/track 01") == 1);
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed().empty());
  CHECK(std::string(rig.job.name()) == "SAVE TRACK 01");
  // It wrote in slices: no single write was more than a step's budget (8 KB at most), and there were several.
  unsigned writes = 0;
  for (const std::string& l : rig.card.log) {
    if (l.rfind("write ", 0) == 0) {
      ++writes;
      CHECK(std::stoul(l.substr(6)) <= app::AtomicSave::kMaxStepBytes);
    }
  }
  CHECK(writes >= 3);
  // The order of what it did to the card: folders, the temp, the bytes, the sync, then one rename and nothing else.
  CHECK(rig.card.log.front() == "mkdir Projects");
  CHECK(rig.card.log[1] == "mkdir Projects/Track 01");
  CHECK(rig.card.log[2] == std::string("create ") + kTmp);
  CHECK(rig.card.log[rig.card.log.size() - 2] == "finish");
  CHECK(rig.card.log.back() == std::string("rename ") + kTmp + ">" + kFile);
}

TEST_CASE("atomic save: Replace keeps the previous version as the backup and removes only the older backup") {
  Rig rig;
  const Bytes old_file = content(5000, 2);
  const Bytes old_bak = content(4000, 3);
  populate(rig.card, old_file, &old_bak);
  const Bytes data = content(9000, 4);
  VecSource source(data);
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.done());
  CHECK(rig.card.same(kFile, data));
  CHECK(rig.card.same(kBak, old_file));
  CHECK_FALSE(rig.card.has(kTmp));
  check_user_files_intact(rig.card);
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed() == std::vector<std::string>{kBak});  // the one deletion: our own previous backup
  CHECK_FALSE(rig.job.healed());
  // The last three calls are the replacement, in the documented order.
  const std::size_t n = rig.card.log.size();
  CHECK(rig.card.log[n - 3] == std::string("remove ") + kBak);
  CHECK(rig.card.log[n - 2] == std::string("rename ") + kFile + ">" + kBak);
  CHECK(rig.card.log[n - 1] == std::string("rename ") + kTmp + ">" + kFile);
}

TEST_CASE("atomic save: Replace without an old file or without an old backup") {
  {
    Rig rig;
    rig.card.make_dir_now("Projects");
    rig.card.make_dir_now(kDir);
    const Bytes data = content(3000, 5);
    VecSource source(data);
    NEED(rig.begin(source, Mode::Replace));
    run(rig.job);
    CHECK(rig.job.done());
    CHECK(rig.card.same(kFile, data));
    CHECK_FALSE(rig.card.has(kBak));
    CHECK(rig.card.removed().empty());
  }
  {
    Rig rig;
    const Bytes old_file = content(1000, 6);
    populate(rig.card, old_file, nullptr);
    const Bytes data = content(3000, 7);
    VecSource source(data);
    NEED(rig.begin(source, Mode::Replace));
    run(rig.job);
    CHECK(rig.job.done());
    CHECK(rig.card.same(kFile, data));
    CHECK(rig.card.same(kBak, old_file));
    CHECK(rig.card.removed().empty());
  }
}

TEST_CASE("atomic save: a name that exists is never replaced by NewOnly") {
  Rig rig;
  const Bytes old_file = content(1000, 8);
  populate(rig.card, old_file, nullptr);
  VecSource source(content(500, 9));
  NEED(rig.begin(source, Mode::NewOnly));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::Exists);
  CHECK(std::string(rig.job.detail()) == "NAME EXISTS");
  CHECK(rig.card.same(kFile, old_file));
  CHECK_FALSE(rig.card.has(kTmp));
  check_user_files_intact(rig.card);
  for (const std::string& l : rig.card.log) {
    CHECK(l.rfind("create", 0) != 0);  // it did not even start a temp file
  }
}

TEST_CASE("atomic save: the gate refuses before anything is touched") {
  Rig rig;
  rig.card.gate_status = hs::WriteStatus::WriteProtected;
  VecSource source(content(500, 10));
  NEED(rig.begin(source, Mode::NewOnly));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::WriteProtected);
  CHECK(std::string(rig.job.detail()) == "CARD WRITE PROTECTED");
  CHECK(rig.card.log.empty());
  CHECK(rig.card.files.empty());
  CHECK(rig.card.dirs.empty());
  CHECK(source.steps == 0);
  // The other gate answers map the same way.
  const std::pair<hs::WriteStatus, Error> map[] = {{hs::WriteStatus::NoCard, Error::NoCard}, {hs::WriteStatus::WriteProtected, Error::WriteProtected},
                                                   {hs::WriteStatus::Unsupported, Error::Unsupported}};
  for (const auto& m : map) {
    Rig other;
    other.card.gate_status = m.first;
    VecSource s(content(10, 1));
    NEED(other.begin(s, Mode::NewOnly));
    run(other.job);
    CHECK(other.job.error() == m.second);
    CHECK(other.card.log.empty());
  }
}

TEST_CASE("atomic save: the gate closing mid-save (the card made write protected) stops the write and cleans up") {
  Rig rig;
  const Bytes old_file = content(1000, 11);
  populate(rig.card, old_file, nullptr);
  VecSource source(content(30000, 12));
  NEED(rig.begin(source, Mode::Replace));
  for (int i = 0; i < 8 && rig.job.running(); ++i) {
    rig.job.step(2000);
  }
  NEED(rig.job.running());
  rig.card.gate_status = hs::WriteStatus::WriteProtected;
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::WriteProtected);
  CHECK(rig.card.same(kFile, old_file));  // (the temp stays: nothing may be removed on a gated card, and nothing reads it)
  check_user_files_intact(rig.card);
}

TEST_CASE("atomic save: the source failing removes the temp and leaves the old file") {
  Rig rig;
  const Bytes old_file = content(1000, 13);
  const Bytes old_bak = content(900, 14);
  populate(rig.card, old_file, &old_bak);
  VecSource source(content(20000, 15));
  source.fail_after = 9000;
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::Source);
  CHECK(std::string(rig.job.detail()) == "TEST SOURCE FAILED");
  CHECK(rig.card.same(kFile, old_file));
  CHECK(rig.card.same(kBak, old_bak));  // the backup is untouched until the new file is checked
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK(rig.card.removed() == std::vector<std::string>{kTmp});
  check_user_files_intact(rig.card);
}

TEST_CASE("atomic save: a full card removes the temp and leaves the old file") {
  Rig rig;
  const Bytes old_file = content(1000, 16);
  populate(rig.card, old_file, nullptr);
  rig.card.space_left = 10000;
  VecSource source(content(20000, 17));
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::NoSpace);
  CHECK(std::string(rig.job.detail()) == "CARD IS FULL");
  CHECK(rig.card.same(kFile, old_file));
  CHECK_FALSE(rig.card.has(kTmp));
  check_user_files_intact(rig.card);
}

TEST_CASE("atomic save: a read-back that differs fails before the old file or its backup is touched") {
  for (int mode = 0; mode < 2; ++mode) {
    Rig rig;
    const Bytes old_file = content(1000, 18);
    const Bytes old_bak = content(900, 19);
    populate(rig.card, old_file, &old_bak);
    VecSource source(content(8000, 20));
    if (mode == 0) {
      rig.card.corrupt_readback = true;
    } else {
      rig.card.short_readback = true;
    }
    NEED(rig.begin(source, Mode::Replace));
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.job.error() == Error::Verify);
    CHECK(rig.card.same(kFile, old_file));
    CHECK(rig.card.same(kBak, old_bak));
    CHECK_FALSE(rig.card.has(kTmp));
    check_user_files_intact(rig.card);
  }
}

TEST_CASE("atomic save: no file handle to read back with is Busy, and cleans up") {
  Rig rig;
  populate(rig.card, content(100, 21), nullptr);
  rig.card.open_status = hs::Status::NoHandle;
  VecSource source(content(3000, 22));
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::Busy);
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK(rig.card.same(kFile, content(100, 21)));
}

TEST_CASE("atomic save: the last rename failing puts the old file back") {
  Rig rig;
  const Bytes old_file = content(1000, 23);
  const Bytes old_bak = content(900, 24);
  populate(rig.card, old_file, &old_bak);
  VecSource source(content(4000, 25));
  NEED(rig.begin(source, Mode::Replace));
  // Find the index of the last mutating call of an uninterrupted run: the commit rename (T -> F).
  {
    Rig probe;
    populate(probe.card, old_file, &old_bak);
    VecSource s(content(4000, 25));
    NEED(probe.begin(s, Mode::Replace));
    run(probe.job);
    NEED(probe.job.done());
    rig.card.fail_at = probe.card.ops - 1;
  }
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::Io);
  CHECK(rig.card.same(kFile, old_file));  // moved back from the backup
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK_FALSE(rig.card.has(kBak));        // the older backup was dropped for the new file that did not arrive: the old file is the one that remains, whole
  check_user_files_intact(rig.card);
  CHECK(rig.card.violations == 0);
}

TEST_CASE("atomic save: heal puts the backup back when an earlier save was cut between the two renames") {
  Rig rig;
  const Bytes old_file = content(1000, 26);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now(kDir);
  rig.card.put(kBak, old_file);               // F was moved to B ...
  rig.card.put(kTmp, content(2000, 27));      // ... the new file is complete in T ... and the power went
  const Bytes data = content(3000, 28);
  VecSource source(data);
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.done());
  CHECK(rig.job.healed());
  CHECK(rig.card.same(kFile, data));
  CHECK(rig.card.same(kBak, old_file));       // the healed old file became the backup of the new one
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("atomic save: a stale temp is removed, a folder or a bad path is refused") {
  {
    Rig rig;
    populate(rig.card, content(100, 29), nullptr);
    rig.card.put(kTmp, content(77, 30));
    VecSource source(content(3000, 31));
    NEED(rig.begin(source, Mode::Replace));
    run(rig.job);
    CHECK(rig.job.done());
    CHECK(rig.card.removed() == std::vector<std::string>{kTmp});
    CHECK_FALSE(rig.card.has(kTmp));
  }
  {
    Rig rig;
    VecSource source(content(10, 1));
    CHECK_FALSE(rig.begin(source, Mode::NewOnly, "Firmware", "x.ptf"));
    CHECK_FALSE(rig.begin(source, Mode::NewOnly, "Projects/Track 01", "project.mt"));
    CHECK_FALSE(rig.begin(source, Mode::NewOnly, "Projects/..", "project.o3pj"));
    CHECK_FALSE(rig.begin(source, Mode::NewOnly, "Samples", "CON.wav"));
    CHECK_FALSE(rig.begin(source, Mode::Replace, "Samples", "REC_001.wav"));  // a WAV has no backup form: never replaced
    CHECK_FALSE(rig.begin(source, Mode::NewOnly, nullptr, "x.wav"));
    CHECK(rig.begin(source, Mode::NewOnly, "Samples", "REC_001.wav"));
    CHECK(rig.card.log.empty());
  }
  {
    Rig rig;  // a folder where the file belongs
    rig.card.make_dir_now("Projects");
    rig.card.make_dir_now(kDir);
    rig.card.make_dir_now(kFile);
    VecSource source(content(10, 1));
    NEED(rig.begin(source, Mode::Replace));
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.card.removed().empty());
  }
}

TEST_CASE("atomic save: cross-linked names (the writer refuses the removal) end the save with \"card needs a check\" and change nothing") {
  Rig rig;
  const Bytes old_file = content(1000, 34);
  populate(rig.card, old_file, nullptr);
  rig.card.put(kTmp, content(50, 35));  // the stale temp of a cut save
  rig.card.fail_at = 2;                 // mkdir Projects (0), mkdir Projects/Track 01 (1), then the removal of the stale temp
  rig.card.fail_with = hs::WriteStatus::Damaged;
  VecSource source(content(3000, 36));
  NEED(rig.begin(source, Mode::Replace));
  run(rig.job);
  CHECK(rig.job.failed());
  CHECK(rig.job.error() == Error::Damaged);
  CHECK(std::string(rig.job.detail()) == "CARD NEEDS A CHECK");
  CHECK(rig.card.same(kFile, old_file));
  CHECK(rig.card.same(kTmp, content(50, 35)));  // nothing was removed, nothing written
  check_user_files_intact(rig.card);
  CHECK(source.steps == 0);
}

TEST_CASE("atomic save: cancel at any step leaves the card as a failure would") {
  const Bytes old_file = content(1000, 32);
  for (unsigned stop = 0; stop < 40; ++stop) {
    Rig rig;
    populate(rig.card, old_file, nullptr);
    VecSource source(content(6000, 33));
    NEED(rig.begin(source, Mode::Replace));
    for (unsigned i = 0; i < stop && rig.job.running(); ++i) {
      rig.job.step(2000);
    }
    const bool was_running = rig.job.running();
    rig.job.cancel();
    if (was_running) {
      CHECK(rig.job.failed());
      CHECK(rig.job.error() == Error::None);
    }
    CHECK_FALSE(rig.card.writing());
    CHECK_FALSE(rig.card.has(kTmp));
    if (!rig.job.done()) {
      CHECK(rig.card.same(kFile, old_file));
    }
    check_user_files_intact(rig.card);
    CHECK(rig.card.violations == 0);
  }
}

// ---- Power loss -----------------------------------------------------------------------------------------------------------------------------------

namespace {

// What the sweeps check after every cut and the reboot, and what a later save of the same file does.
struct Sweep {
  Bytes old_file;
  const Bytes* old_bak = nullptr;
  Bytes fresh;
  Mode mode = Mode::Replace;  // (the sweep is for replacing saves; the new-file one has its own test)
  const char* dir = kDir;
  const char* file = kFile;
  const char* name = "project.o3pj";
  bool had_old = true;

  void setup(MemCard& card) const {
    populate(card, old_file, old_bak);
    if (!had_old) {
      card.files.erase(MemCard::key(file));
    }
  }
};

void sweep(const Sweep& s) {
  // How many mutating calls an uninterrupted save makes.
  std::size_t total = 0;
  {
    Rig rig;
    s.setup(rig.card);
    VecSource source(s.fresh);
    NEED(rig.job.begin(rig.card, rig.card, s.dir, s.name, s.mode, source, "TRACK 01"));
    run(rig.job);
    NEED(rig.job.done());
    total = rig.card.ops;
  }
  CHECK(total >= 8);
  for (std::size_t cut = 0; cut <= total; ++cut) {
    INFO("power cut at mutating call " << cut << " of " << total);
    Rig rig;
    s.setup(rig.card);
    rig.card.cut_at = cut;
    VecSource source(s.fresh);
    NEED(rig.job.begin(rig.card, rig.card, s.dir, s.name, s.mode, source, "TRACK 01"));
    run(rig.job);
    const bool finished = rig.job.done();

    // ---- the card as the power left it
    CHECK(rig.card.violations == 0);
    const Bytes* f = rig.card.get(s.file);
    if (f != nullptr) {
      CHECK((*f == s.fresh || (s.had_old && *f == s.old_file)));  // never a half-written final file
    }
    if (s.mode == Mode::Replace && s.had_old) {
      const bool old_survives = (f != nullptr && *f == s.old_file) || (rig.card.same(kBak, s.old_file));
      const bool new_arrived = f != nullptr && *f == s.fresh;
      CHECK((old_survives || new_arrived));  // the user's data is whole in at least one name
      if (f == nullptr) {
        CHECK(rig.card.same(kBak, s.old_file));  // the window between the two renames: B is the old file
      }
    }
    if (finished) {
      CHECK(rig.card.same(s.file, s.fresh));
    }
    check_user_files_intact(rig.card);
    for (const std::string& removed : rig.card.removed()) {  // only ours
      CHECK((removed == kTmp || removed == kBak));
    }

    // ---- the next boot, and a later save of the same file with other content
    rig.card.revive();
    const Bytes later = content(7000, 77);
    VecSource again(later);
    NEED(rig.job.begin(rig.card, rig.card, s.dir, s.name, Mode::Replace, again, "TRACK 01"));
    run(rig.job);
    CHECK(rig.job.done());
    CHECK(rig.card.same(s.file, later));
    CHECK_FALSE(rig.card.has(kTmp));
    if (s.had_old) {
      CHECK(rig.card.has(kBak));  // some previous version is kept
    }
    CHECK(rig.card.violations == 0);
    check_user_files_intact(rig.card);
  }
}

}  // namespace

TEST_CASE("atomic save: a power cut at every step of a replacing save (old file and old backup)") {
  Sweep s;
  s.old_file = content(5000, 40);
  const Bytes bak = content(4000, 41);
  s.old_bak = &bak;
  s.fresh = content(9000, 42);
  sweep(s);
}

TEST_CASE("atomic save: a power cut at every step of a replacing save (old file, no backup)") {
  Sweep s;
  s.old_file = content(3000, 43);
  s.fresh = content(10000, 44);
  sweep(s);
}

TEST_CASE("atomic save: a power cut at every step of the first save of a file") {
  Sweep s;
  s.had_old = false;
  s.fresh = content(10000, 45);
  sweep(s);
}

TEST_CASE("atomic save: a power cut at every step of a new-file save (a WAV): the final name never holds a partial file") {
  const Bytes data = content(20000, 46);
  std::size_t total = 0;
  {
    Rig rig;
    populate(rig.card, content(10, 1), nullptr);
    VecSource source(data);
    NEED(rig.job.begin(rig.card, rig.card, "Samples", "REC_001.wav", Mode::NewOnly, source, "REC_001"));
    run(rig.job);
    NEED(rig.job.done());
    total = rig.card.ops;
  }
  for (std::size_t cut = 0; cut <= total; ++cut) {
    INFO("power cut at mutating call " << cut << " of " << total);
    Rig rig;
    populate(rig.card, content(10, 1), nullptr);
    rig.card.cut_at = cut;
    VecSource source(data);
    NEED(rig.job.begin(rig.card, rig.card, "Samples", "REC_001.wav", Mode::NewOnly, source, "REC_001"));
    run(rig.job);
    const Bytes* f = rig.card.get("Samples/REC_001.wav");
    CHECK((f == nullptr || *f == data));
    CHECK(rig.card.violations == 0);
    CHECK(rig.card.same("Samples/mine.wav", content(300, 91)));
    CHECK(rig.card.removed().empty());  // a new-file save never removes anything unless it is cleaning up its own temp
    rig.card.revive();
    VecSource again(data);
    NEED(rig.job.begin(rig.card, rig.card, "Samples", "REC_002.wav", Mode::NewOnly, again, "REC_002"));
    run(rig.job);
    CHECK(rig.job.done());
    CHECK(rig.card.same("Samples/REC_002.wav", data));
    CHECK_FALSE(rig.card.has("Samples/_o3save.tmp"));
    CHECK(rig.card.same("Samples/mine.wav", content(300, 91)));
    for (const std::string& removed : rig.card.removed()) {
      CHECK(removed == "Samples/_o3save.tmp");
    }
  }
}

TEST_CASE("atomic save: a failing write or rename at every step (the card survives) leaves the old file and cleans up") {
  const Bytes old_file = content(2500, 47);
  const Bytes bak = content(2000, 48);
  const Bytes fresh = content(9000, 49);
  std::size_t total = 0;
  {
    Rig rig;
    populate(rig.card, old_file, &bak);
    VecSource source(fresh);
    NEED(rig.job.begin(rig.card, rig.card, kDir, "project.o3pj", Mode::Replace, source, "T"));
    run(rig.job);
    NEED(rig.job.done());
    total = rig.card.ops;
  }
  for (std::size_t at = 0; at < total; ++at) {
    INFO("one failing call: " << at << " of " << total);
    Rig rig;
    populate(rig.card, old_file, &bak);
    rig.card.fail_at = at;
    VecSource source(fresh);
    NEED(rig.job.begin(rig.card, rig.card, kDir, "project.o3pj", Mode::Replace, source, "T"));
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.card.violations == 0);
    CHECK(rig.card.same(kFile, old_file));
    CHECK_FALSE(rig.card.has(kTmp));
    CHECK_FALSE(rig.card.writing());
    // The backup: kept, or (when the failure came after it was dropped for the new file) gone: the old file itself is what remains, whole.
    check_user_files_intact(rig.card);
    for (const std::string& removed : rig.card.removed()) {
      CHECK((removed == kTmp || removed == kBak));
    }
  }
}
