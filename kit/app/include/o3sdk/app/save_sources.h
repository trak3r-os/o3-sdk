// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Two ready-made sources for Files::save (app/files.h, atomic_save.h's SaveSource): the bytes of a buffer, and a 16-bit PCM WAV over samples in memory. Both are track3r-os's
// (app/save_service.h BufferSaveSource, WavSaveSource), kept as they are but for the WAV's held check, which belongs to the tracker's sample folders and is not here.
// Files::save(folder, name, data, bytes) and Files::save_wav use them inside the files service; an app has them for a streamed save of its own (a source that wraps one, or adds a
// header). Main loop only.

#include <o3sdk/app/atomic_save.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// The source of a small file already in memory (the kit's settings, an app's whole file).
class BufferSaveSource final : public SaveSource {
 public:
  void set(const std::uint8_t* data, std::size_t size) noexcept {
    data_ = data;
    size_ = size;
  }
  void begin(core::ByteSink& sink) noexcept override {
    sink_ = &sink;
    pos_ = 0;
  }
  formats::Progress step(std::size_t byte_budget) noexcept override;
  std::uint32_t size_hint() const noexcept override { return static_cast<std::uint32_t>(size_); }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  core::ByteSink* sink_ = nullptr;
};

// The source of a 16-bit PCM WAV: the 44-byte header, then the samples as they are in memory (little-endian). `check` (optional) says whether the audio is still the audio it was asked
// to write, asked before every slice: a false fails the save ("CHANGED WHILE SAVING").
class WavSaveSource final : public SaveSource {
 public:
  static constexpr std::size_t kHeaderBytes = 44;
  using Check = bool (*)(const void* ctx) noexcept;
  // false: nothing to write (no samples, 0 frames), more than 2 channels, or a size a WAV cannot hold.
  bool set(const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, Check check = nullptr, const void* ctx = nullptr) noexcept;
  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  std::uint32_t size_hint() const noexcept override { return static_cast<std::uint32_t>(kHeaderBytes + data_bytes_); }
  // The header for `frames` of `channels` at `rate`, into `out` (kHeaderBytes).
  static void make_header(std::uint8_t* out, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate) noexcept;

 private:
  const std::int16_t* pcm_ = nullptr;
  std::uint32_t frames_ = 0;
  std::uint32_t rate_ = 0;
  std::uint8_t channels_ = 1;
  std::uint64_t data_bytes_ = 0;
  std::uint64_t pos_ = 0;  // bytes of the file produced
  Check check_ = nullptr;
  const void* ctx_ = nullptr;
  const char* failure_ = "";
  core::ByteSink* sink_ = nullptr;
  std::uint8_t header_[kHeaderBytes] = {};
};

}  // namespace o3sdk::app
