// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/fat_storage.h>

#include <diskio.h>

#include <cstring>

// ---- The glue: FatFs's view of the one device ------------------------------------------------------------------------------------------
// FatFs calls these functions of this file and nothing else of the platform: the status, the "initialisation" (the device was started by
// mount() before f_mount, so this only reports), the sector read and, since OS-006c, the sector write and the one ioctl a read/write
// configuration needs (CTRL_SYNC). Nothing in this file writes by itself: disk_write runs only when ff.c does, which only the FirmwareStore's
// translation unit makes it (update/src/fat_firmware_store.cpp; tools/check_layers.py rule 6). At global scope on purpose: these are the C
// functions diskio.h declares.
namespace {
o3sdk::platform::mk66::storage::BlockDevice* g_device = nullptr;
}

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0 || g_device == nullptr || !g_device->ready()) {
    return static_cast<DSTATUS>(STA_NOINIT);
  }
  return g_device->write_protected() ? static_cast<DSTATUS>(STA_PROTECT) : static_cast<DSTATUS>(0);  // FatFs refuses to open for writing on STA_PROTECT
}

DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  using o3sdk::platform::mk66::storage::BlockResult;
  if (pdrv != 0 || g_device == nullptr) {
    return RES_PARERR;
  }
  switch (g_device->read(static_cast<std::uint32_t>(sector), count, buff)) {
    case BlockResult::Ok: return RES_OK;
    case BlockResult::OutOfRange: return RES_PARERR;
    case BlockResult::Failed:
    case BlockResult::WriteProtected: return RES_ERROR;
    case BlockResult::NoMedium:
    case BlockResult::Unsupported:
    case BlockResult::NotReady: return RES_NOTRDY;
  }
  return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  using o3sdk::platform::mk66::storage::BlockResult;
  if (pdrv != 0 || g_device == nullptr) {
    return RES_PARERR;
  }
  switch (g_device->write(static_cast<std::uint32_t>(sector), count, buff)) {
    case BlockResult::Ok: return RES_OK;
    case BlockResult::WriteProtected: return RES_WRPRT;
    case BlockResult::OutOfRange: return RES_PARERR;
    case BlockResult::Failed: return RES_ERROR;
    case BlockResult::NoMedium:
    case BlockResult::Unsupported:
    case BlockResult::NotReady: return RES_NOTRDY;
  }
  return RES_ERROR;
}

// Only CTRL_SYNC: every write above returns after the card has programmed its block (CMD13 says it is back in the transfer state), so there is
// nothing pending to complete. The size queries and TRIM belong to f_mkfs and f_expand-with-trim, which this configuration does not compile.
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  (void)buff;
  if (pdrv != 0 || g_device == nullptr) {
    return RES_PARERR;
  }
  if (cmd != CTRL_SYNC) {
    return RES_PARERR;
  }
  return g_device->ready() ? RES_OK : RES_NOTRDY;
}

}  // extern "C"

namespace o3sdk::platform::mk66::storage {
namespace {

using hal::storage::DirEntry;
using hal::storage::DirHandle;
using hal::storage::FileHandle;
using hal::storage::Status;

constexpr std::uint32_t kSaturated = 0xFFFFFFFFu;

std::uint32_t saturate(FSIZE_t v) noexcept { return v > kSaturated ? kSaturated : static_cast<std::uint32_t>(v); }

}  // namespace

FatStorage::~FatStorage() {
  if (g_device == &device_) {
    (void)f_mount(nullptr, "", 0);  // FatFs forgets this object's FATFS (it dereferences the one it has: ours, still alive here)
    g_device = nullptr;
  }
}

Status FatStorage::map(int fresult) noexcept {
  last_error_ = fresult;
  switch (static_cast<FRESULT>(fresult)) {
    case FR_OK: return Status::Ok;
    case FR_NO_FILE:
    case FR_NO_PATH: return Status::NotFound;
    case FR_INVALID_NAME: return Status::BadPath;
    case FR_NO_FILESYSTEM: return Status::Unsupported;
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED: return Status::NotMounted;
    case FR_INVALID_OBJECT: return Status::BadHandle;
    case FR_TOO_MANY_OPEN_FILES: return Status::NoHandle;
    case FR_DENIED: return Status::NotAFile;
    default: return Status::Io;  // FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_TIMEOUT, FR_NOT_ENOUGH_CORE, FR_LOCKED ...
  }
}

// Every handle closed, the volume forgotten (the common part of a mount's start and of release()).
void FatStorage::forget_volume() noexcept {
  for (unsigned i = 0; i < hal::storage::kMaxOpenFiles; ++i) {
    file_used_[i] = false;
  }
  for (unsigned i = 0; i < hal::storage::kMaxOpenDirs; ++i) {
    dir_used_[i] = false;
  }
  mounted_ = false;
  volume_ = hal::storage::VolumeInfo{};
  volume_serial_ = 0;
  ++generation_;
}

void FatStorage::release() noexcept {
  forget_volume();
  if (g_device == &device_) {
    (void)f_mount(nullptr, "", 0);  // FatFs clears its fs_type: a FIL or DIR of that mount fails its next call, even when the same card is mounted again (each mount has its own id)
  }
  device_.release();
}

Status FatStorage::mount() noexcept {
  forget_volume();
  g_device = &device_;
  (void)f_mount(nullptr, "", 0);  // forget any earlier volume object

  switch (device_.start()) {
    case BlockResult::Ok: break;
    case BlockResult::NoMedium: return Status::NoCard;
    case BlockResult::Unsupported: return Status::Unsupported;
    default: return Status::Io;
  }
  const Status s = map(f_mount(&fs_, "", 1));
  if (s != Status::Ok) {
    return s;
  }
  switch (fs_.fs_type) {
    case FS_FAT12: volume_.kind = hal::storage::FsKind::Fat12; break;
    case FS_FAT16: volume_.kind = hal::storage::FsKind::Fat16; break;
    case FS_FAT32: volume_.kind = hal::storage::FsKind::Fat32; break;
    case FS_EXFAT: volume_.kind = hal::storage::FsKind::ExFat; break;
    default: return Status::Unsupported;
  }
  // The data area: (n_fatent - 2) clusters of csize sectors of 512 bytes. A product and a shift, no division.
  const std::uint64_t sectors = static_cast<std::uint64_t>(fs_.n_fatent - 2u) * fs_.csize;
  volume_.capacity_mb = static_cast<std::uint32_t>(sectors >> 11);
  volume_.cluster_bytes = static_cast<std::uint32_t>(fs_.csize) * 512u;
  read_serial();
  mounted_ = true;
  return Status::Ok;
}

// The volume serial (the 32-bit "Volume ID" the format wrote: what Windows prints as XXXX-XXXX), from the boot sector FatFs just mounted. FAT12/16 keep it at
// byte 39, FAT32 at 67, exFAT at 100 (the Microsoft FAT and exFAT specifications). A boot sector that cannot be read leaves it 0.
void FatStorage::read_serial() noexcept {
  volume_serial_ = 0;
  std::uint8_t sector[512];
  if (device_.read(static_cast<std::uint32_t>(fs_.volbase), 1, sector) != BlockResult::Ok) {
    return;
  }
  const unsigned at = fs_.fs_type == FS_EXFAT ? 100u : (fs_.fs_type == FS_FAT32 ? 67u : 39u);
  volume_serial_ = static_cast<std::uint32_t>(sector[at]) | (static_cast<std::uint32_t>(sector[at + 1]) << 8) |
                   (static_cast<std::uint32_t>(sector[at + 2]) << 16) | (static_cast<std::uint32_t>(sector[at + 3]) << 24);
}

Status FatStorage::exists(const char* path, bool* is_dir) noexcept {
  if (!mounted()) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {  // the root has no directory entry for f_stat to find
    if (is_dir != nullptr) {
      *is_dir = true;
    }
    return Status::Ok;
  }
  const Status s = map(f_stat(path, &info_));
  if (s == Status::Ok && is_dir != nullptr) {
    *is_dir = (info_.fattrib & AM_DIR) != 0u;
  }
  return s;
}

Status FatStorage::stat(const char* path, hal::storage::FileStamp& out) noexcept {
  if (!mounted()) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path) || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    return Status::BadPath;
  }
  const Status s = map(f_stat(path, &info_));
  if (s != Status::Ok) {
    return s;
  }
  if ((info_.fattrib & AM_DIR) != 0u) {
    return Status::NotAFile;
  }
  out.size = info_.fsize > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(info_.fsize);
  out.stamp = (static_cast<std::uint32_t>(info_.fdate) << 16) | info_.ftime;
  return Status::Ok;
}

Status FatStorage::open(const char* path, FileHandle& out) noexcept {
  if (!mounted()) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    return Status::NotAFile;  // the root is a directory (FatFs would answer "invalid name" for it)
  }
  unsigned slot = 0;
  while (slot < hal::storage::kMaxOpenFiles && file_used_[slot]) {
    ++slot;
  }
  if (slot == hal::storage::kMaxOpenFiles) {
    return Status::NoHandle;
  }
  Status s = map(f_open(&files_[slot], path, FA_READ));  // FA_READ is the only mode this class ever asks for
  if (s == Status::NotFound) {
    bool dir = false;
    if (exists(path, &dir) == Status::Ok && dir) {
      return Status::NotAFile;  // f_open answers "no file" for a directory
    }
  }
  if (s != Status::Ok) {
    return s;
  }
  file_used_[slot] = true;
  out.slot = static_cast<std::uint8_t>(slot);
  return Status::Ok;
}

Status FatStorage::read(FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || !file_used_[file.slot]) {
    return Status::BadHandle;
  }
  if (!mounted()) {
    return Status::NotMounted;
  }
  constexpr std::size_t kMaxRead = 0x10000000u;  // f_read takes a 32-bit count
  UINT done = 0;
  const Status s = map(f_read(&files_[file.slot], dst, static_cast<UINT>(max < kMaxRead ? max : kMaxRead), &done));
  if (s == Status::Ok) {
    got = done;
  }
  return s;
}

Status FatStorage::seek(FileHandle file, std::uint32_t offset) noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || !file_used_[file.slot]) {
    return Status::BadHandle;
  }
  if (!mounted()) {
    return Status::NotMounted;
  }
  return map(f_lseek(&files_[file.slot], offset));  // a file opened FA_READ is clamped to its end by FatFs
}

Status FatStorage::size(FileHandle file, std::uint32_t& out) const noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || !file_used_[file.slot]) {
    return Status::BadHandle;
  }
  out = saturate(f_size(&files_[file.slot]));
  return Status::Ok;
}

void FatStorage::close(FileHandle& file) noexcept {
  if (file.slot < hal::storage::kMaxOpenFiles && file_used_[file.slot]) {
    (void)f_close(&files_[file.slot]);  // opened FA_READ: nothing to flush, this only invalidates the object
    file_used_[file.slot] = false;
  }
  file.slot = FileHandle::kNone;
}

Status FatStorage::opendir(const char* path, DirHandle& out) noexcept {
  if (!mounted()) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  unsigned slot = 0;
  while (slot < hal::storage::kMaxOpenDirs && dir_used_[slot]) {
    ++slot;
  }
  if (slot == hal::storage::kMaxOpenDirs) {
    return Status::NoHandle;
  }
  const char* fat_path = path[0] == '\0' ? "/" : path;
  Status s = map(f_opendir(&dirs_[slot], fat_path));
  if (s == Status::NotFound) {
    bool dir = true;
    if (exists(path, &dir) == Status::Ok && !dir) {
      return Status::NotADirectory;  // f_opendir answers "no path" for a file
    }
  }
  if (s != Status::Ok) {
    return s;
  }
  dir_used_[slot] = true;
  dir_entries_[slot] = 0;
  out.slot = static_cast<std::uint8_t>(slot);
  return Status::Ok;
}

Status FatStorage::readdir(DirHandle dir, DirEntry& out) noexcept {
  if (dir.slot >= hal::storage::kMaxOpenDirs || !dir_used_[dir.slot]) {
    return Status::BadHandle;
  }
  if (!mounted()) {
    return Status::NotMounted;
  }
  for (;;) {
    if (dir_entries_[dir.slot] >= kMaxDirEntries) {
      return Status::End;
    }
    ++dir_entries_[dir.slot];
    const Status s = map(f_readdir(&dirs_[dir.slot], &info_));
    if (s != Status::Ok) {
      return s;
    }
    // The end of the directory is an empty name. (altname is not cleared at the end: it holds the last entry's short name, so it is never read
    // here. A long name that does not fit FF_LFN_BUF comes back as its short alias in fname, which opens the same file.)
    const char* name = info_.fname;
    if (name[0] == '\0') {
      return Status::End;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }
    std::size_t length = std::strlen(name);
    if (length > hal::storage::kNameMax) {
      length = hal::storage::kNameMax;
      while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0u) == 0x80u) {
        --length;  // do not cut a UTF-8 sequence in the middle
      }
    }
    std::memcpy(out.name, name, length);
    out.name[length] = '\0';
    out.is_dir = (info_.fattrib & AM_DIR) != 0u;
    out.size = out.is_dir ? 0u : saturate(info_.fsize);
    return Status::Ok;
  }
}

void FatStorage::closedir(DirHandle& dir) noexcept {
  if (dir.slot < hal::storage::kMaxOpenDirs && dir_used_[dir.slot]) {
    (void)f_closedir(&dirs_[dir.slot]);
    dir_used_[dir.slot] = false;
  }
  dir.slot = DirHandle::kNone;
}

unsigned FatStorage::open_files() const noexcept {
  unsigned n = 0;
  for (const bool used : file_used_) {
    n += used ? 1u : 0u;
  }
  return n;
}

unsigned FatStorage::open_dirs() const noexcept {
  unsigned n = 0;
  for (const bool used : dir_used_) {
    n += used ? 1u : 0u;
  }
  return n;
}

}  // namespace o3sdk::platform::mk66::storage
