// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test doubles for the read-only SD stack (OS-007e), header-only so the target (QEMU) suite and the host build use the same ones:
//   * Medium        a sparse 512-byte block device: only the sectors a test writes exist, every other one reads as zeros, so a 32 MB FAT32 volume costs
//                   a few KB. It is a hal-level BlockDevice (FatStorage runs on it directly) and the data source of the fake card.
//   * Fat32Builder  formats a FAT32 volume on a Medium and adds directories and files, with long names (LFN entries, a short alias, its checksum),
//                   a cluster of one sector so that every file is a chain, and directories that span clusters. Written from the Microsoft FAT32
//                   specification's byte maps; it shares no code with FatFs, which reads what it writes.
//   * FakeSdBus     an SdBus that is a software SD card: the state machine of the identification sequence, the answers (R1, R2, R3, R6, R7), the CSD
//                   (v1 and v2) and CID it was told to have, the CMD6 status, block reads from a Medium, and the faults a test asks for (a card that
//                   never gets ready, a CRC error on the next reads, a timeout). It logs every command it is given, in order.
// None of this is in an image.

#include <o3sdk/platform/mk66/storage/block_device.h>
#include <o3sdk/platform/mk66/storage/sd_bus.h>
#include <o3sdk/platform/mk66/storage/sd_protocol.h>
#include <o3sdk/platform/mk66/storage/sdhc_facts.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fakes {

namespace st = o3sdk::platform::mk66::storage;
namespace sd = o3sdk::platform::mk66::storage::sd;

// ---- Medium --------------------------------------------------------------------------------------------------------------------------------
class Medium final : public st::BlockDevice {
 public:
  explicit Medium(std::uint32_t blocks) : blocks_(blocks) {}

  std::uint8_t* sector(std::uint32_t lba) {  // creates it (zeros) when it does not exist
    auto it = sectors_.find(lba);
    if (it == sectors_.end()) {
      it = sectors_.emplace(lba, std::vector<std::uint8_t>(512, 0)).first;
    }
    return it->second.data();
  }
  void read_sector(std::uint32_t lba, std::uint8_t* dst) const {
    const auto it = sectors_.find(lba);
    if (it == sectors_.end()) {
      std::memset(dst, 0, 512);
    } else {
      std::memcpy(dst, it->second.data(), 512);
    }
  }
  std::size_t materialised() const { return sectors_.size(); }
  // FNV-1a over every materialised sector (number and bytes): equal before and after means no byte of the medium changed.
  std::uint64_t fingerprint() const {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& kv : sectors_) {
      h = (h ^ kv.first) * 1099511628211ull;
      for (const std::uint8_t b : kv.second) {
        h = (h ^ b) * 1099511628211ull;
      }
    }
    return h;
  }

  // BlockDevice
  bool present() const noexcept override { return present_; }
  st::BlockResult start() noexcept override {
    ++starts;
    started_ = present_ && !fail_start;
    return !present_ ? st::BlockResult::NoMedium : fail_start ? st::BlockResult::Failed : st::BlockResult::Ok;
  }
  bool ready() const noexcept override { return started_ && present_; }
  void release() noexcept override {
    started_ = false;
    ++releases;
  }
  std::uint32_t block_count() const noexcept override { return blocks_; }
  st::BlockResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override {
    if (!ready()) {
      return st::BlockResult::NotReady;
    }
    if (lba >= blocks_ || count > blocks_ - lba) {
      return st::BlockResult::OutOfRange;
    }
    if (fail_reads) {
      return st::BlockResult::Failed;
    }
    ++read_calls;
    blocks_read += count;
    for (std::uint32_t i = 0; i < count; ++i) {
      read_sector(lba + i, static_cast<std::uint8_t*>(dst) + 512u * i);
    }
    return st::BlockResult::Ok;
  }

  st::BlockResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override {
    if (!ready()) {
      return st::BlockResult::NotReady;
    }
    if (lba >= blocks_ || count > blocks_ - lba) {
      return st::BlockResult::OutOfRange;
    }
    if (protect) {
      return st::BlockResult::WriteProtected;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      if (fail_writes_after == 0u) {
        return st::BlockResult::Failed;
      }
      if (fail_writes_after != 0xFFFFFFFFu) {
        --fail_writes_after;
      }
      std::memcpy(sector(lba + i), static_cast<const std::uint8_t*>(src) + 512u * i, 512);
      ++blocks_written;
      written_lbas.push_back(lba + i);
    }
    ++write_calls;
    return st::BlockResult::Ok;
  }
  bool write_protected() const noexcept override { return protect; }

  bool present_ = true;
  bool fail_start = false;                           // start() fails on a card that is in (a card that does not initialise)
  bool fail_reads = false;
  bool protect = false;                              // the medium refuses writes
  std::uint32_t fail_writes_after = 0xFFFFFFFFu;     // the write of the Nth block from now on fails (0: the next one)
  unsigned starts = 0;
  unsigned releases = 0;
  unsigned read_calls = 0;
  unsigned write_calls = 0;
  std::uint64_t blocks_read = 0;
  std::uint64_t blocks_written = 0;
  std::vector<std::uint32_t> written_lbas;

 private:
  std::uint32_t blocks_;
  bool started_ = false;
  std::map<std::uint32_t, std::vector<std::uint8_t>> sectors_;
};

// ---- Fat32Builder -----------------------------------------------------------------------------------------------------------------------------
inline void put16(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

class Fat32Builder {
 public:
  static constexpr std::uint32_t kReserved = 32;
  static constexpr std::uint32_t kRootCluster = 2;

  // `data_clusters` of one 512-byte sector each: at least 65,525, or the volume would be FAT16 (FatFs decides by the cluster count).
  explicit Fat32Builder(Medium& medium, std::uint32_t data_clusters = 65'600) : m_(medium), clusters_(data_clusters) {
    fat_sectors_ = ((clusters_ + 2u) * 4u + 511u) / 512u;
    data_start_ = kReserved + 2u * fat_sectors_;
    total_ = data_start_ + clusters_;
    std::uint8_t* b = m_.sector(0);
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    std::memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, 512);
    b[13] = 1;                      // sectors per cluster
    put16(b + 14, kReserved);
    b[16] = 2;                      // FATs
    b[21] = 0xF8;                   // media
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 32, total_);
    put32(b + 36, fat_sectors_);
    put32(b + 44, kRootCluster);
    put16(b + 48, 1);               // FSInfo
    put16(b + 50, 6);               // backup boot sector
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, 0x1234ABCDu);
    std::memcpy(b + 71, "O3TESTCARD ", 11);
    std::memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;
    std::memcpy(m_.sector(6), b, 512);
    std::uint8_t* fsi = m_.sector(1);
    put32(fsi + 0, 0x41615252u);
    put32(fsi + 484, 0x61417272u);
    put32(fsi + 488, 0xFFFFFFFFu);
    put32(fsi + 492, 0xFFFFFFFFu);
    put32(fsi + 508, 0xAA550000u);
    set_fat(0, 0x0FFFFFF8u);
    set_fat(1, 0x0FFFFFFFu);
    set_fat(kRootCluster, 0x0FFFFFFFu);  // the root directory: one cluster, grown when it fills
    next_free_ = kRootCluster + 1u;
    dirs_[kRootCluster] = DirState{kRootCluster, 0};
  }

  std::uint32_t total_sectors() const { return total_; }
  std::uint32_t data_clusters() const { return clusters_; }
  // Marks clusters as used (single orphan clusters) until `leave_free` remain: a nearly full volume, for the disk-full tests.
  void consume_clusters(std::uint32_t leave_free) {
    while (next_free_ < kRootCluster + clusters_ - leave_free) {
      (void)alloc();
    }
  }

  // A subdirectory of `parent` (kRootCluster for the root): returns its first cluster.
  std::uint32_t make_dir(std::uint32_t parent, const std::string& name) {
    const std::uint32_t c = alloc();
    dirs_[c] = DirState{c, 0};
    add_entry(parent, name, 0x10, c, 0);
    std::uint8_t dot[32];
    std::memset(dot, ' ', 11);
    std::memset(dot + 11, 0, 21);
    dot[0] = '.';
    dot[11] = 0x10;
    put16(dot + 20, c >> 16);
    put16(dot + 26, c);
    append_raw(c, dot);
    std::memset(dot, ' ', 11);
    dot[0] = dot[1] = '.';
    const std::uint32_t up = parent == kRootCluster ? 0u : parent;  // ".." of a first-level directory points at cluster 0
    put16(dot + 20, up >> 16);
    put16(dot + 26, up);
    append_raw(c, dot);
    return c;
  }

  // A file in `dir`: its bytes in a chain of clusters (one sector each).
  void add_file(std::uint32_t dir, const std::string& name, const std::vector<std::uint8_t>& data) {
    std::uint32_t first = 0;
    std::uint32_t prev = 0;
    for (std::size_t off = 0; off < data.size(); off += 512) {
      const std::uint32_t c = alloc();
      if (first == 0) {
        first = c;
      } else {
        set_fat(prev, c);
      }
      prev = c;
      const std::size_t n = data.size() - off < 512 ? data.size() - off : 512;
      std::memcpy(m_.sector(cluster_sector(c)), data.data() + off, n);
    }
    add_entry(dir, name, 0x20, first, static_cast<std::uint32_t>(data.size()));
  }

 private:
  struct DirState {
    std::uint32_t last_cluster;
    unsigned slot;  // the next free 32-byte slot of last_cluster (0..15)
  };

  std::uint32_t cluster_sector(std::uint32_t c) const { return data_start_ + (c - 2u); }

  void set_fat(std::uint32_t cluster, std::uint32_t value) {
    const std::uint32_t at = cluster * 4u;
    for (std::uint32_t copy = 0; copy < 2; ++copy) {
      put32(m_.sector(kReserved + copy * fat_sectors_ + at / 512u) + at % 512u, value);
    }
  }
  std::uint32_t alloc() {
    const std::uint32_t c = next_free_++;
    set_fat(c, 0x0FFFFFFFu);
    return c;
  }

  void append_raw(std::uint32_t dir, const std::uint8_t entry[32]) {
    DirState& d = dirs_.at(dir);
    if (d.slot == 16u) {  // the cluster is full: chain a new one
      const std::uint32_t c = alloc();
      set_fat(d.last_cluster, c);
      d.last_cluster = c;
      d.slot = 0;
    }
    std::memcpy(m_.sector(cluster_sector(d.last_cluster)) + 32u * d.slot, entry, 32);
    ++d.slot;
  }

  static std::vector<std::uint16_t> utf16(const std::string& s) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < s.size();) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c < 0x80) {
        out.push_back(c);
        i += 1;
      } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        out.push_back(static_cast<std::uint16_t>(((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu)));
        i += 2;
      } else {  // 3-byte UTF-8 (the BMP)
        out.push_back(static_cast<std::uint16_t>(((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                                                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu)));
        i += 3;
      }
    }
    return out;
  }

  static bool is_plain_83(const std::string& name, std::uint8_t sfn[11]) {
    std::memset(sfn, ' ', 11);
    const std::size_t dot = name.rfind('.');
    const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3) {
      return false;
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
      const char c = base[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
        return false;
      }
      sfn[i] = static_cast<std::uint8_t>(c);
    }
    for (std::size_t i = 0; i < ext.size(); ++i) {
      const char c = ext[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
        return false;
      }
      sfn[8 + i] = static_cast<std::uint8_t>(c);
    }
    return true;
  }

  // "PATTER~1.MTP" for "pattern_01.mtp": the first six letters and digits upper-cased, ~N, the first three of the extension.
  std::string alias(const std::string& name, std::uint32_t dir, std::uint8_t sfn[11]) {
    std::string base;
    std::string ext;
    const std::size_t dot = name.rfind('.');
    const std::string b = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (const char c : b) {
      if (base.size() < 6 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
        base += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
      }
    }
    for (const char c : e) {
      if (ext.size() < 3 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
        ext += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
      }
    }
    if (base.empty()) {
      base = "X";
    }
    const unsigned n = ++alias_count_[std::to_string(dir) + "/" + base];
    const std::string tail = "~" + std::to_string(n);
    base = base.substr(0, 8 - tail.size()) + tail;
    std::memset(sfn, ' ', 11);
    std::memcpy(sfn, base.data(), base.size());
    std::memcpy(sfn + 8, ext.data(), ext.size());
    return base;
  }

  static std::uint8_t checksum(const std::uint8_t sfn[11]) {
    std::uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
      sum = static_cast<std::uint8_t>(((sum & 1u) << 7) + (sum >> 1) + sfn[i]);
    }
    return sum;
  }

  void add_entry(std::uint32_t dir, const std::string& name, std::uint8_t attr, std::uint32_t first_cluster, std::uint32_t size) {
    std::uint8_t sfn[11];
    if (!is_plain_83(name, sfn)) {
      alias(name, dir, sfn);
      std::vector<std::uint16_t> u = utf16(name);
      const std::size_t count = (u.size() + 12u) / 13u;
      u.push_back(0x0000);
      while (u.size() < count * 13u) {
        u.push_back(0xFFFF);
      }
      const std::uint8_t sum = checksum(sfn);
      static const unsigned kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      for (std::size_t seq = count; seq >= 1; --seq) {  // the last part first
        std::uint8_t e[32] = {};
        e[0] = static_cast<std::uint8_t>(seq | (seq == count ? 0x40u : 0u));
        e[11] = 0x0F;
        e[13] = sum;
        for (unsigned k = 0; k < 13; ++k) {
          put16(e + kOffsets[k], u[(seq - 1) * 13 + k]);
        }
        append_raw(dir, e);
      }
    }
    std::uint8_t e[32] = {};
    std::memcpy(e, sfn, 11);
    e[11] = attr;
    put16(e + 20, first_cluster >> 16);
    put16(e + 26, first_cluster);
    put32(e + 28, size);
    append_raw(dir, e);
  }

  Medium& m_;
  std::uint32_t clusters_;
  std::uint32_t fat_sectors_ = 0;
  std::uint32_t data_start_ = 0;
  std::uint32_t total_ = 0;
  std::uint32_t next_free_ = 3;
  std::map<std::uint32_t, DirState> dirs_;
  std::map<std::string, unsigned> alias_count_;
};

// ---- ExFatBuilder -----------------------------------------------------------------------------------------------------------------------------
// A small exFAT volume (one-sector clusters, one FAT, no upcase table: FatFs folds case with its own): the boot sector, the FAT, an allocation bitmap
// entry in the root directory (FatFs needs to find it to mount), and entry sets (the 0x85 file entry, the 0xC0 stream extension with the name hash,
// the 0xC1 name entries) with the set checksum, as the exFAT specification's byte maps write them. A file is either contiguous (no FAT chain: the
// stream flag NoFatChain) or fragmented (a chain with a cluster left between two of its clusters). Names are ASCII.
class ExFatBuilder {
 public:
  static constexpr std::uint32_t kFatOffset = 24;
  static constexpr std::uint32_t kRootCluster = 4;

  explicit ExFatBuilder(Medium& medium, std::uint32_t clusters = 4096) : m_(medium), clusters_(clusters) {
    fat_sectors_ = ((clusters_ + 2u) * 4u + 511u) / 512u;
    heap_ = kFatOffset + fat_sectors_;
    total_ = heap_ + clusters_;
    std::uint8_t* b = m_.sector(0);
    b[0] = 0xEB;
    b[1] = 0x76;
    b[2] = 0x90;
    std::memcpy(b + 3, "EXFAT   ", 8);
    put32(b + 72, total_);        // VolumeLength (low half; the high half stays zero)
    put32(b + 80, kFatOffset);
    put32(b + 84, fat_sectors_);
    put32(b + 88, heap_);
    put32(b + 92, clusters_);
    put32(b + 96, kRootCluster);
    put32(b + 100, 0xE7FA7000u);
    put16(b + 104, 0x0100);       // exFAT 1.00
    b[108] = 9;                   // 512-byte sectors
    b[109] = 0;                   // one sector per cluster
    b[110] = 1;                   // one FAT
    b[111] = 0x80;
    b[112] = 0xFF;
    b[510] = 0x55;
    b[511] = 0xAA;
    set_fat(0, 0xFFFFFFF8u);
    set_fat(1, 0xFFFFFFFFu);
    set_fat(2, 0xFFFFFFFFu);      // the allocation bitmap (one cluster)
    set_fat(3, 0xFFFFFFFFu);      // (the upcase table's cluster: reserved, empty)
    set_fat(kRootCluster, 0xFFFFFFFFu);
    next_free_ = kRootCluster + 1u;
    std::uint8_t bitmap[32] = {};
    bitmap[0] = 0x81;
    put32(bitmap + 20, 2);        // the bitmap's cluster
    put32(bitmap + 24, (clusters_ + 7u) / 8u);
    dirs_[kRootCluster] = DirState{kRootCluster, 0};
    append_raw(kRootCluster, bitmap);
  }

  std::uint32_t make_dir(std::uint32_t parent, const std::string& name) {
    const std::uint32_t c = alloc();
    dirs_[c] = DirState{c, 0};
    add_set(parent, name, 0x10, c, 512, false);
    return c;
  }

  void add_file(std::uint32_t dir, const std::string& name, const std::vector<std::uint8_t>& data, bool contiguous) {
    std::uint32_t first = 0;
    std::uint32_t prev = 0;
    for (std::size_t off = 0; off < data.size(); off += 512) {
      const std::uint32_t c = alloc();
      if (!contiguous) {
        (void)alloc();  // a gap: the next cluster of the chain is not the one after this
      }
      if (first == 0) {
        first = c;
      } else if (!contiguous) {
        set_fat(prev, c);
      }
      prev = c;
      const std::size_t n = data.size() - off < 512 ? data.size() - off : 512;
      std::memcpy(m_.sector(sector_of(c)), data.data() + off, n);
    }
    add_set(dir, name, 0x20, first, static_cast<std::uint32_t>(data.size()), contiguous);
  }

 private:
  struct DirState {
    std::uint32_t last_cluster;
    unsigned slot;
  };
  std::uint32_t sector_of(std::uint32_t c) const { return heap_ + (c - 2u); }
  void set_fat(std::uint32_t cluster, std::uint32_t value) {
    const std::uint32_t at = cluster * 4u;
    put32(m_.sector(kFatOffset + at / 512u) + at % 512u, value);
  }
  std::uint32_t alloc() {
    const std::uint32_t c = next_free_++;
    set_fat(c, 0xFFFFFFFFu);
    return c;
  }
  void append_raw(std::uint32_t dir, const std::uint8_t entry[32]) {
    DirState& d = dirs_.at(dir);
    if (d.slot == 16u) {
      const std::uint32_t c = alloc();
      set_fat(d.last_cluster, c);
      d.last_cluster = c;
      d.slot = 0;
    }
    std::memcpy(m_.sector(sector_of(d.last_cluster)) + 32u * d.slot, entry, 32);
    ++d.slot;
  }
  static std::uint16_t name_hash(const std::string& name) {
    std::uint16_t sum = 0;
    for (const char ch : name) {
      std::uint16_t c = static_cast<unsigned char>(ch);
      c = (c >= 'a' && c <= 'z') ? static_cast<std::uint16_t>(c - 32) : c;
      sum = static_cast<std::uint16_t>(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + (c & 0xFFu));
      sum = static_cast<std::uint16_t>(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + (c >> 8));
    }
    return sum;
  }
  void add_set(std::uint32_t dir, const std::string& name, std::uint16_t attr, std::uint32_t first, std::uint32_t size, bool no_fat_chain) {
    const std::size_t names = (name.size() + 14u) / 15u;
    std::vector<std::uint8_t> set((2u + names) * 32u, 0);
    set[0] = 0x85;
    set[1] = static_cast<std::uint8_t>(1u + names);
    put16(set.data() + 4, attr);
    std::uint8_t* st = set.data() + 32;
    st[0] = 0xC0;
    st[1] = static_cast<std::uint8_t>(0x01u | (no_fat_chain ? 0x02u : 0u));
    st[3] = static_cast<std::uint8_t>(name.size());
    put16(st + 4, name_hash(name));
    put32(st + 8, size);   // ValidDataLength (low half)
    put32(st + 20, first);
    put32(st + 24, size);  // DataLength (low half)
    for (std::size_t n = 0; n < names; ++n) {
      std::uint8_t* e = set.data() + 64 + 32 * n;
      e[0] = 0xC1;
      for (std::size_t k = 0; k < 15; ++k) {
        const std::size_t at = n * 15 + k;
        put16(e + 2 + 2 * k, at < name.size() ? static_cast<unsigned char>(name[at]) : 0u);
      }
    }
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < set.size(); ++i) {
      if (i == 2) {
        ++i;  // the checksum field itself
        continue;
      }
      sum = static_cast<std::uint16_t>(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + set[i]);
    }
    put16(set.data() + 2, sum);
    for (std::size_t i = 0; i < set.size(); i += 32) {
      append_raw(dir, set.data() + i);
    }
  }

  Medium& m_;
  std::uint32_t clusters_;
  std::uint32_t fat_sectors_ = 0;
  std::uint32_t heap_ = 0;
  std::uint32_t total_ = 0;
  std::uint32_t next_free_ = 5;
  std::map<std::uint32_t, DirState> dirs_;
};

// ---- FakeSdBus ------------------------------------------------------------------------------------------------------------------------------
struct FakeCardSpec {
  bool present = true;
  bool respond = true;          // false: the slot has a card-shaped nothing in it: no command is answered
  bool v2 = true;               // answers CMD8 (spec 2.00 and later)
  bool high_capacity = true;    // CCS when the host asked for it (SDHC / SDXC); false: byte addressing
  unsigned busy_polls = 2;      // ACMD41 answers "busy" this many times before "ready"
  bool never_ready = false;
  bool switch_class = true;     // CCC bit 10: CMD6 exists
  bool hs_supported = true;
  bool hs_switch_fails = false;  // the switch answers with function 0xF
  bool csd_v1 = false;          // the CSD is version 1 (SDSC)
  unsigned data_errors = 0;     // the next N block reads end in a CRC error
  unsigned data_timeouts = 0;   // the next N block reads time out
  bool wp_switch = false;       // the slot's write-protect switch says protected (OS-006c)
  bool csd_wp = false;          // PERM_WRITE_PROTECT in the CSD
  unsigned write_crc_errors = 0;  // the next N block writes end in a CRC status error
  unsigned write_timeouts = 0;    // the next N block writes time out
  unsigned write_busy_polls = 0;  // after a write, CMD13 says "programming" this many times
  bool write_stuck_busy = false;  // after a write, CMD13 never says ready (the clock then runs 1 ms a call so the bound is reached)
  std::uint8_t manufacturer = 0x03;
  char oem[3] = {'S', 'D', 0};
  char product[6] = {'S', 'C', '1', '6', 'G', 0};
  std::uint8_t revision = 0x80;  // 8.0
  std::uint32_t serial = 0x1234ABCDu;
  unsigned year = 2024;
  unsigned month = 7;
};

inline void set_bits(std::uint8_t reg[16], unsigned hi, unsigned lo, std::uint32_t value) {
  for (unsigned n = lo; n <= hi; ++n) {
    std::uint8_t& byte = reg[(127u - n) / 8u];
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (n % 8u));
    byte = static_cast<std::uint8_t>(((value >> (n - lo)) & 1u) != 0u ? (byte | mask) : (byte & ~mask));
  }
}

class FakeSdBus final : public sd::SdBus {
 public:
  struct Seen {
    sd::Cmd cmd;
    std::uint32_t arg;
  };

  FakeSdBus(Medium& medium, const FakeCardSpec& s) : spec(s), medium_(medium) {}

  FakeCardSpec spec;
  std::vector<Seen> log;
  std::vector<std::uint32_t> clocks_asked;   // the max_hz of every set_clock
  std::vector<std::uint32_t> clocks_made;
  unsigned host_width = 1;
  unsigned card_width = 1;
  bool hs_active = false;
  unsigned power_ups = 0;
  unsigned reads_single = 0;
  unsigned reads_multi = 0;
  std::uint64_t blocks_served = 0;
  std::uint32_t block_len = 0;               // what CMD16 set (0: never)
  std::uint32_t slept_ms = 0;
  std::uint32_t last_read_arg = 0;
  std::uint32_t last_write_arg = 0;
  unsigned writes = 0;                       // blocks the fake card stored

  // SdBus
  bool card_present() const noexcept override { return spec.present; }
  sd::Result power_up() noexcept override {
    ++power_ups;
    state_ = State::Idle;
    host_width = 1;
    card_width = 1;
    return spec.present ? sd::Result::Ok : sd::Result::NoCard;
  }
  std::uint32_t set_clock(std::uint32_t max_hz) noexcept override {
    clocks_asked.push_back(max_hz);
    const std::uint32_t hz = st::sdhc::plan_clock(st::sdhc::kSdhcSourceHz, max_hz).hz;
    clocks_made.push_back(hz);
    return hz;
  }
  void set_bus_width(unsigned bits) noexcept override { host_width = bits; }
  void delay_ms(std::uint32_t ms) noexcept override {
    slept_ms += ms;
    now_us_ += ms * 1000u;
  }
  std::uint32_t micros() noexcept override { return now_us_ += stuck_ ? 1000u : 1u; }
  bool write_protect_switch() const noexcept override { return spec.wp_switch; }

  sd::Result command(sd::Cmd cmd, std::uint32_t arg, sd::Response& out) noexcept override {
    if (!sd::known(cmd) || sd::reads_data(cmd) || sd::writes_data(cmd)) {
      return sd::Result::Refused;
    }
    log.push_back({cmd, arg});
    const bool was_app = app_next_;
    app_next_ = false;
    out = sd::Response{};
    if (cmd == sd::Cmd::GoIdle) {
      state_ = State::Idle;
      return sd::Result::Ok;  // no response expected
    }
    if (!spec.present || !spec.respond) {
      return sd::Result::NoResponse;
    }
    if (sd::is_app(cmd) && !was_app) {
      return sd::Result::NoResponse;  // an application command that CMD55 did not announce: a real card ignores or refuses it
    }
    switch (cmd) {
      case sd::Cmd::SendIfCond:
        if (!spec.v2) {
          return sd::Result::NoResponse;
        }
        out.word[0] = arg & 0xFFFu;
        return sd::Result::Ok;
      case sd::Cmd::AppCmd:
        out.word[0] = r1() | sd::kR1AppCmd;
        app_next_ = true;
        return sd::Result::Ok;
      case sd::Cmd::AcmdSdSendOpCond: {
        if (spec.never_ready || polls_++ < spec.busy_polls) {
          out.word[0] = sd::kOcrVoltageWindow;  // busy bit clear
          return sd::Result::Ok;
        }
        state_ = State::Ready;
        const bool ccs = spec.v2 && spec.high_capacity && (arg & sd::kOcrHcs) != 0u;
        out.word[0] = sd::kOcrBusy | sd::kOcrVoltageWindow | (ccs ? sd::kOcrCcs : 0u);
        byte_addressed_ = !ccs;
        return sd::Result::Ok;
      }
      case sd::Cmd::AllSendCid: {
        if (state_ != State::Ready) {
          return sd::Result::NoResponse;
        }
        std::uint8_t reg[16];
        make_cid(reg);
        words(reg, out);
        state_ = State::Ident;
        return sd::Result::Ok;
      }
      case sd::Cmd::SendRelativeAddr:
        if (state_ != State::Ident) {
          return sd::Result::NoResponse;
        }
        state_ = State::Stby;
        out.word[0] = (static_cast<std::uint32_t>(kRca) << 16) | 0x0500u;
        return sd::Result::Ok;
      case sd::Cmd::SendCsd: {
        if (state_ != State::Stby || arg != sd::rca_arg(kRca)) {
          return sd::Result::NoResponse;
        }
        std::uint8_t reg[16];
        make_csd(reg);
        words(reg, out);
        return sd::Result::Ok;
      }
      case sd::Cmd::SelectCard:
        if (state_ != State::Stby || arg != sd::rca_arg(kRca)) {
          return sd::Result::NoResponse;
        }
        state_ = State::Tran;
        out.word[0] = r1();
        return sd::Result::Ok;
      case sd::Cmd::SetBlockLen:
        if (state_ != State::Tran) {
          return sd::Result::NoResponse;
        }
        block_len = arg;
        out.word[0] = r1();
        return sd::Result::Ok;
      case sd::Cmd::AcmdSetBusWidth:
        if (state_ != State::Tran) {
          return sd::Result::NoResponse;
        }
        card_width = arg == sd::kBusWidthArg4 ? 4u : 1u;
        out.word[0] = r1();
        return sd::Result::Ok;
      case sd::Cmd::StopTransmission:
        out.word[0] = r1();
        return sd::Result::Ok;
      case sd::Cmd::SendStatus:
        if (spec.write_stuck_busy && programming_) {
          out.word[0] = (7u << 9);  // programming, not ready for data
        } else if (busy_left_ != 0u) {
          --busy_left_;
          out.word[0] = (7u << 9);
        } else {
          programming_ = false;
          stuck_ = false;
          out.word[0] = r1();
        }
        return sd::Result::Ok;
      default:
        return sd::Result::NoResponse;
    }
  }

  sd::Result read_data(sd::Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, sd::Response& out) noexcept override {
    if (!sd::known(cmd) || !sd::reads_data(cmd)) {
      return sd::Result::Refused;
    }
    log.push_back({cmd, arg});
    out = sd::Response{};
    if (!spec.present || !spec.respond || state_ != State::Tran) {
      return sd::Result::NoResponse;
    }
    out.word[0] = r1();
    if (cmd == sd::Cmd::SwitchFunc) {
      std::uint8_t* s = static_cast<std::uint8_t*>(dst);
      std::memset(s, 0, block_size);
      if (spec.hs_supported) {
        s[13] |= 0x02;  // function 1 of group 1 is supported
      }
      const bool switching = (arg & 0x80000000u) != 0u;
      unsigned selected = hs_active ? 1u : 0u;
      if (switching) {
        selected = (spec.hs_supported && !spec.hs_switch_fails) ? 1u : 0xFu;
        hs_active = selected == 1u;
      }
      s[16] = static_cast<std::uint8_t>(selected);
      return sd::Result::Ok;
    }
    if (block_size != 512u || blocks == 0u) {
      return sd::Result::BadArgument;
    }
    if (spec.data_timeouts != 0u) {
      --spec.data_timeouts;
      return sd::Result::DataTimeout;
    }
    if (spec.data_errors != 0u) {
      --spec.data_errors;
      return sd::Result::DataError;
    }
    last_read_arg = arg;
    const std::uint32_t lba = byte_addressed_ ? arg / 512u : arg;
    if (byte_addressed_ && arg % 512u != 0u) {
      return sd::Result::CardError;
    }
    for (std::uint32_t i = 0; i < blocks; ++i) {
      medium_.read_sector(lba + i, static_cast<std::uint8_t*>(dst) + 512u * i);
    }
    blocks_served += blocks;
    (cmd == sd::Cmd::ReadSingleBlock ? reads_single : reads_multi) += 1u;
    return sd::Result::Ok;
  }

  sd::Result write_data(sd::Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, sd::Response& out) noexcept override {
    if (!sd::known(cmd) || !sd::writes_data(cmd)) {
      return sd::Result::Refused;
    }
    log.push_back({cmd, arg});
    out = sd::Response{};
    if (!spec.present || !spec.respond || state_ != State::Tran) {
      return sd::Result::NoResponse;
    }
    if (block_size != 512u || blocks != 1u) {
      return sd::Result::BadArgument;
    }
    out.word[0] = r1();
    if (spec.csd_wp) {
      out.word[0] |= 1u << 26;  // WP_VIOLATION: the card did not take the block
      return sd::Result::Ok;
    }
    if (spec.write_timeouts != 0u) {
      --spec.write_timeouts;
      return sd::Result::DataTimeout;
    }
    if (spec.write_crc_errors != 0u) {
      --spec.write_crc_errors;
      return sd::Result::DataError;
    }
    if (byte_addressed_ && arg % 512u != 0u) {
      return sd::Result::CardError;
    }
    const std::uint32_t lba = byte_addressed_ ? arg / 512u : arg;
    last_write_arg = arg;
    if (lba >= medium_.block_count()) {
      return sd::Result::CardError;
    }
    std::memcpy(medium_.sector(lba), src, 512);
    medium_.blocks_written += 1;
    ++writes;
    busy_left_ = spec.write_busy_polls;
    programming_ = true;
    stuck_ = spec.write_stuck_busy;
    return sd::Result::Ok;
  }

  bool logged(sd::Cmd c) const {
    for (const Seen& s : log) {
      if (s.cmd == c) {
        return true;
      }
    }
    return false;
  }
  unsigned count(sd::Cmd c) const {
    unsigned n = 0;
    for (const Seen& s : log) {
      n += s.cmd == c ? 1u : 0u;
    }
    return n;
  }

  static constexpr std::uint16_t kRca = 0x4567;

 private:
  enum class State { Idle, Ready, Ident, Stby, Tran };

  std::uint32_t r1() const { return (state_ == State::Tran ? (4u << 9) : (state_ == State::Stby ? (3u << 9) : 0u)) | sd::kR1ReadyForData; }

  void make_cid(std::uint8_t reg[16]) const {
    std::memset(reg, 0, 16);
    set_bits(reg, 127, 120, spec.manufacturer);
    set_bits(reg, 119, 112, static_cast<std::uint8_t>(spec.oem[0]));
    set_bits(reg, 111, 104, static_cast<std::uint8_t>(spec.oem[1]));
    for (unsigned i = 0; i < 5; ++i) {
      set_bits(reg, 103u - 8u * i, 96u - 8u * i, static_cast<std::uint8_t>(spec.product[i]));
    }
    set_bits(reg, 63, 56, spec.revision);
    set_bits(reg, 55, 24, spec.serial);
    set_bits(reg, 19, 12, spec.year - 2000u);
    set_bits(reg, 11, 8, spec.month);
  }

  void make_csd(std::uint8_t reg[16]) const {
    std::memset(reg, 0, 16);
    const std::uint32_t blocks = medium_.block_count();
    set_bits(reg, 95, 84, spec.switch_class ? 0x5B5u : 0x1B5u);
    set_bits(reg, 83, 80, 9);  // READ_BL_LEN: 512
    set_bits(reg, 13, 13, spec.csd_wp ? 1u : 0u);  // PERM_WRITE_PROTECT
    if (spec.csd_v1) {
      set_bits(reg, 127, 126, 0);
      set_bits(reg, 49, 47, 7);                      // C_SIZE_MULT 7: 2^9 blocks per C_SIZE step
      set_bits(reg, 73, 62, blocks / 512u - 1u);     // (C_SIZE + 1) << 9
    } else {
      set_bits(reg, 127, 126, 1);
      set_bits(reg, 69, 48, blocks / 1024u - 1u);    // (C_SIZE + 1) * 512 KiB
    }
  }

  // The four response registers of a 136-bit answer: spec bits 127:8, in the layout the controller hands them over.
  static void words(const std::uint8_t reg[16], sd::Response& out) {
    out.word[3] = (static_cast<std::uint32_t>(reg[0]) << 16) | (static_cast<std::uint32_t>(reg[1]) << 8) | reg[2];
    for (unsigned w = 0; w < 3; ++w) {
      const std::uint8_t* p = reg + 3 + 4 * w;
      out.word[2 - w] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
  }

  Medium& medium_;
  State state_ = State::Idle;
  bool app_next_ = false;
  bool byte_addressed_ = false;
  unsigned polls_ = 0;
  unsigned busy_left_ = 0;
  bool programming_ = false;
  bool stuck_ = false;
  std::uint32_t now_us_ = 1000;
};

}  // namespace fakes
