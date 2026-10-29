// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A host card with a write-protect switch (docs/M2C-PLAN.md section 3, the simulator's `card protect on|off`): any HostCard (DirCard, FatFsStorage) seen through hal::storage::Storage and
// StorageWriter, passed on call for call, except that while the switch is on the gate answers WriteProtected and every call that changes the card is refused with it, as the unit's
// writer does for a card with its switch (platform/mk66/save: device_.write_protected()). Reads, the slot and the counters are the inner card's. DirCard is a ported file and has no
// such switch of its own; this wrapper is o3-sdk's, so that file stays as track3r-os has it.
//
// Host only (platform/host); main thread only. The inner card must outlive this object.

#include <o3sdk/platform/host/host_card.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::host {

class ProtectCard final : public HostCard {
 public:
  explicit ProtectCard(HostCard& inner) noexcept : inner_(inner) {}

  void set_protect(bool on) noexcept { protect_ = on; }
  bool protect() const noexcept { return protect_; }
  HostCard& inner() noexcept { return inner_; }

  // ---- HostCard
  void set_present(bool present) override { inner_.set_present(present); }
  unsigned violations() const override { return inner_.violations(); }

  // ---- hal::storage::Storage
  hal::storage::Status mount() noexcept override { return inner_.mount(); }
  bool mounted() const noexcept override { return inner_.mounted(); }
  bool card_present() const noexcept override { return inner_.card_present(); }
  hal::storage::VolumeInfo volume() const noexcept override { return inner_.volume(); }
  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override { return inner_.exists(path, is_dir); }
  hal::storage::Status stat(const char* path, hal::storage::FileStamp& out) noexcept override { return inner_.stat(path, out); }
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override { return inner_.open(path, out); }
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override { return inner_.read(file, dst, max, got); }
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override { return inner_.seek(file, offset); }
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override { return inner_.size(file, out); }
  void close(hal::storage::FileHandle& file) noexcept override { inner_.close(file); }
  hal::storage::Status opendir(const char* path, hal::storage::DirHandle& out) noexcept override { return inner_.opendir(path, out); }
  hal::storage::Status readdir(hal::storage::DirHandle dir, hal::storage::DirEntry& out) noexcept override { return inner_.readdir(dir, out); }
  void closedir(hal::storage::DirHandle& dir) noexcept override { inner_.closedir(dir); }

  // ---- hal::storage::StorageWriter
  hal::storage::WriteStatus gate() noexcept override {
    const hal::storage::WriteStatus g = inner_.gate();
    return g == hal::storage::WriteStatus::Ok && protect_ ? hal::storage::WriteStatus::WriteProtected : g;
  }
  hal::storage::WriteStatus make_dir(const char* path) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.make_dir(path); }
  hal::storage::WriteStatus create(const char* path) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.create(path); }
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.write(src, n); }
  hal::storage::WriteStatus finish() noexcept override { return inner_.finish(); }  // (closes the open file: it must always be possible)
  void abandon() noexcept override { inner_.abandon(); }
  bool writing() const noexcept override { return inner_.writing(); }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.rename(from, to); }
  hal::storage::WriteStatus create_marker(const char* path) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.create_marker(path); }
  hal::storage::WriteStatus remove(const char* path) noexcept override { return protect_ ? hal::storage::WriteStatus::WriteProtected : inner_.remove(path); }
  void begin_save() noexcept override { inner_.begin_save(); }
  void end_save() noexcept override { inner_.end_save(); }
  std::uint32_t card_id() const noexcept override { return inner_.card_id(); }

 private:
  HostCard& inner_;
  bool protect_ = false;
};

}  // namespace o3sdk::platform::host
