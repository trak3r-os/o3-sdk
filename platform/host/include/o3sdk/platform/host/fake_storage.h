// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The directory side of a card, in memory (OS-010a): hal::storage::Storage over a list of folders and files given by path, for the simulator's gallery
// (a card that is the same on every host) and for the tests of the folder browser (names that no host file system would keep, a card that fails on
// the Nth call, counts of what was asked). Read only like the contract, and like FAT case-insensitive. open() answers Unsupported: the files have no bytes
// here, this is for listings; a file added with its bytes (OS-LOADCH, the Sample load header probe) opens, reads and seeks.
//
// Order: opendir() lists a folder's children in the order they were added (a real card's own order is not sorted either).
//
// Not for the target: standard library allowed here (platform/host); nothing below the app layer includes this header.

#include <o3sdk/hal/storage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o3sdk::platform::host {

class FakeStorage final : public hal::storage::Storage {
 public:
  // Adds a folder (its parents too) / a file (its parent folders too). Paths are '/'-separated, a leading or trailing '/' is ignored. A name is kept
  // exactly as given (any bytes but '/' and NUL), whatever FAT would think of it: the point is to hand the browser names it has to survive.
  void add_dir(const std::string& path);
  void add_file(const std::string& path, std::uint32_t size = 0);
  void add_file(const std::string& path, const std::vector<std::uint8_t>& bytes);  // size = bytes.size(); open() works on it

  // What the card does. A card that is not present or not mounted answers every call with NoCard / NotMounted.
  void set_present(bool present) { present_ = present; }
  // After `calls` more successful readdir() calls (all folders together), the next readdir() answers `status` once; Ok switches it off.
  void fail_readdir_after(unsigned calls, hal::storage::Status status) {
    readdir_fail_in_ = calls;
    readdir_fail_ = status;
  }
  // exists() of any path that ends in `leaf` (case-insensitive) answers `status` instead (an I/O error under a probe); Ok switches it off.
  void fail_exists_of(const std::string& leaf, hal::storage::Status status) {
    exists_fail_leaf_ = leaf;
    exists_fail_ = status;
  }

  // Observation.
  unsigned opendirs() const noexcept { return opendirs_; }
  unsigned readdirs() const noexcept { return readdirs_; }
  unsigned exists_calls() const noexcept { return exists_calls_; }
  unsigned open_dirs() const noexcept;

  hal::storage::Status mount() noexcept override;
  bool mounted() const noexcept override { return present_ && mounted_; }
  bool card_present() const noexcept override { return present_; }
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

 private:
  struct Node {
    std::string path;  // normalised: no leading or trailing '/'
    bool is_dir;
    std::uint32_t size;
    std::vector<std::uint8_t> bytes;  // only a file added with them opens
    bool has_bytes = false;
  };
  struct File {
    bool open = false;
    std::size_t node = 0;
    std::uint32_t pos = 0;
  };
  struct Dir {
    bool open = false;
    std::vector<std::size_t> children;  // indices into nodes_
    std::size_t next = 0;
  };

  static std::string normalise(const std::string& path);
  static bool equal_ci(const std::string& a, const std::string& b) noexcept;
  static std::string parent_of(const std::string& path);
  static std::string leaf_of(const std::string& path);
  int find(const std::string& path) const;  // index into nodes_, -1 if none
  void add_node(const std::string& path, bool is_dir, std::uint32_t size);

  std::vector<Node> nodes_;
  Dir dirs_[hal::storage::kMaxOpenDirs];
  File files_[hal::storage::kMaxOpenFiles];
  bool present_ = true;
  bool mounted_ = true;
  unsigned opendirs_ = 0;
  unsigned readdirs_ = 0;
  unsigned exists_calls_ = 0;
  unsigned readdir_fail_in_ = 0;
  hal::storage::Status readdir_fail_ = hal::storage::Status::Ok;
  std::string exists_fail_leaf_;
  hal::storage::Status exists_fail_ = hal::storage::Status::Ok;
};

}  // namespace o3sdk::platform::host
