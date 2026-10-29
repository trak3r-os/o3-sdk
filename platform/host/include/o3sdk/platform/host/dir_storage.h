// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// hal::storage::Storage over a directory of the host (OS-007e): the card of the simulator and of the host tests. The directory is the
// card's root, as planned ("a temporary copy of a directory"): copy a card's tree, or build one in a test, and point this at
// it. Read-only by construction, like the contract: the only way this file touches the host's file system is open(O_RDONLY),
// opendir(), stat() and read(); there is no call that creates, writes, renames or removes. It also resolves names the way FAT does, without
// regard to case ("workspace/PATTERNS" finds "Workspace/patterns"), so a tree copied off a card behaves on a case-sensitive host as it
// did on the card.
//
// Not for the target: standard library and POSIX are allowed here (platform/host), nothing below the app layer includes this header.
// (DirCard, dir_card.h, is the same directory with a write side: it reads through this class and writes with calls of its own.)

#include <o3sdk/hal/storage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace o3sdk::platform::host {

class DirStorage final : public hal::storage::Storage {
 public:
  explicit DirStorage(std::string root) : root_(std::move(root)) {}
  ~DirStorage();
  DirStorage(const DirStorage&) = delete;
  DirStorage& operator=(const DirStorage&) = delete;

  // Ok when the root is a directory (NoCard when it is not there), and closes every handle first.
  hal::storage::Status mount() noexcept override;
  bool mounted() const noexcept override { return mounted_; }
  bool card_present() const noexcept override;
  hal::storage::VolumeInfo volume() const noexcept override;

  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override;
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override;
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override;
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override;
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override;
  void close(hal::storage::FileHandle& file) noexcept override;
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override;
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override;
  void closedir(hal::storage::DirHandle& dir) noexcept override;

  // Test observation: the files and directories open right now, and the total calls of open() that succeeded.
  unsigned open_files() const noexcept;
  unsigned open_dirs() const noexcept;

  // The host path of `path` under the root, with every component matched case-insensitively. Status::Ok and the real path in `out`;
  // NotFound (a component is missing), NotADirectory (a file in the middle), BadPath, NotMounted. DirCard finds what is on its card with it.
  hal::storage::Status resolve(const char* path, std::string& out) const;

 private:
  std::string root_;
  bool mounted_ = false;
  int files_[hal::storage::kMaxOpenFiles] = {-1, -1};
  // An open directory: the names it held when it was opened, in byte order (strcmp), not the host file system's: a folder lists the same on any machine, so a seed of the QA walk replays the same (OS-QANET).
  struct OpenDir {
    bool open = false;
    std::string path;                // to stat() an entry for its size and kind
    std::vector<std::string> names;  // without "." and ".."
    std::size_t next = 0;
  };
  OpenDir dirs_[hal::storage::kMaxOpenDirs];
};

}  // namespace o3sdk::platform::host
