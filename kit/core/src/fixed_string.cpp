// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/core/fixed_string.h>

#include <cstring>

namespace o3sdk::core::detail {

bool string_append_n(char* buf, std::size_t cap, std::size_t& len, const char* src, std::size_t n) noexcept {
  const std::size_t wanted = (src != nullptr) ? n : 0;
  const std::size_t room = cap - len;
  const std::size_t take = (wanted < room) ? wanted : room;
  if (take != 0) {
    std::memcpy(buf + len, src, take);
  }
  len += take;
  buf[len] = '\0';
  return take == wanted;
}

bool string_append_z(char* buf, std::size_t cap, std::size_t& len, const char* src) noexcept {
  if (src == nullptr) {
    return true;
  }
  // Stops at the terminator, or one byte past the room left (then it cannot fit): at most room + 1 reads.
  const std::size_t room = cap - len;
  std::size_t n = 0;
  while (n <= room && src[n] != '\0') {
    ++n;
  }
  return string_append_n(buf, cap, len, src, n);
}

bool string_equals_z(const char* a, std::size_t a_len, const char* z) noexcept {
  for (std::size_t i = 0; i < a_len; ++i) {
    if (z[i] == '\0' || z[i] != a[i]) {
      return false;
    }
  }
  return z[a_len] == '\0';
}

}  // namespace o3sdk::core::detail
