// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/hal/storage.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>
#include <cstdint>

// `ls [path]` (OS-007e): a directory of the card, one entry a line (a directory as "d  name/", a file with its size). A pager like `log`: the
// listing is the shell's one stream and writes a few lines a pass, only while the output has room, so a long directory loses no line to a
// slow host; any key stops it. Pure: the storage and the shell are arguments, so the host tests run it against a FAT image. A path with
// spaces is taken as typed (the line is split on spaces, the words are joined with one space again).
namespace o3sdk::platform::mk66::console {

inline constexpr std::uint32_t kLsLinesPerTick = 8;  // bounds one pass of the main loop

void run_ls_command(o3sdk::usb::diag::Shell& shell, hal::storage::Storage& storage, const o3sdk::usb::diag::Args& args) noexcept;

// The path of a command line's arguments from argv[first] on, joined with single spaces into `out`. False when it does not fit or there are
// none (the caller substitutes a default).
bool join_path_args(const o3sdk::usb::diag::Args& args, unsigned first, char* out, std::size_t cap) noexcept;

// Closes the directory a pager left open (a listing that a key press ended). Called before a new listing and by the tests.
void ls_release(hal::storage::Storage& storage) noexcept;

}  // namespace o3sdk::platform::mk66::console
