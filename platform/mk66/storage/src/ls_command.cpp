// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/ls_command.h>

#include <o3sdk/platform/mk66/storage/sd_text.h>

#include <cstring>

namespace o3sdk::platform::mk66::console {
namespace {

namespace shell = o3sdk::usb::diag;
using hal::storage::Status;

// A line is at most 14 + 255 bytes and a newline (CR LF on the wire): the pager writes one only while the output has room for twice that.
constexpr std::size_t kLineRoom = 560;

struct Pager {
  hal::storage::Storage* storage = nullptr;
  hal::storage::DirHandle dir{};
  std::uint32_t entries = 0;
};
Pager g_pager;  // the shell has one stream, so there is one pager

bool pager_step(shell::Shell& sh, void*) {
  char buf[hal::storage::kNameMax + 32];
  hal::storage::DirEntry e;
  for (std::uint32_t lines = 0; lines < kLsLinesPerTick && sh.out().room() >= kLineRoom; ++lines) {
    const Status s = g_pager.storage->readdir(g_pager.dir, e);
    if (s == Status::Ok) {
      ++g_pager.entries;
      const std::size_t n = storage::format_ls_entry(e, buf, sizeof buf);
      sh.out().text(buf, n);
      continue;
    }
    if (s != Status::End) {
      sh.out().text("ls: ").text(hal::storage::status_name(s)).nl();
    }
    sh.out().dec(g_pager.entries).text(g_pager.entries == 1u ? " entry\n" : " entries\n");
    g_pager.storage->closedir(g_pager.dir);
    return false;  // finished
  }
  return true;
}

}  // namespace

bool join_path_args(const shell::Args& args, unsigned first, char* out, std::size_t cap) noexcept {
  if (args.argc <= first || cap == 0u) {
    return false;
  }
  std::size_t n = 0;
  for (unsigned i = first; i < args.argc; ++i) {
    const std::size_t len = std::strlen(args.argv[i]);
    const std::size_t need = len + (i > first ? 1u : 0u);
    if (n + need + 1u > cap) {
      return false;
    }
    if (i > first) {
      out[n++] = ' ';
    }
    std::memcpy(out + n, args.argv[i], len);
    n += len;
  }
  out[n] = '\0';
  return true;
}

void ls_release(hal::storage::Storage& storage) noexcept {
  if (g_pager.dir.is_open() && g_pager.storage == &storage) {
    storage.closedir(g_pager.dir);
  }
  g_pager.dir = hal::storage::DirHandle{};
}

void run_ls_command(shell::Shell& sh, hal::storage::Storage& storage, const shell::Args& args) noexcept {
  if (g_pager.dir.is_open() && g_pager.storage != nullptr) {
    g_pager.storage->closedir(g_pager.dir);  // a listing a key press ended left its directory open
  }
  char path[hal::storage::kPathMax + 1];
  if (!join_path_args(args, 1, path, sizeof path)) {
    if (args.argc > 1) {
      sh.out().text("ls: path too long\n");
      return;
    }
    path[0] = '/';
    path[1] = '\0';
  }
  if (!storage.mounted()) {
    sh.out().text("ls: the card is not mounted (`sd` says why)\n");
    return;
  }
  const Status s = storage.opendir(path, g_pager.dir);
  if (s != Status::Ok) {
    sh.out().text("ls: ").text(path).text(": ").text(hal::storage::status_name(s)).nl();
    return;
  }
  g_pager.storage = &storage;
  g_pager.entries = 0;
  sh.out().text("ls ").text(path).text(":\n");
  sh.start_stream(&pager_step, nullptr, true);
}

}  // namespace o3sdk::platform::mk66::console
