// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tiny text helpers of the system layer (private to kit/app/src): no <cstdio>, no heap, no locale. append_dec, append_uint, put_hex2
// and append_clipped are track3r-os's app/text_format.h as they are (renamed namespace); upper_copy does what track3r-os's export_actions.cpp
// upper_copy does, for ASCII, without <cctype>.

#include <o3sdk/core/fixed_string.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// Exactly `digits` decimal digits, zero padded; a larger value keeps its low digits.
inline char* put_dec(char* dst, unsigned value, unsigned digits) noexcept {
  for (unsigned i = digits; i > 0; --i) {
    dst[i - 1] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  }
  return dst + digits;
}

// Two upper-case hexadecimal digits.
inline char* put_hex2(char* dst, unsigned value) noexcept {
  static const char kDigits[] = "0123456789ABCDEF";
  dst[0] = kDigits[(value >> 4) & 0xFu];
  dst[1] = kDigits[value & 0xFu];
  return dst + 2;
}

// Appends `digits` (at most 10) zero-padded decimal digits to `s`; false if the text was cut.
template <std::size_t N>
bool append_dec(core::FixedString<N>& s, unsigned value, unsigned digits) noexcept {
  char tmp[10];
  if (digits > sizeof(tmp)) {
    digits = sizeof(tmp);
  }
  put_dec(tmp, value, digits);
  return s.append(tmp, digits);
}

// Appends the decimal digits of `value`, as many as it has (no padding); false if the text was cut.
template <std::size_t N>
bool append_uint(core::FixedString<N>& s, unsigned value) noexcept {
  unsigned digits = 1;
  for (unsigned v = value; v >= 10u; v /= 10u) {
    ++digits;
  }
  return append_dec(s, value, digits);
}

// The upper-case copy of `in` into `out` (cap bytes with the terminator, cut if it does not fit).
inline void upper_copy(char* out, std::size_t cap, const char* in) noexcept {
  std::size_t n = 0;
  for (; in[n] != '\0' && n + 1u < cap; ++n) {
    const char c = in[n];
    out[n] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  out[n] = '\0';
}

// Appends at most `max` characters of the NUL-terminated `text` (never past its end).
template <std::size_t N>
bool append_clipped(core::FixedString<N>& s, const char* text, std::size_t max) noexcept {
  std::size_t n = 0;
  while (n < max && text[n] != '\0') {
    ++n;
  }
  return s.append(text, n);
}

}  // namespace o3sdk::app
