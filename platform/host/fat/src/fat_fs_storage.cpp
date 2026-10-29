// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/fat_fs_storage.h>

#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fat_image.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace o3sdk::platform::host {

namespace st = hal::storage;
using mk66::storage::BlockResult;

// ---- ImageDevice -----------------------------------------------------------------------------------------------------------------------

ImageDevice::~ImageDevice() { unmap(); }

void ImageDevice::unmap() {
  if (mapped_ && base_ != nullptr) {
    ::munmap(base_, bytes_);
  }
  mapped_ = false;
  base_ = nullptr;
  bytes_ = 0;
  blocks_ = 0;
  mem_.clear();
}

bool ImageDevice::open(const std::string& path, std::string& error) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    error = "cannot open " + path;
    return false;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    error = path + " is not a regular file";
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(info.st_size);
  if (size == 0u || size % 512u != 0u || size > FatImage::kMaxImageBytes) {
    ::close(fd);
    error = path + " is not an image of 512-byte sectors (size " + std::to_string(size) + " bytes; at most " + std::to_string(FatImage::kMaxImageBytes >> 20) + " MiB)";
    return false;
  }
  // PRIVATE: a write of the run goes to the process's own copy of the page; the file is never changed.
  void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) {
    error = "cannot map " + path;
    return false;
  }
  // A FAT volume's first sector: the 0x55AA mark, 512 bytes a sector and a cluster of 1..128 sectors (a partitioned disk image or a wrong file fails here, by name).
  const std::uint8_t* boot = static_cast<const std::uint8_t*>(map);
  const unsigned per_cluster = boot[13];
  const bool is_fat = boot[510] == 0x55u && boot[511] == 0xAAu && (boot[11] | (boot[12] << 8)) == 512 && per_cluster != 0u && per_cluster <= 128u && (per_cluster & (per_cluster - 1u)) == 0u;
  if (!is_fat) {
    ::munmap(map, size);
    error = path + " is not a FAT image (no FAT boot sector in its first sector; a whole-disk image with a partition table is not supported, give the partition)";
    return false;
  }
  unmap();
  base_ = static_cast<std::uint8_t*>(map);
  bytes_ = size;
  mapped_ = true;
  blocks_ = static_cast<std::uint32_t>(size / 512u);
  started_ = false;
  return true;
}

void ImageDevice::adopt(std::vector<std::uint8_t> image) {
  unmap();
  mem_ = std::move(image);
  base_ = mem_.empty() ? nullptr : mem_.data();
  bytes_ = mem_.size();
  blocks_ = static_cast<std::uint32_t>(bytes_ / 512u);
  started_ = false;
}

BlockResult ImageDevice::start() noexcept {
  started_ = present();
  return started_ ? BlockResult::Ok : BlockResult::NoMedium;
}

BlockResult ImageDevice::read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept {
  if (!ready()) {
    return BlockResult::NotReady;
  }
  if (lba >= blocks_ || count > blocks_ - lba) {
    return BlockResult::OutOfRange;
  }
  std::memcpy(dst, base_ + static_cast<std::size_t>(lba) * 512u, static_cast<std::size_t>(count) * 512u);
  return BlockResult::Ok;
}

BlockResult ImageDevice::write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept {
  if (!ready()) {
    return BlockResult::NotReady;
  }
  if (lba >= blocks_ || count > blocks_ - lba) {
    return BlockResult::OutOfRange;
  }
  std::memcpy(base_ + static_cast<std::size_t>(lba) * 512u, src, static_cast<std::size_t>(count) * 512u);
  return BlockResult::Ok;
}

// ---- FatFsStorage ----------------------------------------------------------------------------------------------------------------------

FatFsStorage::~FatFsStorage() = default;

bool FatFsStorage::open_image(const std::string& image, std::string& error) { return device_.open(image, error); }

st::Status FatFsStorage::mount() noexcept {
  if (!present_) {
    return st::Status::NoCard;
  }
  return fat_.mount();
}

void FatFsStorage::set_present(bool present) {
  present_ = present;
  device_.set_present(present);
  if (!present) {
    fat_.release();  // app_main.cpp's poll_card: a card taken out is released at once (every handle closed, the volume forgotten)
  }
}

bool FatFsStorage::export_tree(const std::string& card_dir, const std::string& host_dir, std::string& error) {
  namespace fs = std::filesystem;
  std::vector<st::DirEntry> entries;
  st::DirHandle dir;
  if (fat_.opendir(card_dir.c_str(), dir) != st::Status::Ok) {
    error = "cannot list '" + card_dir + "' on the card";
    return false;
  }
  st::DirEntry e{};
  while (fat_.readdir(dir, e) == st::Status::Ok) {
    entries.push_back(e);
  }
  fat_.closedir(dir);  // (two directory handles at most: the listing is whole before the tree below it is walked)
  for (const st::DirEntry& entry : entries) {
    const std::string card_path = card_dir.empty() ? std::string(entry.name) : card_dir + "/" + entry.name;
    const fs::path host_path = fs::path(host_dir) / entry.name;
    if (entry.is_dir) {
      std::error_code ec;
      fs::create_directories(host_path, ec);
      if (ec || !export_tree(card_path, host_path.string(), error)) {
        if (error.empty()) {
          error = "cannot make " + host_path.string();
        }
        return false;
      }
      continue;
    }
    st::FileHandle file;
    if (fat_.open(card_path.c_str(), file) != st::Status::Ok) {
      error = "cannot read '" + card_path + "' on the card";
      return false;
    }
    std::ofstream out(host_path, std::ios::binary);
    char buf[4096];
    std::size_t got = 0;
    while (fat_.read(file, buf, sizeof buf, got) == st::Status::Ok && got != 0u) {
      out.write(buf, static_cast<std::streamsize>(got));
    }
    fat_.close(file);
    if (!out) {
      error = "cannot write " + host_path.string();
      return false;
    }
  }
  return true;
}

bool FatFsStorage::computer_copy(const std::string& from, const std::string& to, std::string& error) {
  namespace fs = std::filesystem;
  error.clear();
  if (present_) {
    error = "the card is in the slot (a computer copies a pulled card)";
    return false;
  }
  ScratchDir scratch("fatcopy");
  if (!scratch.ok()) {
    error = "no scratch directory could be made";
    return false;
  }
  device_.set_present(true);  // the card is the computer's now: read it whole, copy, write it again, and give the slot back empty
  bool good = fat_.mount() == st::Status::Ok;
  if (!good) {
    error = "the card did not mount on the computer";
  } else {
    good = export_tree("", scratch.path(), error);
  }
  fat_.release();
  device_.set_present(false);
  if (!good) {
    return false;
  }
  std::error_code ec;
  const fs::path root(scratch.path());
  if (!fs::is_directory(root / from, ec) || fs::exists(root / to, ec) || !fs::is_directory((root / to).parent_path(), ec)) {
    error = "copy: '" + from + "' is not a folder of the card, or '" + to + "' exists already, or its parent folder is not there";
    return false;
  }
  fs::copy(root / from, root / to, fs::copy_options::recursive, ec);
  if (ec) {
    error = "copy: " + ec.message();
    return false;
  }
  FatImage image(device_.block_count());
  if (!image.add_tree(scratch.path())) {
    error = "copy: " + image.error();
    return false;
  }
  device_.adopt(image.take());
  return true;
}

}  // namespace o3sdk::platform::host
