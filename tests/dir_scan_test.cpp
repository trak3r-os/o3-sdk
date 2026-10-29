// SPDX-License-Identifier: GPL-3.0-or-later
// OS-010a: the folder scan (app/dir_scan.h) over an in-memory card (platform/host FakeStorage): what the filters list, the order while it is read, a
// bounded amount of storage work in every step, a list that overflows, a card that fails, hostile names (PAR-033: a folder called ".wav"), and the
// job's place in the runner. Host only (the fake keeps std::string).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/dir_scan.h>
#include <o3sdk/core/job.h>
#include <o3sdk/platform/host/fake_storage.h>
#include <o3sdk/platform/host/fakes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3sdk::hal::storage;
using o3sdk::app::DirScan;
using o3sdk::app::kProjectFilter;
using o3sdk::app::kWavFilter;
using o3sdk::app::ListFilter;
using o3sdk::core::JobStatus;
using o3sdk::platform::host::FakeStorage;

namespace {

void add_project(FakeStorage& card, const std::string& name, const std::string& folder = "Projects") {
  card.add_dir(folder + "/" + name);
  card.add_file(folder + "/" + name + "/project.mt", 2320);
}

// Steps to the end; returns the steps taken. The bound is the test's own: a scan that never ends fails here, not by hanging.
unsigned run_all(DirScan& scan, std::uint32_t budget_us = 2000) {
  unsigned steps = 1;
  while (scan.step(budget_us) == JobStatus::Running) {
    if (++steps >= 20000u) {
      FAIL_CHECK("the scan did not end");
      break;
    }
  }
  return steps;
}

std::vector<std::string> names_of(const DirScan& scan) {
  std::vector<std::string> out;
  for (unsigned i = 0; i < scan.index().count(); ++i) {
    out.emplace_back(scan.index().name(i));
  }
  return out;
}

using Names = std::vector<std::string>;

}  // namespace

TEST_CASE("dir_scan: the project filter lists the folders that hold a project.mt, sorted, and nothing else") {
  FakeStorage card;
  add_project(card, "zeta");
  add_project(card, "Alpha");
  add_project(card, "mid");
  add_project(card, "kick.wav");  // a project whose name looks like a sample: it is a folder (PAR-033)
  card.add_dir("Projects/docs");   // no project.mt
  card.add_dir("Projects/empty");
  card.add_file("Projects/notes.txt");
  card.add_file("Projects/song.wav");     // a file called like a sample: not a project
  card.add_file("Projects/project.mt");   // a file called project.mt beside the projects: not a folder, not a project
  card.add_dir("Projects/fake");
  card.add_dir("Projects/fake/project.mt");  // a folder called project.mt: the probe must be a file
  add_project(card, ".Trash");               // hidden names are not listed even when they pass the probe
  card.add_file("Projects/._Alpha", 4096);   // what macOS leaves
  DirScan scan;
  CHECK(scan.state() == DirScan::State::Idle);
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.state() == DirScan::State::Scanning);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Done);
  CHECK(names_of(scan) == Names{"Alpha", "kick.wav", "mid", "zeta"});
  for (unsigned i = 0; i < scan.index().count(); ++i) {
    CHECK(scan.index().is_dir(i));
  }
  CHECK(scan.skipped() == 8u);  // docs, empty, notes.txt, song.wav, project.mt, fake, .Trash, ._Alpha
  CHECK(scan.error() == hs::Status::Ok);
  CHECK_FALSE(scan.truncated());
  CHECK(card.open_dirs() == 0u);  // the directory is closed at the end
  CHECK(scan.progress() == o3sdk::core::kJobProgressMax);
}

TEST_CASE("dir_scan: the wav filter lists every folder and the .wav files, by what the card says each is") {
  FakeStorage card;
  card.add_file("Samples/Kick.WAV");
  card.add_file("Samples/snare.wav");
  card.add_file("Samples/mp3.WaV");
  card.add_file("Samples/notes.txt");
  card.add_file("Samples/y.wav.bak");
  card.add_file("Samples/.wav");       // the extension alone is no name, and a hidden one
  card.add_file("Samples/wav");
  card.add_dir("Samples/x.wav");       // a folder named like a sample: listed as a folder (PAR-033: it must not be opened as a file)
  card.add_dir("Samples/drums");
  card.add_file("Samples/drums/a.wav");
  DirScan scan;
  NEED(scan.begin(card, "/Samples/", kWavFilter) == DirScan::Begin::Started);  // slashes around the path mean nothing
  run_all(scan);
  CHECK(names_of(scan) == Names{"drums", "x.wav", "Kick.WAV", "mp3.WaV", "snare.wav"});  // folders first
  CHECK(scan.index().is_dir(0));
  CHECK(scan.index().is_dir(1));
  CHECK_FALSE(scan.index().is_dir(2));
  CHECK(card.exists_calls() == 0u);  // no probe for this filter
  CHECK(DirScan::ends_with_ci("a.wav", ".wav"));
  CHECK(DirScan::ends_with_ci("A.WAV", ".wav"));
  CHECK_FALSE(DirScan::ends_with_ci(".wav", ".wav"));
  CHECK_FALSE(DirScan::ends_with_ci("wav", ".wav"));
  CHECK_FALSE(DirScan::ends_with_ci("a.wa", ".wav"));
  CHECK_FALSE(DirScan::ends_with_ci("a.wav", ""));
  const ListFilter all_hidden_too = {".wav", nullptr, false};
  NEED(scan.begin(card, "Samples", all_hidden_too) == DirScan::Begin::Started);  // a different filter is a different listing
  run_all(scan);
  CHECK_FALSE(DirScan::ends_with_ci(".wav", ".wav"));
  CHECK(scan.index().count() == 5u);  // the file called ".wav" has no stem: not listed even when hidden names are
}

TEST_CASE("dir_scan: the wav filter lists AIFF and 8SVX files and Surge's .wt too; ends_with_any_ci takes a list of extensions") {
  FakeStorage card;
  for (const char* n : {"a.wav", "b.AIF", "c.aiff", "d.Aifc", "e.8svx", "f.svx", "g.IFF", "h.wt", "i.aifx", "j.aif.bak", "k.mid"}) card.add_file((std::string("S/") + n).c_str());
  card.add_file("S/.aif");
  DirScan scan;
  NEED(scan.begin(card, "S", kWavFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(names_of(scan) == Names{"a.wav", "b.AIF", "c.aiff", "d.Aifc", "e.8svx", "f.svx", "g.IFF", "h.wt"});
  CHECK(DirScan::ends_with_any_ci("x.AIFF", ".wav .aif .aiff"));
  CHECK(DirScan::ends_with_any_ci("x.wav", ".wav .aif .aiff"));
  CHECK_FALSE(DirScan::ends_with_any_ci("x.aifc", ".wav .aif .aiff"));
  CHECK_FALSE(DirScan::ends_with_any_ci("x.aif", ""));
  CHECK_FALSE(DirScan::ends_with_any_ci(".aif", ".aif"));
  CHECK(DirScan::ends_with_any_ci("x.8svx", ".8svx"));
}

TEST_CASE("dir_scan: a step does a bounded amount of storage work, whatever it is offered") {
  FakeStorage card;
  for (unsigned i = 0; i < 200; ++i) {
    card.add_dir("Many/f" + std::to_string(1000 + i));
  }
  const ListFilter every_folder = {nullptr, nullptr, true};
  DirScan scan;
  NEED(scan.begin(card, "Many", every_folder) == DirScan::Begin::Started);
  CHECK(scan.step(0x7FFFFFFF) == JobStatus::Running);  // an enormous budget
  CHECK(card.readdirs() <= DirScan::kMaxUnitsPerStep);
  CHECK(card.readdirs() >= 1u);
  CHECK(scan.index().count() == card.readdirs());
  const unsigned after_first = card.readdirs();
  CHECK(scan.step(0) == JobStatus::Running);  // none: it still moves, by one
  CHECK(card.readdirs() == after_first + 1u);
  run_all(scan, 0x7FFFFFFF);
  CHECK(scan.index().count() == 200u);
  CHECK(scan.state() == DirScan::State::Done);
}

TEST_CASE("dir_scan: the pass of the app (2 ms) reads about one project a step, and the list is sorted after every one") {
  FakeStorage card;
  for (unsigned i = 0; i < 40; ++i) {
    add_project(card, "song-" + std::to_string(900 - 7 * i));  // arrives in descending order
  }
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  unsigned steps = 0;
  unsigned last_count = 0;
  unsigned last_exists = 0;
  while (scan.step(2000) == JobStatus::Running) {
    ++steps;
    CHECK(card.exists_calls() - last_exists <= 4u);  // at most one folder a step: its four probes (.deleted, project.o3pj, project.o3pj.bak, then project.mt: OS-FILES part B) cost 32 units
    last_exists = card.exists_calls();
    CHECK(scan.index().count() >= last_count);  // never shrinks
    last_count = scan.index().count();
    const Names now = names_of(scan);
    for (std::size_t i = 1; i < now.size(); ++i) {
      CHECK(now[i - 1] < now[i]);  // sorted while it is still being read (the names are all the same width)
    }
    if (steps >= 1000u) {
      FAIL_CHECK("the scan did not end");
      break;
    }
  }
  CHECK(steps >= 39u);  // not in one gulp
  CHECK(scan.index().count() == 40u);
  CHECK(card.exists_calls() == 160u);  // (every folder is asked for the .deleted marker first; an OG project is found by its fourth probe)
}

TEST_CASE("dir_scan: native and OG projects are told apart by their tag, the native file first (OS-007o)") {
  using o3sdk::app::EntryTag;
  FakeStorage card;
  add_project(card, "og only");
  card.add_dir("Projects/native only");
  card.add_file("Projects/native only/project.o3pj", 5000);
  add_project(card, "both");  // an OG folder the user saved over: the native file is the newer state
  card.add_file("Projects/both/project.o3pj", 5000);
  card.add_dir("Projects/neither");
  card.add_dir("Projects/folder named like it");
  card.add_dir("Projects/folder named like it/project.o3pj");  // the probe must be a file
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(names_of(scan) == Names{"both", "native only", "og only"});
  CHECK(scan.index().tag(0) == EntryTag::Native);
  CHECK(scan.index().tag(1) == EntryTag::Native);
  CHECK(scan.index().tag(2) == EntryTag::Og);
  CHECK(scan.index().tag(3) == EntryTag::None);  // outside the list
  // A filter with no native probe tags what it lists as OG, and the wav filter tags nothing.
  const ListFilter og_only = {nullptr, "project.mt", true};
  NEED(scan.begin(card, "Projects", og_only) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(names_of(scan) == Names{"both", "og only"});
  CHECK(scan.index().tag(0) == EntryTag::Og);
}

TEST_CASE("dir_scan: a folder with a backup and no project file is RECOVER, with the file it is native, the file wins over the OG one (OS-007o)") {
  using o3sdk::app::EntryTag;
  FakeStorage card;
  card.add_dir("Projects/cut save");  // the cut between the two renames: no project.o3pj, the old one as .bak, the new one in the temp
  card.add_file("Projects/cut save/project.o3pj.bak", 5000);
  card.add_file("Projects/cut save/_o3save.tmp", 5200);
  card.add_dir("Projects/saved twice");  // the normal state after any replacing save
  card.add_file("Projects/saved twice/project.o3pj", 5000);
  card.add_file("Projects/saved twice/project.o3pj.bak", 4900);
  add_project(card, "og with a stray bak");  // an OG folder; a native backup beside it (a recovered one) is a native project to recover
  card.add_file("Projects/og with a stray bak/project.o3pj.bak", 4000);
  add_project(card, "og");
  card.add_dir("Projects/only a temp");  // nothing of the user's: not a project
  card.add_file("Projects/only a temp/_o3save.tmp", 100);
  card.add_dir("Projects/bak is a folder");
  card.add_dir("Projects/bak is a folder/project.o3pj.bak");
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(names_of(scan) == Names{"cut save", "og", "og with a stray bak", "saved twice"});
  CHECK(scan.index().tag(0) == EntryTag::Recover);
  CHECK(scan.index().tag(1) == EntryTag::Og);
  CHECK(scan.index().tag(2) == EntryTag::Recover);
  CHECK(scan.index().tag(3) == EntryTag::Native);
}

TEST_CASE("dir_scan: a folder with more projects than the list holds fills it, says so, and stops reading") {
  FakeStorage card;
  for (unsigned i = 0; i < 300; ++i) {
    add_project(card, "p" + std::to_string(1000 + i));
  }
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Done);
  CHECK(scan.truncated());
  CHECK(scan.index().count() == o3sdk::app::DirIndex::kMaxEntries);
  CHECK(scan.index().full());
  CHECK(card.readdirs() <= o3sdk::app::DirIndex::kMaxEntries + 1u);  // it stopped at the first that did not fit
  CHECK(card.open_dirs() == 0u);
  CHECK(std::string(scan.index().name(0)) == "p1000");  // the first that arrived are the ones kept
}

TEST_CASE("dir_scan: a card or a folder that is not there is a state, not a crash") {
  FakeStorage card;
  card.add_dir("Workspace");
  card.add_file("Projects");  // a file where the folder should be
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Failed);
  CHECK(scan.state() == DirScan::State::NoFolder);
  CHECK(scan.error() == hs::Status::NotADirectory);
  CHECK(std::string(scan.detail()) == "NO FOLDER");
  CHECK(scan.step(2000) == JobStatus::Failed);  // stepping again changes nothing

  FakeStorage empty_card;
  NEED(scan.begin(empty_card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Failed);
  CHECK(scan.state() == DirScan::State::NoFolder);
  CHECK(scan.error() == hs::Status::NotFound);

  empty_card.set_present(false);
  NEED(scan.begin(empty_card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Failed);
  CHECK(scan.state() == DirScan::State::NoCard);
  CHECK(std::string(scan.detail()) == "NO CARD");
  CHECK(empty_card.open_dirs() == 0u);

  empty_card.set_present(true);  // present, never mounted
  empty_card.add_dir("Projects");
  (void)empty_card.mount();
  NEED(scan.begin(empty_card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Done);  // an empty Projects folder is a listing of nothing, not an error
  CHECK(scan.index().count() == 0u);
}

TEST_CASE("dir_scan: a card that fails midway ends the scan, keeps what was read and closes the directory") {
  FakeStorage card;
  for (unsigned i = 0; i < 10; ++i) {
    add_project(card, "p" + std::to_string(i));
  }
  card.fail_readdir_after(3, hs::Status::Io);
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Failed);
  CHECK(scan.error() == hs::Status::Io);
  CHECK(std::string(scan.detail()) == "CARD ERROR");
  CHECK(scan.index().count() == 3u);
  CHECK(card.open_dirs() == 0u);

  FakeStorage probe_card;
  for (unsigned i = 0; i < 5; ++i) {
    add_project(probe_card, "q" + std::to_string(i));
  }
  probe_card.fail_exists_of("project.mt", hs::Status::Io);  // an I/O error under the probe is the card's, not a verdict on the folder
  NEED(scan.begin(probe_card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Failed);
  CHECK(scan.index().count() == 0u);
  CHECK(probe_card.open_dirs() == 0u);
}

TEST_CASE("dir_scan: a card that is remounted under a scan fails it, and what it gives back is not a handle that is no longer its own") {
  FakeStorage card;
  for (unsigned i = 0; i < 10; ++i) {
    add_project(card, "p" + std::to_string(i));
  }
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Running);
  (void)card.mount();  // closes every handle, the scan's included
  CHECK(scan.step(2000) == JobStatus::Failed);  // BadHandle
  CHECK(scan.state() == DirScan::State::Failed);
  CHECK(scan.error() == hs::Status::BadHandle);
  hs::DirHandle other;  // someone else opens a directory: the slot the scan had
  NEED(card.opendir("Projects", other) == hs::Status::Ok);
  scan.abandon();
  scan.cancel();
  CHECK(card.open_dirs() == 1u);  // the scan did not close the other's directory
  card.closedir(other);
  CHECK(card.open_dirs() == 0u);
}

TEST_CASE("dir_scan: hostile names: a path too long to probe, a name FAT cannot hold, a name as long as a name goes") {
  FakeStorage card;
  std::string deep;  // a folder path of 249 bytes
  for (unsigned i = 0; i < 125; ++i) {
    deep += (i == 0 ? "d" : "/d");
  }
  NEED(deep.size() == 249u);
  add_project(card, std::string(255, 'n'), deep);  // 249 + 1 + 255 + 1 + 10 = 516 bytes: more than a path may be
  add_project(card, std::string(200, 'm'), deep);  // 461: fits
  add_project(card, "bad\tname", deep);            // a control byte: valid_path refuses it, so the probe says BadPath: skipped
  add_project(card, "caf\xC3\xA9", deep);          // UTF-8
  DirScan scan;
  NEED(scan.begin(card, deep.c_str(), kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  CHECK(scan.state() == DirScan::State::Done);
  CHECK(names_of(scan) == Names{"caf\xC3\xA9", std::string(200, 'm')});
  CHECK(scan.skipped() == 2u);

  FakeStorage long_card;
  add_project(long_card, std::string(255, 'L'));  // the longest name: listed whole
  NEED(scan.begin(long_card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  NEED(scan.index().count() == 1u);
  CHECK(scan.index().name_length(0) == 255u);
  CHECK(std::string(scan.index().name(0)) == std::string(255, 'L'));
}

TEST_CASE("dir_scan: begin refuses what is not a folder path and says when the listing is already there") {
  FakeStorage card;
  add_project(card, "a");
  DirScan scan;
  CHECK(scan.begin(card, "Pro:jects", kProjectFilter) == DirScan::Begin::BadPath);
  CHECK(scan.begin(card, "a/../b", kProjectFilter) == DirScan::Begin::BadPath);
  CHECK(scan.begin(card, nullptr, kProjectFilter) == DirScan::Begin::BadPath);
  CHECK(scan.begin(card, std::string(256, 'x').c_str(), kProjectFilter) == DirScan::Begin::BadPath);
  CHECK(scan.state() == DirScan::State::Idle);  // nothing changed
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  run_all(scan);
  const unsigned reads = card.readdirs();
  CHECK(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Cached);  // nothing to run
  CHECK(scan.begin(card, "/Projects/", kProjectFilter) == DirScan::Begin::Cached);
  CHECK(card.readdirs() == reads);
  CHECK(scan.index().count() == 1u);
  CHECK(scan.begin(card, "Projects", kWavFilter) == DirScan::Begin::Started);  // another filter
  run_all(scan);
  CHECK(scan.begin(card, "Other", kWavFilter) == DirScan::Begin::Started);       // another folder
  scan.invalidate();
  CHECK(scan.state() == DirScan::State::Idle);
  CHECK(scan.index().count() == 0u);
  CHECK(scan.begin(card, "Other", kWavFilter) == DirScan::Begin::Started);       // forgotten: it reads again
}

TEST_CASE("dir_scan: cancel and abandon give the directory back") {
  FakeStorage card;
  for (unsigned i = 0; i < 30; ++i) {
    add_project(card, "p" + std::to_string(100 + i));
  }
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Running);
  CHECK(card.open_dirs() == 1u);
  scan.cancel();
  CHECK(scan.state() == DirScan::State::Cancelled);
  CHECK(card.open_dirs() == 0u);
  CHECK(scan.index().count() >= 1u);  // what was read stays listed
  CHECK(scan.step(2000) == JobStatus::Failed);

  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  CHECK(scan.step(2000) == JobStatus::Running);
  CHECK(card.open_dirs() == 1u);
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);  // restarted while running: the old handle is closed first
  CHECK(card.open_dirs() == 0u);  // (the new scan opens its own at its first step)
  CHECK(scan.step(2000) == JobStatus::Running);
  CHECK(card.open_dirs() == 1u);
  scan.abandon();
  CHECK(card.open_dirs() == 0u);
}

TEST_CASE("dir_scan: it runs in the job runner, a pass at a time, and finishes") {
  FakeStorage card;
  for (unsigned i = 0; i < 40; ++i) {
    add_project(card, "song-" + std::to_string(100 + i));
  }
  o3sdk::platform::host::FakeClock clock;
  o3sdk::core::JobRunner runner;
  DirScan scan;
  NEED(scan.begin(card, "Projects", kProjectFilter) == DirScan::Begin::Started);
  const int slot = runner.start(scan);
  NEED(slot >= 0);
  unsigned passes = 0;
  while (runner.state(static_cast<unsigned>(slot)) == o3sdk::core::SlotState::Running) {
    (void)runner.run(clock, 2000);
    clock.advance(16000);
    if (++passes >= 1000u) {
      FAIL_CHECK("the job did not end");
      break;
    }
  }
  CHECK(runner.state(static_cast<unsigned>(slot)) == o3sdk::core::SlotState::Done);
  CHECK(passes >= 20u);  // about a project a pass: the screen is never held for the whole folder
  CHECK(runner.stats(static_cast<unsigned>(slot)).steps == passes);
  CHECK(scan.index().count() == 40u);
  runner.release(static_cast<unsigned>(slot));
  CHECK(std::string(scan.name()) == "SCAN");
}

TEST_CASE("dir_scan: OS-USBDISK 6.3: the folder Windows makes on a volume and the dot-files of a computer are never listed, whatever the filter") {
  FakeStorage card;
  card.add_dir("Samples/System Volume Information");
  card.add_dir("Samples/SYSTEM VOLUME INFORMATION");  // FAT names compare without case
  card.add_dir("Samples/System Volume Information 2");  // a folder of the user's that only starts like it
  card.add_file("Samples/System Volume Information.wav");
  card.add_dir("Samples/.Trashes");
  card.add_file("Samples/._kick.wav");
  card.add_file("Samples/kick.wav");
  card.add_dir("Samples/drums");
  for (const ListFilter& filter : {kWavFilter, ListFilter{".wav", nullptr, false}}) {  // (a filter that lists hidden names still does not list the volume folder)
    DirScan scan;
    NEED(scan.begin(card, "Samples", filter) == DirScan::Begin::Started);
    run_all(scan);
    const Names names = names_of(scan);
    CHECK(std::find(names.begin(), names.end(), "System Volume Information") == names.end());
    CHECK(std::find(names.begin(), names.end(), "SYSTEM VOLUME INFORMATION") == names.end());
    CHECK(std::find(names.begin(), names.end(), "System Volume Information 2") != names.end());
    CHECK(std::find(names.begin(), names.end(), "System Volume Information.wav") != names.end());
    CHECK(std::find(names.begin(), names.end(), "kick.wav") != names.end());
    CHECK(std::find(names.begin(), names.end(), "drums") != names.end());
  }
}
