// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app file API on the hello app, end to end in the simulator (docs/M2C-PLAN.md section 3): the scenario apps/hello/scenarios/files.txt runs as it is, the kit's settings (the
// theme, the MIDI channel) survive a reboot and a cold start, a settings file with an unknown key, a newer minor version or a newer major version is read as the format says, a card
// that is out, protected or absent is told apart, and a power cut at every write of a HELLO save leaves the old riff or the new one, never neither. The kit's own rules are
// app_files_test.cpp and app_settings_test.cpp.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/settings.h>
#include <o3sdk/formats/chunk_file.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_firmware.h>
#include <o3sdk/platform/host/fake_midi.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace o3sdk;
namespace host = o3sdk::platform::host;
namespace hst = o3sdk::hal::storage;
using o3sdk::test::MemCard;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::string state(sim::Harness& h) {
  char line[512];
  (void)h.runtime().state_line(line, sizeof line);
  return line;
}

bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

void tap(sim::Harness& h, app::Button b) {
  h.press(b);
  h.run_ms(1);
  h.release(b);
  h.run_ms(1);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

Bytes read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const Bytes& b) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A settings file by hand: SETS of `version` with the entries, and `chunks` more.
Bytes settings_file(std::uint16_t version, const std::vector<std::pair<std::uint16_t, float>>& entries, unsigned unknown_chunks, std::size_t trailing) {
  Bytes buf(512);
  formats::chunk::Writer w(buf.data(), buf.size(), "O3SK");
  REQUIRE(w.begin("SETS"));
  REQUIRE(w.u16(version));
  REQUIRE(w.u16(static_cast<std::uint16_t>(entries.size())));
  for (const auto& e : entries) {
    REQUIRE(w.u16(e.first));
    REQUIRE(w.f32(e.second));
  }
  const std::uint8_t zeros[8] = {};
  REQUIRE(w.bytes(zeros, trailing));
  REQUIRE(w.end());
  for (unsigned i = 0; i < unknown_chunks; ++i) {
    REQUIRE(w.begin("APPV"));
    REQUIRE(w.u16(0x0100));
    REQUIRE(w.end());
  }
  buf.resize(w.finish());
  return buf;
}

// HELLO's card as the harness wants it, over an in-memory card (to cut it at every write).
class MemHostCard final : public host::HostCard {
 public:
  explicit MemHostCard(MemCard& m) : m_(m) {}
  void set_present(bool present) override { m_.gate_status = present ? hst::WriteStatus::Ok : hst::WriteStatus::NoCard; }
  unsigned violations() const override { return m_.violations; }
  hst::Status mount() noexcept override { return m_.mount(); }
  bool mounted() const noexcept override { return m_.gate_status == hst::WriteStatus::Ok; }
  bool card_present() const noexcept override { return mounted(); }
  hst::VolumeInfo volume() const noexcept override { return m_.volume(); }
  hst::Status exists(const char* p, bool* d) noexcept override { return m_.exists(p, d); }
  hst::Status stat(const char* p, hst::FileStamp& o) noexcept override { return m_.stat(p, o); }
  hst::Status open(const char* p, hst::FileHandle& o) noexcept override { return m_.open(p, o); }
  hst::Status read(hst::FileHandle f, void* d, std::size_t n, std::size_t& g) noexcept override { return m_.read(f, d, n, g); }
  hst::Status seek(hst::FileHandle f, std::uint32_t o) noexcept override { return m_.seek(f, o); }
  hst::Status size(hst::FileHandle f, std::uint32_t& o) const noexcept override { return m_.size(f, o); }
  void close(hst::FileHandle& f) noexcept override { m_.close(f); }
  hst::Status opendir(const char* p, hst::DirHandle& o) noexcept override { return m_.opendir(p, o); }
  hst::Status readdir(hst::DirHandle d, hst::DirEntry& o) noexcept override { return m_.readdir(d, o); }
  void closedir(hst::DirHandle& d) noexcept override { m_.closedir(d); }
  hst::WriteStatus gate() noexcept override { return m_.gate(); }
  hst::WriteStatus make_dir(const char* p) noexcept override { return m_.make_dir(p); }
  hst::WriteStatus create(const char* p) noexcept override { return m_.create(p); }
  hst::WriteStatus write(const void* s, std::size_t n) noexcept override { return m_.write(s, n); }
  hst::WriteStatus finish() noexcept override { return m_.finish(); }
  void abandon() noexcept override { m_.abandon(); }
  bool writing() const noexcept override { return m_.writing(); }
  hst::WriteStatus rename(const char* a, const char* b) noexcept override { return m_.rename(a, b); }
  hst::WriteStatus create_marker(const char* p) noexcept override { return m_.create_marker(p); }
  hst::WriteStatus remove(const char* p) noexcept override { return m_.remove(p); }
  void begin_save() noexcept override { m_.begin_save(); }
  void end_save() noexcept override { m_.end_save(); }
  std::uint32_t card_id() const noexcept override { return m_.card_id(); }

 private:
  MemCard& m_;
};

}  // namespace

#ifdef O3SDK_HELLO_SCENARIOS
TEST_CASE("sim files: scenarios/files.txt runs as it is, and leaves the files it says") {
  host::ScratchDir card_dir("sim-files-card");
  host::ScratchDir out_dir("sim-files-out");
  REQUIRE(card_dir.ok());
  REQUIRE(out_dir.ok());
  host::DirCard card(card_dir.path());
  host::FakeMidi midi;  // (the scenario has a MIDI line: the MIDI section of the Config needs a device)
  sim::Harness h(o3sdk_app(), nullptr, &midi, &card);
  REQUIRE(h.start());
  const std::filesystem::path file = std::filesystem::path(O3SDK_HELLO_SCENARIOS) / "files.txt";
  const std::string text = read_text(file);
  REQUIRE_FALSE(text.empty());
  const sim::ScenarioResult r = sim::run_scenario(h, text, out_dir.path());
  CHECK_MESSAGE(r.ok, file.string() << ":" << r.line << ": " << r.error);
  CHECK(r.snapshots == 8);
  CHECK(card.violations() == 0u);
  const std::filesystem::path apps = std::filesystem::path(card_dir.path()) / "Apps" / "hello";
  CHECK(std::filesystem::is_regular_file(apps / "riff.o3a"));
  CHECK(std::filesystem::is_regular_file(apps / "riff.o3a.bak"));
  CHECK(std::filesystem::is_regular_file(apps / "system.o3s"));
  CHECK_FALSE(std::filesystem::exists(apps / "_o3save.tmp"));
  CHECK(h.runtime().frame_failures() == 0u);
}
#endif

#ifdef O3SDK_HELLO_SCENARIOS
TEST_CASE("sim files: the FIRMWARE section says SAVING while a card job runs, and opens once it is over") {
  host::ScratchDir card_dir("sim-files-fw");
  host::DirCard card(card_dir.path());
  host::FakeFirmwareSink sink;
  std::string error;
  REQUIRE(sim::load_firmware_dir(sink, std::string(O3SDK_HELLO_SCENARIOS) + "/firmware", error));
  sim::Harness h(o3sdk_app(), &sink, nullptr, &card);
  REQUIRE(h.start());
  static Bytes big(200 * 1024, 0x5A);
  app::Files& files = h.runtime().context().files();
  REQUIRE(files.save("", "big.o3a", big.data(), big.size()));
  h.run_ms(3);
  CHECK(files.busy());
  tap(h, app::Button::Config);  // the FIRMWARE section is the first: opening its list asks the blocker
  h.run_ms(5);
  CHECK(has(state(h), "sys=1 section=firmware fw=closed"));
  CHECK(files.busy());
  tap(h, app::Button::Config);  // closes the screen
  h.run_ms(1500);
  CHECK_FALSE(files.busy());
  CHECK(files.status().phase == app::Files::Status::Done);
  tap(h, app::Button::Config);
  h.run_ms(5);
  CHECK(has(state(h), "sys=1 section=firmware fw=list"));
  CHECK(card.violations() == 0u);
}
#endif

TEST_CASE("sim files: scenario expects fail with their line, and card commands need a card") {
  host::ScratchDir card_dir("sim-files-expect");
  host::ScratchDir out_dir("sim-files-expect-out");
  host::DirCard card(card_dir.path());
  sim::Harness h(o3sdk_app(), nullptr, nullptr, &card);
  REQUIRE(h.start());
  sim::ScenarioResult r = sim::run_scenario(h, "wait 10\nexpect state card=ready\nexpect file Apps/hello/riff.o3a\n", out_dir.path());
  CHECK_FALSE(r.ok);
  CHECK(r.line == 3);
  r = sim::run_scenario(h, "expect nofile Apps/hello/riff.o3a\nexpect state card=none\n", out_dir.path());
  CHECK_FALSE(r.ok);
  CHECK(r.line == 2);
  r = sim::run_scenario(h, "card out\nexpect state card=none\ncard in\nexpect state card=ready\ncard protect on\nexpect state card=readonly\ncard protect off\nexpect state card=ready\n", out_dir.path());
  CHECK_MESSAGE(r.ok, r.error);
  r = sim::run_scenario(h, "card sideways\n", out_dir.path());
  CHECK_FALSE(r.ok);

  sim::Harness bare(o3sdk_app());  // no card
  REQUIRE(bare.start());
  r = sim::run_scenario(bare, "card out\n", out_dir.path());
  CHECK_FALSE(r.ok);
  r = sim::run_scenario(bare, "expect file Apps/hello/x.o3a\n", out_dir.path());
  CHECK_FALSE(r.ok);
  r = sim::run_scenario(bare, "expect state card=none\nexpect state files=idle\nreboot\nexpect state card=none\n", out_dir.path());
  CHECK_MESSAGE(r.ok, r.error);
}

TEST_CASE("sim files: HELLO saves its riff, a protected card refuses and says so, a card put back works, and a load restores the riff") {
  host::ScratchDir card_dir("sim-files-save");
  host::DirCard card(card_dir.path());
  sim::Harness h(o3sdk_app(), nullptr, nullptr, &card);
  REQUIRE(h.start());
  const std::filesystem::path riff = std::filesystem::path(card_dir.path()) / "Apps" / "hello" / "riff.o3a";
  CHECK(has(state(h), "card=ready files=idle"));
  tap(h, app::Button::Delete);  // (the app object is shared by every test: the default riff first)
  CHECK(has(state(h), "riff=0"));
  tap(h, app::Button::Insert);
  tap(h, app::Button::Insert);
  CHECK(has(state(h), "riff=2"));
  tap(h, app::Button::File);
  h.run_ms(60);
  CHECK(has(state(h), "files=done"));
  REQUIRE(std::filesystem::is_regular_file(riff));
  const Bytes saved = read_bytes(riff.string());
  CHECK(saved.size() > 40);
  CHECK(std::memcmp(saved.data(), "FORM", 4) == 0);
  CHECK(std::memcmp(saved.data() + 8, "HRIF", 4) == 0);

  // The save does not stop the sound or the screen: frames keep coming while it runs.
  h.record(true);
  tap(h, app::Button::Insert);
  const std::uint32_t frames = h.runtime().frames();
  tap(h, app::Button::File);
  h.run_ms(40);
  CHECK(h.runtime().frames() > frames);
  CHECK(h.runtime().frame_failures() == 0u);
  CHECK(h.engine().stats().blocks > 20u);
  h.record(false);

  h.card_protect(true);
  tap(h, app::Button::File);
  h.run_ms(20);
  CHECK(has(state(h), "card=readonly files=failed"));
  h.card_protect(false);
  tap(h, app::Button::Insert);  // riff 4 in memory, 3 on the card
  tap(h, app::Button::SampleLoad);
  h.run_ms(60);
  CHECK(has(state(h), "files=done"));
  CHECK(has(state(h), "riff=3"));
  h.card_out();
  tap(h, app::Button::SampleLoad);
  h.run_ms(20);
  CHECK(has(state(h), "card=none files=failed"));
  h.card_in();
  tap(h, app::Button::SampleLoad);
  h.run_ms(60);
  CHECK(has(state(h), "card=ready files=done"));
  CHECK(card.violations() == 0u);

  // A file that is not a riff (the load worked and the content is not ours) changes nothing.
  write_bytes(riff.string(), Bytes(60, 7));
  tap(h, app::Button::Insert);
  tap(h, app::Button::SampleLoad);
  h.run_ms(60);
  CHECK(has(state(h), "riff=4"));
}

TEST_CASE("sim files: the theme and the MIDI channel survive a reboot and a cold start; defaults are never written") {
  host::ScratchDir card_dir("sim-files-settings");
  const std::filesystem::path system = std::filesystem::path(card_dir.path()) / "Apps" / "hello" / "system.o3s";
  host::DirCard card(card_dir.path());
  {
    host::FakeMidi midi;
    sim::Harness h(o3sdk_app(), nullptr, &midi, &card);
    REQUIRE(h.start());
    h.run_ms(2000);
    CHECK_FALSE(std::filesystem::exists(system));  // nothing the user changed, nothing written
    CHECK(has(state(h), "theme=0 midich=0"));

    app::Context& ctx = h.runtime().context();
    ctx.set_theme(ui::ThemeId::White);
    hal::midi::Config c = ctx.midi().config();
    c.channel = 9;
    c.follow = true;
    ctx.midi().set_config(c);
    h.run_ms(1400);
    CHECK_FALSE(std::filesystem::exists(system));  // not yet: 1.5 s of quiet
    h.run_ms(300);
    CHECK(std::filesystem::is_regular_file(system));

    REQUIRE(h.reboot());
    CHECK(has(state(h), "theme=2 midich=9"));
    CHECK(h.runtime().context().midi().config().follow);
    CHECK(h.runtime().context().theme() == ui::ThemeId::White);
    h.run_ms(2000);
    CHECK(has(state(h), "card=ready"));
  }
  {  // a cold start: another harness over the same directory
    host::FakeMidi midi;
    sim::Harness h(o3sdk_app(), nullptr, &midi, &card);
    REQUIRE(h.start());
    CHECK(has(state(h), "theme=2 midich=9"));
    CHECK(h.runtime().context().files().settings_load() == app::Files::SettingsLoad::Loaded);
    // The Config screen's own change, with keys: DISPLAY's theme steps to the next.
    tap(h, app::Button::Config);
    h.run_ms(50);
    CHECK(h.runtime().system_open());
    tap(h, app::Button::Right);  // MIDI -> CARD (no FIRMWARE section here)
    tap(h, app::Button::Right);  // CARD -> DISPLAY
    CHECK(has(state(h), "section=display"));
    h.turn(1);
    h.run_ms(1);
    tap(h, app::Button::Config);
    h.run_ms(1700);
    REQUIRE(h.reboot());
    CHECK_FALSE(has(state(h), "theme=2 "));
    CHECK(has(state(h), "midich=9"));
  }
  CHECK(card.violations() == 0u);
}

TEST_CASE("sim files: a settings file with an unknown key, a newer minor and a chunk it does not know is read as far as it is known, and not rewritten") {
  host::ScratchDir card_dir("sim-files-newer");
  const std::string system = card_dir.path() + "/Apps/hello/system.o3s";
  // Version 1.2 with a key of a later firmware (77), the theme White and channel 9, an APPV chunk and two bytes beyond the entries.
  const Bytes file = settings_file(0x0102, {{1, 2.0f}, {77, 1.0f}, {2, 9.0f}}, 1, 2);
  write_bytes(system, file);
  host::DirCard card(card_dir.path());
  host::FakeMidi midi;
  sim::Harness h(o3sdk_app(), nullptr, &midi, &card);
  REQUIRE(h.start());
  const app::Files& files = h.runtime().context().files();
  CHECK(files.settings_load() == app::Files::SettingsLoad::Loaded);
  CHECK(has(state(h), "theme=2 midich=9"));
  CHECK(files.settings_report().applied == 2);
  CHECK(files.settings_report().skipped_keys == 1);
  CHECK(files.settings_report().unknown_chunks == 1);
  CHECK(files.settings_report().newer_minor);
  h.run_ms(2500);
  CHECK(read_bytes(system) == file);  // what was read is what is saved: nothing is rewritten for it
  // The user changes something: the file is rewritten by this version (it knows nine keys; the later firmware's is dropped).
  h.runtime().context().set_theme(ui::ThemeId::Black);
  h.run_ms(1700);
  const Bytes after = read_bytes(system);
  CHECK(after != file);
  app::KitValues v;
  app::settings::Report rep;
  CHECK(app::settings::read(after.data(), after.size(), v, rep) == app::settings::Result::Ok);
  CHECK(v.theme == ui::ThemeId::Black);
  CHECK(v.midi.channel == 9);
  CHECK(rep.skipped_keys == 0);
}

TEST_CASE("sim files: a settings file of a newer major version, a damaged one or one the card cannot read leaves the app's defaults, and nothing is overwritten") {
  const std::pair<const char*, Bytes> cases[] = {
      {"newer major", settings_file(0x0200, {{1, 2.0f}, {2, 9.0f}}, 0, 0)},
      {"damaged", [] {
         Bytes b = settings_file(0x0100, {{1, 2.0f}}, 0, 0);
         b[30] ^= 0x04;
         return b;
       }()},
      {"truncated", [] {
         Bytes b = settings_file(0x0100, {{1, 2.0f}}, 0, 0);
         b.resize(b.size() - 9);
         return b;
       }()},
      {"empty", Bytes()},
  };
  for (const auto& c : cases) {
    CAPTURE(c.first);
    host::ScratchDir card_dir("sim-files-refused");
    const std::string system = card_dir.path() + "/Apps/hello/system.o3s";
    write_bytes(system, c.second);
    host::DirCard card(card_dir.path());
    sim::Harness h(o3sdk_app(), nullptr, nullptr, &card);
    REQUIRE(h.start());
    CHECK(h.runtime().context().files().settings_load() == app::Files::SettingsLoad::Refused);
    CHECK(has(state(h), "theme=0 midich=0"));
    h.run_ms(2000);
    CHECK(read_bytes(system) == c.second);  // the user's file stays as it is until a setting changes
    h.runtime().context().set_theme(ui::ThemeId::Black);
    h.run_ms(1700);
    h.runtime().context().set_theme(ui::ThemeId::White);
    h.run_ms(1700);
    const Bytes now = read_bytes(system);
    if (std::strcmp(c.first, "newer major") == 0) {
      CHECK(h.runtime().context().files().settings_protected());
      CHECK(now == c.second);  // the user's newer file is never overwritten, however often a setting changes
      CHECK_FALSE(std::filesystem::exists(system + ".bak"));
      CHECK(h.runtime().context().theme() == ui::ThemeId::White);  // the session keeps its values
    } else {
      app::KitValues v;
      app::settings::Report rep;
      CHECK(app::settings::read(now.data(), now.size(), v, rep) == app::settings::Result::Ok);  // the next save writes a good file over the damage
      CHECK(v.theme == ui::ThemeId::White);
    }
  }
}

TEST_CASE("sim files: without a card the app runs, files say NO CARD and the kit keeps nothing") {
  sim::Harness h(o3sdk_app());
  REQUIRE(h.start());
  CHECK_FALSE(h.runtime().context().files().available());
  CHECK(has(state(h), "card=none files=idle"));
  tap(h, app::Button::File);
  h.run_ms(20);
  CHECK(has(state(h), "card=none files=failed"));
  h.runtime().context().set_theme(ui::ThemeId::White);
  h.run_ms(2000);
  REQUIRE(h.reboot());
  CHECK(has(state(h), "theme=0 "));
  CHECK(h.runtime().frame_failures() == 0u);
}

TEST_CASE("sim files: a power cut at every write of a HELLO save leaves the old riff or the new one, never neither") {
  MemCard base;
  base.make_dir_now("Apps");
  base.make_dir_now("Apps/hello");
  {  // the card of a user who saved a riff before: the default riff, saved by HELLO itself
    MemHostCard card(base);
    sim::Harness h(o3sdk_app(), nullptr, nullptr, &card);
    REQUIRE(h.start());
    tap(h, app::Button::Delete);  // (the app object is shared by every test: the default riff)
    tap(h, app::Button::File);
    h.run_ms(80);
    REQUIRE(has(state(h), "files=done"));
  }
  REQUIRE(base.has("Apps/hello/riff.o3a"));
  const Bytes old_file = *base.get("Apps/hello/riff.o3a");
  unsigned cuts = 0;
  bool completed = false;
  for (std::size_t cut = 0; cut < 100 && !completed; ++cut) {
    MemCard mem;
    mem.files = base.files;
    mem.dirs = base.dirs;
    MemHostCard card(mem);
    sim::Harness h(o3sdk_app(), nullptr, nullptr, &card);
    REQUIRE(h.start());
    tap(h, app::Button::Delete);  // riff 0 in memory, whatever an earlier iteration left
    REQUIRE(has(state(h), "riff=0"));
    tap(h, app::Button::Insert);
    tap(h, app::Button::Insert);  // riff 2 is the new one
    mem.ops = 0;
    mem.cut_at = cut;
    tap(h, app::Button::File);
    h.run_ms(100);
    CAPTURE(cut);
    if (!mem.dead) {  // the save finished before the cut: whole
      completed = true;
      CHECK(has(state(h), "files=done"));
      CHECK(mem.get("Apps/hello/riff.o3a") != nullptr);
      CHECK(*mem.get("Apps/hello/riff.o3a") != old_file);
      break;
    }
    ++cuts;
    mem.revive();  // the power is back
    const bool f = mem.has("Apps/hello/riff.o3a");
    const bool b = mem.has("Apps/hello/riff.o3a.bak");
    CHECK((f || b));  // never neither
    // A restart and a LOAD find the riff, the old or the new one, and nothing else.
    REQUIRE(h.reboot());
    tap(h, app::Button::Insert);
    tap(h, app::Button::Insert);
    tap(h, app::Button::Insert);  // riff 3 in memory: a load must put 0 or 2 back
    tap(h, app::Button::SampleLoad);
    h.run_ms(60);
    const std::string s = state(h);
    CHECK(has(s, "files=done"));
    CHECK((has(s, "riff=0") || has(s, "riff=2")));
    // And the next save heals the card.
    tap(h, app::Button::File);
    h.run_ms(100);
    CHECK(has(state(h), "files=done"));
    CHECK(mem.has("Apps/hello/riff.o3a"));
    CHECK_FALSE(mem.has("Apps/hello/_o3save.tmp"));
    CHECK(mem.violations == 0u);
  }
  CHECK(completed);
  CHECK(cuts >= 6);
}

TEST_CASE("sim files: a card put in after the boot has its theme and channel applied, and one change keeps the other keys of its file") {
  host::ScratchDir card_dir("sim-files-late");
  const std::string system = card_dir.path() + "/Apps/hello/system.o3s";
  app::KitValues held;
  held.theme = ui::ThemeId::White;
  held.midi.channel = 9;
  held.midi.follow = true;
  held.midi.out_channel = 14;
  Bytes file(app::settings::kMaxBytes);
  file.resize(app::settings::write(held, file.data(), file.size()));
  write_bytes(system, file);
  host::DirCard card(card_dir.path());
  card.set_present(false);  // the unit boots with no card in the slot
  host::FakeMidi midi;
  sim::Harness h(o3sdk_app(), nullptr, &midi, &card);
  REQUIRE(h.start());
  CHECK(has(state(h), "card=none"));
  CHECK(has(state(h), "theme=0 midich=0"));
  h.run_ms(2000);
  h.runtime().context().set_theme(ui::ThemeId::Black);  // the user changes the theme with no card: nothing can be written
  h.run_ms(1700);
  CHECK(read_bytes(system) == file);
  h.card_in();  // the card comes in
  h.run_ms(20);
  CHECK(has(state(h), "card=ready"));
  CHECK(has(state(h), "theme=2 midich=9"));  // the file's values are the user's
  h.run_ms(2000);
  CHECK(read_bytes(system) == file);  // and nothing is written for them
  h.runtime().context().set_theme(ui::ThemeId::Black);
  h.run_ms(1700);
  const Bytes after = read_bytes(system);
  app::KitValues v;
  app::settings::Report rep;
  REQUIRE(app::settings::read(after.data(), after.size(), v, rep) == app::settings::Result::Ok);
  CHECK(v.theme == ui::ThemeId::Black);
  CHECK(v.midi.channel == 9);
  CHECK(v.midi.out_channel == 14);
  CHECK(v.midi.follow);
  CHECK(card.violations() == 0u);
}
