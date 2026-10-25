// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The card's WRITE side (OS-007n). hal/storage.h stays read-only on purpose; this is the separate header and the separate review that its first lines
// promised. What it offers is deliberately not a file system: it is the handful of calls a SAVE needs, each of them refused unless the path guard
// (save_guard.h) allows it, and all of them refused unless the card passes the gate below.
//
// THE GATE. Every call that changes the card is refused unless a FAT12/16/32 volume is mounted and not write protected (WriteStatus NoCard, Unsupported, WriteProtected). Any such card
// is written, as every save does (maintainer decision 2026-10-17: no test-card marker, no volume check; OS-004d had already made the marker an off-by-default build option, now removed).
// The path guard (save_guard.h) is what decides WHAT may be written. Reading is never gated.
//
// The contract (all main loop, each call blocks for the card's time, a few ms for a few KB; callers keep a call to a chunk):
//   * one file is open for writing at a time (create / write / finish or abandon): a save writes one temporary file;
//   * nothing is written to a final name: create() takes a Temp path only, a final file appears by rename() (save_guard.h has the whole table);
//   * remove() takes a Temp or a Backup path only; the implementation also refuses (Damaged) to remove a file whose first cluster another directory entry uses;
//   * nothing formats, nothing removes a folder, nothing leaves Projects/, Samples/, Settings/, Export/ and instruments/ (the .pti library, OS-INSTUI S7). OS-FILES part B adds one thing to a
//     user's card: an empty marker file Projects/<name>/.deleted (create_marker); a project folder is never renamed and no rename crosses a folder.
// A write that is cut by the loss of power (or the card being pulled) leaves what AtomicSave documents (app/atomic_save.h): never a half-written final file.

#include <o3sdk/hal/save_guard.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::hal::storage {

enum class WriteStatus : std::uint8_t {
  Ok = 0,
  NoCard,          // no card, or the volume is not mounted
  WriteProtected,  // the card's write-protect switch or bits
  Unsupported,     // not FAT12/16/32 (exFAT writes are not exercised by this tree)
  BadPath,         // the path guard refused it (never from AtomicSave: its paths are built from the guard's own names)
  Exists,          // create() or rename() onto a name that is there
  NotFound,        // a folder or file that should be there is not
  NoSpace,         // the volume or the directory is full
  Busy,            // a file is already open for writing, or no file handle is free
  Damaged,         // two directory entries share one chain (a cut inside a rename): the card needs a check on a computer, nothing is removed
  Io,              // the card failed
  Host,            // OS-USBDISK U2a: the card is the computer's (the disk is out): nothing is written, nothing is dropped, the save layer waits for the eject
};

inline constexpr const char* write_status_name(WriteStatus s) noexcept {
  switch (s) {
    case WriteStatus::Ok: return "ok";
    case WriteStatus::NoCard: return "no card";
    case WriteStatus::WriteProtected: return "card is write protected";
    case WriteStatus::Unsupported: return "file system not supported for writing";
    case WriteStatus::BadPath: return "path refused";
    case WriteStatus::Exists: return "name exists";
    case WriteStatus::NotFound: return "not found";
    case WriteStatus::NoSpace: return "card is full";
    case WriteStatus::Busy: return "busy";
    case WriteStatus::Damaged: return "card needs a check (cross-linked files)";
    case WriteStatus::Io: return "i/o error";
    case WriteStatus::Host: return "card is on the computer";
  }
  return "?";
}

class StorageWriter {
 public:
  // Whether a change may be asked for now: Ok, or the first reason it may not (NoCard, WriteProtected, Unsupported). Cheap enough to ask before a save and
  // for the UI. Every mutating call below asks it again itself.
  virtual WriteStatus gate() noexcept = 0;

  // A folder of the guard's list ("Projects", "Projects/<name>", "Samples", "Settings"): created when absent, Ok when it is there. The parent must exist (NotFound).
  virtual WriteStatus make_dir(const char* path) noexcept = 0;

  // Creates the Temp file at `path` and leaves it open for writing: Exists when it is there (never truncated), Busy when a file is already open.
  virtual WriteStatus create(const char* path) noexcept = 0;
  // Appends `n` bytes to the open file: all of them or an error (NoSpace for a short write).
  virtual WriteStatus write(const void* src, std::size_t n) noexcept = 0;
  // Flushes the open file to the card (data and directory entry) and closes it. After it, even on an error, no file is open.
  virtual WriteStatus finish() noexcept = 0;
  // Closes the open file, best effort and without looking at the result (the caller is about to remove it). Always safe.
  virtual void abandon() noexcept = 0;
  virtual bool writing() const noexcept = 0;

  // Renames `from` to `to` inside one folder as save_guard.h may_rename() allows; Exists when `to` is there (nothing is replaced). NotFound when `from` is not.
  virtual WriteStatus rename(const char* from, const char* to) noexcept = 0;
  // OS-FILES part B, DELETE: creates the empty marker file `path` (save_guard.h Place::Marker: Projects/<name>/.deleted) and closes it: one directory entry, no cluster. Exists when it is there (nothing is
  // touched). Busy while a file is open for writing.
  virtual WriteStatus create_marker(const char* path) noexcept = 0;
  // Removes a Temp or a Backup file; Ok when it is not there.
  virtual WriteStatus remove(const char* path) noexcept = 0;

  // A SAVE is one or more jobs (a project and its samples, OS-FU2). The caller says where it begins and ends so that an implementation can hold the whole save to the card it began
  // on: begin_save() pins the card (a later mutating call on another card is refused with NoCard), end_save() lets go. begin_save() always pins afresh, so a save whose end_save()
  // was missed does no harm. card_id() names the volume now mounted (0: none or unknown): a caller that remembers what it wrote on a card asks it to know the card changed.
  // The defaults pin nothing and know nothing.
  virtual void begin_save() noexcept {}
  virtual void end_save() noexcept {}
  virtual std::uint32_t card_id() const noexcept { return 0; }

 protected:
  StorageWriter() = default;
  ~StorageWriter() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  StorageWriter(const StorageWriter&) = delete;
  StorageWriter& operator=(const StorageWriter&) = delete;
};

}  // namespace o3sdk::hal::storage
