// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What survives the reset: four words the app keeps in .noinit RAM (the platform's crash record lives in the same section, first: platform/mk66 sections.h), so that the
// boot after a fault can say which fault was armed. The object itself is the app's (crashtest_app.cpp: no initialiser and no constructor, either would run at boot and erase
// it); these are the functions over it. Single writer: the main loop, before the fault and once at the next boot. A magic and a check word make a RAM that powered up with
// noise "no marker".

#include <cstdint>

namespace crashtest {

struct Marker {
  volatile std::uint32_t magic;
  volatile std::uint32_t kind;  // 0: nothing was armed, else Kind + 1
  volatile std::uint32_t seq;   // faults provoked since the RAM last powered up
  volatile std::uint32_t check;
};
static_assert(sizeof(Marker) == 16 && alignof(Marker) == 4, "four words, word aligned by their type");

inline constexpr std::uint32_t kMarkerMagic = 0x4354464Bu;  // "CTFK"

constexpr std::uint32_t marker_check(std::uint32_t kind, std::uint32_t seq) noexcept { return kMarkerMagic ^ (kind * 0x9E3779B1u) ^ (seq * 0x85EBCA6Bu); }

// The kind and the count the marker holds; false (and `kind`, `seq` untouched) when it is not valid.
inline bool marker_read(const Marker& m, std::uint32_t& kind, std::uint32_t& seq) noexcept {
  const std::uint32_t k = m.kind;
  const std::uint32_t s = m.seq;
  if (m.magic != kMarkerMagic || m.check != marker_check(k, s)) {
    return false;
  }
  kind = k;
  seq = s;
  return true;
}

// The magic goes last: a write cut short (a fault in the middle of it is not possible, a reset is) leaves a marker that does not validate or the previous one.
inline void marker_write(Marker& m, std::uint32_t kind, std::uint32_t seq) noexcept {
  m.magic = 0;
  m.kind = kind;
  m.seq = seq;
  m.check = marker_check(kind, seq);
  m.magic = kMarkerMagic;
}

}  // namespace crashtest
