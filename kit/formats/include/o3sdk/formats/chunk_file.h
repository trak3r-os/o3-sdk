// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The chunk container of o3sdk's own files (docs/M2C-PLAN.md decision D7, docs/formats/o3sdk-chunk-file.md): the shape of track3r-os's settings and project files
// (an IFF-style FORM, big-endian, even-aligned chunks, a CRC-32 trailer), written into a buffer and read from one. An app that wants versioned data in a file of its own
// uses it; the kit's settings file (app/settings.h) is one. Nothing here touches a card, allocates or blocks: it is a codec over memory, `Files::save` and `Files::load`
// (app/files.h) move the bytes.
//
//   "FORM"  u32 length  <type: 4 chars>  chunk chunk ... "CRC " chunk
//   chunk = id (4 chars, 0x20..0x7E)  u32 payload length  payload  [one zero pad byte when the length is odd]
//
// `length` counts everything after itself (the type, the chunks with their headers and pads); the file is length + 8 bytes and bytes after that are ignored. The CRC chunk is
// last, its payload `u16 version (0x0100)`, `u32 crc` (a newer minor may carry more), the CRC-32 (formats/crc32.h) of every byte before its header. By convention the first
// u16 of a known chunk's payload is its version, major in the high byte: a reader refuses a newer major and reads a newer minor as far as it knows it.
//
// Writer: begin() / put / end() once per chunk, then finish(); any call after a failure (the buffer is full) is refused and finish() answers 0.
// Reader: open() checks the whole container first (the CRC included), so nothing is decoded from a file that fails; next() then walks the chunks in order.
//
// Contexts: any (pure functions of the bytes). No heap.

#include <cstddef>
#include <cstdint>

namespace o3sdk::formats::chunk {

inline constexpr std::size_t kHeaderBytes = 12;   // "FORM", the length, the type
inline constexpr std::size_t kCrcChunkBytes = 14;  // "CRC ", 6, u16 version, u32 crc
inline constexpr std::uint16_t kCrcVersion = 0x0100;

class Writer {
 public:
  // A file of `type` (four characters) into `buf` of `cap` bytes.
  Writer(std::uint8_t* buf, std::size_t cap, const char (&type)[5]) noexcept;

  // Opens a chunk `id` (four characters, 0x20..0x7E); the previous one must be closed. false: refused.
  bool begin(const char (&id)[5]) noexcept;
  // Payload of the open chunk, big-endian (f32 as its bit pattern).
  bool u8(std::uint8_t v) noexcept;
  bool u16(std::uint16_t v) noexcept;
  bool u32(std::uint32_t v) noexcept;
  bool f32(float v) noexcept;
  bool bytes(const void* src, std::size_t n) noexcept;
  // Closes the open chunk: its length and the pad byte.
  bool end() noexcept;
  // One whole chunk.
  bool chunk(const char (&id)[5], const void* payload, std::size_t n) noexcept;

  // The CRC chunk and the FORM length: the size of the finished file, or 0 when anything was refused. Once; the writer is spent.
  std::size_t finish() noexcept;
  bool ok() const noexcept { return ok_; }
  std::size_t size() const noexcept { return pos_; }  // bytes so far

 private:
  bool room(std::size_t n) noexcept;
  void put32(std::size_t at, std::uint32_t v) noexcept;

  std::uint8_t* buf_;
  std::size_t cap_;
  std::size_t pos_ = 0;
  std::size_t open_at_ = 0;  // the offset of the open chunk's header; 0 while none is open
  bool ok_ = true;
  bool finished_ = false;
};

enum class Error : std::uint8_t {
  None,
  Truncated,  // fewer bytes than the FORM says, or than its header
  BadMagic,   // not "FORM", or another type
  BadCrc,     // the CRC chunk does not match what is before it
  Malformed,  // an odd or short length, a chunk past the end, a pad that is not zero, an id that is not printable, no CRC chunk last
};
const char* error_name(Error e) noexcept;

struct Chunk {
  char id[4];
  const std::uint8_t* data;  // into the buffer open() was given
  std::uint32_t size;
};

class Reader {
 public:
  // Checks the container: FORM, `type`, lengths, pads, ids and the CRC. Ok: next() walks the chunks (the CRC chunk is not one of them).
  Error open(const std::uint8_t* data, std::size_t n, const char (&type)[5]) noexcept;
  // The next chunk in file order; false at the end.
  bool next(Chunk& out) noexcept;
  void rewind() noexcept { pos_ = kHeaderBytes; }
  std::size_t file_size() const noexcept { return file_end_; }  // length + 8, once open() said Ok
  std::uint16_t crc_version() const noexcept { return crc_version_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t end_ = 0;       // the end of the chunks (the CRC chunk's header)
  std::size_t file_end_ = 0;
  std::size_t pos_ = 0;
  std::uint16_t crc_version_ = 0;
};

// A cursor over a payload, big-endian. A read past the end gives 0 and clears ok(); skip() likewise.
class Fields {
 public:
  Fields(const std::uint8_t* data, std::size_t n) noexcept : p_(data), left_(n) {}
  explicit Fields(const Chunk& c) noexcept : p_(c.data), left_(c.size) {}
  std::uint8_t u8() noexcept;
  std::uint16_t u16() noexcept;
  std::uint32_t u32() noexcept;
  float f32() noexcept;
  void skip(std::size_t n) noexcept;
  std::size_t left() const noexcept { return left_; }
  bool ok() const noexcept { return ok_; }

 private:
  const std::uint8_t* p_;
  std::size_t left_;
  bool ok_ = true;
};

inline bool id_is(const Chunk& c, const char (&id)[5]) noexcept {
  return c.id[0] == id[0] && c.id[1] == id[1] && c.id[2] == id[2] && c.id[3] == id[3];
}

}  // namespace o3sdk::formats::chunk
