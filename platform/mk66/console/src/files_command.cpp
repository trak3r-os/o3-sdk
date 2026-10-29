// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/console/files_command.h>

#include <o3sdk/formats/crc32.h>
#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/storage/ls_command.h>

#include <cstring>

namespace o3sdk::platform::mk66::console {
namespace {

namespace shell = o3sdk::usb::diag;
using audio::diag::Text;
using hal::storage::Status;

const char* yes(bool v) noexcept { return v ? "yes" : "no"; }

const char* card_word(app::Files::Card c) noexcept {
  switch (c) {
    case app::Files::Card::None: return "none";
    case app::Files::Card::Ready: return "ready";
    case app::Files::Card::ReadOnly: return "readonly";
    case app::Files::Card::Unsupported: return "unsupported";
    case app::Files::Card::OnComputer: return "computer";
    case app::Files::Card::InUse: return "inuse";
  }
  return "none";
}

const char* phase_word(app::Files::Status::Phase p) noexcept {
  switch (p) {
    case app::Files::Status::Idle: return "idle";
    case app::Files::Status::Running: return "running";
    case app::Files::Status::Done: return "done";
    case app::Files::Status::Failed: return "failed";
  }
  return "idle";
}

const char* load_word(app::Files::SettingsLoad l) noexcept {
  switch (l) {
    case app::Files::SettingsLoad::Unavailable: return "unavailable";
    case app::Files::SettingsLoad::NoCard: return "no-card";
    case app::Files::SettingsLoad::NoFile: return "no-file";
    case app::Files::SettingsLoad::Loaded: return "loaded";
    case app::Files::SettingsLoad::FromBackup: return "backup";
    case app::Files::SettingsLoad::Refused: return "refused";
    case app::Files::SettingsLoad::NotRead: return "not-read";
  }
  return "?";
}

// The state of one `files crc` (the shell has one stream, so there is one).
struct CrcRun {
  hal::storage::Storage* storage = nullptr;
  hal::storage::FileHandle file{};
  formats::Crc32 crc;
  std::uint32_t bytes = 0;
  char path[hal::storage::kPathMax + 1] = {};
};
CrcRun g_crc;

bool crc_step(shell::Shell& sh, void*) {
  std::uint8_t buf[1024];
  std::uint32_t this_tick = 0;
  while (this_tick < kFilesCrcBytesPerTick) {
    std::size_t got = 0;
    const Status s = g_crc.storage->read(g_crc.file, buf, sizeof buf, got);
    if (s != Status::Ok) {
      sh.out().text("files crc: ").text(g_crc.path).text(": ").text(hal::storage::status_name(s)).nl();
      g_crc.storage->close(g_crc.file);
      return false;
    }
    if (got == 0u) {  // the end of the file
      char line[hal::storage::kPathMax + 48];
      Text t(line, sizeof line);
      t.str("files crc ").str(g_crc.path).str(": crc ");
      const std::uint32_t crc = g_crc.crc.value();
      for (int shift = 28; shift >= 0; shift -= 4) {
        t.chr("0123456789abcdef"[(crc >> shift) & 0xFu]);
      }
      t.str(" size ").dec(g_crc.bytes).nl();
      sh.out().text(line, t.size());
      g_crc.storage->close(g_crc.file);
      return false;
    }
    g_crc.crc.update(buf, got);
    g_crc.bytes += static_cast<std::uint32_t>(got);
    this_tick += static_cast<std::uint32_t>(got);
  }
  return true;
}

}  // namespace

FilesFigures files_figures(const app::Files& files) noexcept {
  FilesFigures f;
  f.app = files.app_id();
  f.available = files.available();
  f.card = files.card();
  f.generation = files.generation();
  f.status = files.status();
  f.listed = files.listed();
  f.loaded = files.loaded();
  f.stats = files.stats();
  f.settings_dirty = files.settings_dirty();
  f.settings_load = files.settings_load();
  f.settings_report = files.settings_report();
  return f;
}

std::size_t format_files_report(const FilesFigures& f, char* buf, std::size_t cap) noexcept {
  Text t(buf, cap);
  t.str("files: app ").str(f.app[0] != '\0' ? f.app : "-").str(" available ").str(yes(f.available)).str(" card ").str(card_word(f.card)).str(" generation ").dec(f.generation).nl();
  t.str("files: phase ").str(phase_word(f.status.phase)).str(" text ").str(f.status.text[0] != '\0' ? f.status.text : "-").str(" permille ").dec(f.status.permille);
  t.str(" listed ").dec(static_cast<std::uint32_t>(f.listed)).str(" loaded ").dec(f.loaded).nl();
  t.str("files: jobs ").dec(f.stats.jobs).str(" failed ").dec(f.stats.failed).str(" steps ").dec(f.stats.steps).str(" max-step ").dec(f.stats.max_step_us).str(" us overruns ").dec(f.stats.overruns).nl();
  if (f.stats.last_bytes != 0u && f.stats.last_us != 0u) {
    const std::uint64_t kbs = static_cast<std::uint64_t>(f.stats.last_bytes) * 1000u / f.stats.last_us;
    t.str("files: last-save ").dec(f.stats.last_bytes).str(" bytes ").dec(f.stats.last_us).str(" us ").dec(static_cast<std::uint32_t>(kbs)).str(" kB/s").nl();
  }
  const app::settings::Report& r = f.settings_report;
  t.str("files: settings dirty ").str(yes(f.settings_dirty)).str(" read ").str(load_word(f.settings_load)).str(" applied ").dec(r.applied);
  t.str(" skipped-keys ").dec(r.skipped_keys).str(" skipped-values ").dec(r.skipped_values).str(" unknown-chunks ").dec(r.unknown_chunks).str(" newer-minor ").str(yes(r.newer_minor)).nl();
  return t.size();
}

void files_release(hal::storage::Storage& storage) noexcept {
  if (g_crc.file.is_open() && g_crc.storage == &storage) {
    storage.close(g_crc.file);
  }
  g_crc.file = hal::storage::FileHandle{};
}

void run_files_command(shell::Shell& sh, const app::Files& files, hal::storage::Storage& storage, const shell::Args& args) noexcept {
  if (g_crc.file.is_open() && g_crc.storage != nullptr) {
    g_crc.storage->close(g_crc.file);  // a CRC a key press ended left its file open
    g_crc.file = hal::storage::FileHandle{};
  }
  if (args.argc == 1) {
    char buf[kFilesReportRoom];
    const std::size_t n = format_files_report(files_figures(files), buf, sizeof buf);
    sh.out().text(buf, n);
    return;
  }
  if (args.argc >= 3 && std::strcmp(args.argv[1], "crc") == 0) {
    if (!join_path_args(args, 2, g_crc.path, sizeof g_crc.path)) {
      sh.out().text("files crc: path too long\n");
      return;
    }
    if (!storage.mounted()) {
      sh.out().text("files crc: the card is not mounted (`sd` says why)\n");
      return;
    }
    const Status s = storage.open(g_crc.path, g_crc.file);
    if (s != Status::Ok) {
      g_crc.file = hal::storage::FileHandle{};
      sh.out().text("files crc: ").text(g_crc.path).text(": ").text(hal::storage::status_name(s)).nl();
      return;
    }
    g_crc.storage = &storage;
    g_crc.crc.reset();
    g_crc.bytes = 0;
    sh.start_stream(&crc_step, nullptr, true);
    return;
  }
  sh.out().text("usage: files [crc <path>]\n");
}

}  // namespace o3sdk::platform::mk66::console
