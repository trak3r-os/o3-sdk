// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/dir_storage.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <strings.h>

namespace o3sdk::platform::host {
namespace {

using hal::storage::DirEntry;
using hal::storage::DirHandle;
using hal::storage::FileHandle;
using hal::storage::Status;

bool is_directory(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A name in `dir` that equals `want` without regard to case: the exact spelling first (one stat, the usual case), then a scan.
bool find_ci(const std::string& dir, const std::string& want, std::string& found) {
  struct stat st;
  if (::stat((dir + "/" + want).c_str(), &st) == 0) {
    found = want;
    return true;
  }
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return false;
  }
  bool ok = false;
  while (const dirent* e = ::readdir(d)) {
    if (::strcasecmp(e->d_name, want.c_str()) == 0) {
      found = e->d_name;
      ok = true;
      break;
    }
  }
  ::closedir(d);
  return ok;
}

}  // namespace

DirStorage::~DirStorage() {
  for (unsigned i = 0; i < hal::storage::kMaxOpenFiles; ++i) {
    if (files_[i] >= 0) {
      ::close(files_[i]);
    }
  }
}

Status DirStorage::mount() noexcept {
  for (unsigned i = 0; i < hal::storage::kMaxOpenFiles; ++i) {
    if (files_[i] >= 0) {
      ::close(files_[i]);
      files_[i] = -1;
    }
  }
  for (OpenDir& d : dirs_) {
    d = OpenDir{};
  }
  mounted_ = is_directory(root_);
  return mounted_ ? Status::Ok : Status::NoCard;
}

bool DirStorage::card_present() const noexcept { return is_directory(root_); }

hal::storage::VolumeInfo DirStorage::volume() const noexcept {
  hal::storage::VolumeInfo info;
  info.kind = mounted_ ? hal::storage::FsKind::HostDirectory : hal::storage::FsKind::None;
  return info;
}

Status DirStorage::resolve(const char* path, std::string& out) const {
  if (!mounted_) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  std::string cur = root_;
  const char* p = path;
  while (*p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') {
      break;
    }
    const char* end = p;
    while (*end != '\0' && *end != '/') {
      ++end;
    }
    if (!is_directory(cur)) {
      return Status::NotADirectory;  // the previous component is a file
    }
    std::string found;
    if (!find_ci(cur, std::string(p, static_cast<std::size_t>(end - p)), found)) {
      return Status::NotFound;
    }
    cur += "/" + found;
    p = end;
  }
  out = cur;
  return Status::Ok;
}

Status DirStorage::exists(const char* path, bool* is_dir) noexcept {
  std::string real;
  const Status s = resolve(path, real);
  if (s != Status::Ok) {
    return s;
  }
  struct stat st;
  if (::stat(real.c_str(), &st) != 0) {
    return Status::NotFound;
  }
  if (is_dir != nullptr) {
    *is_dir = S_ISDIR(st.st_mode);
  }
  return Status::Ok;
}

Status DirStorage::open(const char* path, FileHandle& out) noexcept {
  std::string real;
  const Status s = resolve(path, real);
  if (s != Status::Ok) {
    return s;
  }
  struct stat st;
  if (::stat(real.c_str(), &st) != 0) {
    return Status::NotFound;
  }
  if (S_ISDIR(st.st_mode)) {
    return Status::NotAFile;
  }
  unsigned slot = 0;
  while (slot < hal::storage::kMaxOpenFiles && files_[slot] >= 0) {
    ++slot;
  }
  if (slot == hal::storage::kMaxOpenFiles) {
    return Status::NoHandle;
  }
  const int fd = ::open(real.c_str(), O_RDONLY | O_CLOEXEC);  // the only way this backend opens a file
  if (fd < 0) {
    return errno == ENOENT ? Status::NotFound : Status::Io;
  }
  files_[slot] = fd;
  out.slot = static_cast<std::uint8_t>(slot);
  return Status::Ok;
}

Status DirStorage::read(FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || files_[file.slot] < 0) {
    return Status::BadHandle;
  }
  const ssize_t n = ::read(files_[file.slot], dst, max);
  if (n < 0) {
    return Status::Io;
  }
  got = static_cast<std::size_t>(n);
  return Status::Ok;
}

Status DirStorage::seek(FileHandle file, std::uint32_t offset) noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || files_[file.slot] < 0) {
    return Status::BadHandle;
  }
  struct stat st;
  if (::fstat(files_[file.slot], &st) != 0) {
    return Status::Io;
  }
  const off_t end = st.st_size;
  const off_t want = static_cast<off_t>(offset) > end ? end : static_cast<off_t>(offset);  // clamped, as the contract says
  return ::lseek(files_[file.slot], want, SEEK_SET) == want ? Status::Ok : Status::Io;
}

Status DirStorage::size(FileHandle file, std::uint32_t& out) const noexcept {
  if (file.slot >= hal::storage::kMaxOpenFiles || files_[file.slot] < 0) {
    return Status::BadHandle;
  }
  struct stat st;
  if (::fstat(files_[file.slot], &st) != 0) {
    return Status::Io;
  }
  out = st.st_size > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(st.st_size);
  return Status::Ok;
}

void DirStorage::close(FileHandle& file) noexcept {
  if (file.slot < hal::storage::kMaxOpenFiles && files_[file.slot] >= 0) {
    ::close(files_[file.slot]);
    files_[file.slot] = -1;
  }
  file.slot = FileHandle::kNone;
}

Status DirStorage::opendir(const char* path, DirHandle& out) noexcept {
  std::string real;
  const Status s = resolve(path, real);
  if (s != Status::Ok) {
    return s;
  }
  if (!is_directory(real)) {
    return ::access(real.c_str(), F_OK) == 0 ? Status::NotADirectory : Status::NotFound;
  }
  unsigned slot = 0;
  while (slot < hal::storage::kMaxOpenDirs && dirs_[slot].open) {
    ++slot;
  }
  if (slot == hal::storage::kMaxOpenDirs) {
    return Status::NoHandle;
  }
  DIR* d = ::opendir(real.c_str());
  if (d == nullptr) {
    return Status::Io;
  }
  OpenDir& od = dirs_[slot];
  od = OpenDir{};
  while (const dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) {
      od.names.emplace_back(e->d_name);
    }
  }
  ::closedir(d);
  std::sort(od.names.begin(), od.names.end(), [](const std::string& a, const std::string& b) { return std::strcmp(a.c_str(), b.c_str()) < 0; });  // bytes, no locale
  od.open = true;
  od.path = real;
  out.slot = static_cast<std::uint8_t>(slot);
  return Status::Ok;
}

Status DirStorage::readdir(DirHandle dir, DirEntry& out) noexcept {
  if (dir.slot >= hal::storage::kMaxOpenDirs || !dirs_[dir.slot].open) {
    return Status::BadHandle;
  }
  OpenDir& od = dirs_[dir.slot];
  while (od.next < od.names.size()) {
    const std::string& name = od.names[od.next++];
    if (name.size() > hal::storage::kNameMax) {
      continue;  // a name no FAT volume can hold is skipped
    }
    struct stat st;
    if (::stat((od.path + "/" + name).c_str(), &st) != 0) {
      continue;  // vanished or a dangling link
    }
    std::memcpy(out.name, name.c_str(), name.size() + 1u);
    out.is_dir = S_ISDIR(st.st_mode);
    out.size = out.is_dir ? 0u : (st.st_size > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(st.st_size));
    return Status::Ok;
  }
  return Status::End;
}

void DirStorage::closedir(DirHandle& dir) noexcept {
  if (dir.slot < hal::storage::kMaxOpenDirs) {
    dirs_[dir.slot] = OpenDir{};
  }
  dir.slot = DirHandle::kNone;
}

unsigned DirStorage::open_files() const noexcept {
  unsigned n = 0;
  for (const int fd : files_) {
    n += fd >= 0 ? 1u : 0u;
  }
  return n;
}

unsigned DirStorage::open_dirs() const noexcept {
  unsigned n = 0;
  for (const OpenDir& d : dirs_) {
    n += d.open ? 1u : 0u;
  }
  return n;
}

}  // namespace o3sdk::platform::host
