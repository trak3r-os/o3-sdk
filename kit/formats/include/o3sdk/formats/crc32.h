// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3sdk::formats {

// CRC-32/ISO-HDLC, the one of zlib, PNG and Ethernet (reflected polynomial 0xEDB88320, initial value and final
// xor 0xFFFFFFFF), so Python's zlib.crc32 reproduces it. check("123456789") == 0xCBF43926. Table-driven by
// nibble: 64 bytes of constants, two lookups per byte.
class Crc32 {
 public:
  void reset() noexcept { state_ = 0xFFFFFFFFu; }
  void update(const std::uint8_t* data, std::size_t n) noexcept;
  std::uint32_t value() const noexcept { return ~state_; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept;

}  // namespace o3sdk::formats
