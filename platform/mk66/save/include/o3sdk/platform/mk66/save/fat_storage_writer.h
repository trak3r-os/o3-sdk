// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// hal::storage::StorageWriter over FatFs (OS-007n): the save layer's one translation unit that names a write-class FatFs call besides the firmware writer's
// (tools/check_layers.py rule 6). It uses the volume FatStorage mounted (FatFs keeps one), applies the path guard (hal/save_guard.h) to the full path right
// before every write-class call, checks the gate (a mounted FAT12/16/32 volume, not write protected; any such card, no marker: OS-ANYCARD) before every
// mutating call and again before every chunk, and refuses to remove a file whose first cluster another directory entry of its folder uses. Memory: one FIL for the
// file being written, one FIL and one DIR and one FILINFO for the cross-link probe: about 1.5 KB, which the image places in SDRAM (an object of the sample arena's
// tail), never in SRAM_U.

#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/fat_storage.h>

#include <ff.h>

namespace o3sdk::platform::mk66::save {

class FatStorageWriter final : public hal::storage::StorageWriter {
 public:
  FatStorageWriter(storage::FatStorage& fat, storage::BlockDevice& device) noexcept : fat_(fat), device_(device) {}

  hal::storage::WriteStatus gate() noexcept override;
  // OS-USBDISK U2a: who has the card. While it is the computer's (CardOwner::Host) gate() answers Host (not NoCard: the save layer keeps what it has to write and the strip says why). Main loop; null: never Host.
  void attach_owner(const storage::CardSwap& swap) noexcept { swap_ = &swap; }
  hal::storage::WriteStatus make_dir(const char* path) noexcept override;
  hal::storage::WriteStatus create(const char* path) noexcept override;
  hal::storage::WriteStatus create_marker(const char* path) noexcept override;
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override;
  hal::storage::WriteStatus finish() noexcept override;
  void abandon() noexcept override;
  bool writing() const noexcept override { return writing_; }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override;
  hal::storage::WriteStatus remove(const char* path) noexcept override;
  // A save (all its jobs: a project and its samples) holds to the card it began on: begin_save() pins the volume's serial, end_save() unpins, card_id() is the serial now mounted.
  void begin_save() noexcept override;
  void end_save() noexcept override { pinned_ = false; }
  std::uint32_t card_id() const noexcept override { return fat_.mounted() ? fat_.volume_serial() : 0u; }

  // FRESULT of the last FatFs call that failed (0 when none did), for the log.
  int last_fatfs_error() const noexcept { return last_fresult_; }

 private:
  hal::storage::WriteStatus from_fresult(FRESULT r) noexcept;
  bool exists(const char* path, bool* is_dir) noexcept;
  hal::storage::WriteStatus shares_chain(const char* path) noexcept;
  // gate() plus the card's identity (OS-004d): every mutating call of a save, make_dir() included, must find the volume it pinned at begin_save().
  hal::storage::WriteStatus gate_same_card() noexcept;

  storage::FatStorage& fat_;
  storage::BlockDevice& device_;
  const storage::CardSwap* swap_ = nullptr;
  // The card of the save in progress. Nothing else tells a card swapped (or re-mounted) in the middle of a save from the one it
  // began on, so begin_save() (once per save: OS-FU5; a save is a project and its samples, one job each, and a swap between two jobs must not split it over two cards) pins the
  // volume's serial and every later mutating call, make_dir() included, refuses with NoCard when it is not the same. end_save() unpins. A caller that never says (the tests of the
  // jobs) gets the pin of its first make_dir(); a rename or remove with no pin (a boot-time recovery) has none to compare.
  std::uint32_t pin_serial_ = 0;
  // OS-SDSWAP: and the mount it began on (FatStorage::generation(): every mount and every release is another number). The serial alone would let the SAME card, taken out and put back,
  // go on with a file handle of the first mount; a pulled card ends the save for good, whatever card comes next.
  std::uint32_t pin_generation_ = 0;
  bool pinned_ = false;
  FIL wr_{};
  FIL probe_{};
  DIR dir_{};
  FILINFO info_{};
  bool writing_ = false;
  int last_fresult_ = 0;
};

}  // namespace o3sdk::platform::mk66::save
