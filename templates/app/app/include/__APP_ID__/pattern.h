// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the template keeps on the SD card, and the bytes it is kept in: a 16-step pattern (the notes the pads recorded) and one setting
// (the keyboard octave). Pure (no card, no state of the app), so a host test covers the format (tests/pattern_test.cpp). The card is
// reached through Context::files() (o3-sdk kit/app/include/o3sdk/app/files.h: read its header comment first; docs/NEW-APP.md section 13 and
// docs/FRAMEWORK.md "Files on the card" say more): these functions only turn the data into a file's bytes and back, with the kit's chunk container
// (docs/formats/o3sdk-chunk-file.md: big-endian, a CRC-32 trailer), so a cut or a damaged file is refused whole and never half applied.
//
// Why a file of the app's own for the setting: the kit's settings file (system.o3s) holds only what the system Config screen shows and the
// app cannot add a key to it. An app's own values go in a file of its own, as here.
//
//   pattern.o3a   type "PATN", one chunk STEP: u16 version (0x0100), u8 step count (16), 16 notes (0: a rest, else 1..127)
//   config.o3a    type "CONF", one chunk OCTV: u16 version (0x0100), u8 octave + 3 (so 0..5 for the octaves -3..+2)
// A newer minor version (the low byte) is read as far as this one knows it; a newer major, a wrong count or a value out of range is refused.

#include <@APP_ID@/pad_map.h>

#include <o3sdk/formats/chunk_file.h>

#include <cstddef>
#include <cstdint>

namespace @APP_ID@ {

inline constexpr unsigned kPatternSteps = 16;
inline constexpr const char* kPatternFile = "pattern.o3a";  // in the app's folder on the card, Apps/<id>/ (the CMake id)
inline constexpr const char* kConfigFile = "config.o3a";
// The largest file each makes (54 and 38 bytes) with room to spare; the buffers that hold them are word aligned (Files::save reads them).
inline constexpr std::size_t kPatternFileMax = 96;
inline constexpr std::size_t kConfigFileMax = 64;

struct Pattern {
  std::uint8_t note[kPatternSteps] = {};  // 0: a rest
  unsigned cursor = 0;                    // the step the next recorded note goes to (not kept in the file)

  // Writes `n` into the step under the cursor and moves the cursor on (it wraps). A note out of 1..127 is a rest.
  void record(int n) noexcept {
    note[cursor] = n >= 1 && n <= 127 ? static_cast<std::uint8_t>(n) : 0;
    cursor = (cursor + 1u) % kPatternSteps;
  }
  void clear() noexcept {
    for (std::uint8_t& n : note) {
      n = 0;
    }
    cursor = 0;
  }
  unsigned used() const noexcept {
    unsigned count = 0;
    for (std::uint8_t n : note) {
      count += n != 0 ? 1u : 0u;
    }
    return count;
  }
  friend bool operator==(const Pattern& a, const Pattern& b) noexcept {
    for (unsigned i = 0; i < kPatternSteps; ++i) {
      if (a.note[i] != b.note[i]) {
        return false;
      }
    }
    return true;  // (the cursor is not part of the file)
  }
};

// The pattern as a file: the size written, or 0 when `cap` is too small.
inline std::size_t write_pattern(const Pattern& p, std::uint8_t* out, std::size_t cap) noexcept {
  namespace chunk = o3sdk::formats::chunk;
  chunk::Writer w(out, cap, "PATN");
  if (!w.begin("STEP") || !w.u16(0x0100) || !w.u8(kPatternSteps) || !w.bytes(p.note, sizeof p.note) || !w.end()) {
    return 0;
  }
  return w.finish();
}

// A pattern from a file's bytes: false, and `p` untouched, when the file is damaged, short, of a newer major version, not 16 steps or has a
// note above 127. Everything is checked before anything is applied; the cursor goes back to the first step.
inline bool read_pattern(const std::uint8_t* data, std::size_t n, Pattern& p) noexcept {
  namespace chunk = o3sdk::formats::chunk;
  chunk::Reader r;
  chunk::Chunk c{};
  if (r.open(data, n, "PATN") != chunk::Error::None) {  // the container's checks: length, ids, pads and the CRC
    return false;
  }
  while (r.next(c)) {
    if (!chunk::id_is(c, "STEP")) {
      continue;  // a chunk of a newer version of the file
    }
    chunk::Fields f(c);
    const std::uint16_t version = f.u16();
    const unsigned steps = f.u8();
    Pattern got;
    if ((version >> 8) != 1u || steps != kPatternSteps) {
      return false;
    }
    for (std::uint8_t& note : got.note) {
      note = f.u8();
      if (note > 127u) {
        return false;
      }
    }
    if (!f.ok()) {
      return false;
    }
    p = got;
    return true;
  }
  return false;
}

// The setting as a file.
inline std::size_t write_config(int octave, std::uint8_t* out, std::size_t cap) noexcept {
  namespace chunk = o3sdk::formats::chunk;
  chunk::Writer w(out, cap, "CONF");
  if (!w.begin("OCTV") || !w.u16(0x0100) || !w.u8(static_cast<std::uint8_t>(clamp_octave(octave) - kMinOctave)) || !w.end()) {
    return 0;
  }
  return w.finish();
}

// The octave from a file's bytes: false, `octave` untouched, when it is damaged or out of range.
inline bool read_config(const std::uint8_t* data, std::size_t n, int& octave) noexcept {
  namespace chunk = o3sdk::formats::chunk;
  chunk::Reader r;
  chunk::Chunk c{};
  if (r.open(data, n, "CONF") != chunk::Error::None) {
    return false;
  }
  while (r.next(c)) {
    if (!chunk::id_is(c, "OCTV")) {
      continue;
    }
    chunk::Fields f(c);
    const std::uint16_t version = f.u16();
    const unsigned v = f.u8();
    if (!f.ok() || (version >> 8) != 1u || v > static_cast<unsigned>(kMaxOctave - kMinOctave)) {
      return false;
    }
    octave = static_cast<int>(v) + kMinOctave;
    return true;
  }
  return false;
}

}  // namespace @APP_ID@
