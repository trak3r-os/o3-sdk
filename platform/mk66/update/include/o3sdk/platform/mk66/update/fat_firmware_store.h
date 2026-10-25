// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FirmwareStore over FatFs (OS-006c): the one place in the tree that opens a file for writing, renames, deletes or syncs. It uses the volume
// FatStorage mounted (FatFs keeps one), checks the device's write-protect state before every mutating call, applies the
// path guard to the full path right before each write-class FatFs call, and refuses exFAT (FatFs's exFAT write support is not exercised by this tree's
// tests; the Tracker's card is FAT32). Statics only: two FIL (the upload and the read-back), one DIR, one FILINFO, about 1.4 KB.

#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>
#include <o3sdk/platform/mk66/update/firmware_store.h>

#include <ff.h>

namespace o3sdk::platform::mk66::update {

class FatFirmwareStore final : public FirmwareStore {
 public:
  // Any writable FAT card with a Firmware/ folder is accepted: no marker file (OS-FWANY, OS-ANYCARD).
  FatFirmwareStore(storage::FatStorage& fat, storage::BlockDevice& device) noexcept : fat_(fat), device_(device) {}

  WriteState state() noexcept override;
  Fault begin_upload(const char* name, std::uint32_t bytes) noexcept override;
  Fault append(const std::uint8_t* data, std::size_t n) noexcept override;
  Fault close_upload() noexcept override;
  Fault commit_upload() noexcept override;
  void abort_upload() noexcept override;
  bool uploading() const noexcept override { return uploading_; }
  Fault open_read(const char* name, std::uint32_t& size) noexcept override;
  Fault read(std::uint8_t* dst, std::size_t max, std::size_t& got) noexcept override;
  void close_read() noexcept override;
  Fault remove(const char* name) noexcept override;
  Fault write_fwinfo(const char* name) noexcept override;
  Fault remove_fwinfo() noexcept override;
  Fault list_begin() noexcept override;
  bool list_next(FwEntry& out) noexcept override;
  void list_end() noexcept override;

  // FRESULT of the last FatFs call that failed (0 when none did), for the console.
  int last_fatfs_error() const noexcept { return last_fresult_; }

 private:
  Fault writable() noexcept;
  Fault from_fresult(FRESULT r) noexcept;
  bool exists(const char* path, bool* is_dir) noexcept;
  unsigned count_ptf() noexcept;
  void drop_temp() noexcept;

  storage::FatStorage& fat_;
  storage::BlockDevice& device_;
  FIL wr_{};
  FIL rd_{};
  DIR dir_{};
  FILINFO info_{};
  bool uploading_ = false;
  bool wr_open_ = false;
  bool rd_open_ = false;
  bool dir_open_ = false;
  std::uint32_t entries_ = 0;
  std::uint32_t declared_ = 0;
  std::uint32_t upload_generation_ = 0;  // OS-SDSWAP: the mount the upload began on (FatStorage::generation()): a card taken out ends it, whichever card comes next
  bool same_mount() const noexcept { return fat_.mounted() && fat_.generation() == upload_generation_; }
  char target_[kImageNameMax + 1] = {};
  int last_fresult_ = 0;
};

}  // namespace o3sdk::platform::mk66::update
