// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// hal::storage::Storage over FatFs and a BlockDevice (OS-007e): the card's FAT12/16/32 or exFAT volume, read only. This class opens files with FA_READ
// and calls no FatFs function that writes; what writes (OS-006c) is the FirmwareStore (update/firmware_store.h) in its own translation unit, through the
// same mounted volume FatFs keeps, and nothing else may (tools/check_layers.py rule 6 fails on a write-class symbol anywhere else).
//
// One FatStorage may be mounted at a time: FatFs reaches its device through a pointer the glue keeps (one volume, drive 0). Paths are
// UTF-8 relative to the volume's root (hal/storage.h valid_path); FatFs matches names without regard to case, as FAT does. All memory is
// the object's (a FATFS, kMaxOpenFiles FIL, kMaxOpenDirs DIR and one FILINFO: about 3.5 KB with exFAT); nothing is allocated.

#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/block_device.h>

#include <ff.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::storage {

class FatStorage final : public hal::storage::Storage {
 public:
  explicit FatStorage(BlockDevice& device) noexcept : device_(device) {}
  // FatFs keeps a pointer to the volume object it was mounted with (its table of volumes): a FatStorage that goes away takes itself out of it, or
  // the next mount would write through a dead object. The image's one FatStorage is static and never does; the tests' are on stacks.
  ~FatStorage();
  FatStorage(const FatStorage&) = delete;
  FatStorage& operator=(const FatStorage&) = delete;

  // Starts the device (the card's own initialisation), then mounts the volume (f_mount, immediately: the boot sector is read now). Ok, NoCard
  // (nothing in the slot or nothing answered), Unsupported (a card or a file system FatFs does not take: no FAT volume found, an SDUC card),
  // Io (the card failed). Every handle is closed first, so a second call is a clean retry.
  hal::storage::Status mount() noexcept override;
  // OS-SDSWAP: unmounts without touching the card: every handle is closed (a later call on it answers BadHandle), FatFs forgets the volume (a FIL or DIR of the old mount
  // is invalid for good: FatFs numbers each mount), the device forgets the medium (BlockDevice::release). mounted() is false and every call answers NotMounted until mount()
  // succeeds again. For a card that was taken out, and for the later hand-over of the card to a USB host. Idempotent.
  void release() noexcept;
  // Counts every mount() and release(): a writer that began on one volume holds the number and stops when it changes (the same card put back is another mount).
  std::uint32_t generation() const noexcept { return generation_; }
  bool mounted() const noexcept override { return mounted_ && device_.present(); }
  bool card_present() const noexcept override { return device_.present(); }
  hal::storage::VolumeInfo volume() const noexcept override { return volume_; }
  // The volume's 32-bit serial number (0 when not mounted or unreadable), for `diag fw`: the host matches it against the card it means to write.
  std::uint32_t volume_serial() const noexcept { return volume_serial_; }

  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override;
  hal::storage::Status stat(const char* path, hal::storage::FileStamp& out) noexcept override;  // OS-BUG-REMOUNT: one f_stat
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override;
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override;
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override;
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override;
  void close(hal::storage::FileHandle& file) noexcept override;
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override;
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override;
  void closedir(hal::storage::DirHandle& dir) noexcept override;

  // The most entries one directory handle returns before it says End: the FAT limit (65,536 entries of 32 bytes, 2 MiB), so a file
  // system that never ends a listing (the f_readdir fault of R0.15b, which is not in the R0.15a vendored here) cannot hold the main loop.
  static constexpr std::uint32_t kMaxDirEntries = 65'536;

  // Observation (tests, `diag sd`).
  unsigned open_files() const noexcept;
  unsigned open_dirs() const noexcept;
  // FRESULT of the last FatFs call that failed (FR_OK when none did), for the diagnostics.
  int last_fatfs_error() const noexcept { return last_error_; }

 private:
  hal::storage::Status map(int fresult) noexcept;
  void read_serial() noexcept;
  void forget_volume() noexcept;

  BlockDevice& device_;
  FATFS fs_{};
  FIL files_[hal::storage::kMaxOpenFiles]{};
  bool file_used_[hal::storage::kMaxOpenFiles] = {};
  DIR dirs_[hal::storage::kMaxOpenDirs]{};
  bool dir_used_[hal::storage::kMaxOpenDirs] = {};
  std::uint32_t dir_entries_[hal::storage::kMaxOpenDirs] = {};
  FILINFO info_{};  // the entry f_readdir / f_stat fills, copied out at once
  bool mounted_ = false;
  hal::storage::VolumeInfo volume_{};
  std::uint32_t volume_serial_ = 0;
  std::uint32_t generation_ = 0;
  int last_error_ = 0;
};

}  // namespace o3sdk::platform::mk66::storage
