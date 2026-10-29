// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The memory of the files service's jobs (app/files.h): the atomic save with its 2 KiB read-back buffer, the folder scan with its sorted index (9 KB), the settings file's buffer and the
// staging buffer of a streamed load, and the names and paths of the request in hand. About 16 KB. The platform places one where its memory is: the unit's image in the SDRAM
// (`O3SDK_BIG`, as track3r-os places the save layer's), the simulator on the heap. Platform::files_work hands it to the runtime; an image or a test rig without one has no files.
// The runtime is the only user; an app never touches it.

#include <o3sdk/app/atomic_save.h>
#include <o3sdk/app/dir_scan.h>
#include <o3sdk/app/files.h>
#include <o3sdk/app/save_sources.h>
#include <o3sdk/app/settings.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// The writer the files service gives its jobs: the platform's writer behind one more rule. hal/app_guard.h is a superset of track3r-os's guard (it falls through to save::classify, so
// the writer's own guard admits Projects/, Settings/, instruments/, AppExport/ as well: the ported tests walk those rows), and the service narrows it here: every change it makes (a
// folder, a temporary file, a rename, a marker, a removal) is refused with BadPath unless the path is `Apps`, `Apps/<id>` or below it, or `Samples` or below it. Nothing reaches the card
// from the service otherwise, whatever a bug above builds. Reads, the gate and the card's identity are the writer's. Main loop only.
class ScopedWriter final : public hal::storage::StorageWriter {
 public:
  void bind(hal::storage::StorageWriter* inner, const char* app_id) noexcept {
    inner_ = inner;
    id_ = app_id;
  }
  // Is `path` one the service may change?
  bool in_scope(const char* path) const noexcept;
  unsigned refused() const noexcept { return refused_; }  // changes refused since bind (a test asserts it is 0)

  hal::storage::WriteStatus gate() noexcept override { return inner_->gate(); }
  hal::storage::WriteStatus make_dir(const char* path) noexcept override { return allow(path) ? inner_->make_dir(path) : hal::storage::WriteStatus::BadPath; }
  hal::storage::WriteStatus create(const char* path) noexcept override { return allow(path) ? inner_->create(path) : hal::storage::WriteStatus::BadPath; }
  hal::storage::WriteStatus write(const void* src, std::size_t n) noexcept override { return inner_->write(src, n); }
  hal::storage::WriteStatus finish() noexcept override { return inner_->finish(); }
  void abandon() noexcept override { inner_->abandon(); }
  bool writing() const noexcept override { return inner_->writing(); }
  hal::storage::WriteStatus rename(const char* from, const char* to) noexcept override {
    return allow(from) && allow(to) ? inner_->rename(from, to) : hal::storage::WriteStatus::BadPath;
  }
  hal::storage::WriteStatus create_marker(const char* path) noexcept override { return allow(path) ? inner_->create_marker(path) : hal::storage::WriteStatus::BadPath; }
  hal::storage::WriteStatus remove(const char* path) noexcept override { return allow(path) ? inner_->remove(path) : hal::storage::WriteStatus::BadPath; }
  void begin_save() noexcept override { inner_->begin_save(); }
  void end_save() noexcept override { inner_->end_save(); }
  std::uint32_t card_id() const noexcept override { return inner_->card_id(); }

 private:
  bool allow(const char* path) noexcept {
    const bool ok = in_scope(path);
    refused_ += ok ? 0u : 1u;
    return ok;
  }
  hal::storage::StorageWriter* inner_ = nullptr;
  const char* id_ = "";
  unsigned refused_ = 0;
};

class Files::Work {
 public:
  static constexpr std::size_t kStageBytes = 2048;  // a streamed load reads this much, then offers it to the sink
  static constexpr std::size_t kFolderBytes = 40;
  static constexpr std::size_t kNameBytes = 48;
  static constexpr std::size_t kStemBytes = 32;

  ScopedWriter scope;  // the writer every job writes through (bound by Files::bind)
  AtomicSave save;
  DirScan scan;
  BufferSaveSource buffer_source;
  WavSaveSource wav_source;
  // The settings file as written and as read (a CRC-checked chunk file, settings.h); the staging of a streamed load. Word-aligned: they are plain bytes for memcpy and the card's
  // DMA, but a declared alignment costs nothing and a static_assert keeps it declared.
  struct Buffers {
    alignas(4) std::uint8_t settings[settings::kMaxBytes];
    alignas(4) std::uint8_t stage[kStageBytes];
  };
  Buffers buf = {};
  // The request in hand: copies of the names the app gave (they need not outlive the call), and the paths built from them.
  char folder[kFolderBytes] = {};
  char name[kNameBytes] = {};
  char stem[kStemBytes] = {};
  char dir[hal::storage::save::kPathCap] = {};   // "Apps/<id>[/<folder>]" or "Samples"
  char path[hal::storage::save::kPathCap + 8] = {};
  char backup[hal::storage::save::kPathCap + 8] = {};
};

static_assert(alignof(Files::Work::Buffers) == 4 && offsetof(Files::Work::Buffers, settings) % 4 == 0 && offsetof(Files::Work::Buffers, stage) % 4 == 0 &&
                  sizeof(Files::Work::Buffers) % 4 == 0,
              "the settings and staging buffers are word aligned");
static_assert(alignof(Files::Work) >= 4, "the work memory is word aligned");

}  // namespace o3sdk::app
