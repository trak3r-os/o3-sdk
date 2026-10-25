// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A block device as the file system's glue sees it (OS-007e): numbered 512-byte blocks. The SD card is one (sd_card.h), and so is the in-memory
// image of the tests; FatFs reaches it through the glue in src/fat_storage.cpp and nothing else does. OS-006c adds `write` and `write_protected`:
// the glue's disk_write is the only caller, and the only thing that reaches FatFs's write calls is the FirmwareStore (update/firmware_store.h), whose
// path guard and the card gate decide what may be written. hal::storage::Storage, what everything else reads the card through, still has no write call.

#include <cstdint>

namespace o3sdk::platform::mk66::storage {

enum class BlockResult : std::uint8_t {
  Ok,
  NoMedium,     // nothing in the slot, or nothing that answers
  Unsupported,  // a medium this device does not take (an SDUC card)
  NotReady,     // not started, or the medium was taken out
  OutOfRange,   // the blocks asked for are not all on the medium
  Failed,       // the medium or the bus failed
  WriteProtected,  // write(): the medium refuses writes (a protect switch, or the card's own write-protect bits)
};

class BlockDevice {
 public:
  // A medium is in the slot (the card-detect switch). No bus traffic.
  virtual bool present() const noexcept = 0;
  // Brings the medium up, or up again (every call starts from the medium's reset). Ok: ready() is true. Blocks for as long as that takes
  // (bounded).
  virtual BlockResult start() noexcept = 0;
  // The device is initialised and holds a medium.
  virtual bool ready() const noexcept = 0;
  // OS-SDSWAP: forgets the medium (it was taken out, or is handed to someone else): ready() is false until start() succeeds again. No bus traffic. The default
  // does nothing (a device with no state of its own to drop).
  virtual void release() noexcept {}
  // Capacity in 512-byte blocks.
  virtual std::uint32_t block_count() const noexcept = 0;
  // Reads `count` blocks starting at `lba` into `dst` (any alignment, `count` * 512 bytes). All or nothing is not promised: on a failure the
  // buffer holds whatever was read before it.
  virtual BlockResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept = 0;
  // Writes `count` blocks starting at `lba` from `src` (any alignment). Not all-or-nothing: on a failure the blocks before the failing one are written.
  // Returns only after the medium has taken and programmed every block (a write is synchronous: CTRL_SYNC has nothing left to flush).
  virtual BlockResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept = 0;
  // The medium refuses writes (a protect switch, or its own write-protect bits). Valid once ready().
  virtual bool write_protected() const noexcept = 0;

 protected:
  BlockDevice() = default;
  ~BlockDevice() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  BlockDevice(const BlockDevice&) = delete;
  BlockDevice& operator=(const BlockDevice&) = delete;
};

}  // namespace o3sdk::platform::mk66::storage
