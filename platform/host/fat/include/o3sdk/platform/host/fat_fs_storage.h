// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The unit's file system on a host card (OS-QANET D1, docs/specs/OS-QANET.md section 7): hal::storage::Storage and hal::storage::StorageWriter over a FAT32 IMAGE FILE, through the very code of the
// image: platform/mk66/storage FatStorage (the read side, FatFs R0.15a with the unit's ffconf.h: long names, UTF-8 at the API, OEM code page 437, exFAT, no clock) and platform/mk66/save
// FatStorageWriter (the write side, with the real path guard and the card gate), on a block device that is the image. The simulator's `card <image>` mounts one. What the simulator's other card, DirCard
// (a scratch folder), cannot show is here: FatFs's long-name matching and its code-page folding, its directory order, its listing of a folder of many entries, its rename in two directory writes, the
// stamp of a file, the volume's serial, and a full volume.
//
// THE IMAGE is read through a PRIVATE mapping (mmap MAP_PRIVATE): what a run writes goes to private memory and the image file is never changed, as DirCard's scratch copy never changes the fixture;
// a 64 MiB sparse image costs the pages that are touched. A run starts again from the file.
//
// DIFFERENCES FROM THE UNIT (named, not hidden): the block device is memory, not the SDHC bus (no wait states, no CRC or timeout faults; D2 models the time); a card has 4 to 32 KB clusters and the
// image one sector (see fat_image.h: a FAT32 volume of 64 MiB needs it); the file system code is the unit's and so is its configuration, nothing here has its own. The stamp of a file the run writes is
// the fixed FF_NORTC_* time, as on the unit. The simulator's FatStorage is released when the card is pulled and mounted again when it is put back, as app_main.cpp's poll_card does it.
//
// FatFs keeps ONE volume (a pointer in its glue): one FatFsStorage is mounted at a time in a process. Host only (platform/host), main thread only.

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/host/host_card.h>
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o3sdk::platform::host {

// The image as a BlockDevice: 512-byte blocks of a file mapped privately, or of memory (adopt(): a volume FatImage just built).
class ImageDevice final : public mk66::storage::BlockDevice {
 public:
  ImageDevice() = default;
  ~ImageDevice();
  // Maps the image file (a regular file of a multiple of 512 bytes, at most FatImage::kMaxImageBytes). false: `error` says why.
  bool open(const std::string& path, std::string& error);
  // Takes a volume built in memory in place of the mapping.
  void adopt(std::vector<std::uint8_t> image);
  bool loaded() const { return base_ != nullptr; }
  const std::uint8_t* data() const { return base_; }
  void set_present(bool present) { present_ = present; }

  bool present() const noexcept override { return present_ && loaded(); }
  mk66::storage::BlockResult start() noexcept override;
  bool ready() const noexcept override { return started_ && present(); }
  void release() noexcept override { started_ = false; }
  std::uint32_t block_count() const noexcept override { return blocks_; }
  mk66::storage::BlockResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override;
  mk66::storage::BlockResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override;
  bool write_protected() const noexcept override { return false; }

 private:
  void unmap();
  std::uint8_t* base_ = nullptr;
  std::size_t bytes_ = 0;
  bool mapped_ = false;           // base_ is a mapping (else the vector)
  std::vector<std::uint8_t> mem_;
  std::uint32_t blocks_ = 0;
  bool present_ = true;
  bool started_ = false;
};

class FatFsStorage final : public HostCard {
 public:
  FatFsStorage() : fat_(device_), writer_(fat_, device_) {}
  ~FatFsStorage() override;

  // Maps `image`; nothing is mounted yet. false: `error` says why (not there, not a multiple of 512 bytes, too large).
  bool open_image(const std::string& image, std::string& error);

  // The computer's copy of a folder of the PULLED card (`card copy`, outside the unit's write guard): the card's tree is exported to a scratch folder, the folder copied there, and the volume written
  // again (FatImage, so the files get new clusters and a new order; the card is the computer's meanwhile, as in the unit). `from` and `to` are relative paths of the card.
  bool computer_copy(const std::string& from, const std::string& to, std::string& error);

  // ---- HostCard
  void set_present(bool present) override;
  unsigned violations() const override { return 0; }  // FatStorageWriter answers BadPath to a path the guard refuses and keeps no count

  // ---- hal::storage::Storage: FatStorage's
  hal::storage::Status mount() noexcept override;
  bool mounted() const noexcept override { return fat_.mounted(); }
  bool card_present() const noexcept override { return fat_.card_present(); }
  hal::storage::VolumeInfo volume() const noexcept override { return fat_.volume(); }
  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override { return fat_.exists(path, is_dir); }
  hal::storage::Status stat(const char* path, hal::storage::FileStamp& out) noexcept override { return fat_.stat(path, out); }
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override { return fat_.open(path, out); }
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override { return fat_.read(file, dst, max, got); }
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override { return fat_.seek(file, offset); }
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override { return fat_.size(file, out); }
  void close(hal::storage::FileHandle& file) noexcept override { fat_.close(file); }
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override { return fat_.opendir(path, out); }
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override { return fat_.readdir(dir, out); }
  void closedir(hal::storage::DirHandle& dir) noexcept override { fat_.closedir(dir); }

  // ---- hal::storage::StorageWriter: FatStorageWriter's
  hal::storage::WriteStatus gate() noexcept override { return writer_.gate(); }
  hal::storage::WriteStatus make_dir(const char* path) noexcept override { return writer_.make_dir(path); }
  hal::storage::WriteStatus create(const char* path) noexcept override { return writer_.create(path); }
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override { return writer_.write(src, n); }
  hal::storage::WriteStatus finish() noexcept override { return writer_.finish(); }
  void abandon() noexcept override { writer_.abandon(); }
  bool writing() const noexcept override { return writer_.writing(); }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override { return writer_.rename(from, to); }
  hal::storage::WriteStatus create_marker(const char* path) noexcept override { return writer_.create_marker(path); }
  hal::storage::WriteStatus remove(const char* path) noexcept override { return writer_.remove(path); }
  void begin_save() noexcept override { writer_.begin_save(); }
  void end_save() noexcept override { writer_.end_save(); }
  std::uint32_t card_id() const noexcept override { return writer_.card_id(); }

  const mk66::storage::FatStorage& fat() const { return fat_; }

 private:
  bool export_tree(const std::string& card_dir, const std::string& host_dir, std::string& error);

  ImageDevice device_;
  mk66::storage::FatStorage fat_;
  mk66::save::FatStorageWriter writer_;
  bool present_ = true;
};

}  // namespace o3sdk::platform::host
