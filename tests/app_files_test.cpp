// SPDX-License-Identifier: GPL-3.0-or-later
// The app file API (app/files.h, docs/M2C-PLAN.md section 3): saves, loads, listings, the project folders and their DELETE marker, WAV saves, cancel, a card that goes out or is
// protected or is the computer's, a power cut at every write of a save, and the kit's own settings (the debounce, the yield to an app's save, the power-off flush). The card is the
// in-memory MemCard (faults, cuts, a log of every change) or, where names and sizes matter, a real directory (DirCard).
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"

#include <o3sdk/app/files.h>
#include <o3sdk/app/files_work.h>
#include <o3sdk/app/settings.h>
#include <o3sdk/formats/chunk_file.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fakes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace o3sdk;
using app::Files;
using app::KitValues;
namespace hst = o3sdk::hal::storage;
namespace host = o3sdk::platform::host;
using o3sdk::test::MemCard;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes pattern(std::size_t n, unsigned seed = 1) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<std::uint8_t>(i * 31u + seed * 17u + (i >> 8));
  }
  return b;
}

bool claim_flag = false;
bool claim_fn() noexcept { return claim_flag; }

// One files service over a card, a clock a millisecond a pass, and the kit's values it keeps.
struct Rig {
  std::unique_ptr<Files::Work> work = std::make_unique<Files::Work>();
  Files files;
  host::FakeClock clock;
  KitValues values;
  std::uint64_t passes = 0;

  Rig(hst::Storage& storage, hst::StorageWriter& writer, const char* id = "test") {
    claim_flag = false;
    files.bind(&storage, &writer, work.get(), id, &claim_fn);
    clock.set(1000);
  }
  void pass() {
    clock.advance(core::kTicksPerMs);
    files.poll(clock, Files::kJobBudgetUs, values);
    KitValues loaded;
    if (files.take_loaded(loaded)) {  // what Runtime does with the settings of a card put in after the boot
      values = loaded;
    }
    ++passes;
  }
  // Passes until nothing is running (at most `cap`); the number of passes it took.
  unsigned pump(unsigned cap = 5000) {
    unsigned n = 0;
    do {
      pass();
    } while (files.busy() && ++n < cap);
    return n + 1u;
  }
  void idle_ms(unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
      pass();
    }
  }
  bool done(const char* text) const { return files.status().phase == Files::Status::Done && std::strcmp(files.status().text, text) == 0; }
  bool failed(const char* text) const { return files.status().phase == Files::Status::Failed && std::strcmp(files.status().text, text) == 0; }
};

// The card of the tests that look at what was written: Apps/test, with its folders there.
void seed(MemCard& card) {
  card.make_dir_now("Apps");
  card.make_dir_now("Apps/test");
}

// Every change the files service made was inside Apps/<id>/ or Samples/ (docs/M2C-PLAN.md section 3: the guard's rows, and nothing else), and the guard refused nothing.
void check_scope(const MemCard& card, const char* id = "test") {
  CHECK(card.violations == 0u);
  const std::string apps = std::string("apps/") + id;
  for (const std::string& line : card.log) {
    const std::size_t sp = line.find(' ');
    if (sp == std::string::npos || line.compare(0, 5, "write") == 0) {
      continue;
    }
    std::string rest = line.substr(sp + 1);  // "A" or, for a rename, "A>B": every path of it is in scope
    while (!rest.empty()) {
      const std::size_t gt = rest.find('>');
      const std::string one = MemCard::key(rest.substr(0, gt).c_str());
      CAPTURE(line);
      CHECK(((one == "apps" || one == apps || one.rfind(apps + "/", 0) == 0) || one == "samples" || one.rfind("samples/", 0) == 0));
      rest = gt == std::string::npos ? std::string() : rest.substr(gt + 1);
    }
  }
}

KitValues changed_values_for_scope() {
  KitValues v;
  v.theme = ui::ThemeId::Black;
  v.midi.channel = 5;
  return v;
}

void write_host_file(const std::string& path, const Bytes& b) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A sink that takes a few bytes at a time and is full every third call.
class SlowSink final : public core::ByteSink {
 public:
  core::IoResult write(const void* src, std::size_t n) noexcept override {
    if (++calls % 3 == 0) {
      return {0, core::IoStatus::WouldBlock};
    }
    const std::size_t take = n < 7 ? n : 7;
    got.insert(got.end(), static_cast<const std::uint8_t*>(src), static_cast<const std::uint8_t*>(src) + take);
    return {take, core::IoStatus::Ok};
  }
  Bytes got;
  unsigned calls = 0;
};

class FailingSink final : public core::ByteSink {
 public:
  core::IoResult write(const void*, std::size_t) noexcept override { return {0, core::IoStatus::Failed}; }
};

class CountedSource final : public app::SaveSource {
 public:
  void begin(core::ByteSink& sink) noexcept override {
    sink_ = &sink;
    pos_ = 0;
    ++begins;
  }
  formats::Progress step(std::size_t budget) noexcept override {
    while (budget != 0 && pos_ < data.size()) {
      const std::size_t n = std::min(budget, data.size() - pos_);
      const core::IoResult r = sink_->write(data.data() + pos_, n);
      if (r.status != core::IoStatus::Ok) {
        return formats::Progress::Failed;
      }
      pos_ += r.count;
      budget -= r.count;
    }
    return pos_ >= data.size() ? formats::Progress::Done : formats::Progress::Running;
  }
  void end() noexcept override { ++ends; }
  Bytes data;
  unsigned begins = 0;
  unsigned ends = 0;

 private:
  core::ByteSink* sink_ = nullptr;
  std::size_t pos_ = 0;
};

}  // namespace

// ---- Saves and loads ------------------------------------------------------------------------------------------------------------

TEST_CASE("files: a save writes the file whole, the second keeps the first as .bak, a load reads it back") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  NEED(rig.files.available());
  CHECK(rig.files.card() == Files::Card::Ready);
  const Bytes one = pattern(3000, 1);
  const Bytes two = pattern(5000, 2);

  CHECK(rig.files.status().phase == Files::Status::Idle);
  REQUIRE(rig.files.save("", "riff.o3a", one.data(), one.size()));
  CHECK(rig.files.busy());
  CHECK(rig.files.status().phase == Files::Status::Running);
  CHECK(std::strcmp(rig.files.status().text, "SAVE") == 0);
  CHECK_FALSE(card.has("Apps/test/riff.o3a"));  // the call only recorded the request
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(rig.files.status().permille == 1000);
  CHECK(card.same("Apps/test/riff.o3a", one));
  CHECK_FALSE(card.has("Apps/test/riff.o3a.bak"));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  CHECK(rig.files.stats().jobs == 1);
  CHECK(rig.files.stats().last_bytes == one.size());

  REQUIRE(rig.files.save("", "riff.o3a", two.data(), two.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/riff.o3a", two));
  CHECK(card.same("Apps/test/riff.o3a.bak", one));

  Bytes back(8000, 0xAA);
  REQUIRE(rig.files.load("", "riff.o3a", back.data(), back.size()));
  rig.pump();
  CHECK(rig.done("LOADED"));
  CHECK(rig.files.loaded() == two.size());
  CHECK(std::equal(two.begin(), two.end(), back.begin()));
  CHECK(back[two.size()] == 0xAA);  // nothing past the file is touched
  CHECK_FALSE(rig.files.recovered());
  CHECK(card.open_handles == 0);
  check_scope(card);
}

TEST_CASE("files: a save is a job of the main loop, a bounded amount of card work a pass, and the writer is told where it begins and ends") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes big = pattern(100 * 1024, 3);
  REQUIRE(rig.files.save("", "big.o3a", big.data(), big.size()));
  const unsigned passes = rig.pump();
  CHECK(passes > 12);  // not one pass: the render and the screen go on between its slices
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/big.o3a", big));
  for (const std::string& line : card.log) {
    if (line.compare(0, 6, "write ") == 0) {
      CHECK(std::stoul(line.substr(6)) <= app::AtomicSave::kMaxStepBytes);  // a write is at most the budget's worth (512 .. 8192)
    }
  }
  CHECK(card.begins == 1);
  CHECK(card.ends == 1);
  CHECK(rig.files.stats().steps >= passes - 1);
}

TEST_CASE("files: what the guard or the arguments refuse is refused at once, with a reason, and starts nothing") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(100);
  const char* bad_names[] = {"riff.ptf", "riff.hex", "riff.o3pj", "riff", ".o3a", "riff.O3A2", "a/b.o3a", "../x.o3a", "x y.", "", "aux.o3a", "nul.txt"};
  for (const char* name : bad_names) {
    CAPTURE(name);
    CHECK_FALSE(rig.files.save("", name, data.data(), data.size()));
    CHECK(rig.failed("BAD NAME"));
    CHECK_FALSE(rig.files.busy());
  }
  const char* bad_folders[] = {"a.b", "..", "a/b", "Apps/x", ".deleted", "x.o3a", "CON"};
  for (const char* folder : bad_folders) {
    CAPTURE(folder);
    CHECK_FALSE(rig.files.save(folder, "riff.o3a", data.data(), data.size()));
    CHECK(rig.failed("BAD NAME"));
  }
  CHECK_FALSE(rig.files.save("", "riff.o3a", nullptr, 0));
  CHECK(rig.failed("NOTHING TO SAVE"));
  CHECK_FALSE(rig.files.save("", "riff.o3a", data.data(), 0));
  CHECK_FALSE(rig.files.load("", "riff.ptf", nullptr, 4));
  CHECK_FALSE(rig.files.list("", nullptr, 4));
  CHECK_FALSE(rig.files.remove_folder(""));
  CHECK(card.ops == 0);  // nothing reached the card
  CHECK(rig.files.generation() >= 1);

  // A request while one is running is refused and the running one's status stays.
  REQUIRE(rig.files.save("", "a.o3a", data.data(), data.size()));
  CHECK_FALSE(rig.files.save("", "b.o3a", data.data(), data.size()));
  CHECK_FALSE(rig.files.load("", "a.o3a", nullptr, 0));
  CHECK_FALSE(rig.files.list("", nullptr, 0));
  CHECK(rig.files.status().phase == Files::Status::Running);
  CHECK(std::strcmp(rig.files.status().text, "SAVE") == 0);
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.has("Apps/test/a.o3a"));
  CHECK_FALSE(card.has("Apps/test/b.o3a"));
  check_scope(card);
}

TEST_CASE("files: NewOnly refuses a name that is taken and leaves the file and no temporary behind") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes one = pattern(200, 1);
  const Bytes two = pattern(300, 2);
  REQUIRE(rig.files.save("", "kit.o3a", one.data(), one.size(), Files::Mode::NewOnly));
  rig.pump();
  CHECK(rig.done("SAVED"));
  REQUIRE(rig.files.save("", "kit.o3a", two.data(), two.size(), Files::Mode::NewOnly));
  rig.pump();
  CHECK(rig.failed("NAME EXISTS"));
  CHECK(card.same("Apps/test/kit.o3a", one));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  CHECK_FALSE(card.has("Apps/test/kit.o3a.bak"));
  CHECK_FALSE(rig.files.busy());
  check_scope(card);
}

TEST_CASE("files: a streamed save runs the caller's source, begun once and ended once, also when the save is refused or cancelled") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  CountedSource src;
  src.data = pattern(9000, 5);
  REQUIRE(rig.files.save("", "stream.o3a", src, Files::Mode::Replace));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/stream.o3a", src.data));
  CHECK(src.begins == 1);
  CHECK(src.ends >= 1);

  // Cancelled before it started: the source is let go though it was never begun.
  CountedSource unused;
  unused.data = pattern(100);
  REQUIRE(rig.files.save("", "never.o3a", unused, Files::Mode::Replace));
  rig.files.cancel();
  CHECK(unused.ends == 1);
  CHECK(unused.begins == 0);
  CHECK(rig.failed("CANCELLED"));
  CHECK_FALSE(rig.files.busy());
  rig.idle_ms(20);
  CHECK_FALSE(card.has("Apps/test/never.o3a"));

  // A source that fails ends the save with its own words and leaves no half file.
  struct Failing final : app::SaveSource {
    void begin(core::ByteSink&) noexcept override {}
    formats::Progress step(std::size_t) noexcept override { return formats::Progress::Failed; }
    const char* failure() const noexcept override { return "NOT TODAY"; }
  } failing;
  REQUIRE(rig.files.save("", "bad.o3a", failing, Files::Mode::Replace));
  rig.pump();
  CHECK(rig.failed("NOT TODAY"));
  CHECK_FALSE(card.has("Apps/test/bad.o3a"));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  check_scope(card);
}

TEST_CASE("files: without a card capability every call is refused with NO CARD and the app runs") {
  Files none;  // never bound
  const Bytes data = pattern(10);
  CHECK_FALSE(none.available());
  CHECK(none.card() == Files::Card::None);
  CHECK_FALSE(none.save("", "a.o3a", data.data(), data.size()));
  CHECK(none.status().phase == Files::Status::Failed);
  CHECK(std::strcmp(none.status().text, "NO CARD") == 0);
  CHECK_FALSE(none.load("", "a.o3a", nullptr, 0));
  CHECK_FALSE(none.list("", nullptr, 0));
  CHECK_FALSE(none.busy());
  host::FakeClock clock;
  none.poll(clock, 2000, KitValues{});  // a no-op
}

// ---- Loads --------------------------------------------------------------------------------------------------------------------

TEST_CASE("files: a load reads the .bak when the file is gone, says NOT FOUND when neither is there, and refuses a file that does not fit") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes old = pattern(700, 4);
  card.put("Apps/test/riff.o3a.bak", old);  // a save was cut after the old file moved aside: only the backup is left
  Bytes dst(1000, 0xAA);
  REQUIRE(rig.files.load("", "riff.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED BACKUP"));
  CHECK(rig.files.recovered());
  CHECK(rig.files.loaded() == old.size());
  CHECK(std::equal(old.begin(), old.end(), dst.begin()));

  REQUIRE(rig.files.load("", "nothing.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.failed("NOT FOUND"));
  CHECK_FALSE(rig.files.recovered());

  Bytes small(100, 0xAA);
  REQUIRE(rig.files.load("", "riff.o3a", small.data(), small.size()));
  rig.pump();
  CHECK(rig.failed("FILE TOO BIG"));
  CHECK(small == Bytes(100, 0xAA));  // untouched
  CHECK(card.open_handles == 0);

  card.put("Apps/test/riff.o3a", pattern(50, 9));  // and with the file there, the file wins over the backup
  REQUIRE(rig.files.load("", "riff.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED"));
  CHECK(rig.files.loaded() == 50);
  CHECK_FALSE(rig.files.recovered());
  // Nothing of a load touches the card's contents.
  CHECK(card.ops == 0);
}

TEST_CASE("files: a load reads a slice of the file a pass, and a card that changes under it fails it with CARD CHANGED") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes big = pattern(60'000, 6);
  card.put("Apps/test/big.o3a", big);
  Bytes dst(big.size());
  REQUIRE(rig.files.load("", "big.o3a", dst.data(), dst.size()));
  const unsigned passes = rig.pump();
  CHECK(passes >= 4);  // 16 KiB a pass: 60,000 bytes are four
  CHECK(rig.done("LOADED"));
  CHECK(dst == big);

  REQUIRE(rig.files.load("", "big.o3a", dst.data(), dst.size()));
  rig.pass();  // one slice read
  CHECK(rig.files.busy());
  CHECK(rig.files.status().permille > 0);
  CHECK(rig.files.status().permille < 1000);
  const std::uint32_t generation = rig.files.generation();
  rig.files.card_changed();
  CHECK(rig.files.generation() > generation);
  rig.pump();
  CHECK(rig.failed("CARD CHANGED"));
  CHECK(card.open_handles == 0);  // the file was closed
}

TEST_CASE("files: a streamed load pushes the file into the sink in slices, waits when it is full, and stops when it fails") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(5000, 7);
  card.put("Apps/test/wave.o3a", data);
  SlowSink sink;
  REQUIRE(rig.files.load("", "wave.o3a", sink));
  rig.pump(100'000);
  CHECK(rig.done("LOADED"));
  CHECK(sink.got == data);
  CHECK(card.open_handles == 0);

  FailingSink bad;
  REQUIRE(rig.files.load("", "wave.o3a", bad));
  rig.pump();
  CHECK(rig.failed("SINK FAILED"));
  CHECK(card.open_handles == 0);
}

// ---- Listings, project folders, DELETE ---------------------------------------------------------------------------------------------

TEST_CASE("files: a listing is sorted, folders first, with sizes; what the app could not use or the kit keeps is not in it") {
  host::ScratchDir dir("files-list");
  REQUIRE(dir.ok());
  host::DirCard card(dir.path());
  REQUIRE(card.mount() == hst::Status::Ok);
  const std::string base = dir.path() + "/Apps/test";
  for (const char* sub : {"/Zed", "/alpha", "/Dot.ted", "/Gone"}) {
    std::filesystem::create_directories(base + sub);
  }
  write_host_file(base + "/b.o3a", pattern(10));
  write_host_file(base + "/A.WAV", pattern(20));
  write_host_file(base + "/notes.txt", pattern(30));
  write_host_file(base + "/x.ptf", pattern(40));                    // not an extension an app may write: not listed
  write_host_file(base + "/system.o3s", pattern(50));               // the kit's: not listed
  write_host_file(base + "/system.o3s.bak", pattern(50));
  write_host_file(base + "/have.o3a", pattern(60));
  write_host_file(base + "/have.o3a.bak", pattern(61));             // the backup of a file that is there: not listed
  write_host_file(base + "/riff.o3a.bak", pattern(70));             // a file whose save was cut: listed as the file, to recover
  write_host_file(base + "/.hidden.o3a", pattern(5));
  write_host_file(base + "/_o3save.tmp", pattern(5));
  write_host_file(base + "/Gone/.deleted", Bytes());                // DELETE: the folder is hidden
  write_host_file(base + "/Zed/song.o3a", pattern(80));
  write_host_file(base + "/Zed/notes.txt", pattern(90));
  write_host_file(base + "/alpha/system.o3s", pattern(3));         // (below the app's folder it is an ordinary file)

  Rig rig(card, card);
  Files::Entry out[32];
  REQUIRE(rig.files.list("", out, 32));
  rig.pump();
  CHECK(rig.done("LISTED"));
  struct Want {
    const char* name;
    std::uint32_t size;
    bool folder;
    bool recover;
  };
  const Want want[] = {{"alpha", 0, true, false}, {"Zed", 0, true, false},   {"A.WAV", 20, false, false}, {"b.o3a", 10, false, false},
                       {"have.o3a", 60, false, false}, {"notes.txt", 30, false, false}, {"riff.o3a", 70, false, true}};
  REQUIRE(rig.files.listed() == sizeof want / sizeof want[0]);
  CHECK_FALSE(rig.files.list_truncated());
  for (std::size_t i = 0; i < rig.files.listed(); ++i) {
    CAPTURE(i);
    CHECK(std::strcmp(out[i].name, want[i].name) == 0);
    CHECK(out[i].size == want[i].size);
    CHECK(out[i].folder == want[i].folder);
    CHECK(out[i].recover == want[i].recover);
  }

  // A project folder: its files only.
  REQUIRE(rig.files.list("Zed", out, 32));
  rig.pump();
  REQUIRE(rig.files.listed() == 2);
  CHECK(std::strcmp(out[0].name, "notes.txt") == 0);
  CHECK(std::strcmp(out[1].name, "song.o3a") == 0);
  CHECK(out[1].size == 80);

  // alpha holds a system.o3s of the user's: below the app's folder the kit's name means nothing.
  REQUIRE(rig.files.list("alpha", out, 32));
  rig.pump();
  CHECK(rig.files.listed() == 1);

  // A short list: what fits, and it says there was more.
  REQUIRE(rig.files.list("", out, 3));
  rig.pump();
  CHECK(rig.files.listed() == 3);
  CHECK(rig.files.list_truncated());
  CHECK(std::strcmp(out[2].name, "A.WAV") == 0);

  // A folder that is not there lists as empty (a first run), and a card that is out says so.
  REQUIRE(rig.files.list("Nowhere", out, 32));
  rig.pump();
  CHECK(rig.done("LISTED"));
  CHECK(rig.files.listed() == 0);
  card.set_present(false);
  CHECK_FALSE(rig.files.list("", out, 32));
  CHECK(rig.failed("NO CARD"));
  CHECK(card.violations() == 0u);
}

TEST_CASE("files: a project folder is made by a save into it, DELETE marks it and hides it, and a save into it is refused") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(400, 8);
  REQUIRE(rig.files.save("Song 1", "pat.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/Song 1/pat.o3a", data));
  CHECK(card.dirs.count("apps/test/song 1") == 1);

  // DELETE: one empty marker file, nothing removed.
  const std::size_t files_before = card.files.size();
  REQUIRE(rig.files.remove_folder("Song 1"));
  rig.pump();
  CHECK(rig.done("DELETED"));
  CHECK(card.has("Apps/test/Song 1/.deleted"));
  CHECK(card.files.size() == files_before + 1);
  CHECK(card.same("Apps/test/Song 1/pat.o3a", data));
  CHECK(card.removed().empty());
  // Again: the same answer.
  REQUIRE(rig.files.remove_folder("Song 1"));
  rig.pump();
  CHECK(rig.done("DELETED"));
  // A save into the deleted folder is refused (the user's DELETE stands), and a folder that is not there cannot be deleted.
  REQUIRE(rig.files.save("Song 1", "pat.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.failed("FOLDER DELETED"));
  REQUIRE(rig.files.save("Song 1", "more.wav", data.data(), data.size()));
  rig.pump();
  CHECK(rig.failed("FOLDER DELETED"));
  CHECK_FALSE(card.has("Apps/test/Song 1/more.wav"));
  REQUIRE(rig.files.remove_folder("Nowhere"));
  rig.pump();
  CHECK(rig.failed("NO SUCH FOLDER"));
  CHECK_FALSE(rig.files.busy());
  check_scope(card);
}

// ---- WAV ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("files: a WAV is saved NewOnly as <stem>_NNN.wav with the first free number, in the app's folder, a project's or Samples/") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  std::vector<std::int16_t> pcm(200);  // 100 frames of stereo
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<std::int16_t>(i * 300 - 20000);
  }
  card.put("Apps/test/take_001.wav", Bytes(10, 1));  // taken
  card.put("Apps/test/take_003.wav", Bytes(10, 2));
  REQUIRE(rig.files.save_wav("", pcm.data(), 100, 2, 44100, "take"));
  rig.pump();
  CHECK(rig.done("SAVED"));
  const Bytes* wav = card.get("Apps/test/take_002.wav");
  NEED(wav != nullptr);
  CHECK(wav->size() == 44 + pcm.size() * 2);
  CHECK(std::memcmp(wav->data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(wav->data() + 8, "WAVEfmt ", 8) == 0);
  CHECK((*wav)[22] == 2);                                                              // two channels
  CHECK(((*wav)[24] | ((*wav)[25] << 8)) == 44100);                                    // the rate
  CHECK((*wav)[40] == (pcm.size() * 2) % 256);                                         // the data chunk's size
  CHECK(std::memcmp(wav->data() + 44, pcm.data(), pcm.size() * 2) == 0);
  CHECK(card.get("Apps/test/take_001.wav")->size() == 10);  // the others untouched
  REQUIRE(rig.files.save_wav("", pcm.data(), 100, 2, 44100, "take"));
  rig.pump();
  CHECK(card.has("Apps/test/take_004.wav"));

  REQUIRE(rig.files.save_wav("Song 1", pcm.data(), 50, 1, 22050, "kick"));
  rig.pump();
  CHECK(card.has("Apps/test/Song 1/kick_001.wav"));
  REQUIRE(rig.files.save_wav("Samples", pcm.data(), 50, 1, 22050, "take"));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.has("Samples/take_001.wav"));
  check_scope(card);

  // What is refused: a stem that is no name, too long a stem, no audio, three channels, no rate.
  const std::string long_stem(28, 'a');
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 100, 2, 44100, long_stem.c_str()));
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 100, 2, 44100, "a:b"));
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 100, 2, 44100, ""));
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 0, 2, 44100, "take"));
  CHECK_FALSE(rig.files.save_wav("", nullptr, 100, 2, 44100, "take"));
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 100, 3, 44100, "take"));
  CHECK_FALSE(rig.files.save_wav("", pcm.data(), 100, 2, 0, "take"));
  const std::string stem27(27, 'b');
  REQUIRE(rig.files.save_wav("", pcm.data(), 10, 1, 8000, stem27.c_str()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.has(("Apps/test/" + stem27 + "_001.wav").c_str()));
}

TEST_CASE("files: a WAV with every number taken says so") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  for (unsigned n = 1; n <= 999; ++n) {
    char path[64];
    std::snprintf(path, sizeof path, "Apps/test/full_%03u.wav", n);
    card.put(path, Bytes(1, 0));
  }
  std::int16_t pcm[4] = {1, 2, 3, 4};
  REQUIRE(rig.files.save_wav("", pcm, 4, 1, 8000, "full"));
  const unsigned passes = rig.pump();
  CHECK(rig.failed("NO FREE NAME"));
  CHECK(passes > 100);  // the numbers are tried a few a pass, never all in one
  CHECK(card.ops == 0);
}

// ---- Cancel, a card that goes, a cut -------------------------------------------------------------------------------------------

TEST_CASE("files: cancel gives a running save up at once, the old file whole and the temporary file gone; a request not yet started never touches the card") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes old = pattern(1000, 1);
  card.put("Apps/test/riff.o3a", old);
  const Bytes big = pattern(80'000, 2);
  REQUIRE(rig.files.save("", "riff.o3a", big.data(), big.size()));
  unsigned n = 0;
  while (!card.has("Apps/test/_o3save.tmp") && ++n < 100) {
    rig.pass();
  }
  REQUIRE(card.has("Apps/test/_o3save.tmp"));
  rig.pass();
  CHECK(rig.files.busy());
  rig.files.cancel();
  CHECK_FALSE(rig.files.busy());
  CHECK(rig.failed("CANCELLED"));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  CHECK(card.same("Apps/test/riff.o3a", old));
  CHECK(card.ends == card.begins);
  // The next one works.
  REQUIRE(rig.files.save("", "riff.o3a", big.data(), big.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/riff.o3a", big));
  CHECK(card.same("Apps/test/riff.o3a.bak", old));

  const std::size_t ops = card.ops;
  REQUIRE(rig.files.save("", "next.o3a", big.data(), big.size()));
  rig.files.cancel();
  rig.idle_ms(30);
  CHECK(card.ops == ops);
  CHECK(rig.failed("CANCELLED"));
  check_scope(card);
}

TEST_CASE("files: a card pulled mid-save fails the save with NO CARD and leaves the old file; the next save goes through") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes old = pattern(2000, 1);
  const Bytes next = pattern(30'000, 2);
  card.put("Apps/test/riff.o3a", old);
  REQUIRE(rig.files.save("", "riff.o3a", next.data(), next.size()));
  unsigned n = 0;
  while (!card.has("Apps/test/_o3save.tmp") && ++n < 100) {
    rig.pass();
  }
  const std::uint32_t generation = rig.files.generation();
  card.gate_status = hst::WriteStatus::NoCard;  // the card is out
  rig.pump();
  CHECK(rig.failed("NO CARD"));
  CHECK(rig.files.card() == Files::Card::None);
  CHECK(rig.files.generation() > generation);
  CHECK(card.same("Apps/test/riff.o3a", old));
  CHECK_FALSE(rig.files.save("", "riff.o3a", next.data(), next.size()));
  CHECK(rig.failed("NO CARD"));

  card.gate_status = hst::WriteStatus::Ok;  // back
  CHECK(rig.files.card() == Files::Card::Ready);
  REQUIRE(rig.files.save("", "riff.o3a", next.data(), next.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/riff.o3a", next));
  CHECK(card.same("Apps/test/riff.o3a.bak", old));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  CHECK(card.ends == card.begins);
  check_scope(card);
}

TEST_CASE("files: a power cut at every write of a save leaves the old file or the new one, whole, and a load finds it") {
  const Bytes old = pattern(3000, 1);
  const Bytes fresh = pattern(20000, 2);
  unsigned cuts = 0;
  for (std::size_t cut = 0; cut < 200; ++cut) {
    MemCard card;
    seed(card);
    card.put("Apps/test/riff.o3a", old);
    card.cut_at = cut;
    bool finished = false;
    {
      Rig rig(card, card);
      REQUIRE(rig.files.save("", "riff.o3a", fresh.data(), fresh.size()));
      rig.pump();
      finished = rig.files.status().phase == Files::Status::Done && !card.dead;
    }
    CAPTURE(cut);
    if (finished) {
      CHECK(card.same("Apps/test/riff.o3a", fresh));  // the cut came after the last write: the save is whole
      break;
    }
    ++cuts;
    card.revive();  // the power is back: a new run on the same card
    const bool f_old = card.same("Apps/test/riff.o3a", old);
    const bool f_new = card.same("Apps/test/riff.o3a", fresh);
    const bool b_old = card.same("Apps/test/riff.o3a.bak", old);
    // The file is whole (the old one or the new one), or it is gone and the backup is the old one, whole.
    CHECK((f_old || f_new || (!card.has("Apps/test/riff.o3a") && b_old)));
    Rig rig(card, card);
    Bytes dst(30000);
    REQUIRE(rig.files.load("", "riff.o3a", dst.data(), dst.size()));
    rig.pump();
    CHECK((rig.done("LOADED") || rig.done("LOADED BACKUP")));  // (the backup when the cut left no file)
    const std::size_t n = rig.files.loaded();
    CHECK(((n == old.size() && std::equal(old.begin(), old.end(), dst.begin())) || (n == fresh.size() && std::equal(fresh.begin(), fresh.end(), dst.begin()))));
    // And the next save heals what the cut left: a stale temporary, a file moved aside.
    REQUIRE(rig.files.save("", "riff.o3a", fresh.data(), fresh.size()));
    rig.pump();
    CHECK(rig.done("SAVED"));
    CHECK(card.same("Apps/test/riff.o3a", fresh));
    CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
    check_scope(card);
  }
  CHECK(cuts > 8);  // a save is more than a handful of changes: the sweep really cut it at each of them
}

// ---- The card's states --------------------------------------------------------------------------------------------------------

TEST_CASE("files: a protected, unsupported or computer-held card, and the update path's claim, refuse a save with their reason; loads and listings work where the card can be read") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(100);
  card.put("Apps/test/a.o3a", data);
  Bytes dst(200);
  Files::Entry out[8];

  card.gate_status = hst::WriteStatus::WriteProtected;
  CHECK(rig.files.card() == Files::Card::ReadOnly);
  CHECK_FALSE(rig.files.save("", "b.o3a", data.data(), data.size()));
  CHECK(rig.failed("CARD WRITE PROTECTED"));
  CHECK_FALSE(rig.files.remove_folder("x"));
  CHECK(rig.failed("CARD WRITE PROTECTED"));
  REQUIRE(rig.files.load("", "a.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED"));
  REQUIRE(rig.files.list("", out, 8));
  rig.pump();
  CHECK(rig.files.listed() == 1);

  card.gate_status = hst::WriteStatus::Unsupported;
  CHECK(rig.files.card() == Files::Card::Unsupported);
  CHECK_FALSE(rig.files.save("", "b.o3a", data.data(), data.size()));
  CHECK(rig.failed("EXFAT CARD NOT SUPPORTED"));
  REQUIRE(rig.files.load("", "a.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED"));

  card.gate_status = hst::WriteStatus::Host;
  CHECK(rig.files.card() == Files::Card::OnComputer);
  CHECK_FALSE(rig.files.save("", "b.o3a", data.data(), data.size()));
  CHECK(rig.failed("CARD ON COMPUTER - EJECT FIRST"));
  CHECK_FALSE(rig.files.load("", "a.o3a", dst.data(), dst.size()));
  CHECK(rig.failed("CARD ON COMPUTER - EJECT FIRST"));

  card.gate_status = hst::WriteStatus::Ok;
  claim_flag = true;
  CHECK(rig.files.card() == Files::Card::InUse);
  CHECK_FALSE(rig.files.save("", "b.o3a", data.data(), data.size()));
  CHECK(rig.failed("FIRMWARE UPDATE RUNNING"));
  claim_flag = false;
  CHECK(rig.files.card() == Files::Card::Ready);
  REQUIRE(rig.files.save("", "b.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK_FALSE(card.has("Apps/test/c.o3a"));
  check_scope(card);

  // A request that was taken and found the card gone when its turn came says so.
  REQUIRE(rig.files.save("", "c.o3a", data.data(), data.size()));
  card.gate_status = hst::WriteStatus::NoCard;
  rig.pump();
  CHECK(rig.failed("NO CARD"));
  CHECK_FALSE(card.has("Apps/test/c.o3a"));
  CHECK(card.ends == card.begins);
}

TEST_CASE("files: the generation moves when the card's state or identity changes") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  rig.pass();
  const std::uint32_t g0 = rig.files.generation();
  rig.idle_ms(5);
  CHECK(rig.files.generation() == g0);  // nothing changed
  card.gate_status = hst::WriteStatus::NoCard;
  rig.pass();
  const std::uint32_t g1 = rig.files.generation();
  CHECK(g1 > g0);
  card.gate_status = hst::WriteStatus::Ok;
  card.id = 7;  // another card in the same slot
  rig.pass();
  CHECK(rig.files.generation() > g1);
}

// ---- Reporting -------------------------------------------------------------------------------------------------------------------

TEST_CASE("files: the strip says the job while it runs, its result once, a refusal once, and nothing when the app asked for quiet") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(40'000, 3);
  Files::Strip line;
  CHECK_FALSE(rig.files.strip(line));
  REQUIRE(rig.files.save("", "riff.o3a", data.data(), data.size()));
  unsigned running = 0;
  int last = -1;
  while (rig.files.busy() && running < 1000) {
    rig.pass();
    if (rig.files.busy()) {
      REQUIRE(rig.files.strip(line));
      CHECK(std::strcmp(line.text, "SAVE RIFF.O3A") == 0);
      CHECK_FALSE(line.result);
      CHECK(line.progress >= last);
      last = line.progress;
      ++running;
    }
  }
  CHECK(running > 5);
  CHECK(last > 0);
  REQUIRE(rig.files.strip(line));
  CHECK(std::strcmp(line.text, "SAVED RIFF.O3A") == 0);
  CHECK(line.result);
  CHECK(line.progress == -1);
  CHECK_FALSE(rig.files.strip(line));  // once

  card.gate_status = hst::WriteStatus::WriteProtected;
  CHECK_FALSE(rig.files.save("", "riff.o3a", data.data(), data.size()));
  REQUIRE(rig.files.strip(line));
  CHECK(std::strcmp(line.text, "CARD WRITE PROTECTED") == 0);
  CHECK(line.result);
  card.gate_status = hst::WriteStatus::Ok;

  // A failed job: the verb and the reason.
  card.fail_at = card.ops + 2;
  REQUIRE(rig.files.save("", "riff.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.files.status().phase == Files::Status::Failed);
  REQUIRE(rig.files.strip(line));
  CHECK(std::strncmp(line.text, "SAVE FAILED: ", 13) == 0);
  CHECK(line.result);

  // Quiet: the status is polled all the same, the strip says nothing.
  card.fail_at = ~std::size_t{0};
  rig.files.set_quiet(true);
  REQUIRE(rig.files.save("", "quiet.o3a", data.data(), data.size()));
  rig.pass();
  CHECK_FALSE(rig.files.strip(line));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK_FALSE(rig.files.strip(line));
  CHECK_FALSE(rig.files.save("", "quiet.ptf", data.data(), data.size()));
  CHECK_FALSE(rig.files.strip(line));
  CHECK(rig.failed("BAD NAME"));
}

// ---- The kit's settings -------------------------------------------------------------------------------------------------------

namespace {

KitValues changed_values() {
  KitValues v;
  v.theme = ui::ThemeId::Black;
  v.midi.channel = 5;
  return v;
}

bool settings_on_card(const MemCard& card, KitValues& out) {
  const Bytes* b = card.get("Apps/test/system.o3s");
  if (b == nullptr) {
    return false;
  }
  app::settings::Report rep;
  return app::settings::read(b->data(), b->size(), out, rep) == app::settings::Result::Ok;
}

}  // namespace

TEST_CASE("files: the kit's settings are written 1.5 s after the last change, not before, and not again until they change") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  rig.idle_ms(3000);  // the first pass takes what the values are as saved: nothing changed, nothing is written
  CHECK(card.ops == 0);
  CHECK_FALSE(rig.files.settings_dirty());

  rig.values = changed_values();
  rig.pass();
  CHECK(rig.files.settings_dirty());
  rig.idle_ms(1400);
  CHECK_FALSE(card.has("Apps/test/system.o3s"));
  rig.values.midi.channel = 6;  // another change: the quiet starts again
  rig.idle_ms(1400);
  CHECK_FALSE(card.has("Apps/test/system.o3s"));
  rig.idle_ms(200);
  rig.pump();
  KitValues on_card;
  REQUIRE(settings_on_card(card, on_card));
  CHECK(on_card.theme == ui::ThemeId::Black);
  CHECK(on_card.midi.channel == 6);
  CHECK_FALSE(rig.files.settings_dirty());
  // The settings job leaves the app's status alone and shows nothing.
  CHECK(rig.files.status().phase == Files::Status::Idle);
  Files::Strip line;
  CHECK_FALSE(rig.files.strip(line));
  const std::size_t ops = card.ops;
  rig.idle_ms(4000);
  CHECK(card.ops == ops);  // not again until they change

  rig.values.midi.channel = 2;
  rig.idle_ms(1600);
  rig.pump();
  REQUIRE(settings_on_card(card, on_card));
  CHECK(on_card.midi.channel == 2);
  CHECK(card.has("Apps/test/system.o3s.bak"));  // the previous file is the backup
  check_scope(card);
}

TEST_CASE("files: the settings wait for a card that can be written, and are written when it can") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  rig.pass();
  rig.values = changed_values();
  card.gate_status = hst::WriteStatus::NoCard;
  rig.idle_ms(2000);
  CHECK(rig.files.settings_dirty());
  card.gate_status = hst::WriteStatus::WriteProtected;
  rig.idle_ms(500);
  CHECK(rig.files.settings_dirty());
  card.gate_status = hst::WriteStatus::Host;  // the computer has it: they wait for the eject
  rig.idle_ms(500);
  CHECK(rig.files.settings_dirty());
  claim_flag = true;  // the update path
  rig.idle_ms(500);
  CHECK(rig.files.settings_dirty());
  CHECK(card.ops == 0);
  claim_flag = false;
  card.gate_status = hst::WriteStatus::Ok;
  rig.idle_ms(3);
  rig.pump();
  KitValues on_card;
  REQUIRE(settings_on_card(card, on_card));
  CHECK(on_card.theme == ui::ThemeId::Black);
  CHECK_FALSE(rig.files.settings_dirty());
}

TEST_CASE("files: an app's request is not made to wait for the settings: the settings give way and are written after it") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  rig.pass();
  rig.values = changed_values();
  rig.idle_ms(1500);
  unsigned n = 0;
  while (!card.has("Apps/test/_o3save.tmp") && ++n < 50) {  // the settings job is under way
    rig.pass();
  }
  REQUIRE(card.has("Apps/test/_o3save.tmp"));
  CHECK(rig.files.busy());
  const Bytes data = pattern(300, 4);
  REQUIRE(rig.files.save("", "riff.o3a", data.data(), data.size()));  // taken, not refused
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.same("Apps/test/riff.o3a", data));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  // The settings were given up, not lost: they are still due, and go once the app's job is over.
  rig.idle_ms(1600);
  rig.pump();
  KitValues on_card;
  REQUIRE(settings_on_card(card, on_card));
  CHECK(on_card.theme == ui::ThemeId::Black);
  check_scope(card);
}

TEST_CASE("files: the power-off writes the settings at once, waits for them, refuses new work, and does not wait for a card that cannot be written") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  rig.pass();
  rig.values = changed_values();
  rig.pass();
  const Bytes data = pattern(100);
  REQUIRE(rig.files.save("", "x.o3a", data.data(), data.size()));  // a request that has not started
  rig.files.power_begin();
  CHECK(rig.failed("POWERING OFF"));
  CHECK_FALSE(rig.files.save("", "y.o3a", data.data(), data.size()));
  CHECK(rig.failed("POWERING OFF"));
  CHECK(rig.files.power_busy());  // the settings are due
  unsigned n = 0;
  while (rig.files.power_busy() && ++n < 100) {
    rig.pass();
  }
  CHECK(n < 100);
  KitValues on_card;
  REQUIRE(settings_on_card(card, on_card));  // no 1.5 s: they were written at once
  CHECK(on_card.theme == ui::ThemeId::Black);
  CHECK_FALSE(card.has("Apps/test/x.o3a"));

  // A card that cannot be written holds nothing up.
  MemCard locked;
  seed(locked);
  locked.gate_status = hst::WriteStatus::WriteProtected;
  Rig other(locked, locked);
  other.pass();
  other.values = changed_values();
  other.pass();
  other.files.power_begin();
  other.pass();
  CHECK_FALSE(other.files.power_busy());
}

TEST_CASE("files: the power-off cancels a running save at once and gives the flush up") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes old = pattern(200, 1);
  const Bytes big = pattern(60'000, 2);
  card.put("Apps/test/riff.o3a", old);
  REQUIRE(rig.files.save("", "riff.o3a", big.data(), big.size()));
  unsigned n = 0;
  while (!card.has("Apps/test/_o3save.tmp") && ++n < 50) {
    rig.pass();
  }
  rig.values = changed_values();
  rig.files.power_begin();
  CHECK(rig.files.power_busy());
  CHECK_FALSE(rig.files.past_write());  // still writing: a cancel is not a rollback of a nearly finished save
  rig.files.power_cancel();
  CHECK_FALSE(rig.files.power_busy());
  CHECK(card.same("Apps/test/riff.o3a", old));
  CHECK_FALSE(card.has("Apps/test/_o3save.tmp"));
  rig.idle_ms(50);
  CHECK_FALSE(card.has("Apps/test/system.o3s"));  // no flush after the cancel
  check_scope(card);
}

TEST_CASE("files: the settings read at boot apply over the app's defaults; a missing, backup, damaged or newer file is told apart, and nothing is rewritten for what was read") {
  const KitValues saved = changed_values();
  Bytes file(app::settings::kMaxBytes);
  file.resize(app::settings::write(saved, file.data(), file.size()));
  REQUIRE(!file.empty());
  {  // a card with nothing on it
    MemCard card;
    seed(card);
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::NoFile);
    CHECK(v == KitValues{});
    rig.idle_ms(3000);
    CHECK(card.ops == 0);
  }
  {  // the file
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s", file);
    Rig rig(card, card);
    KitValues v;
    v.midi.out_channel = 9;  // the app's default for a setting the file has
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Loaded);
    CHECK(v.theme == ui::ThemeId::Black);
    CHECK(v.midi.channel == 5);
    CHECK(v.midi.out_channel == 1);  // the file wins
    CHECK(rig.files.settings_report().applied == 32);
    rig.values = v;
    rig.idle_ms(3000);
    CHECK(card.ops == 0);  // what was read is what is saved
    rig.values.midi.channel = 8;
    rig.idle_ms(1600);
    rig.pump();
    KitValues on_card;
    REQUIRE(settings_on_card(card, on_card));
    CHECK(on_card.midi.channel == 8);
  }
  {  // only the backup (a save cut between its renames)
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s.bak", file);
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::FromBackup);
    CHECK(v.midi.channel == 5);
  }
  {  // damaged
    MemCard card;
    seed(card);
    Bytes bad = file;
    bad[20] ^= 0x01;
    card.put("Apps/test/system.o3s", bad);
    Rig rig(card, card);
    KitValues v;
    v.midi.channel = 3;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    CHECK(rig.files.settings_error() == app::settings::Result::BadCrc);
    CHECK(v.midi.channel == 3);  // the app's default stays
    rig.values = v;
    rig.idle_ms(3000);
    CHECK(card.ops == 0);
  }
  {  // too big, and one the card cannot read
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s", Bytes(app::settings::kMaxBytes + 1, 0));
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    card.put("Apps/test/system.o3s", file);
    card.read_fault = MemCard::key("Apps/test/system.o3s");
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    CHECK(card.open_handles == 0);
  }
  {  // no card capability, and a card that is out
    Files none;
    KitValues v;
    CHECK(none.load_settings(v) == Files::SettingsLoad::Unavailable);
    MemCard card;
    seed(card);
    Rig rig(card, card);
    // (a MemCard is always mounted: the host card's NoCard is covered by the simulator tests)
    CHECK(rig.files.settings_load() == Files::SettingsLoad::NotRead);
  }
}

TEST_CASE("files: the app's id is the folder, and an id the guard would refuse leaves the service off") {
  MemCard card;
  seed(card);
  const char* bad[] = {"", "a.b", "has/slash", "0123456789abcdef", ".hidden", "con", nullptr};
  for (const char* id : bad) {
    Files::Work* work = new Files::Work;
    Files files;
    files.bind(&card, &card, work, id, nullptr);
    CHECK_FALSE(files.available());
    delete work;
  }
  Files::Work* work = new Files::Work;
  Files files;
  files.bind(&card, &card, work, "o3_303", nullptr);
  CHECK(files.available());
  CHECK(std::strcmp(files.app_id(), "o3_303") == 0);
  files.bind(&card, &card, nullptr, "o3_303", nullptr);  // no memory for the jobs: off
  CHECK_FALSE(files.available());
  delete work;
}

// ---- The files service only changes Apps/<id>/ and Samples/ ----------------------------------------------------------------------

TEST_CASE("files: the scoped writer refuses, with no card call, every change outside Apps/<id>/ and Samples/, though the guard of the writer admits track3r-os's rows") {
  MemCard card;
  seed(card);
  app::ScopedWriter scope;
  scope.bind(&card, "test");
  // The writer's own guard (hal/app_guard.h is a superset of track3r-os's) takes these: that is what the scope is for.
  CHECK(hst::app_guard::may_create("Projects/x/_o3save.tmp"));
  CHECK(hst::app_guard::may_make_dir("Settings"));
  CHECK(hst::app_guard::may_create_marker("Projects/x/.deleted"));
  CHECK(hst::app_guard::may_rename("Settings/device.o3st", "Settings/device.o3st.bak"));
  CHECK(hst::app_guard::may_remove("Settings/device.o3st.bak"));
  card.make_dir_now("Projects");
  card.make_dir_now("Projects/x");
  card.make_dir_now("Settings");
  card.put("Settings/device.o3st", pattern(10));
  card.put("Settings/device.o3st.bak", pattern(10));
  CHECK(scope.make_dir("Projects") == hst::WriteStatus::BadPath);
  CHECK(scope.make_dir("Settings") == hst::WriteStatus::BadPath);
  CHECK(scope.create("Projects/x/_o3save.tmp") == hst::WriteStatus::BadPath);
  CHECK(scope.create_marker("Projects/x/.deleted") == hst::WriteStatus::BadPath);
  CHECK(scope.rename("Settings/device.o3st", "Settings/device.o3st.bak") == hst::WriteStatus::BadPath);
  CHECK(scope.remove("Settings/device.o3st.bak") == hst::WriteStatus::BadPath);
  CHECK(scope.make_dir("AppExport") == hst::WriteStatus::BadPath);
  CHECK(scope.make_dir("Apps/other") == hst::WriteStatus::BadPath);   // another app's folder
  CHECK(scope.make_dir("Apps/testx") == hst::WriteStatus::BadPath);   // not a prefix match
  CHECK(scope.create("Firmware/_o3save.tmp") == hst::WriteStatus::BadPath);
  CHECK(scope.rename("Apps/test/_o3save.tmp", "Projects/x/a.o3a") == hst::WriteStatus::BadPath);  // out of scope at either end
  CHECK(scope.rename("Settings/_o3save.tmp", "Apps/test/a.o3a") == hst::WriteStatus::BadPath);
  CHECK(scope.make_dir(nullptr) == hst::WriteStatus::BadPath);
  CHECK(scope.refused() == 13);
  CHECK(card.ops == 0);
  CHECK(card.log.empty());
  CHECK(card.violations == 0u);  // it never got as far as the guard
  // And what is in scope goes through (the guard then has its say): the folders, the temporary file, any case.
  CHECK(scope.make_dir("Apps") == hst::WriteStatus::Ok);
  CHECK(scope.make_dir("apps/TEST") == hst::WriteStatus::Ok);
  CHECK(scope.make_dir("Apps/test/Song 1") == hst::WriteStatus::Ok);
  CHECK(scope.make_dir("Samples") == hst::WriteStatus::Ok);
  CHECK(scope.create("Apps/test/_o3save.tmp") == hst::WriteStatus::Ok);
  CHECK(scope.finish() == hst::WriteStatus::Ok);
  CHECK(scope.remove("Apps/test/_o3save.tmp") == hst::WriteStatus::Ok);
  CHECK(scope.refused() == 13);
}

TEST_CASE("files: the screenshot scope admits Screenshots/ and nothing else, and the app's scope never admits it") {
  MemCard card;
  seed(card);
  // The writer's guard takes track3r-os's Screenshots rows (hal/save_guard.h): Shift + Power writes Screenshots/_o3save.tmp and renames it to SHOT_nnnn.bmp.
  CHECK(hst::app_guard::may_make_dir("Screenshots"));
  CHECK(hst::app_guard::may_create("Screenshots/_o3save.tmp"));
  CHECK(hst::app_guard::may_rename("Screenshots/_o3save.tmp", "Screenshots/SHOT_0001.bmp"));
  app::ScopedWriter shots;
  shots.bind(&card, "test", app::ScopedWriter::Scope::Screenshots);
  CHECK(shots.in_scope("Screenshots"));
  CHECK(shots.in_scope("screenshots/SHOT_0001.bmp"));
  CHECK_FALSE(shots.in_scope("Screenshotsx"));
  CHECK_FALSE(shots.in_scope("Apps/test/a.o3a"));
  CHECK_FALSE(shots.in_scope("Samples/a.wav"));
  CHECK(shots.make_dir("Apps") == hst::WriteStatus::BadPath);
  CHECK(shots.make_dir("Apps/test") == hst::WriteStatus::BadPath);
  CHECK(shots.create("Settings/_o3save.tmp") == hst::WriteStatus::BadPath);
  CHECK(shots.rename("Screenshots/_o3save.tmp", "Apps/test/a.o3a") == hst::WriteStatus::BadPath);
  CHECK(shots.refused() == 4);
  CHECK(card.ops == 0);
  CHECK(shots.make_dir("Screenshots") == hst::WriteStatus::Ok);
  CHECK(shots.create("Screenshots/_o3save.tmp") == hst::WriteStatus::Ok);
  CHECK(shots.finish() == hst::WriteStatus::Ok);
  CHECK(shots.rename("Screenshots/_o3save.tmp", "Screenshots/SHOT_0001.bmp") == hst::WriteStatus::Ok);
  CHECK(shots.refused() == 4);
  CHECK(card.violations == 0u);
  // The app's own scope (what Files::bind makes) refuses the same folder.
  app::ScopedWriter mine;
  mine.bind(&card, "test");
  CHECK_FALSE(mine.in_scope("Screenshots"));
  CHECK(mine.make_dir("Screenshots") == hst::WriteStatus::BadPath);
  CHECK(mine.create("Screenshots/_o3save.tmp") == hst::WriteStatus::BadPath);
  CHECK(mine.rename("Screenshots/_o3save.tmp", "Screenshots/SHOT_0002.bmp") == hst::WriteStatus::BadPath);
  CHECK(mine.remove("Screenshots/SHOT_0001.bmp") == hst::WriteStatus::BadPath);
  CHECK(mine.refused() == 4);
}

TEST_CASE("files: every operation of the service, run on a recording card, changes only Apps/<id>/ and Samples/") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(9000, 3);
  std::int16_t pcm[64] = {};
  CountedSource src;
  src.data = pattern(3000, 4);
  Bytes dst(20000);
  Files::Entry out[16];
  SlowSink sink;
  rig.pass();
  // saves: root, a project folder, NewOnly twice (the second refused), a stream, a replacement, WAVs in every place
  REQUIRE(rig.files.save("", "a.o3a", data.data(), data.size()));
  rig.pump();
  REQUIRE(rig.files.save("Song 1", "a.o3a", data.data(), data.size()));
  rig.pump();
  REQUIRE(rig.files.save("Song 1", "a.o3a", data.data(), data.size()));  // replaces: a rename to .bak
  rig.pump();
  REQUIRE(rig.files.save("", "n.json", data.data(), data.size(), Files::Mode::NewOnly));
  rig.pump();
  REQUIRE(rig.files.save("", "n.json", data.data(), data.size(), Files::Mode::NewOnly));
  rig.pump();
  CHECK(rig.failed("NAME EXISTS"));
  REQUIRE(rig.files.save("", "s.o3a", src, Files::Mode::Replace));
  rig.pump();
  REQUIRE(rig.files.save_wav("", pcm, 32, 2, 44100, "take"));
  rig.pump();
  REQUIRE(rig.files.save_wav("Song 1", pcm, 32, 2, 44100, "take"));
  rig.pump();
  REQUIRE(rig.files.save_wav("Samples", pcm, 32, 2, 44100, "take"));
  rig.pump();
  // loads, with the .bak fallback, streamed and not; listings; DELETE and a save into the deleted folder
  REQUIRE(rig.files.save("", "a.o3a", data.data(), data.size()));  // (a second save: the first is the .bak)
  rig.pump();
  card.files.erase(MemCard::key("Apps/test/a.o3a"));
  REQUIRE(rig.files.load("", "a.o3a", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED BACKUP"));
  REQUIRE(rig.files.load("Song 1", "a.o3a", sink));
  rig.pump(100000);
  REQUIRE(rig.files.list("", out, 16));
  rig.pump();
  REQUIRE(rig.files.remove_folder("Song 1"));
  rig.pump();
  REQUIRE(rig.files.save("Song 1", "b.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.failed("FOLDER DELETED"));
  // a cancelled save, a card that fails mid-save, the settings job (twice, the second over the first's backup), the power-off's flush
  const Bytes big = pattern(60000, 5);
  REQUIRE(rig.files.save("", "big.o3a", big.data(), big.size()));
  rig.pass();
  rig.pass();
  rig.pass();
  rig.files.cancel();
  card.fail_at = card.ops + 3;
  REQUIRE(rig.files.save("", "f.o3a", data.data(), data.size()));
  rig.pump();
  CHECK(rig.files.status().phase == Files::Status::Failed);
  rig.values = changed_values_for_scope();
  rig.idle_ms(1600);
  rig.pump();
  rig.values.midi.channel = 11;
  rig.idle_ms(1600);
  rig.pump();
  rig.values.theme = ui::ThemeId::White;
  rig.pass();
  rig.files.power_begin();
  for (unsigned i = 0; i < 100 && rig.files.power_busy(); ++i) {
    rig.pass();
  }
  // What was written: both roots, and the settings, and nothing else.
  bool apps = false;
  bool samples = false;
  bool settings = false;
  for (const std::string& line : card.log) {
    apps = apps || line.find("Apps/test/Song 1/a.o3a") != std::string::npos;
    samples = samples || line.find("Samples/take_001.wav") != std::string::npos;
    settings = settings || line.find("system.o3s") != std::string::npos;
  }
  CHECK(apps);
  CHECK(samples);
  CHECK(settings);
  CHECK(card.ops > 40);
  check_scope(card);
}

// ---- The kit's settings file is the kit's ------------------------------------------------------------------------------------------

TEST_CASE("files: no call of the app's can change the kit's settings file, whatever its case; below a project folder the name is the app's") {
  MemCard card;
  seed(card);
  Rig rig(card, card);
  const Bytes data = pattern(100);
  CountedSource src;
  src.data = data;
  for (const char* name : {"system.o3s", "SYSTEM.O3S", "System.o3s", "sYsTeM.O3s"}) {
    CAPTURE(name);
    CHECK_FALSE(rig.files.save("", name, data.data(), data.size()));
    CHECK(rig.failed("RESERVED NAME"));
    CHECK_FALSE(rig.files.save("", name, data.data(), data.size(), Files::Mode::NewOnly));
    CHECK(rig.failed("RESERVED NAME"));
    CHECK_FALSE(rig.files.save("", name, src, Files::Mode::Replace));
    CHECK(rig.failed("RESERVED NAME"));
    CHECK_FALSE(rig.files.busy());
  }
  CHECK(src.begins == 0);
  CHECK(card.ops == 0);
  CHECK_FALSE(card.has("Apps/test/system.o3s"));
  card.put("Apps/test/system.o3s", pattern(20, 9));  // a real settings file stays as it is
  CHECK_FALSE(rig.files.save("", "System.O3S", data.data(), data.size()));
  CHECK(card.same("Apps/test/system.o3s", pattern(20, 9)));
  // Reading it is not changing it; and a project folder's system.o3s is an ordinary file of the app's.
  Bytes dst(100);
  REQUIRE(rig.files.load("", "system.o3s", dst.data(), dst.size()));
  rig.pump();
  CHECK(rig.done("LOADED"));
  REQUIRE(rig.files.save("Song 1", "system.o3s", data.data(), data.size()));
  rig.pump();
  CHECK(rig.done("SAVED"));
  CHECK(card.has("Apps/test/Song 1/system.o3s"));
  // "Samples" is no project folder: only a WAV may name it.
  CHECK_FALSE(rig.files.save("Samples", "x.o3a", data.data(), data.size()));
  CHECK(rig.failed("BAD NAME"));
  CHECK_FALSE(rig.files.list("samples", nullptr, 0));
  CHECK_FALSE(rig.files.remove_folder("SAMPLES"));
  check_scope(card);
}

TEST_CASE("files: a card put in after the boot has its settings read, and one change then keeps the other keys the card holds") {
  MemCard card;
  seed(card);
  KitValues held = changed_values_for_scope();
  held.midi.out_channel = 12;
  held.midi.follow = true;
  Bytes file(app::settings::kMaxBytes);
  file.resize(app::settings::write(held, file.data(), file.size()));
  card.put("Apps/test/system.o3s", file);
  card.gate_status = hst::WriteStatus::NoCard;  // the unit booted with no card
  Rig rig(card, card);
  rig.idle_ms(3000);
  CHECK(rig.values == KitValues{});
  CHECK(card.ops == 0);

  card.gate_status = hst::WriteStatus::Ok;  // the card is put in
  rig.files.card_changed();
  rig.idle_ms(3);
  CHECK(rig.values == held);  // its settings are applied as at a boot
  CHECK(rig.files.settings_load() == Files::SettingsLoad::Loaded);
  CHECK_FALSE(rig.files.settings_dirty());
  rig.idle_ms(3000);
  CHECK(card.ops == 0);  // nothing is written for what was read

  rig.values.theme = ui::ThemeId::White;  // one change
  rig.idle_ms(1600);
  rig.pump();
  KitValues on_card;
  REQUIRE(settings_on_card(card, on_card));
  CHECK(on_card.theme == ui::ThemeId::White);
  CHECK(on_card.midi.channel == 5);       // the other keys survived
  CHECK(on_card.midi.out_channel == 12);
  CHECK(on_card.midi.follow);

  // A change made before any card was read is not lost and nothing is written until the read: the file wins on the keys it has.
  MemCard late;
  seed(late);
  late.put("Apps/test/system.o3s", file);
  late.gate_status = hst::WriteStatus::NoCard;
  Rig other(late, late);
  other.pass();
  other.values.midi.out_channel = 2;
  other.idle_ms(2000);
  late.gate_status = hst::WriteStatus::Ok;
  other.files.card_changed();
  other.pass();
  CHECK(late.ops == 0);  // the read came first
  CHECK(other.values == held);
}

TEST_CASE("files: a card with a settings file that is newer, too big or unreadable is never written to; a damaged one is replaced, after the backup is tried") {
  const KitValues saved = changed_values_for_scope();
  Bytes good(app::settings::kMaxBytes);
  good.resize(app::settings::write(saved, good.data(), good.size()));
  auto newer = [] {
    Bytes b(512);
    formats::chunk::Writer w(b.data(), b.size(), "O3SK");
    w.begin("SETS");
    w.u16(0x0200);
    w.u16(1);
    w.u16(1);
    w.f32(2.0f);
    w.end();
    b.resize(w.finish());
    return b;
  }();
  {  // a newer major: kept in memory for the session, the file and its backup untouched, the strip says so once
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s", newer);
    card.put("Apps/test/system.o3s.bak", good);
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    CHECK(rig.files.settings_error() == app::settings::Result::NewerMajor);
    CHECK(rig.files.settings_protected());
    CHECK(v == KitValues{});  // (the backup is not read: the newer file is the user's current one)
    Files::Strip line;
    REQUIRE(rig.files.strip(line));
    CHECK(std::strncmp(line.text, "SETTINGS NOT SAVED", 18) == 0);
    CHECK_FALSE(rig.files.strip(line));
    rig.values = v;
    rig.pass();
    rig.values.theme = ui::ThemeId::White;
    rig.idle_ms(1600);
    rig.values.midi.channel = 4;
    rig.idle_ms(1600);
    rig.files.power_begin();
    CHECK_FALSE(rig.files.power_busy());  // and the power-off does not wait for a write that will not come
    rig.idle_ms(20);
    CHECK(card.ops == 0);
    CHECK(card.same("Apps/test/system.o3s", newer));
    CHECK(card.same("Apps/test/system.o3s.bak", good));
    CHECK(rig.values.theme == ui::ThemeId::White);  // the session keeps its values
  }
  {  // another card, one that holds a good file: the protection is the card's, not the session's
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s", newer);
    Rig rig(card, card);
    KitValues v;
    (void)rig.files.load_settings(v);
    CHECK(rig.files.settings_protected());
    card.put("Apps/test/system.o3s", good);
    card.gate_status = hst::WriteStatus::NoCard;
    rig.idle_ms(5);
    card.gate_status = hst::WriteStatus::Ok;
    rig.idle_ms(5);
    CHECK_FALSE(rig.files.settings_protected());
    CHECK(rig.values == saved);
  }
  {  // too big, and unreadable: protected as well
    MemCard card;
    seed(card);
    card.put("Apps/test/system.o3s", Bytes(app::settings::kMaxBytes + 1, 0));
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    CHECK(rig.files.settings_protected());
  }
  {  // damaged: the backup is tried first
    for (const char* how : {"crc", "cut"}) {
      CAPTURE(how);
      Bytes bad = good;
      if (std::strcmp(how, "crc") == 0) {
        bad[20] ^= 0x01;
      } else {
        bad.resize(bad.size() - 9);
      }
      MemCard card;
      seed(card);
      card.put("Apps/test/system.o3s", bad);
      card.put("Apps/test/system.o3s.bak", good);
      Rig rig(card, card);
      KitValues v;
      CHECK(rig.files.load_settings(v) == Files::SettingsLoad::FromBackup);
      CHECK(v == saved);
      CHECK_FALSE(rig.files.settings_protected());
      rig.values = v;
      rig.values.midi.channel = 9;  // and a change writes a good file over the damage
      rig.idle_ms(1600);
      rig.pump();
      KitValues on_card;
      REQUIRE(settings_on_card(card, on_card));
      CHECK(on_card.midi.channel == 9);
      CHECK(on_card.theme == saved.theme);
    }
    MemCard card;  // damaged, and a backup that is no good either: the defaults, a good file by the next change
    seed(card);
    Bytes bad = good;
    bad[20] ^= 0x01;
    card.put("Apps/test/system.o3s", bad);
    card.put("Apps/test/system.o3s.bak", bad);
    Rig rig(card, card);
    KitValues v;
    CHECK(rig.files.load_settings(v) == Files::SettingsLoad::Refused);
    CHECK(rig.files.settings_error() == app::settings::Result::BadCrc);
    CHECK_FALSE(rig.files.settings_protected());
    rig.values.theme = ui::ThemeId::Black;
    rig.idle_ms(1600);
    rig.pump();
    KitValues on_card;
    CHECK(settings_on_card(card, on_card));
  }
}
