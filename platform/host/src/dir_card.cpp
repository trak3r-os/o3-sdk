// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/hal/app_guard.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace o3sdk::platform::host {
namespace {

using hal::storage::Status;
using hal::storage::WriteStatus;
namespace guard = hal::storage::app_guard;
namespace fs = std::filesystem;

WriteStatus from_errno(int e) { return e == ENOSPC || e == EDQUOT ? WriteStatus::NoSpace : WriteStatus::Io; }

bool is_directory(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Whether `inner` is `outer` or below it (both absolute and canonical).
bool within(const fs::path& inner, const fs::path& outer) {
  const fs::path rel = inner.lexically_relative(outer);
  return !rel.empty() && *rel.begin() != "..";
}

// Copies the files and directories of `from` into the existing directory `to`. A symbolic link anywhere in the tree is refused, and so is anything that is not a regular file or a directory (a
// pipe, a socket, a device): DirStorage follows links, so a copied link could lead a save out of the scratch directory.
bool copy_tree(const fs::path& from, const fs::path& to, std::string& error) {
  std::error_code ec;
  for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path src = it->path();
    const fs::file_status st = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (fs::is_symlink(st)) {
      error = "'" + src.string() + "' is a symbolic link (a fixture has none: a card path could lead out of the scratch directory)";
      return false;
    }
    const fs::path dst = to / src.filename();
    if (fs::is_directory(st)) {
      if (!fs::create_directory(dst, ec) || ec) {
        break;
      }
      if (!copy_tree(src, dst, error)) {
        return false;
      }
    } else if (fs::is_regular_file(st)) {
      fs::copy_file(src, dst, ec);
      if (ec) {
        break;
      }
    } else {
      error = "'" + src.string() + "' is neither a regular file nor a directory";
      return false;
    }
  }
  if (ec) {
    error = "copying '" + from.string() + "': " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

// ---- ScratchDir ----------------------------------------------------------------------------------------------------------------------------

ScratchDir::ScratchDir(const char* tag) {
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec);
  if (ec) {
    return;
  }
  std::string pattern = (base / (std::string("o3sdk-") + tag + "-XXXXXX")).string();
  if (::mkdtemp(&pattern[0]) != nullptr) {
    path_ = pattern;
  }
}

ScratchDir::~ScratchDir() {
  if (!path_.empty()) {
    std::error_code ec;
    fs::remove_all(path_, ec);  // the directory mkdtemp made, and only that
  }
}

bool ScratchDir::copy_from(const std::string& fixture, std::string& error) const {
  std::error_code ec;
  if (!ok()) {
    error = "no scratch directory could be made";
    return false;
  }
  if (!fs::is_directory(fixture, ec)) {
    error = "'" + fixture + "' is not a directory";
    return false;
  }
  const fs::path from = fs::canonical(fixture, ec);
  const fs::path scratch = ec ? fs::path() : fs::canonical(path_, ec);
  if (ec) {
    error = "cannot resolve '" + fixture + "': " + ec.message();
    return false;
  }
  if (within(scratch, from)) {
    error = "'" + fixture + "' contains the scratch directory the card is made in (" + path_ + ")";  // a copy of it would copy itself
    return false;
  }
  return copy_tree(from, path_, error);
}

// ---- DirCard: the slot and the read side ---------------------------------------------------------------------------------------------------

DirCard::~DirCard() { abandon(); }

void DirCard::set_present(bool present) {
  if (!present) {
    abandon();  // taken out in the middle of a write: the Temp stays as far as it got
  }
  present_ = present;
  mounted_ = false;  // a card put back is mounted again by mount(), as the platform does it
}

Status DirCard::mount() noexcept {
  if (!present_) {
    return Status::NoCard;
  }
  const Status s = storage_.mount();  // (closes every handle first)
  mounted_ = s == Status::Ok;
  return s;
}

Status DirCard::exists(const char* path, bool* is_dir) noexcept { return mounted() ? storage_.exists(path, is_dir) : Status::NotMounted; }
Status DirCard::open(const char* path, hal::storage::FileHandle& out) noexcept { return mounted() ? storage_.open(path, out) : Status::NotMounted; }
Status DirCard::read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept {
  return mounted() ? storage_.read(file, dst, max, got) : Status::NotMounted;
}
Status DirCard::seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept { return mounted() ? storage_.seek(file, offset) : Status::NotMounted; }
Status DirCard::size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept { return mounted() ? storage_.size(file, out) : Status::NotMounted; }
void DirCard::close(hal::storage::FileHandle& file) noexcept { storage_.close(file); }  // (a handle is let go whatever the slot says)
Status DirCard::opendir(const char* path, hal::storage::DirHandle& out) noexcept { return mounted() ? storage_.opendir(path, out) : Status::NotMounted; }
Status DirCard::readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept { return mounted() ? storage_.readdir(dir, out) : Status::NotMounted; }
void DirCard::closedir(hal::storage::DirHandle& dir) noexcept { storage_.closedir(dir); }

// ---- DirCard: the write side ---------------------------------------------------------------------------------------------------------------

WriteStatus DirCard::gate() noexcept { return mounted() ? WriteStatus::Ok : WriteStatus::NoCard; }

void DirCard::begin_save() noexcept {
  pinned_ = true;
  pin_id_ = id_;
}

WriteStatus DirCard::pre() noexcept {
  const WriteStatus g = gate();
  if (g != WriteStatus::Ok) {
    return g;
  }
  return pinned_ && id_ != pin_id_ ? WriteStatus::NoCard : WriteStatus::Ok;  // another card than the one the save began on
}

WriteStatus DirCard::locate(const char* path, std::string& parent_real, std::string& leaf, bool& there) noexcept {
  const char* const slash = std::strrchr(path, '/');
  const std::string parent = slash != nullptr ? std::string(path, static_cast<std::size_t>(slash - path)) : std::string();
  leaf = slash != nullptr ? std::string(slash + 1) : std::string(path);
  if (parent.empty()) {
    (void)storage_.resolve("/", parent_real);  // the root
  } else if (storage_.resolve(parent.c_str(), parent_real) != Status::Ok || !is_directory(parent_real)) {
    return WriteStatus::NotFound;
  }
  std::string whole;
  there = storage_.resolve(path, whole) == Status::Ok;
  return WriteStatus::Ok;
}

WriteStatus DirCard::make_dir(const char* path) noexcept {
  if (!guard::may_make_dir(path)) {
    ++violations_;
    return WriteStatus::BadPath;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  std::string parent, leaf;
  bool there = false;
  if (const WriteStatus l = locate(path, parent, leaf, there); l != WriteStatus::Ok) {
    return l;
  }
  if (there) {
    std::string real;
    (void)storage_.resolve(path, real);
    return is_directory(real) ? WriteStatus::Ok : WriteStatus::Exists;  // a folder that is there is Ok; a file of that name is in the way
  }
  return ::mkdir((parent + "/" + leaf).c_str(), 0755) == 0 ? WriteStatus::Ok : from_errno(errno);
}

WriteStatus DirCard::create(const char* path) noexcept {
  if (!guard::may_create(path)) {
    ++violations_;
    return WriteStatus::BadPath;
  }
  if (fd_ >= 0) {
    return WriteStatus::Busy;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  std::string parent, leaf;
  bool there = false;
  if (const WriteStatus l = locate(path, parent, leaf, there); l != WriteStatus::Ok) {
    return l;
  }
  if (there) {
    return WriteStatus::Exists;  // never truncated
  }
  fd_ = ::open((parent + "/" + leaf).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  return fd_ >= 0 ? WriteStatus::Ok : from_errno(errno);
}

WriteStatus DirCard::create_marker(const char* path) noexcept {
  if (!guard::may_create_marker(path)) {
    ++violations_;
    return WriteStatus::BadPath;
  }
  if (fd_ >= 0) {
    return WriteStatus::Busy;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  std::string parent, leaf;
  bool there = false;
  if (const WriteStatus l = locate(path, parent, leaf, there); l != WriteStatus::Ok) {
    return l;
  }
  if (there) {
    return WriteStatus::Exists;
  }
  const int fd = ::open((parent + "/" + leaf).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    return from_errno(errno);
  }
  ::close(fd);  // one directory entry, no bytes
  return WriteStatus::Ok;
}

WriteStatus DirCard::write(const void* src, std::size_t n) noexcept {
  if (fd_ < 0) {
    return WriteStatus::NotFound;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(src);
  while (n != 0) {
    const ssize_t put = ::write(fd_, bytes, n);
    if (put < 0) {
      if (errno == EINTR) {
        continue;
      }
      return from_errno(errno);
    }
    bytes += put;
    n -= static_cast<std::size_t>(put);
  }
  return WriteStatus::Ok;
}

WriteStatus DirCard::finish() noexcept {
  if (fd_ < 0) {
    return WriteStatus::NotFound;
  }
  const int fd = fd_;
  fd_ = -1;  // no file is open after it, even on an error
  const bool closed = ::close(fd) == 0;
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  return closed ? WriteStatus::Ok : WriteStatus::Io;  // (no fsync: the directory is scratch, nothing of it has to survive a power cut)
}

void DirCard::abandon() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

WriteStatus DirCard::rename(const char* from, const char* to) noexcept {
  if (!guard::may_rename(from, to)) {
    ++violations_;
    return WriteStatus::BadPath;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  std::string from_real;
  if (storage_.resolve(from, from_real) != Status::Ok) {
    return WriteStatus::NotFound;
  }
  std::string parent, leaf;
  bool there = false;
  if (const WriteStatus l = locate(to, parent, leaf, there); l != WriteStatus::Ok) {
    return l;
  }
  if (there) {
    return WriteStatus::Exists;  // nothing is replaced (the names compared without regard to case, as FAT does)
  }
  return ::rename(from_real.c_str(), (parent + "/" + leaf).c_str()) == 0 ? WriteStatus::Ok : from_errno(errno);
}

WriteStatus DirCard::remove(const char* path) noexcept {
  if (!guard::may_remove(path)) {
    ++violations_;
    return WriteStatus::BadPath;
  }
  if (fd_ >= 0) {
    return WriteStatus::Busy;
  }
  if (const WriteStatus g = pre(); g != WriteStatus::Ok) {
    return g;
  }
  std::string real;
  if (storage_.resolve(path, real) != Status::Ok) {
    return WriteStatus::Ok;  // not there
  }
  return ::unlink(real.c_str()) == 0 ? WriteStatus::Ok : from_errno(errno);
}

}  // namespace o3sdk::platform::host
