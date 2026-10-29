// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StorageWriter over FatFs (OS-007n). Besides the firmware writer (update/src/fat_firmware_store.cpp), THE ONLY translation unit of the tree that names a
// FatFs call that writes (f_write, f_sync, f_unlink, f_rename, f_mkdir, ...): tools/check_layers.py rule 6 fails the build if another source mentions one. Every such
// call is preceded by the path guard on the full path it is about to touch (hal/save_guard.h: create takes a Temp only, remove a Temp or a Backup only, rename the
// three pairs of the guard, make_dir a folder of its list) and by gate(): a mounted FAT12/16/32 volume, not write protected (any such card: no marker, OS-ANYCARD). Nothing here formats, truncates an existing file or removes a folder: create() is FA_CREATE_NEW.
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/hal/app_guard.h>

#include <cstring>

namespace o3sdk::platform::mk66::save {
namespace {

using hal::storage::WriteStatus;
namespace guard = hal::storage::app_guard;

constexpr unsigned kMaxProbe = 1024;  // entries of a folder the cross-link probe looks at (a bound on a listing that never ends)

bool same_name(const char* a, const char* b) noexcept {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (guard::detail::lower(*a) != guard::detail::lower(*b)) {
      return false;
    }
  }
  return *a == *b;
}

// "folder/name" -> "folder" into `out`; false when there is no folder part or it does not fit.
bool parent_of(const char* path, char* out, std::size_t cap) noexcept {
  const char* slash = nullptr;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/') {
      slash = p;
    }
  }
  if (slash == nullptr || static_cast<std::size_t>(slash - path) + 1 > cap) {
    return false;
  }
  std::memcpy(out, path, static_cast<std::size_t>(slash - path));
  out[slash - path] = '\0';
  return true;
}

}  // namespace

WriteStatus FatStorageWriter::from_fresult(FRESULT r) noexcept {
  if (r != FR_OK) {
    last_fresult_ = static_cast<int>(r);
  }
  switch (r) {
    case FR_OK: return WriteStatus::Ok;
    case FR_NO_FILE:
    case FR_NO_PATH: return WriteStatus::NotFound;
    case FR_EXIST: return WriteStatus::Exists;
    case FR_DENIED: return WriteStatus::NoSpace;  // a full volume or a full directory
    case FR_WRITE_PROTECTED: return WriteStatus::WriteProtected;
    case FR_INVALID_NAME: return WriteStatus::BadPath;
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM: return WriteStatus::NoCard;
    case FR_LOCKED:
    case FR_TOO_MANY_OPEN_FILES: return WriteStatus::Busy;
    default: return WriteStatus::Io;  // FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_INVALID_OBJECT, FR_TIMEOUT, ...
  }
}

bool FatStorageWriter::exists(const char* path, bool* is_dir) noexcept {
  FILINFO fi;
  if (f_stat(path, &fi) != FR_OK) {
    return false;
  }
  if (is_dir != nullptr) {
    *is_dir = (fi.fattrib & AM_DIR) != 0u;
  }
  return true;
}

WriteStatus FatStorageWriter::gate() noexcept {
  if (swap_ != nullptr && swap_->owner() == storage::CardOwner::Host) {
    return WriteStatus::Host;
  }
  if (!fat_.mounted()) {
    return WriteStatus::NoCard;
  }
  const hal::storage::FsKind kind = fat_.volume().kind;
  if (kind != hal::storage::FsKind::Fat12 && kind != hal::storage::FsKind::Fat16 && kind != hal::storage::FsKind::Fat32) {
    return WriteStatus::Unsupported;
  }
  if (device_.write_protected()) {
    return WriteStatus::WriteProtected;
  }
  return WriteStatus::Ok;
}

WriteStatus FatStorageWriter::gate_same_card() noexcept {
  const WriteStatus g = gate();
  if (g == WriteStatus::Ok && pinned_ && (fat_.volume_serial() != pin_serial_ || fat_.generation() != pin_generation_)) {
    return WriteStatus::NoCard;  // another card, or the same one taken out and mounted again (OS-SDSWAP): the save that began on the first stops
  }
  return g;
}

void FatStorageWriter::begin_save() noexcept {
  pin_serial_ = fat_.volume_serial();
  pin_generation_ = fat_.generation();
  pinned_ = fat_.mounted();
}

WriteStatus FatStorageWriter::make_dir(const char* path) noexcept {
  if (!guard::may_make_dir(path)) {
    return WriteStatus::BadPath;
  }
  if (!pinned_) {
    begin_save();  // a save that did not say it began: its first folder is where it begins
  }
  const WriteStatus g = gate_same_card();  // (a later job of the save, or the first one's next folder: the card the save began on)
  if (g != WriteStatus::Ok) {
    return g;
  }
  bool is_dir = false;
  if (exists(path, &is_dir)) {
    return is_dir ? WriteStatus::Ok : WriteStatus::Exists;  // a file in the way is never touched
  }
  return from_fresult(f_mkdir(path));
}

WriteStatus FatStorageWriter::create(const char* path) noexcept {
  if (!guard::may_create(path)) {
    return WriteStatus::BadPath;
  }
  if (writing_) {
    return WriteStatus::Busy;
  }
  const WriteStatus g = gate_same_card();
  if (g != WriteStatus::Ok) {
    return g;
  }
  const WriteStatus r = from_fresult(f_open(&wr_, path, FA_CREATE_NEW | FA_WRITE));  // Exists when it is there: an existing file is never truncated
  writing_ = r == WriteStatus::Ok;
  return r;
}

WriteStatus FatStorageWriter::create_marker(const char* path) noexcept {
  if (!guard::may_create_marker(path)) {
    return WriteStatus::BadPath;
  }
  if (writing_) {
    return WriteStatus::Busy;
  }
  const WriteStatus g = gate_same_card();
  if (g != WriteStatus::Ok) {
    return g;
  }
  const WriteStatus r = from_fresult(f_open(&probe_, path, FA_CREATE_NEW | FA_WRITE));  // Exists when it is there; a file is never truncated
  if (r != WriteStatus::Ok) {
    return r;
  }
  return from_fresult(f_close(&probe_));  // (no byte written: the entry, with size 0 and no cluster, goes to the card here)
}

WriteStatus FatStorageWriter::write(const void* src, std::size_t n) noexcept {
  if (!writing_) {
    return WriteStatus::NotFound;
  }
  const WriteStatus g = gate_same_card();  // the card may have been changed or pulled since the last chunk
  if (g != WriteStatus::Ok) {
    return g;
  }
  UINT written = 0;
  const FRESULT r = f_write(&wr_, src, static_cast<UINT>(n), &written);
  if (r != FR_OK) {
    return from_fresult(r);
  }
  return written == n ? WriteStatus::Ok : WriteStatus::NoSpace;
}

WriteStatus FatStorageWriter::finish() noexcept {
  if (!writing_) {
    return WriteStatus::NotFound;
  }
  writing_ = false;  // whatever happens next, no file is open afterwards
  const FRESULT synced = f_sync(&wr_);
  const FRESULT closed = f_close(&wr_);
  return from_fresult(synced != FR_OK ? synced : closed);
}

void FatStorageWriter::abandon() noexcept {
  if (writing_) {
    writing_ = false;
    (void)f_close(&wr_);  // best effort: the caller removes the file next
  }
}

WriteStatus FatStorageWriter::rename(const char* from, const char* to) noexcept {
  if (!guard::may_rename(from, to)) {
    return WriteStatus::BadPath;
  }
  const WriteStatus g = gate_same_card();
  if (g != WriteStatus::Ok) {
    return g;
  }
  return from_fresult(f_rename(from, to));  // FR_EXIST when `to` is there: nothing is replaced
}

// Is the first cluster of `path` also the first cluster of another file of its folder that the guard knows (a Target, a Backup or a Temp)? A cut inside f_rename leaves
// two directory entries on one chain (FatFs's own comment: "an interruption can cause a cross-link"), and removing one of them would free the chain the other uses. The
// partner of such an entry is always a name this layer made, so only those are opened. Ok: not shared. Damaged: shared. Io: could not tell.
WriteStatus FatStorageWriter::shares_chain(const char* path) noexcept {
  FRESULT r = f_open(&probe_, path, FA_READ);
  if (r != FR_OK) {
    return from_fresult(r);
  }
  const DWORD mine = probe_.obj.sclust;
  (void)f_close(&probe_);
  if (mine == 0) {
    return WriteStatus::Ok;  // an empty file owns no chain
  }
  char folder[guard::kPathCap];
  if (!parent_of(path, folder, sizeof folder) || f_opendir(&dir_, folder) != FR_OK) {
    return WriteStatus::Io;
  }
  WriteStatus verdict = WriteStatus::Ok;
  for (unsigned i = 0; i < kMaxProbe; ++i) {
    r = f_readdir(&dir_, &info_);
    if (r != FR_OK) {
      verdict = WriteStatus::Io;
      break;
    }
    if (info_.fname[0] == '\0') {
      break;
    }
    char other[guard::kPathCap];
    if ((info_.fattrib & AM_DIR) != 0u || std::strlen(info_.fname) > guard::kNameMax + 8 ||
        !guard::join(other, sizeof other, folder, info_.fname) || same_name(other, path)) {
      continue;
    }
    const guard::Place place = guard::classify(other);
    if (place == guard::Place::None || place == guard::Place::Folder || f_open(&probe_, other, FA_READ) != FR_OK) {
      continue;
    }
    const bool shared = probe_.obj.sclust == mine;
    (void)f_close(&probe_);
    if (shared) {
      verdict = WriteStatus::Damaged;
      break;
    }
  }
  (void)f_closedir(&dir_);
  return verdict;
}

WriteStatus FatStorageWriter::remove(const char* path) noexcept {
  if (!guard::may_remove(path)) {
    return WriteStatus::BadPath;
  }
  const WriteStatus g = gate_same_card();
  if (g != WriteStatus::Ok) {
    return g;
  }
  bool is_dir = false;
  if (!exists(path, &is_dir)) {
    return WriteStatus::Ok;
  }
  if (is_dir) {
    return WriteStatus::BadPath;  // a folder is never removed
  }
  if (writing_) {
    return WriteStatus::Busy;  // nothing is removed while a file is open for writing: the caller abandons it first
  }
  const WriteStatus shared = shares_chain(path);
  if (shared != WriteStatus::Ok) {
    return shared;
  }
  return from_fresult(f_unlink(path));
}

}  // namespace o3sdk::platform::mk66::save
