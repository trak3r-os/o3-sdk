// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A card of the host with a write side (OS-E2E-1b, docs/specs/OS-E2E.md): hal::storage::Storage and hal::storage::StorageWriter over one directory, for the simulator's `card` command
// and the host tests of the card commands. The directory is the card's root; it is a scratch copy (ScratchDir below), never a card or a folder of the user's.
//
// The read side is DirStorage (dir_storage.h): case-insensitive like FAT, handles in the contract's fixed pools. The write side follows the contract of hal/storage_write.h and the rules of
// tests/support/mem_card.h (which has the same faults for the power-loss sweeps; the simulator has no use for those, and sim/ must not include tests/support):
//   * every call that changes the card is checked by the REAL path guard (hal/save_guard.h) first; a path it refuses is BadPath and counts in violations();
//   * one file is open for writing at a time; create() takes a Temp path only, never truncates (Exists), never makes a parent (NotFound);
//   * a final name appears by rename(), which never replaces (Exists, FAT's rule, with the names compared without regard to case);
//   * remove() takes a Temp or a Backup only and answers Ok for a file that is not there;
//   * the gate: NoCard unless the card is in the slot and mounted. No write-protect and no file system check: it is a host directory (volume().kind is HostDirectory).
//
// Pulling the card (set_present(false)) is the slot's switch: every call answers NoCard or NotMounted from then on, and a file that was being written is closed where it is (the torn Temp a save
// leaves when the card is taken out; AtomicSave removes it, or the next save does). set_present(true) puts the same card back (the same volume id, as a real card is), and mount() mounts it again.
//
// Not for the target: standard library and POSIX are allowed here (platform/host); nothing below the app layer includes this header. Main thread only.

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/host/dir_storage.h>
#include <o3sdk/platform/host/host_card.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace o3sdk::platform::host {

// A fresh directory in the host's temporary folder ("<tmp>/o3sdk-<tag>-XXXXXX"), removed with everything in it when the object goes. It deletes the directory it made and nothing else.
class ScratchDir {
 public:
  explicit ScratchDir(const char* tag);
  ~ScratchDir();
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  bool ok() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  // Copies the contents of the directory `fixture` into this one, whole trees of regular files and directories. A symbolic link or another kind of file anywhere inside is refused (DirStorage
  // follows links, so a copied one could lead a save out of this directory), and so is a `fixture` that contains this directory (it would copy itself). false: `error` says why.
  bool copy_from(const std::string& fixture, std::string& error) const;

 private:
  std::string path_;
};

class DirCard final : public HostCard {
 public:
  // `root` is an existing directory: the card's root. Nothing is mounted yet.
  explicit DirCard(std::string root) : storage_(std::move(root)) {}
  ~DirCard();

  // The slot. A card put back is not mounted until mount() (on the device the platform mounts it, then tells the app: App::card_changed).
  void set_present(bool present) override;
  // Mutating calls the path guard refused since the card was made (a save that asks for an unguarded path is a bug; a test asserts it is 0).
  unsigned violations() const override { return violations_; }
  // Test hook: the volume's id, as card_id() answers it (begin_save() pins it; a call after the id moved is NoCard). 1 to start with.
  void set_id(std::uint32_t id) { id_ = id; }

  // ---- hal::storage::Storage: DirStorage, answering NoCard / NotMounted while the card is pulled
  hal::storage::Status mount() noexcept override;
  bool mounted() const noexcept override { return present_ && mounted_; }
  bool card_present() const noexcept override { return present_ && storage_.card_present(); }
  hal::storage::VolumeInfo volume() const noexcept override { return mounted() ? storage_.volume() : hal::storage::VolumeInfo{}; }
  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override;
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override;
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override;
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override;
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override;
  void close(hal::storage::FileHandle& file) noexcept override;
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override;
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override;
  void closedir(hal::storage::DirHandle& dir) noexcept override;

  // ---- hal::storage::StorageWriter
  hal::storage::WriteStatus gate() noexcept override;
  hal::storage::WriteStatus make_dir(const char* path) noexcept override;
  hal::storage::WriteStatus create(const char* path) noexcept override;
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override;
  hal::storage::WriteStatus finish() noexcept override;
  void abandon() noexcept override;
  bool writing() const noexcept override { return fd_ >= 0; }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override;
  hal::storage::WriteStatus create_marker(const char* path) noexcept override;
  hal::storage::WriteStatus remove(const char* path) noexcept override;
  void begin_save() noexcept override;
  void end_save() noexcept override { pinned_ = false; }
  std::uint32_t card_id() const noexcept override { return id_; }

 private:
  // The checks every call that changes the card starts with: the gate, then the card the save began on.
  hal::storage::WriteStatus pre() noexcept;
  // `path` split into the host directory of its parent (the parent must exist: NotFound) and its own name as spelled, and whether it is there already (any case).
  hal::storage::WriteStatus locate(const char* path, std::string& parent_real, std::string& leaf, bool& there) noexcept;

  DirStorage storage_;
  bool present_ = true;
  bool mounted_ = false;
  int fd_ = -1;  // the file open for writing
  std::uint32_t id_ = 1;
  bool pinned_ = false;
  std::uint32_t pin_id_ = 0;
  unsigned violations_ = 0;
};

}  // namespace o3sdk::platform::host
