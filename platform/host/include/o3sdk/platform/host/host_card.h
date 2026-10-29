// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A card of the host as the simulator holds it (OS-QANET D1): the read side (hal::storage::Storage) and the write side (hal::storage::StorageWriter) of one card, and the slot's switch. DirCard
// (dir_card.h: a scratch directory) and FatFsStorage (fat_fs_storage.h: a FAT32 image through the unit's FatFs) are the two. The simulator's card commands and its project loader see this and nothing
// more, so that one scenario runs on either. Host only (platform/host); main thread only.

#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>

namespace o3sdk::platform::host {

class HostCard : public hal::storage::Storage, public hal::storage::StorageWriter {
 public:
  virtual ~HostCard() = default;
  // The slot: false takes the card out (every call answers NoCard or NotMounted from then on, a file being written stays torn), true puts it back (not mounted until mount()).
  virtual void set_present(bool present) = 0;
  // Mutating calls the path guard refused since the card was made (a save that asks for an unguarded path is a bug; a test asserts it is 0).
  virtual unsigned violations() const = 0;
};

}  // namespace o3sdk::platform::host
