// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/core/byte_io.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace o3sdk::test {

// A byte source over memory that can misbehave on request: short reads, WouldBlock, a failure at an offset,
// and the two ways a broken implementation breaks the contract.
class MemorySource final : public core::ByteSource {
 public:
  enum class Lie : std::uint8_t { None, TooMany, OkZero };

  MemorySource(const std::uint8_t* data, std::size_t n) : data_(data), size_(n) {}
  explicit MemorySource(const std::vector<std::uint8_t>& v) : data_(v.data()), size_(v.size()) {}

  std::size_t max_per_read = static_cast<std::size_t>(-1);
  std::uint32_t block_every = 0;  // every Nth call answers WouldBlock, 0 = never
  std::size_t fail_at = static_cast<std::size_t>(-1);  // reads at or past this offset answer Failed
  Lie lie = Lie::None;

  std::size_t position() const { return pos_; }

  core::IoResult read(void* dst, std::size_t max) noexcept override {
    ++calls_;
    if (block_every != 0 && calls_ % block_every == 0) {
      return {0, core::IoStatus::WouldBlock};
    }
    if (lie == Lie::TooMany) {
      return {max + 1, core::IoStatus::Ok};
    }
    if (lie == Lie::OkZero) {
      return {0, core::IoStatus::Ok};
    }
    if (pos_ >= fail_at) {
      return {0, core::IoStatus::Failed};
    }
    if (pos_ >= size_) {
      return {0, core::IoStatus::End};
    }
    std::size_t n = std::min({max, max_per_read, size_ - pos_});
    if (fail_at != static_cast<std::size_t>(-1)) {
      n = std::min(n, fail_at - pos_);
    }
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
    return {n, core::IoStatus::Ok};
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint32_t calls_ = 0;
};

// A byte sink into a vector, with the same repertoire of misbehaviour.
class VectorSink final : public core::ByteSink {
 public:
  enum class Lie : std::uint8_t { None, TooMany, OkZero, End };

  std::vector<std::uint8_t> bytes;
  std::size_t max_per_write = static_cast<std::size_t>(-1);
  std::uint32_t block_every = 0;
  std::size_t fail_after = static_cast<std::size_t>(-1);  // writes once this many bytes are in answer Failed
  Lie lie = Lie::None;

  core::IoResult write(const void* src, std::size_t n) noexcept override {
    ++calls_;
    if (block_every != 0 && calls_ % block_every == 0) {
      return {0, core::IoStatus::WouldBlock};
    }
    if (lie == Lie::TooMany) {
      return {n + 1, core::IoStatus::Ok};
    }
    if (lie == Lie::OkZero) {
      return {0, core::IoStatus::Ok};
    }
    if (lie == Lie::End) {
      return {0, core::IoStatus::End};
    }
    if (bytes.size() >= fail_after) {
      return {0, core::IoStatus::Failed};
    }
    std::size_t take = std::min(n, max_per_write);
    if (fail_after != static_cast<std::size_t>(-1)) {
      take = std::min(take, fail_after - bytes.size());
    }
    const auto* p = static_cast<const std::uint8_t*>(src);
    bytes.insert(bytes.end(), p, p + take);
    return {take, core::IoStatus::Ok};
  }

 private:
  std::uint32_t calls_ = 0;
};

}  // namespace o3sdk::test
