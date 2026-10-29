// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The console's `files` command (docs/M2C-PLAN.md section 3, slice FA; o3-sdk's own, a read): what the app file API is doing and has cost, and the CRC-32 of a file of the card to compare
// with the simulator's.
//
//   files             one report of lines that start `files:` and are made of `name value` words, so that o3console's expect patterns and a person read the same:
//     files: app hello available yes card ready generation 3
//     files: phase done text SAVED permille 1000 listed 0 loaded 0
//     files: jobs 12 failed 1 steps 140 max-step 1820 us overruns 0
//     files: last-save 84 bytes 12500 us 6 kB/s            (only after a save)
//     files: settings dirty no read loaded applied 9 skipped-keys 0 skipped-values 0 unknown-chunks 0 newer-minor no
//   files crc <path>  the CRC-32 and size of the file `path` (a card path, spaces allowed), read a slice a pass of the main loop like `ls`:
//     files crc Apps/hello/riff.o3a: crc 1a2b3c4d size 84
//
// `max-step` is the longest slice of the main loop a job took (the budget is Files::kJobBudgetUs): the number a card's latency is judged by on the unit. The command only reads: the card
// through hal::storage::Storage and the service through its const accessors. Pure: the figures and the shell are arguments, so the host tests pin the words.

#include <o3sdk/app/files.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::console {

// The room the console gives the report; the worst case (every counter at its maximum) is pinned to fit it.
inline constexpr std::size_t kFilesReportRoom = 640;
inline constexpr std::uint32_t kFilesCrcBytesPerTick = 8192;  // `files crc` reads this much a pass of the main loop

struct FilesFigures {
  const char* app = "";
  bool available = false;
  app::Files::Card card = app::Files::Card::None;
  std::uint32_t generation = 0;
  app::Files::Status status{app::Files::Status::Idle, 0, ""};
  std::size_t listed = 0;
  std::uint32_t loaded = 0;
  app::Files::Stats stats;
  bool settings_dirty = false;
  app::Files::SettingsLoad settings_load = app::Files::SettingsLoad::NotRead;
  app::settings::Report settings_report;
};

FilesFigures files_figures(const app::Files& files) noexcept;

// The report into `buf` (at most `cap` bytes with the terminating zero); returns the length.
std::size_t format_files_report(const FilesFigures& f, char* buf, std::size_t cap) noexcept;

// The command: `files` and `files crc <path>`.
void run_files_command(o3sdk::usb::diag::Shell& shell, const app::Files& files, hal::storage::Storage& storage, const o3sdk::usb::diag::Args& args) noexcept;

// Closes the file a `files crc` that a key press ended left open. Called before a new one and by the tests.
void files_release(hal::storage::Storage& storage) noexcept;

}  // namespace o3sdk::platform::mk66::console
