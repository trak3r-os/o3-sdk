// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/formats/crc32.h>

namespace o3sdk::formats {

namespace {

struct Table {
  std::uint32_t entry[16];
};

constexpr Table make_table() noexcept {
  Table t{};
  for (std::uint32_t i = 0; i < 16; ++i) {
    std::uint32_t c = i;
    for (int bit = 0; bit < 4; ++bit) {
      c = (c & 1u) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    t.entry[i] = c;
  }
  return t;
}

constexpr Table kTable = make_table();

}  // namespace

void Crc32::update(const std::uint8_t* data, std::size_t n) noexcept {
  std::uint32_t s = state_;
  for (std::size_t i = 0; i < n; ++i) {
    s ^= data[i];
    s = kTable.entry[s & 15u] ^ (s >> 4);
    s = kTable.entry[s & 15u] ^ (s >> 4);
  }
  state_ = s;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept {
  Crc32 c;
  c.update(data, n);
  return c.value();
}

}  // namespace o3sdk::formats
