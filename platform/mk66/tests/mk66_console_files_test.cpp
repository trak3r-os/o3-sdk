// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `files` command (console/files_command.h): the words o3console and a hardware session read, the worst case in its buffer, and `files crc <path>` through the real shell over a card
// directory (a key press that ends it leaks no file).
#include <doctest/doctest.h>

#include <o3sdk/formats/crc32.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/dir_storage.h>
#include <o3sdk/platform/mk66/console/files_command.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace console = o3sdk::platform::mk66::console;
namespace app = o3sdk::app;
namespace hs = o3sdk::hal::storage;
namespace host = o3sdk::platform::host;
namespace diag = o3sdk::usb::diag;

namespace {

std::string report(const console::FilesFigures& f) {
  char buf[console::kFilesReportRoom];
  const std::size_t n = console::format_files_report(f, buf, sizeof buf);
  REQUIRE(n == std::strlen(buf));
  return std::string(buf, n);
}

bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

struct Capture {
  std::string text;
  static void write(void* self, const char* data, std::size_t n) { static_cast<Capture*>(self)->text.append(data, n); }
  static void flush(void*) {}
  diag::Sink sink() { return diag::Sink{&Capture::write, &Capture::flush, this, nullptr}; }
};

app::Files g_files;  // (an unbound service: the report of a build without a card)
hs::Storage* g_storage = nullptr;
void cmd_files(diag::Shell& sh, const diag::Args& a) { console::run_files_command(sh, g_files, *g_storage, a); }

std::string run_line(diag::Shell& sh, Capture& cap, const char* line, unsigned ticks = 200) {
  cap.text.clear();
  for (const char* p = line; *p != '\0'; ++p) {
    const std::uint8_t b = static_cast<std::uint8_t>(*p);
    (void)sh.feed(&b, 1);
  }
  const std::uint8_t cr = '\r';
  (void)sh.feed(&cr, 1);
  for (unsigned i = 0; i < ticks && (sh.streaming() || i == 0); ++i) {
    sh.tick();
  }
  return cap.text;
}

}  // namespace

TEST_CASE("files report: a service with no card says so, in lines that start files:") {
  const console::FilesFigures f = console::files_figures(g_files);
  const std::string text = report(f);
  CHECK(has(text, "files: app - available no card none generation 1\n"));
  CHECK(has(text, "files: phase idle text - permille 0 listed 0 loaded 0\n"));
  CHECK(has(text, "files: jobs 0 failed 0 steps 0 max-step 0 us overruns 0\n"));
  CHECK(has(text, "files: settings dirty no read not-read applied 0 skipped-keys 0 skipped-values 0 unknown-chunks 0 newer-minor no\n"));
  CHECK_FALSE(has(text, "last-save"));
  std::size_t at = 0;
  unsigned lines = 0;
  while (at < text.size()) {
    const std::size_t nl = text.find('\n', at);
    REQUIRE(nl != std::string::npos);
    CHECK(text.compare(at, 7, "files: ") == 0);
    at = nl + 1;
    ++lines;
  }
  CHECK(lines == 4);
}

TEST_CASE("files report: a card, a finished save and its speed") {
  console::FilesFigures f;
  f.app = "hello";
  f.available = true;
  f.card = app::Files::Card::Ready;
  f.generation = 3;
  f.status = app::Files::Status{app::Files::Status::Done, 1000, "SAVED"};
  f.stats.jobs = 12;
  f.stats.failed = 1;
  f.stats.steps = 140;
  f.stats.max_step_us = 1820;
  f.stats.overruns = 2;
  f.stats.last_bytes = 84;
  f.stats.last_us = 12500;
  f.settings_load = app::Files::SettingsLoad::Loaded;
  f.settings_report.applied = 9;
  f.settings_report.skipped_keys = 1;
  f.settings_report.newer_minor = true;
  const std::string text = report(f);
  CHECK(has(text, "files: app hello available yes card ready generation 3\n"));
  CHECK(has(text, "files: phase done text SAVED permille 1000 listed 0 loaded 0\n"));
  CHECK(has(text, "files: jobs 12 failed 1 steps 140 max-step 1820 us overruns 2\n"));
  CHECK(has(text, "files: last-save 84 bytes 12500 us 6 kB/s\n"));
  CHECK(has(text, "files: settings dirty no read loaded applied 9 skipped-keys 1 skipped-values 0 unknown-chunks 0 newer-minor yes\n"));
}

TEST_CASE("files report: the worst case fits the room, and a short buffer is cut and terminated") {
  console::FilesFigures f;
  f.app = "o3_303-app-name";
  f.available = true;
  f.card = app::Files::Card::OnComputer;
  f.generation = 0xFFFFFFFFu;
  f.status = app::Files::Status{app::Files::Status::Failed, 65535, "CARD ON COMPUTER - EJECT FIRST"};
  f.listed = 0xFFFFFFFFu;
  f.loaded = 0xFFFFFFFFu;
  f.stats = app::Files::Stats{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 1u};
  f.settings_dirty = true;
  f.settings_load = app::Files::SettingsLoad::Refused;
  f.settings_report = app::settings::Report{65535, 65535, 65535, 65535, true};
  const std::string text = report(f);
  CHECK(text.size() < console::kFilesReportRoom);
  CHECK(has(text, "newer-minor yes\n"));
  char tiny[40];
  const std::size_t n = console::format_files_report(f, tiny, sizeof tiny);
  CHECK(n < sizeof tiny);
  CHECK(tiny[n] == '\0');
}

TEST_CASE("files crc: the CRC-32 and size of a card file, a slice a pass, and a key press leaks nothing") {
  host::ScratchDir scratch("files-crc");
  REQUIRE(scratch.ok());
  std::vector<std::uint8_t> bytes(20000);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i * 7u + 3u);
  }
  {
    std::ofstream out(scratch.path() + "/riff.o3a", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  host::DirStorage storage(scratch.path());
  REQUIRE(storage.mount() == hs::Status::Ok);
  g_storage = &storage;
  Capture cap;
  const diag::Command table[] = {diag::command("files", "files [crc <path>]", &cmd_files)};
  diag::Shell sh(table, 1, diag::Output(cap.sink()));

  char want[80];
  std::snprintf(want, sizeof want, "files crc riff.o3a: crc %08x size 20000", o3sdk::formats::crc32(bytes.data(), bytes.size()));
  std::string out = run_line(sh, cap, "files crc riff.o3a");
  CHECK(has(out, want));
  CHECK_FALSE(sh.streaming());

  out = run_line(sh, cap, "files crc nothing.o3a");
  CHECK(has(out, "files crc: nothing.o3a: not found"));
  out = run_line(sh, cap, "files crc");
  CHECK(has(out, "usage: files [crc <path>]"));
  out = run_line(sh, cap, "files");
  CHECK(has(out, "files: app - available no card none"));

  // A CRC that a key press ended after one pass leaves its file open until the next command, which closes it.
  cap.text.clear();
  for (const char* p = "files crc riff.o3a\r"; *p != '\0'; ++p) {
    const std::uint8_t b = static_cast<std::uint8_t>(*p);
    (void)sh.feed(&b, 1);
  }
  sh.tick();
  sh.stop_stream();
  out = run_line(sh, cap, "files crc riff.o3a");
  CHECK(has(out, want));
  console::files_release(storage);
}
