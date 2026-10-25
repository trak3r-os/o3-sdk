// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SD card as the OS sees it (OS-007e): a volume of files and directories, READ ONLY. The side effects of a write are not in this
// contract at all: there is no create, write, mkdir, remove, rename or sync, so a layer above cannot ask for one and no implementation
// can be handed one. The first slice of storage ships read-only on purpose (the test card holds the maintainer's projects); a write side is
// a later item that adds its own header and its own review, it does not grow this one by a function.
//
// Shape: a small read-only card API, reduced to what first-party code reads with: mount, exists,
// open, read, seek, size, close, and directory iteration. Handles are small values naming a slot of the implementation's own fixed pool
// (kMaxOpenFiles / kMaxOpenDirs): nothing is allocated, nothing is owned by the caller, a copy of a handle is another name for the same
// slot. A path is UTF-8, '/'-separated, relative to the card's root (a leading '/' is allowed and means the same), and is checked by
// valid_path() below before an implementation looks at it. FAT is case-insensitive and so is every implementation of this contract
// (the host one too), so "Workspace" and "WORKSPACE" are one directory.
//
// Contexts: main loop only (`main-only`). A call blocks for as long as the card takes (a few ms for a sector read; a
// mount can take a second or more on a card that is slow to wake) and is never made from an interrupt or from the audio path. Callers keep
// each call small (a read of a few KB) so the main loop stays responsive; formats' readers already work that way (formats/format.h).

#include <cstddef>
#include <cstdint>

namespace o3sdk::hal::storage {

// What a call answers. Every non-Ok result leaves the output arguments untouched (a handle stays closed, `got` is not written) except
// where a function says otherwise.
enum class Status : std::uint8_t {
  Ok = 0,
  NoCard,         // no card in the slot, or none answered the initialisation
  NotMounted,     // mount() has not succeeded (or the card was taken away)
  Unsupported,    // a card or a file system this layer does not read: not an SD memory card, not FAT12/16/32 or exFAT
  Io,             // the card or the bus failed: a timeout, a CRC or a protocol error, a sector the card would not give
  NotFound,       // no such file or directory (also: a component of the path that is missing)
  NotAFile,       // open() of a directory
  NotADirectory,  // opendir() of a file, or a file used as a component of a path
  BadPath,        // not valid_path(): empty, too long, a "." or ".." component, a character FAT cannot hold
  NoHandle,       // every handle of that kind is in use (kMaxOpenFiles, kMaxOpenDirs)
  BadHandle,      // the handle is not open
  End,            // readdir(): no more entries (not an error)
};

inline constexpr const char* status_name(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NoCard: return "no card";
    case Status::NotMounted: return "not mounted";
    case Status::Unsupported: return "unsupported";
    case Status::Io: return "i/o error";
    case Status::NotFound: return "not found";
    case Status::NotAFile: return "not a file";
    case Status::NotADirectory: return "not a directory";
    case Status::BadPath: return "bad path";
    case Status::NoHandle: return "no free handle";
    case Status::BadHandle: return "bad handle";
    case Status::End: return "end";
  }
  return "?";
}

// ---- What is mounted ----------------------------------------------------------------------------------------------------------------
enum class FsKind : std::uint8_t { None, Fat12, Fat16, Fat32, ExFat, HostDirectory };

inline constexpr const char* fs_kind_name(FsKind k) noexcept {
  switch (k) {
    case FsKind::None: return "none";
    case FsKind::Fat12: return "FAT12";
    case FsKind::Fat16: return "FAT16";
    case FsKind::Fat32: return "FAT32";
    case FsKind::ExFat: return "exFAT";
    case FsKind::HostDirectory: return "host directory";
  }
  return "?";
}

struct VolumeInfo {
  FsKind kind = FsKind::None;
  std::uint32_t capacity_mb = 0;    // the volume's data area in MiB (2^20 bytes); 0 when the implementation has no such notion
  std::uint32_t cluster_bytes = 0;  // the allocation unit; 0 likewise
};

// ---- Handles, entries, limits ---------------------------------------------------------------------------------------------------------
inline constexpr unsigned kMaxOpenFiles = 2;  // open at once; a loader reads one file while a directory listing is open (kMaxOpenDirs)
inline constexpr unsigned kMaxOpenDirs = 2;
inline constexpr std::size_t kPathMax = 511;  // bytes of a path, without the NUL
inline constexpr std::size_t kNameMax = 255;  // bytes of one entry's name, without the NUL (FAT: 255 UTF-16 units; a longer UTF-8 name is cut at a character)

struct FileHandle {
  static constexpr std::uint8_t kNone = 0xFF;
  std::uint8_t slot = kNone;
  constexpr bool is_open() const noexcept { return slot != kNone; }
};
struct DirHandle {
  static constexpr std::uint8_t kNone = 0xFF;
  std::uint8_t slot = kNone;
  constexpr bool is_open() const noexcept { return slot != kNone; }
};

// See Storage::stat: a file's size and modification stamp.
struct FileStamp {
  std::uint32_t size = 0;   // bytes; saturates at 4 GiB - 1
  std::uint32_t stamp = 0;  // FAT date << 16 | time; 0: none
  bool operator==(const FileStamp& o) const noexcept { return size == o.size && stamp == o.stamp; }
};

struct DirEntry {
  char name[kNameMax + 1];  // UTF-8, NUL-terminated; never "." or ".."
  std::uint32_t size;       // bytes; 0 for a directory; saturates at 4 GiB - 1 for a larger exFAT file
  bool is_dir;
};

// ---- Paths ----------------------------------------------------------------------------------------------------------------------------
// True for a path both implementations accept: non-null, 1..kPathMax bytes, made of components separated by '/', none empty (a leading
// '/' and a trailing '/' are fine), none "." or "..", none containing a control character or one of \ : * ? " < > |. Characters of 0x80 and
// above are UTF-8 and are not looked at (FatFs checks its own table). The root itself is "/" (or "").
constexpr bool valid_path(const char* path) noexcept {
  if (path == nullptr) {
    return false;
  }
  std::size_t length = 0;
  std::size_t component = 0;  // bytes in the current component
  std::size_t dots = 0;       // of them, '.'
  for (const char* p = path; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (++length > kPathMax) {
      return false;
    }
    if (c == '/') {
      if (component != 0 && dots == component && component <= 2) {
        return false;  // "." or ".."
      }
      component = 0;
      dots = 0;
      continue;
    }
    if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
    ++component;
    dots += c == '.' ? 1u : 0u;
  }
  if (component != 0 && dots == component && component <= 2) {
    return false;
  }
  return true;
}

// ---- The contract ---------------------------------------------------------------------------------------------------------------------
class Storage {
 public:
  // Brings the card and its file system up (or checks that they still are). Ok: mounted() is true and volume() says what. NoCard,
  // Unsupported or Io otherwise; calling it again retries from scratch (every handle is closed first).
  virtual Status mount() noexcept = 0;
  virtual bool mounted() const noexcept = 0;
  // A card is in the slot (the card-detect switch; the host's directory exists). Cheap, no bus traffic.
  virtual bool card_present() const noexcept = 0;
  virtual VolumeInfo volume() const noexcept = 0;

  // Ok: the path names a file or a directory (`is_dir` says which when given); NotFound otherwise.
  virtual Status exists(const char* path, bool* is_dir) noexcept = 0;

  // OS-BUG-REMOUNT: what identifies the file's contents cheaply: its size and, where the volume keeps one, its modification stamp (a FAT volume: date << 16 | time, 2 s steps). `stamp` is 0 where
  // the implementation has none. NotFound / NotAFile as open() answers. The default asks open() and size() (no stamp); a FAT volume overrides it with one directory lookup.
  virtual Status stat(const char* path, FileStamp& out) noexcept {
    FileHandle file;
    const Status s = open(path, file);
    if (s != Status::Ok) {
      return s;
    }
    std::uint32_t bytes = 0;
    const Status z = size(file, bytes);
    close(file);
    if (z == Status::Ok) {
      out = FileStamp{bytes, 0};
    }
    return z;
  }

  // Opens a file for reading, positioned at 0. Ok: `out` is open. NotAFile for a directory.
  virtual Status open(const char* path, FileHandle& out) noexcept = 0;
  // Reads up to `max` bytes at the file's position. Ok with got == 0 is the end of the file (never a short Ok that means "later": a
  // read returns fewer than `max` bytes only at the end or when `max` was larger than what one call moves). `got` is written on Ok only.
  virtual Status read(FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept = 0;
  // Moves the position to `offset` (past the end is clamped to the end, as a read-only file cannot grow).
  virtual Status seek(FileHandle file, std::uint32_t offset) noexcept = 0;
  virtual Status size(FileHandle file, std::uint32_t& out) const noexcept = 0;
  // Closes and invalidates the handle (a closed or never-opened one is left alone). Always succeeds: there is nothing to flush.
  virtual void close(FileHandle& file) noexcept = 0;

  // Opens a directory for listing. Ok: `out` is open, at the first entry.
  virtual Status opendir(const char* path, DirHandle& out) noexcept = 0;
  // The next entry in the directory's own order (not sorted); never "." or "..". End at the last one, then End again.
  virtual Status readdir(DirHandle dir, DirEntry& out) noexcept = 0;
  virtual void closedir(DirHandle& dir) noexcept = 0;

 protected:
  Storage() = default;
  ~Storage() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;
};

}  // namespace o3sdk::hal::storage
