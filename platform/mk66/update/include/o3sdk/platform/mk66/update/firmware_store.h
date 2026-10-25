// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one writer of the SD card (OS-006c): what the firmware update path may do to Firmware/ and nothing more. hal::storage::Storage, the contract
// everything else reads the card through, has no write call and keeps none; this is the separate contract the spec asks for, and the FatFs-backed
// implementation (fat_firmware_store.h) is the only translation unit that may name a write-class FatFs symbol (tools/check_layers.py rule 6).
//
// What it can do, all of it through the path guard (path_guard.h), on a FAT12/16/32 volume that is not write protected, in a Firmware/ folder that
// already exists (the writer never makes a folder). Any such card will do: no marker file (OS-FWANY, OS-ANYCARD):
//   * one upload at a time: begin_upload(name, bytes) creates Firmware/_o3up.tmp (replacing a stale one of an interrupted upload), reserves the size,
//     append() writes, close_upload() syncs and closes, commit_upload() renames it to Firmware/<name> (refused if that exists); abort_upload() closes
//     and deletes the temporary file. A half-written file is never a .ptf.
//   * read access to the temporary file and to finished images (open_read / read / close_read): for the read-back verification and for `fw arm`.
//   * remove(name): the temporary file and images that do not look official (path_guard.h) only, and never the last .ptf of the folder.
//   * write_fwinfo(name) / remove_fwinfo(): Firmware/_fwinfo, whose content is exactly the bare name (no newline), read back and compared.
//   * list(): the names and sizes of Firmware/ in directory order (what the bootloader's reset button walks).
// Every method answers a Fault (fault.h); Ok is success. Main loop only; every call blocks for the card's time (bounded by sd_policy.h).

#include <o3sdk/platform/mk66/update/fault.h>
#include <o3sdk/platform/mk66/update/path_guard.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::update {

inline constexpr std::uint32_t kMaxImageFileBytes = 4u * 1024u * 1024u;  // the largest HEX file `fw put` takes (a 1 MiB image is at most about 2.3 MB of HEX)

struct WriteState {
  bool mounted = false;
  bool exfat = false;
  bool write_protected = false;
  bool dir = false;          // Firmware/ exists
  bool enabled = false;      // every condition above holds: a write may be asked for
  Fault why = Fault::NoCard;  // the first condition that does not hold (Ok when enabled)
  char fs[8] = {};           // "FAT32", ...
  char serial[10] = {};      // "XXXX-XXXX", or empty when unknown
};

struct FwEntry {
  char name[kImageNameMax + 1];  // a longer name is cut; it can never be one the writer made
  std::uint32_t size;
};

class FirmwareStore {
 public:
  virtual WriteState state() noexcept = 0;

  // Creates the temporary file for an upload of `bytes` bytes that will become `name`, and reserves that much room. BadName, Unsupported (bytes 0 or over
  // kMaxImageFileBytes), Busy, the conditions of state() (Disabled, NoCard, WriteProtected, NoDir), Exists, NoSpace, Io.
  virtual Fault begin_upload(const char* name, std::uint32_t bytes) noexcept = 0;
  virtual Fault append(const std::uint8_t* data, std::size_t n) noexcept = 0;
  // Syncs and closes the temporary file.
  virtual Fault close_upload() noexcept = 0;
  // Renames the temporary file to Firmware/<name> (the name of begin_upload). Exists if the name appeared meanwhile.
  virtual Fault commit_upload() noexcept = 0;
  // Closes the temporary file if it is open and deletes it. Always safe, never fails.
  virtual void abort_upload() noexcept = 0;
  virtual bool uploading() const noexcept = 0;

  // Read access: `name` is the temporary file or an image name. One file at a time.
  virtual Fault open_read(const char* name, std::uint32_t& size) noexcept = 0;
  virtual Fault read(std::uint8_t* dst, std::size_t max, std::size_t& got) noexcept = 0;  // got == 0: the end of the file
  virtual void close_read() noexcept = 0;

  virtual Fault remove(const char* name) noexcept = 0;

  // Firmware/_fwinfo: created new (Exists if one is there), the bare `name`, synced, closed, read back and compared. `name` must be an image that exists.
  virtual Fault write_fwinfo(const char* name) noexcept = 0;
  // Deletes Firmware/_fwinfo; Ok when there is none.
  virtual Fault remove_fwinfo() noexcept = 0;

  // The listing of Firmware/: list_begin(), then list_next() until it says false, then list_end(). Directories and "."/".." are skipped.
  virtual Fault list_begin() noexcept = 0;
  virtual bool list_next(FwEntry& out) noexcept = 0;
  virtual void list_end() noexcept = 0;

 protected:
  FirmwareStore() = default;
  ~FirmwareStore() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  FirmwareStore(const FirmwareStore&) = delete;
  FirmwareStore& operator=(const FirmwareStore&) = delete;
};

}  // namespace o3sdk::platform::mk66::update
