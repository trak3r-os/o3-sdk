// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK 6.3 (F5): the volume-dirty flag, read from the card after a Host session. A computer that was unplugged without an eject leaves it set; the remount only WARNS (a dirty flag is routinely left set by hosts
// that never clear it, so refusing writes would strand users). Pure over a BlockDevice (block 0, the first partition's boot sector, the first FAT sector: three reads at most), no FatFs, main loop only.
//
//   FAT12/16  the boot sector's byte at 0x25 bit 0 (the NT dirty flag) and, FAT16 only, FAT[1] ClnShutBit (0x8000 of the second entry; set means clean).
//   FAT32     the byte at 0x41 bit 0 and FAT[1] bit 27 (0x08000000, set means clean).
//   exFAT     VolumeFlags (0x6A) bit 1, VolumeDirty.
// The behaviour of each operating system after a clean eject and after a pulled cable is NOT verified here (open: fixtures taken from Windows, macOS and Linux); the tests pin the bits on images built by hand.

#include <o3sdk/platform/mk66/storage/block_device.h>

#include <cstdint>
#include <cstring>

namespace o3sdk::platform::mk66::storage {

enum class VolumeFlag : std::uint8_t { Clean, Dirty, Unknown };  // Unknown: a read failed, FAT12, or nothing that looks like a FAT or exFAT boot sector

namespace volume_dirty_detail {
constexpr std::uint16_t le16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
constexpr std::uint32_t le32(const std::uint8_t* p) noexcept { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }

constexpr bool is_boot_sector(const std::uint8_t* s) noexcept {
  if (s[510] != 0x55 || s[511] != 0xAA) {
    return false;
  }
  if (std::memcmp(s + 3, "EXFAT   ", 8) == 0) {
    return true;
  }
  const std::uint8_t spc = s[13];
  return le16(s + 11) == 512 && spc != 0 && (spc & (spc - 1)) == 0 && le16(s + 14) != 0 && (s[16] == 1 || s[16] == 2);
}
}  // namespace volume_dirty_detail

inline VolumeFlag volume_flag(BlockDevice& dev) noexcept {
  using namespace volume_dirty_detail;
  alignas(4) std::uint8_t sec[512];
  if (dev.read(0, 1, sec) != BlockResult::Ok) {
    return VolumeFlag::Unknown;
  }
  std::uint32_t base = 0;
  if (!is_boot_sector(sec)) {  // a partitioned card: the first partition that exists (as FatFs looks for its volume)
    bool found = false;
    std::uint8_t table[64];  // (the boot sector read below overwrites `sec`)
    const bool mbr = sec[510] == 0x55 && sec[511] == 0xAA;
    std::memcpy(table, sec + 446, sizeof table);
    for (unsigned i = 0; mbr && i < 4 && !found; ++i) {
      const std::uint8_t* e = table + 16 * i;
      if (e[4] != 0 && le32(e + 8) != 0) {
        base = le32(e + 8);
        found = dev.read(base, 1, sec) == BlockResult::Ok && is_boot_sector(sec);
      }
    }
    if (!found) {
      return VolumeFlag::Unknown;
    }
  }
  if (std::memcmp(sec + 3, "EXFAT   ", 8) == 0) {
    return (le16(sec + 0x6A) & 2u) != 0 ? VolumeFlag::Dirty : VolumeFlag::Clean;
  }
  const std::uint32_t fat_sectors = le16(sec + 22) != 0 ? le16(sec + 22) : le32(sec + 36);
  const std::uint32_t total = le16(sec + 19) != 0 ? le16(sec + 19) : le32(sec + 32);
  const std::uint32_t root_sectors = (static_cast<std::uint32_t>(le16(sec + 17)) * 32u + 511u) / 512u;
  const std::uint32_t overhead = le16(sec + 14) + static_cast<std::uint32_t>(sec[16]) * fat_sectors + root_sectors;
  const std::uint32_t clusters = total > overhead ? (total - overhead) / sec[13] : 0u;
  const bool fat32 = le16(sec + 22) == 0;
  if (!fat32 && clusters < 4085u) {
    return VolumeFlag::Unknown;  // FAT12: no ClnShutBit; its boot flag alone is not worth a warning
  }
  bool dirty = ((fat32 ? sec[0x41] : sec[0x25]) & 1u) != 0;
  const std::uint32_t fat0 = base + le16(sec + 14);
  if (dev.read(fat0, 1, sec) != BlockResult::Ok) {
    return dirty ? VolumeFlag::Dirty : VolumeFlag::Unknown;
  }
  dirty = dirty || (fat32 ? (le32(sec + 4) & 0x08000000u) == 0 : (le16(sec + 2) & 0x8000u) == 0);
  return dirty ? VolumeFlag::Dirty : VolumeFlag::Clean;
}

}  // namespace o3sdk::platform::mk66::storage
