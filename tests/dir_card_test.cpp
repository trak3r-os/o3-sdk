// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-1b: DirCard, the simulator's card (platform/host/dir_card.h): the read side of DirStorage and the write side of hal/storage_write.h over a scratch directory. What these tests hold
// down is what a save needs of a card and what the card must refuse: the real path guard in front of every change, a Temp that is created and never truncated, a rename that never replaces
// (FAT's rule, names compared without regard to case), a remove of a Temp or a Backup only, the gate, and a card pulled in the middle of a write.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/hal/save_guard.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/host/dir_card.h>

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace hs = o3sdk::hal::storage;
using o3sdk::platform::host::DirCard;
using o3sdk::platform::host::ScratchDir;

namespace {

void put_file(const fs::path& path, const char* text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string host_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every byte of a file of the card, read through the card's own read side.
bool card_read(DirCard& card, const char* path, std::string& out) {
  hs::FileHandle f;
  if (card.open(path, f) != hs::Status::Ok) {
    return false;
  }
  out.clear();
  for (;;) {
    char buf[64];
    std::size_t got = 0;
    if (card.read(f, buf, sizeof buf, got) != hs::Status::Ok) {
      card.close(f);
      return false;
    }
    if (got == 0) {
      break;
    }
    out.append(buf, got);
  }
  card.close(f);
  return true;
}

// A card over a fresh scratch directory, mounted, with "Projects" on it.
struct Rig {
  ScratchDir scratch{"dircard-test"};
  DirCard card{scratch.path()};
  Rig() {
    CHECK(scratch.ok());
    put_file(fs::path(scratch.path()) / "Projects" / "keep.txt", "k");
    CHECK(card.mount() == hs::Status::Ok);
  }
  fs::path host(const char* rel) const { return fs::path(scratch.path()) / rel; }
  // Saves `text` as Projects/<folder>/project.o3pj the way AtomicSave does: Temp, write, finish, rename to the final name.
  hs::WriteStatus save(const char* folder, const char* text) {
    const std::string dir = std::string("Projects/") + folder;
    const std::string temp = dir + "/_o3save.tmp";
    const std::string target = dir + "/project.o3pj";
    hs::WriteStatus s = card.make_dir(dir.c_str());
    if (s != hs::WriteStatus::Ok) {
      return s;
    }
    s = card.create(temp.c_str());
    if (s != hs::WriteStatus::Ok) {
      return s;
    }
    s = card.write(text, std::strlen(text));
    if (s != hs::WriteStatus::Ok) {
      card.abandon();
      return s;
    }
    s = card.finish();
    return s != hs::WriteStatus::Ok ? s : card.rename(temp.c_str(), target.c_str());
  }
};

}  // namespace

TEST_CASE("dir card: a scratch directory is made, filled from a fixture, and removed with everything in it") {
  fs::path where;
  {
    ScratchDir scratch("dircard-scratch");
    NEED(scratch.ok());
    where = scratch.path();
    CHECK(fs::is_directory(where));
    CHECK(where.filename().string().rfind("o3sdk-dircard-scratch-", 0) == 0u);

    ScratchDir fixture("dircard-fixture");
    NEED(fixture.ok());
    put_file(fs::path(fixture.path()) / "Projects" / "Song" / "project.o3pj", "native");
    put_file(fs::path(fixture.path()) / "readme.txt", "r");
    std::string error;
    CHECK(scratch.copy_from(fixture.path(), error));
    CHECK(error.empty());
    CHECK(host_text(where / "Projects" / "Song" / "project.o3pj") == "native");
    CHECK(host_text(where / "readme.txt") == "r");
    CHECK(host_text(fs::path(fixture.path()) / "readme.txt") == "r");  // a copy: the fixture stays

    CHECK_FALSE(scratch.copy_from((fs::path(fixture.path()) / "readme.txt").string(), error));  // a file is no fixture
    CHECK(error.find("is not a directory") != std::string::npos);
    CHECK_FALSE(scratch.copy_from((fs::path(fixture.path()) / "nowhere").string(), error));
  }
  CHECK_FALSE(fs::exists(where));  // gone with the object

  ScratchDir a("dircard-scratch"), b("dircard-scratch");
  CHECK(a.path() != b.path());  // every one is its own directory (two simulators of a --twice run, tests in parallel)
}

TEST_CASE("dir card: a fixture with a symbolic link, or a pipe, is refused, so no card path leads out of the scratch directory") {
  ScratchDir outside("dircard-outside");
  NEED(outside.ok());
  put_file(fs::path(outside.path()) / "secret.txt", "s");
  ScratchDir fixture("dircard-fixture");
  NEED(fixture.ok());
  put_file(fs::path(fixture.path()) / "Projects" / "Song" / "project.o3pj", "native");
  std::error_code ec;
  const auto refused = [&](const char* name) {
    ScratchDir scratch("dircard-scratch");
    std::string error;
    NEED(scratch.ok());
    CHECK_FALSE(scratch.copy_from(fixture.path(), error));
    CHECK_MESSAGE(error.find(name) != std::string::npos, name << ": " << error);
    CHECK(error.find("symbolic link") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(scratch.path()) / "Projects" / "Song" / name));  // nothing of the link was copied
  };

  fs::create_directory_symlink(outside.path(), fs::path(fixture.path()) / "Projects" / "Song" / "away", ec);  // a link to a directory outside
  NEED(!ec);
  refused("away");
  fs::remove(fs::path(fixture.path()) / "Projects" / "Song" / "away");

  fs::create_symlink(fs::path(outside.path()) / "secret.txt", fs::path(fixture.path()) / "peek.txt", ec);  // a link to a file outside
  NEED(!ec);
  refused("peek.txt");
  fs::create_symlink("nowhere", fs::path(fixture.path()) / "peek2.txt", ec);  // a dangling link counts too
  NEED(!ec);
  fs::remove(fs::path(fixture.path()) / "peek.txt");
  refused("peek2.txt");
  fs::remove(fs::path(fixture.path()) / "peek2.txt");

  // the fixture folder reached through a link is fine: what is copied are its regular files
  ScratchDir via("dircard-via");
  NEED(via.ok());
  const fs::path alias = fs::path(via.path()) / "alias";
  fs::create_directory_symlink(fixture.path(), alias, ec);
  NEED(!ec);
  ScratchDir scratch("dircard-scratch");
  NEED(scratch.ok());
  std::string error;
  CHECK(scratch.copy_from(alias.string(), error));
  CHECK(host_text(fs::path(scratch.path()) / "Projects" / "Song" / "project.o3pj") == "native");
  CHECK_FALSE(fs::is_symlink(fs::symlink_status(fs::path(scratch.path()) / "Projects")));

  // a pipe is neither a file nor a folder
  ScratchDir pipes("dircard-scratch");
  NEED(pipes.ok());
  NEED(::mkfifo((fs::path(fixture.path()) / "pipe").c_str(), 0600) == 0);
  CHECK_FALSE(pipes.copy_from(fixture.path(), error));
  CHECK(error.find("neither a regular file nor a directory") != std::string::npos);
  CHECK(fs::exists(outside.path()) == true);
  CHECK(host_text(fs::path(outside.path()) / "secret.txt") == "s");  // the outside was never touched
}

TEST_CASE("dir card: a fixture that contains the scratch directory is refused (it would copy itself)") {
  ScratchDir outer("dircard-outer");
  NEED(outer.ok());
  fs::create_directories(fs::path(outer.path()) / "tmp");
  put_file(fs::path(outer.path()) / "readme.txt", "r");
  // the scratch directory is made in the host's temporary folder (TMPDIR): put that inside `outer`, so that `outer` contains it
  const char* old = std::getenv("TMPDIR");
  const std::string saved = old != nullptr ? old : "";
  const fs::path tmp = fs::path(outer.path()) / "tmp";
  NEED(::setenv("TMPDIR", tmp.c_str(), 1) == 0);
  ScratchDir inner("dircard-inner");
  if (old != nullptr) {
    ::setenv("TMPDIR", saved.c_str(), 1);
  } else {
    ::unsetenv("TMPDIR");
  }
  NEED(inner.ok());
  NEED(fs::path(inner.path()).parent_path() == tmp);

  std::string error;
  CHECK_FALSE(inner.copy_from(outer.path(), error));  // a folder above it
  CHECK(error.find("contains the scratch directory") != std::string::npos);
  CHECK_FALSE(inner.copy_from(tmp.string(), error));  // its own parent
  CHECK(error.find("contains the scratch directory") != std::string::npos);
  CHECK_FALSE(inner.copy_from(inner.path(), error));  // itself
  CHECK(error.find("contains the scratch directory") != std::string::npos);
  CHECK(fs::is_empty(inner.path()));  // and nothing was copied before the refusal
  // a sibling folder is no problem
  put_file(fs::path(outer.path()) / "sib" / "a.txt", "a");
  CHECK(inner.copy_from((fs::path(outer.path()) / "sib").string(), error));
  CHECK(host_text(fs::path(inner.path()) / "a.txt") == "a");
}

TEST_CASE("dir card: the read side is the directory, case-insensitive, and the card is not mounted until it is") {
  ScratchDir scratch("dircard-test");
  NEED(scratch.ok());
  put_file(fs::path(scratch.path()) / "Projects" / "Song" / "project.o3pj", "native");
  DirCard card(scratch.path());
  bool is_dir = false;
  CHECK(card.card_present());
  CHECK_FALSE(card.mounted());
  CHECK(card.exists("Projects", &is_dir) == hs::Status::NotMounted);
  CHECK(card.mount() == hs::Status::Ok);
  CHECK(card.mounted());
  CHECK(card.volume().kind == hs::FsKind::HostDirectory);

  CHECK((card.exists("projects/SONG", &is_dir) == hs::Status::Ok && is_dir));
  CHECK((card.exists("PROJECTS/song/Project.O3PJ", &is_dir) == hs::Status::Ok && !is_dir));
  CHECK(card.exists("Projects/Gone", &is_dir) == hs::Status::NotFound);
  std::string text;
  CHECK((card_read(card, "projects/song/project.o3pj", text) && text == "native"));
  hs::DirHandle d;
  NEED(card.opendir("Projects", d) == hs::Status::Ok);
  hs::DirEntry e;
  NEED(card.readdir(d, e) == hs::Status::Ok);
  CHECK((std::string(e.name) == "Song" && e.is_dir));
  CHECK(card.readdir(d, e) == hs::Status::End);
  card.closedir(d);
  CHECK(card.exists("Projects", &is_dir) == hs::Status::Ok);

  DirCard nowhere((fs::path(scratch.path()) / "not-a-card").string());
  CHECK_FALSE(nowhere.card_present());
  CHECK(nowhere.mount() == hs::Status::NoCard);
}

TEST_CASE("dir card: a save is a Temp that is written, finished and renamed to its final name") {
  Rig r;
  CHECK(r.card.gate() == hs::WriteStatus::Ok);
  CHECK(r.save("Song", "version one") == hs::WriteStatus::Ok);
  std::string text;
  CHECK((card_read(r.card, "Projects/Song/project.o3pj", text) && text == "version one"));
  CHECK(host_text(r.host("Projects/Song/project.o3pj")) == "version one");
  CHECK_FALSE(fs::exists(r.host("Projects/Song/_o3save.tmp")));  // the rename took it
  CHECK_FALSE(r.card.writing());
  CHECK(r.card.violations() == 0u);

  // a later chunk is appended to the open file, and a longer one goes through in pieces
  CHECK(r.card.make_dir("Projects/Song") == hs::WriteStatus::Ok);  // a folder that is there is Ok
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK(r.card.writing());
  CHECK(r.card.write("ab", 2) == hs::WriteStatus::Ok);
  const std::string big(100000, 'x');
  CHECK(r.card.write(big.data(), big.size()) == hs::WriteStatus::Ok);
  CHECK(r.card.finish() == hs::WriteStatus::Ok);
  CHECK_FALSE(r.card.writing());
  CHECK(fs::file_size(r.host("Projects/Song/_o3save.tmp")) == 100002u);
  CHECK(r.card.write("x", 1) == hs::WriteStatus::NotFound);  // nothing is open
  CHECK(r.card.finish() == hs::WriteStatus::NotFound);
}

TEST_CASE("dir card: every call that changes the card goes through the path guard, and a refused one changes nothing") {
  Rig r;
  CHECK(r.card.make_dir("Foo") == hs::WriteStatus::BadPath);               // not a folder of the guard's list
  CHECK(r.card.make_dir("Projects/bad:name") == hs::WriteStatus::BadPath);  // not a name it takes
  CHECK(r.card.make_dir("Projects/Song/deeper/still") == hs::WriteStatus::BadPath);
  CHECK(r.card.create("Projects/Song/project.o3pj") == hs::WriteStatus::BadPath);  // a Target is never created: it appears by a rename
  CHECK(r.card.create("Projects/notes.txt") == hs::WriteStatus::BadPath);
  CHECK(r.card.create("Elsewhere/_o3save.tmp") == hs::WriteStatus::BadPath);
  CHECK(r.card.create_marker("Projects/Song/readme.txt") == hs::WriteStatus::BadPath);
  CHECK(r.card.remove("Projects/keep.txt") == hs::WriteStatus::BadPath);  // a file of the user's
  CHECK(r.card.remove("Projects/Song/project.o3pj") == hs::WriteStatus::BadPath);  // a Target
  CHECK(r.card.rename("Projects/keep.txt", "Projects/kept.txt") == hs::WriteStatus::BadPath);
  CHECK(r.card.violations() == 10u);
  CHECK(host_text(r.host("Projects/keep.txt")) == "k");
  CHECK_FALSE(fs::exists(r.host("Foo")));
  CHECK_FALSE(r.card.writing());

  // the guard decides before the gate and the disk: even with a card that is gone, an unguarded path is BadPath, not NoCard
  r.card.set_present(false);
  CHECK(r.card.make_dir("Foo") == hs::WriteStatus::BadPath);
  CHECK(r.card.make_dir("Projects/Song") == hs::WriteStatus::NoCard);
  CHECK(r.card.violations() == 11u);
}

TEST_CASE("dir card: a rename never replaces, a Temp is never truncated, and a folder cannot be made over a file") {
  Rig r;
  NEED(r.save("Song", "first") == hs::WriteStatus::Ok);

  // Temp -> Target onto a Target that is there: Exists, both files as they were
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK(r.card.write("second", 6) == hs::WriteStatus::Ok);
  CHECK(r.card.finish() == hs::WriteStatus::Ok);
  CHECK(r.card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::Exists);
  CHECK(r.card.rename("Projects/Song/_o3save.tmp", "projects/SONG/PROJECT.O3PJ") == hs::WriteStatus::Exists);  // FAT: the case does not make another name
  CHECK(host_text(r.host("Projects/Song/project.o3pj")) == "first");
  CHECK(host_text(r.host("Projects/Song/_o3save.tmp")) == "second");

  // a Temp that is there is not created again (never truncated), whatever the case
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Exists);
  CHECK(r.card.create("projects/song/_O3SAVE.TMP") == hs::WriteStatus::Exists);
  CHECK(host_text(r.host("Projects/Song/_o3save.tmp")) == "second");
  CHECK_FALSE(r.card.writing());

  // the replacing save of AtomicSave: the old Target becomes its Backup, then the Temp takes the final name
  CHECK(r.card.rename("Projects/Song/project.o3pj", "Projects/Song/project.o3pj.bak") == hs::WriteStatus::Ok);
  CHECK(r.card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::Ok);
  CHECK(host_text(r.host("Projects/Song/project.o3pj")) == "second");
  CHECK(host_text(r.host("Projects/Song/project.o3pj.bak")) == "first");
  // a restore onto the Target that is there is refused too
  CHECK(r.card.rename("Projects/Song/project.o3pj.bak", "Projects/Song/project.o3pj") == hs::WriteStatus::Exists);
  CHECK(host_text(r.host("Projects/Song/project.o3pj")) == "second");
  // a rename of what is not there
  CHECK(r.card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::NotFound);
  CHECK(r.card.violations() == 0u);

  // a parent that is not there is NotFound (nothing makes a parent), a name that is a file is in the way of a folder
  CHECK(r.card.create("Projects/Nowhere/_o3save.tmp") == hs::WriteStatus::NotFound);
  CHECK(r.card.make_dir("Projects/Song/samples/deep") == hs::WriteStatus::BadPath);
  CHECK(r.card.make_dir("Projects/Song/samples") == hs::WriteStatus::Ok);
  CHECK(r.card.make_dir("Projects/SONG/SAMPLES") == hs::WriteStatus::Ok);  // the folder that is there, in another case: Ok, and no second folder
  std::size_t entries = 0;
  for (const fs::directory_entry& e : fs::directory_iterator(r.host("Projects/Song"))) {
    entries += e.is_directory() ? 1u : 0u;
  }
  CHECK(entries == 1u);
  put_file(r.host("Projects/Taken"), "a file");
  CHECK(r.card.make_dir("Projects/Taken") == hs::WriteStatus::Exists);
}

TEST_CASE("dir card: one file is open for writing at a time, and remove takes a Temp or a Backup only") {
  Rig r;
  NEED(r.save("Song", "one") == hs::WriteStatus::Ok);
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK(r.card.create("Projects/Song/samples/_o3save.tmp") == hs::WriteStatus::Busy);
  CHECK(r.card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::Busy);
  CHECK(r.card.create_marker("Projects/Song/.deleted") == hs::WriteStatus::Busy);
  r.card.abandon();
  CHECK_FALSE(r.card.writing());
  CHECK(fs::exists(r.host("Projects/Song/_o3save.tmp")));  // abandon closes; the caller removes
  CHECK(r.card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK_FALSE(fs::exists(r.host("Projects/Song/_o3save.tmp")));
  CHECK(r.card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);  // not there: Ok

  NEED(r.card.rename("Projects/Song/project.o3pj", "Projects/Song/project.o3pj.bak") == hs::WriteStatus::Ok);
  CHECK(r.card.remove("projects/song/PROJECT.O3PJ.BAK") == hs::WriteStatus::Ok);  // a Backup, in any case
  CHECK_FALSE(fs::exists(r.host("Projects/Song/project.o3pj.bak")));
  CHECK(r.card.violations() == 0u);

  // the DELETE marker: one empty file, new, once
  CHECK(r.card.create_marker("Projects/Song/.deleted") == hs::WriteStatus::Ok);
  CHECK(fs::file_size(r.host("Projects/Song/.deleted")) == 0u);
  CHECK(r.card.create_marker("Projects/Song/.deleted") == hs::WriteStatus::Exists);
  CHECK(r.card.create_marker("Projects/Nowhere/.deleted") == hs::WriteStatus::NotFound);
  CHECK(r.card.remove("Projects/Song/.deleted") == hs::WriteStatus::BadPath);  // never removed
  CHECK(fs::exists(r.host("Projects/Song/.deleted")));
}

TEST_CASE("dir card: a card that is pulled answers NoCard, leaves the torn Temp where it was, and reads again when it is back") {
  Rig r;
  NEED(r.save("Song", "whole") == hs::WriteStatus::Ok);
  NEED(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  NEED(r.card.write("half of it", 10) == hs::WriteStatus::Ok);

  r.card.set_present(false);  // taken out in the middle of a write
  CHECK_FALSE(r.card.card_present());
  CHECK_FALSE(r.card.mounted());
  CHECK(r.card.gate() == hs::WriteStatus::NoCard);
  CHECK_FALSE(r.card.writing());  // the file was closed where it was
  CHECK(r.card.write("more", 4) == hs::WriteStatus::NotFound);
  CHECK(r.card.make_dir("Projects/Other") == hs::WriteStatus::NoCard);
  CHECK(r.card.create("Projects/Other/_o3save.tmp") == hs::WriteStatus::NoCard);
  CHECK(r.card.rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj") == hs::WriteStatus::NoCard);
  CHECK(r.card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::NoCard);
  bool is_dir = false;
  hs::FileHandle f;
  hs::DirHandle d;
  CHECK(r.card.exists("Projects", &is_dir) == hs::Status::NotMounted);
  CHECK(r.card.open("Projects/Song/project.o3pj", f) == hs::Status::NotMounted);
  CHECK(r.card.opendir("Projects", d) == hs::Status::NotMounted);
  CHECK(r.card.mount() == hs::Status::NoCard);
  CHECK(r.card.volume().kind == hs::FsKind::None);
  CHECK(r.card.violations() == 0u);
  CHECK(host_text(r.host("Projects/Song/_o3save.tmp")) == "half of it");  // the torn Temp is on the disk

  r.card.set_present(true);
  CHECK(r.card.card_present());
  CHECK_FALSE(r.card.mounted());  // in the slot, not mounted yet: the platform mounts it, then tells the app
  CHECK(r.card.gate() == hs::WriteStatus::NoCard);
  CHECK(r.card.mount() == hs::Status::Ok);
  CHECK(r.card.gate() == hs::WriteStatus::Ok);
  std::string text;
  CHECK((card_read(r.card, "Projects/Song/project.o3pj", text) && text == "whole"));
  CHECK((card_read(r.card, "Projects/Song/_o3save.tmp", text) && text == "half of it"));
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Exists);  // the save that was cut finds its Temp there
  CHECK(r.card.remove("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
  CHECK(r.save("Song2", "after") == hs::WriteStatus::Ok);
}

TEST_CASE("dir card: a save pins the card it began on, and another card is NoCard") {
  Rig r;
  CHECK(r.card.card_id() == 1u);
  r.card.begin_save();
  CHECK(r.card.make_dir("Projects/Song") == hs::WriteStatus::Ok);
  r.card.set_id(2);  // another card in the slot between two jobs of one save
  CHECK(r.card.card_id() == 2u);
  CHECK(r.card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::NoCard);
  CHECK(r.card.make_dir("Projects/Other") == hs::WriteStatus::NoCard);
  CHECK_FALSE(fs::exists(r.host("Projects/Other")));
  r.card.end_save();
  CHECK(r.card.make_dir("Projects/Other") == hs::WriteStatus::Ok);  // unpinned: the card that is there is the one written
  r.card.begin_save();                                               // and pins afresh
  CHECK(r.card.create("Projects/Other/_o3save.tmp") == hs::WriteStatus::Ok);
  r.card.abandon();
}

TEST_CASE("dir card: a file being written is let go with the card") {
  ScratchDir scratch("dircard-test");
  NEED(scratch.ok());
  put_file(fs::path(scratch.path()) / "Projects" / "Song" / "a.txt", "a");
  {
    DirCard card(scratch.path());
    NEED(card.mount() == hs::Status::Ok);
    NEED(card.create("Projects/Song/_o3save.tmp") == hs::WriteStatus::Ok);
    CHECK(card.write("x", 1) == hs::WriteStatus::Ok);
  }  // destroyed with the file open: it is closed, the directory is still there to be removed
  CHECK(fs::exists(fs::path(scratch.path()) / "Projects" / "Song" / "_o3save.tmp"));
}
