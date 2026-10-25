// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CRC-32 (the zlib / IEEE 802.3 one: polynomial 0xEDB88320 reflected, initial 0xFFFFFFFF, final xor 0xFFFFFFFF; `crc32("123456789")` is 0xCBF43926),
// four bits at a time from a 16-entry table that the compiler builds, so the 64 bytes are the whole cost (OS-006c). Host tools compute the same with
// Python's zlib.crc32. Pure.

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::update {

namespace detail {
struct Crc32Table {
  std::uint32_t v[16];
};
constexpr Crc32Table make_crc32_table() noexcept {
  Crc32Table t{};
  for (std::uint32_t i = 0; i < 16u; ++i) {
    std::uint32_t c = i;
    for (unsigned k = 0; k < 4u; ++k) {
      c = (c & 1u) != 0u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    t.v[i] = c;
  }
  return t;
}
inline constexpr Crc32Table kCrc32Table = make_crc32_table();
}  // namespace detail

class Crc32 {
 public:
  void reset() noexcept { state_ = 0xFFFFFFFFu; }
  void add(std::uint8_t b) noexcept {
    state_ = detail::kCrc32Table.v[(state_ ^ b) & 0xFu] ^ (state_ >> 4);
    state_ = detail::kCrc32Table.v[(state_ ^ (static_cast<std::uint32_t>(b) >> 4)) & 0xFu] ^ (state_ >> 4);
  }
  void add(const std::uint8_t* data, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      add(data[i]);
    }
  }
  std::uint32_t value() const noexcept { return ~state_; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

}  // namespace o3sdk::platform::mk66::update
