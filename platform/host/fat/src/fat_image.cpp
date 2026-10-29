// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/fat_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fcntl.h>
#include <unistd.h>

namespace o3sdk::platform::host {
namespace {

namespace fs = std::filesystem;

void put16(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

constexpr std::uint32_t kStampDate = ((2024u - 1980u) << 9) | (11u << 5) | 1u;  // 2024-11-01, FF_NORTC_*; the time is 00:00:00

// UTF-8 to UTF-16 (a character past the BMP is a surrogate pair); a byte sequence that is not UTF-8 is taken one byte a character (the host's names are what they are).
std::vector<std::uint16_t> utf16(const std::string& s) {
  std::vector<std::uint16_t> out;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const std::size_t n = c < 0x80u ? 1u : ((c & 0xE0u) == 0xC0u ? 2u : ((c & 0xF0u) == 0xE0u ? 3u : ((c & 0xF8u) == 0xF0u ? 4u : 1u)));
    if (i + n > s.size() || n == 1u) {
      out.push_back(c);
      ++i;
      continue;
    }
    std::uint32_t cp = c & (0xFFu >> (n + 1u));
    for (std::size_t k = 1; k < n; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    if (cp >= 0x10000u) {
      cp -= 0x10000u;
      out.push_back(static_cast<std::uint16_t>(0xD800u + (cp >> 10)));
      out.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
    } else {
      out.push_back(static_cast<std::uint16_t>(cp));
    }
    i += n;
  }
  return out;
}

bool is_plain_83(const std::string& name, std::uint8_t sfn[11]) {
  std::memset(sfn, ' ', 11);
  const std::size_t dot = name.rfind('.');
  const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
  if (base.empty() || base.size() > 8 || ext.size() > 3) {
    return false;
  }
  const auto good = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; };
  for (std::size_t i = 0; i < base.size(); ++i) {
    if (!good(base[i])) {
      return false;
    }
    sfn[i] = static_cast<std::uint8_t>(base[i]);
  }
  for (std::size_t i = 0; i < ext.size(); ++i) {
    if (!good(ext[i])) {
      return false;
    }
    sfn[8 + i] = static_cast<std::uint8_t>(ext[i]);
  }
  return true;
}

std::uint8_t checksum(const std::uint8_t sfn[11]) {
  std::uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) {
    sum = static_cast<std::uint8_t>(((sum & 1u) << 7) + (sum >> 1) + sfn[i]);
  }
  return sum;
}

}  // namespace

FatImage::FatImage(std::uint32_t blocks) : blocks_(blocks) {
  if (blocks < kMinBlocks || static_cast<std::size_t>(blocks) * 512u > kMaxImageBytes) {
    fail("a FAT32 image of one-sector clusters is " + std::to_string(kMinBlocks * 512u / (1024u * 1024u) + 1u) + " MiB to " + std::to_string(kMaxImageBytes >> 20) + " MiB");
    return;
  }
  // The clusters and the two FATs that map them: a fixpoint (the FATs take sectors the clusters would have had).
  fat_sectors_ = (blocks * 4u + 511u) / 512u;
  for (int i = 0; i < 4; ++i) {
    clusters_ = blocks - kReserved - 2u * fat_sectors_;
    fat_sectors_ = ((clusters_ + 2u) * 4u + 511u) / 512u;
  }
  clusters_ = blocks - kReserved - 2u * fat_sectors_;
  data_start_ = kReserved + 2u * fat_sectors_;
  image_.assign(static_cast<std::size_t>(blocks) * 512u, 0);
  std::uint8_t* b = sector(0);
  b[0] = 0xEB;
  b[1] = 0x58;
  b[2] = 0x90;
  std::memcpy(b + 3, "MSWIN4.1", 8);
  put16(b + 11, 512);
  b[13] = 1;  // sectors per cluster
  put16(b + 14, kReserved);
  b[16] = 2;  // FATs
  b[21] = 0xF8;
  put16(b + 24, 63);
  put16(b + 26, 255);
  put32(b + 32, blocks);
  put32(b + 36, fat_sectors_);
  put32(b + 44, kRootCluster);
  put16(b + 48, 1);  // FSInfo
  put16(b + 50, 6);  // backup boot sector
  b[64] = 0x80;
  b[66] = 0x29;
  put32(b + 67, 0x1234ABCDu);
  std::memcpy(b + 71, "O3HOSTCARD ", 11);
  std::memcpy(b + 82, "FAT32   ", 8);
  b[510] = 0x55;
  b[511] = 0xAA;
  std::memcpy(sector(6), b, 512);
  std::uint8_t* fsi = sector(1);
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

bool FatImage::fail(const std::string& what) {
  if (error_.empty()) {
    error_ = what;
  }
  return false;
}

void FatImage::set_fat(std::uint32_t cluster, std::uint32_t value) {
  const std::uint32_t at = cluster * 4u;
  for (std::uint32_t copy = 0; copy < 2; ++copy) {
    put32(sector(kReserved + copy * fat_sectors_ + at / 512u) + at % 512u, value);
  }
}

std::uint32_t FatImage::alloc() {
  if (next_free_ >= clusters_ + kRootCluster) {
    fail("the image is full (" + std::to_string(static_cast<unsigned long long>(clusters_) * 512u >> 20) + " MiB of clusters)");
    return 0;
  }
  const std::uint32_t c = next_free_++;
  set_fat(c, 0x0FFFFFFFu);
  return c;
}

void FatImage::append_raw(std::uint32_t dir, const std::uint8_t entry[32]) {
  DirState& d = dirs_.at(dir);
  if (d.slot == 16u) {  // the cluster is full: chain a new one
    const std::uint32_t c = alloc();
    if (c == 0u) {
      return;
    }
    set_fat(d.last_cluster, c);
    d.last_cluster = c;
    d.slot = 0;
  }
  std::memcpy(sector(cluster_sector(d.last_cluster)) + 32u * d.slot, entry, 32);
  ++d.slot;
}

// "PATTER~1.MTP" for "pattern_01.mtp": the first six letters and digits upper-cased, ~N, the first three of the extension.
void FatImage::alias(const std::string& name, std::uint32_t dir, std::uint8_t sfn[11]) {
  std::string base;
  std::string ext;
  const std::size_t dot = name.rfind('.');
  const std::string b = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
  const auto fold = [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); };
  const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  for (const char c : b) {
    if (base.size() < 6 && alnum(c)) {
      base += fold(c);
    }
  }
  for (const char c : e) {
    if (ext.size() < 3 && alnum(c)) {
      ext += fold(c);
    }
  }
  if (base.empty()) {
    base = "X";
  }
  const unsigned n = ++alias_count_[std::to_string(dir) + "/" + base + "." + ext];
  const std::string tail = "~" + std::to_string(n);
  base = base.substr(0, 8 - std::min<std::size_t>(tail.size(), 7u)) + tail;
  std::memset(sfn, ' ', 11);
  std::memcpy(sfn, base.data(), std::min<std::size_t>(base.size(), 8u));
  std::memcpy(sfn + 8, ext.data(), ext.size());
}

void FatImage::add_entry(std::uint32_t dir, const std::string& name, std::uint8_t attr, std::uint32_t first_cluster, std::uint32_t size) {
  std::uint8_t sfn[11];
  if (!is_plain_83(name, sfn)) {
    alias(name, dir, sfn);
    std::vector<std::uint16_t> u = utf16(name);
    if (u.size() > 255u) {
      fail("a name of more than 255 characters: " + name.substr(0, 40));
      return;
    }
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
  put16(e + 16, kStampDate);  // created
  put16(e + 18, kStampDate);  // last access
  put16(e + 20, first_cluster >> 16);
  put16(e + 24, kStampDate);  // written (the time of both is 0)
  put16(e + 26, first_cluster);
  put32(e + 28, size);
  append_raw(dir, e);
}

std::uint32_t FatImage::make_dir(std::uint32_t parent, const std::string& name) {
  if (!ok()) {
    return 0;
  }
  const std::uint32_t c = alloc();
  if (c == 0u) {
    return 0;
  }
  dirs_[c] = DirState{c, 0};
  add_entry(parent, name, 0x10, c, 0);
  std::uint8_t dot[32];
  std::memset(dot, ' ', 11);
  std::memset(dot + 11, 0, 21);
  dot[0] = '.';
  dot[11] = 0x10;
  put16(dot + 16, kStampDate);
  put16(dot + 18, kStampDate);
  put16(dot + 20, c >> 16);
  put16(dot + 24, kStampDate);
  put16(dot + 26, c);
  append_raw(c, dot);
  std::memset(dot, ' ', 11);
  dot[0] = dot[1] = '.';
  const std::uint32_t up = parent == kRootCluster ? 0u : parent;  // ".." of a first-level directory points at cluster 0
  put16(dot + 20, up >> 16);
  put16(dot + 26, up);
  append_raw(c, dot);
  return ok() ? c : 0u;
}

bool FatImage::add_file(std::uint32_t parent, const std::string& name, const std::uint8_t* data, std::size_t size) {
  if (!ok()) {
    return false;
  }
  std::uint32_t first = 0;
  std::uint32_t prev = 0;
  for (std::size_t off = 0; off < size; off += 512) {
    const std::uint32_t c = alloc();
    if (c == 0u) {
      return false;
    }
    if (first == 0u) {
      first = c;
    } else {
      set_fat(prev, c);
    }
    prev = c;
    std::memcpy(sector(cluster_sector(c)), data + off, std::min<std::size_t>(512u, size - off));
  }
  add_entry(parent, name, 0x20, first, static_cast<std::uint32_t>(size));
  return ok();
}

bool FatImage::add_tree_at(const std::string& dir, std::uint32_t cluster) {
  std::error_code ec;
  std::vector<std::pair<std::string, fs::file_status>> entries;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
    entries.emplace_back(e.path().filename().string(), e.symlink_status(ec));
  }
  if (ec) {
    return fail("cannot list " + dir + ": " + ec.message());
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [name, status] : entries) {
    const std::string path = (fs::path(dir) / name).string();
    if (fs::is_directory(status)) {
      const std::uint32_t sub = make_dir(cluster, name);
      if (sub == 0u || !add_tree_at(path, sub)) {
        return false;
      }
    } else if (fs::is_regular_file(status)) {
      std::ifstream in(path, std::ios::binary);
      if (!in.is_open()) {
        return fail("cannot read " + path);
      }
      const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const std::uintmax_t expect = fs::file_size(path, ec);
      if (ec || bytes.size() != expect) {
        return fail("cannot read " + path + " completely (" + std::to_string(bytes.size()) + " of " + std::to_string(expect) + " bytes)");
      }
      if (!add_file(cluster, name, bytes.data(), bytes.size())) {
        return false;
      }
    } else {
      return fail(path + " is a link or another kind of file (only regular files and folders go into an image)");
    }
  }
  return true;
}

bool FatImage::add_tree(const std::string& dir) {
  if (!ok()) {
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return fail(dir + " is not a directory");
  }
  return add_tree_at(dir, kRootCluster);
}

bool FatImage::write_file(const std::string& path, std::string& error) const {
  if (!ok()) {
    error = error_;
    return false;
  }
  const std::string tmp = path + ".tmp";  // written whole, then renamed: a failed run leaves the old image
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    error = "cannot create " + tmp;
    return false;
  }
  bool good = ::ftruncate(fd, static_cast<off_t>(image_.size())) == 0;  // a sparse file: the holes read as zeros
  static const std::uint8_t kZero[512] = {};
  for (std::size_t lba = 0; good && lba < blocks_; ++lba) {
    const std::uint8_t* s = image_.data() + lba * 512u;
    if (std::memcmp(s, kZero, 512) != 0) {
      good = ::pwrite(fd, s, 512, static_cast<off_t>(lba * 512u)) == 512;
    }
  }
  good = (::close(fd) == 0) && good;
  if (good && ::rename(tmp.c_str(), path.c_str()) != 0) {
    good = false;
  }
  if (!good) {
    ::unlink(tmp.c_str());
    error = "cannot write " + path;
  }
  return good;
}

}  // namespace o3sdk::platform::host
