// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A FAT32 volume built in memory and written as a sparse image file (OS-QANET D1, docs/specs/OS-QANET.md section 7; test tooling, never in the image). It is what the host tool `o3fatimg`
// (tests/tools/o3fatimg.cpp) writes a fixture folder into, and what the simulator's `card copy` rebuilds a card with (FatFsCard::computer_copy): the computer's side of a card, outside the unit's
// write guard. FatFs's own formatter and writer are off in the unit's configuration (FF_USE_MKFS 0) and the write-class calls are named in two sources only (tools/check_layers.py rule 6), so the
// volume is written from the Microsoft FAT specification's byte maps, the same ones as the test double fakes::Fat32Builder (platform/mk66/tests/mk66_storage_fakes.h) uses; FatFs, the unit's
// own, reads what this writes.
//
// THE VOLUME: FAT32, 512-byte sectors, ONE sector a cluster (the unit's card has 4 to 32 KB clusters; with one sector a 64 MiB volume has more than the 65,525 clusters FatFs needs to call it FAT32,
// as its volume type is decided by the cluster count alone), 32 reserved sectors, two FATs, the FSInfo and the backup boot sector, the volume id 0x1234ABCD and the label O3HOSTCARD. Every
// entry carries the stamp 2024-11-01 00:00:00 (FF_NORTC_*, the time FatFs gives a file the unit writes: no entry has a stamp of 0, which hal::storage::FileStamp reads as "none"). A name that is not
// a plain upper-case 8.3 name gets long-name entries and a short alias (PATTER~1.MTP), as a computer writes it (a lower-case 8.3 name too: no NT case flags, so `project.mt` is a long name with the alias
// PROJEC~1.MT). Directories are written in the order they are added; add_tree() adds a folder's entries sorted by name (byte order), so a listing of the image reads as DirStorage's does.
//
// Not for the target: the standard library and POSIX are allowed here (platform/host). Main thread only.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace o3sdk::platform::host {

class FatImage {
 public:
  static constexpr std::uint32_t kReserved = 32;
  static constexpr std::uint32_t kRootCluster = 2;
  static constexpr std::uint32_t kMinBlocks = 66'000;           // under it the volume would have fewer than 65,525 clusters and FatFs would call it FAT16
  static constexpr std::uint32_t kDefaultBlocks = 131'072;      // 64 MiB
  static constexpr std::size_t kMaxImageBytes = std::size_t{1} << 30;  // 1 GiB: the whole image is held in memory

  // Formats a volume of `blocks` sectors (kMinBlocks .. kMaxImageBytes / 512). ok() is false and error() says why when it cannot.
  explicit FatImage(std::uint32_t blocks = kDefaultBlocks);

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }
  std::uint32_t blocks() const { return blocks_; }
  std::uint32_t data_clusters() const { return clusters_; }
  std::uint32_t free_clusters() const { return clusters_ + kRootCluster > next_free_ ? clusters_ + kRootCluster - next_free_ : 0u; }

  // A folder of `parent` (kRootCluster for the root); returns its first cluster, 0 when the image is full.
  std::uint32_t make_dir(std::uint32_t parent, const std::string& name);
  // A file of `parent`; false when the image is full (error() says so).
  bool add_file(std::uint32_t parent, const std::string& name, const std::uint8_t* data, std::size_t size);
  // The whole tree of the host folder `dir` into the root (regular files and folders only; a link or another kind of file is refused, as ScratchDir::copy_from refuses it). false: error().
  bool add_tree(const std::string& dir);

  // The image's bytes (blocks() * 512), and the sparse file of them: only the sectors that are not all zero are written, the rest is a hole.
  const std::vector<std::uint8_t>& bytes() const { return image_; }
  std::vector<std::uint8_t> take() { return std::move(image_); }
  bool write_file(const std::string& path, std::string& error) const;

 private:
  struct DirState {
    std::uint32_t last_cluster = 0;
    unsigned slot = 0;  // the next free 32-byte slot of last_cluster (0..15)
  };
  bool add_tree_at(const std::string& dir, std::uint32_t cluster);
  std::uint8_t* sector(std::uint32_t lba) { return image_.data() + static_cast<std::size_t>(lba) * 512u; }
  std::uint32_t cluster_sector(std::uint32_t c) const { return data_start_ + (c - 2u); }
  void set_fat(std::uint32_t cluster, std::uint32_t value);
  std::uint32_t alloc();
  void append_raw(std::uint32_t dir, const std::uint8_t entry[32]);
  void add_entry(std::uint32_t dir, const std::string& name, std::uint8_t attr, std::uint32_t first_cluster, std::uint32_t size);
  void alias(const std::string& name, std::uint32_t dir, std::uint8_t sfn[11]);
  bool fail(const std::string& what);

  std::vector<std::uint8_t> image_;
  std::string error_;
  std::uint32_t blocks_ = 0;
  std::uint32_t clusters_ = 0;
  std::uint32_t fat_sectors_ = 0;
  std::uint32_t data_start_ = 0;
  std::uint32_t next_free_ = 3;
  std::map<std::uint32_t, DirState> dirs_;
  std::map<std::string, unsigned> alias_count_;
};

}  // namespace o3sdk::platform::host
