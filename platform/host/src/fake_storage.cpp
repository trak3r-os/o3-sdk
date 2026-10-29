// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/host/fake_storage.h>

#include <cstring>

namespace o3sdk::platform::host {

using hal::storage::DirEntry;
using hal::storage::DirHandle;
using hal::storage::FileHandle;
using hal::storage::Status;

std::string FakeStorage::normalise(const std::string& path) {
  std::size_t b = 0;
  std::size_t e = path.size();
  while (b < e && path[b] == '/') {
    ++b;
  }
  while (e > b && path[e - 1] == '/') {
    --e;
  }
  return path.substr(b, e - b);
}

bool FakeStorage::equal_ci(const std::string& a, const std::string& b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const unsigned char x = static_cast<unsigned char>(a[i]);
    const unsigned char y = static_cast<unsigned char>(b[i]);
    const unsigned char fx = (x >= 'A' && x <= 'Z') ? static_cast<unsigned char>(x + 32) : x;
    const unsigned char fy = (y >= 'A' && y <= 'Z') ? static_cast<unsigned char>(y + 32) : y;
    if (fx != fy) {
      return false;
    }
  }
  return true;
}

std::string FakeStorage::parent_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string FakeStorage::leaf_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

int FakeStorage::find(const std::string& path) const {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (equal_ci(nodes_[i].path, path)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void FakeStorage::add_node(const std::string& path, bool is_dir, std::uint32_t size) {
  const std::string p = normalise(path);
  if (p.empty()) {
    return;
  }
  const std::string parent = parent_of(p);
  if (!parent.empty() && find(parent) < 0) {
    add_node(parent, true, 0);
  }
  if (find(p) < 0) {
    nodes_.push_back(Node{p, is_dir, size, {}, false});
  }
}

void FakeStorage::add_dir(const std::string& path) { add_node(path, true, 0); }
void FakeStorage::add_file(const std::string& path, std::uint32_t size) { add_node(path, false, size); }
void FakeStorage::add_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  add_node(path, false, static_cast<std::uint32_t>(bytes.size()));
  const int i = find(normalise(path));
  if (i >= 0 && !nodes_[static_cast<std::size_t>(i)].is_dir) {
    nodes_[static_cast<std::size_t>(i)].bytes = bytes;
    nodes_[static_cast<std::size_t>(i)].has_bytes = true;
  }
}

unsigned FakeStorage::open_dirs() const noexcept {
  unsigned n = 0;
  for (const Dir& d : dirs_) {
    n += d.open ? 1u : 0u;
  }
  return n;
}

Status FakeStorage::mount() noexcept {
  for (Dir& d : dirs_) {
    d = Dir{};
  }
  for (File& f : files_) {
    f = File{};
  }
  mounted_ = present_;
  return present_ ? Status::Ok : Status::NoCard;
}

hal::storage::VolumeInfo FakeStorage::volume() const noexcept {
  hal::storage::VolumeInfo v;
  if (mounted()) {
    v.kind = hal::storage::FsKind::HostDirectory;
  }
  return v;
}

Status FakeStorage::exists(const char* path, bool* is_dir) noexcept {
  ++exists_calls_;
  if (!present_) {
    return Status::NoCard;
  }
  if (!mounted_) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  const std::string p = normalise(path);
  if (exists_fail_ != Status::Ok && !exists_fail_leaf_.empty() && equal_ci(leaf_of(p), exists_fail_leaf_)) {
    return exists_fail_;
  }
  if (p.empty()) {
    if (is_dir != nullptr) {
      *is_dir = true;
    }
    return Status::Ok;
  }
  const int i = find(p);
  if (i < 0) {
    // A file in the middle of the path is not a folder to look into.
    for (std::string up = parent_of(p); !up.empty(); up = parent_of(up)) {
      const int j = find(up);
      if (j >= 0 && !nodes_[static_cast<std::size_t>(j)].is_dir) {
        return Status::NotADirectory;
      }
    }
    return Status::NotFound;
  }
  if (is_dir != nullptr) {
    *is_dir = nodes_[static_cast<std::size_t>(i)].is_dir;
  }
  return Status::Ok;
}

Status FakeStorage::open(const char* path, FileHandle& out) noexcept {
  if (!present_) {
    return Status::NoCard;
  }
  if (!mounted_) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  const int i = find(normalise(path));
  if (i < 0) {
    return Status::NotFound;
  }
  const Node& n = nodes_[static_cast<std::size_t>(i)];
  if (n.is_dir) {
    return Status::NotAFile;
  }
  if (!n.has_bytes) {
    return Status::Unsupported;  // a listing-only file
  }
  for (unsigned slot = 0; slot < hal::storage::kMaxOpenFiles; ++slot) {
    if (!files_[slot].open) {
      files_[slot] = File{true, static_cast<std::size_t>(i), 0};
      out.slot = static_cast<std::uint8_t>(slot);
      return Status::Ok;
    }
  }
  return Status::NoHandle;
}

Status FakeStorage::read(FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept {
  if (!file.is_open() || file.slot >= hal::storage::kMaxOpenFiles || !files_[file.slot].open) {
    return Status::BadHandle;
  }
  File& f = files_[file.slot];
  const std::vector<std::uint8_t>& b = nodes_[f.node].bytes;
  const std::size_t left = f.pos < b.size() ? b.size() - f.pos : 0u;
  const std::size_t n = max < left ? max : left;
  if (n != 0u) {
    std::memcpy(dst, b.data() + f.pos, n);
  }
  f.pos += static_cast<std::uint32_t>(n);
  got = n;
  return Status::Ok;
}

Status FakeStorage::seek(FileHandle file, std::uint32_t offset) noexcept {
  if (!file.is_open() || file.slot >= hal::storage::kMaxOpenFiles || !files_[file.slot].open) {
    return Status::BadHandle;
  }
  File& f = files_[file.slot];
  const std::size_t total = nodes_[f.node].bytes.size();
  f.pos = offset < total ? offset : static_cast<std::uint32_t>(total);
  return Status::Ok;
}

Status FakeStorage::size(FileHandle file, std::uint32_t& out) const noexcept {
  if (!file.is_open() || file.slot >= hal::storage::kMaxOpenFiles || !files_[file.slot].open) {
    return Status::BadHandle;
  }
  out = static_cast<std::uint32_t>(nodes_[files_[file.slot].node].bytes.size());
  return Status::Ok;
}

void FakeStorage::close(FileHandle& file) noexcept {
  if (file.is_open() && file.slot < hal::storage::kMaxOpenFiles) {
    files_[file.slot].open = false;
  }
  file = FileHandle{};
}

Status FakeStorage::opendir(const char* path, DirHandle& out) noexcept {
  ++opendirs_;
  if (!present_) {
    return Status::NoCard;
  }
  if (!mounted_) {
    return Status::NotMounted;
  }
  if (!hal::storage::valid_path(path)) {
    return Status::BadPath;
  }
  const std::string p = normalise(path);
  if (!p.empty()) {
    const int i = find(p);
    if (i < 0) {
      return Status::NotFound;
    }
    if (!nodes_[static_cast<std::size_t>(i)].is_dir) {
      return Status::NotADirectory;
    }
  }
  for (unsigned slot = 0; slot < hal::storage::kMaxOpenDirs; ++slot) {
    if (!dirs_[slot].open) {
      Dir d;
      d.open = true;
      for (std::size_t n = 0; n < nodes_.size(); ++n) {
        if (equal_ci(parent_of(nodes_[n].path), p)) {
          d.children.push_back(n);
        }
      }
      dirs_[slot] = d;
      out.slot = static_cast<std::uint8_t>(slot);
      return Status::Ok;
    }
  }
  return Status::NoHandle;
}

Status FakeStorage::readdir(DirHandle dir, DirEntry& out) noexcept {
  if (!dir.is_open() || dir.slot >= hal::storage::kMaxOpenDirs || !dirs_[dir.slot].open) {
    return Status::BadHandle;
  }
  if (!present_) {
    return Status::NoCard;
  }
  if (readdir_fail_ != Status::Ok) {
    if (readdir_fail_in_ == 0u) {
      const Status s = readdir_fail_;
      readdir_fail_ = Status::Ok;
      return s;
    }
    --readdir_fail_in_;
  }
  Dir& d = dirs_[dir.slot];
  ++readdirs_;
  if (d.next >= d.children.size()) {
    return Status::End;
  }
  const Node& n = nodes_[d.children[d.next++]];
  const std::string leaf = leaf_of(n.path);
  const std::size_t len = leaf.size() < hal::storage::kNameMax ? leaf.size() : hal::storage::kNameMax;
  std::memcpy(out.name, leaf.data(), len);
  out.name[len] = '\0';
  out.size = n.size;
  out.is_dir = n.is_dir;
  return Status::Ok;
}

void FakeStorage::closedir(DirHandle& dir) noexcept {
  if (dir.is_open() && dir.slot < hal::storage::kMaxOpenDirs) {
    dirs_[dir.slot] = Dir{};
  }
  dir = DirHandle{};
}

}  // namespace o3sdk::platform::host
