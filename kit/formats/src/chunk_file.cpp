// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/formats/chunk_file.h>

#include <o3sdk/formats/crc32.h>

#include <cstring>

namespace o3sdk::formats::chunk {

namespace {

constexpr bool printable(char c) noexcept { return c >= 0x20 && c <= 0x7E; }

constexpr bool id_ok(const char (&id)[5]) noexcept { return printable(id[0]) && printable(id[1]) && printable(id[2]) && printable(id[3]); }

std::uint32_t get32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

// ---- Writer -----------------------------------------------------------------------------------------------------------------------

Writer::Writer(std::uint8_t* buf, std::size_t cap, const char (&type)[5]) noexcept : buf_(buf), cap_(cap) {
  if (buf == nullptr || cap < kHeaderBytes + kCrcChunkBytes || !id_ok(type)) {
    ok_ = false;
    return;
  }
  std::memcpy(buf_, "FORM", 4);
  put32(4, 0);
  std::memcpy(buf_ + 8, type, 4);
  pos_ = kHeaderBytes;
}

bool Writer::room(std::size_t n) noexcept {
  // What is left must also hold the CRC chunk, so that finish() cannot fail for want of room (a pad byte is asked for as the byte it is, by end()).
  if (!ok_ || finished_ || n > cap_ - pos_ || cap_ - pos_ - n < kCrcChunkBytes) {
    ok_ = false;
    return false;
  }
  return true;
}

void Writer::put32(std::size_t at, std::uint32_t v) noexcept {
  buf_[at] = static_cast<std::uint8_t>(v >> 24);
  buf_[at + 1] = static_cast<std::uint8_t>(v >> 16);
  buf_[at + 2] = static_cast<std::uint8_t>(v >> 8);
  buf_[at + 3] = static_cast<std::uint8_t>(v);
}

bool Writer::begin(const char (&id)[5]) noexcept {
  if (open_at_ != 0u || !id_ok(id) || !room(8)) {
    ok_ = false;
    return false;
  }
  open_at_ = pos_;
  std::memcpy(buf_ + pos_, id, 4);
  put32(pos_ + 4, 0);
  pos_ += 8;
  return true;
}

bool Writer::u8(std::uint8_t v) noexcept {
  if (open_at_ == 0u || !room(1)) {
    ok_ = false;
    return false;
  }
  buf_[pos_++] = v;
  return true;
}

bool Writer::u16(std::uint16_t v) noexcept {
  const std::uint8_t b[2] = {static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
  return bytes(b, 2);
}

bool Writer::u32(std::uint32_t v) noexcept {
  const std::uint8_t b[4] = {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
  return bytes(b, 4);
}

bool Writer::f32(float v) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  return u32(bits);
}

bool Writer::bytes(const void* src, std::size_t n) noexcept {
  if (open_at_ == 0u || (n != 0u && src == nullptr) || !room(n)) {
    ok_ = false;
    return false;
  }
  if (n != 0u) {
    std::memcpy(buf_ + pos_, src, n);
  }
  pos_ += n;
  return true;
}

bool Writer::end() noexcept {
  if (open_at_ == 0u || !ok_) {
    ok_ = false;
    return false;
  }
  const std::size_t n = pos_ - open_at_ - 8u;
  if (n > 0xFFFFFFFFu) {
    ok_ = false;
    return false;
  }
  put32(open_at_ + 4, static_cast<std::uint32_t>(n));
  if ((n & 1u) != 0u) {
    if (!room(1)) {
      return false;
    }
    buf_[pos_++] = 0;
  }
  open_at_ = 0;
  return true;
}

bool Writer::chunk(const char (&id)[5], const void* payload, std::size_t n) noexcept { return begin(id) && bytes(payload, n) && end(); }

std::size_t Writer::finish() noexcept {
  if (!ok_ || finished_ || open_at_ != 0u || cap_ - pos_ < kCrcChunkBytes) {
    ok_ = false;
    return 0;
  }
  finished_ = true;
  put32(4, static_cast<std::uint32_t>(pos_ + kCrcChunkBytes - 8u));  // the CRC covers the FORM length: it is set first
  const std::uint32_t crc = crc32(buf_, pos_);
  std::memcpy(buf_ + pos_, "CRC ", 4);
  put32(pos_ + 4, 6);
  buf_[pos_ + 8] = static_cast<std::uint8_t>(kCrcVersion >> 8);
  buf_[pos_ + 9] = static_cast<std::uint8_t>(kCrcVersion);
  put32(pos_ + 10, crc);
  pos_ += kCrcChunkBytes;
  return pos_;
}

// ---- Reader -----------------------------------------------------------------------------------------------------------------------

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "ok";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::BadCrc: return "bad crc";
    case Error::Malformed: return "malformed";
  }
  return "?";
}

Error Reader::open(const std::uint8_t* data, std::size_t n, const char (&type)[5]) noexcept {
  data_ = nullptr;
  end_ = file_end_ = pos_ = 0;
  if (data == nullptr || n < kHeaderBytes) {
    return Error::Truncated;
  }
  if (std::memcmp(data, "FORM", 4) != 0 || std::memcmp(data + 8, type, 4) != 0) {
    return Error::BadMagic;
  }
  const std::uint32_t length = get32(data + 4);
  if (length < 4u || (length & 1u) != 0u) {
    return Error::Malformed;
  }
  if (static_cast<std::uint64_t>(length) + 8u > n) {
    return Error::Truncated;
  }
  const std::size_t file_end = static_cast<std::size_t>(length) + 8u;
  // Walk every chunk: ids printable, lengths inside the FORM, pads zero; the CRC chunk is the last and ends exactly at the end.
  std::size_t pos = kHeaderBytes;
  while (true) {
    if (file_end - pos < 8u) {
      return Error::Malformed;  // no CRC chunk
    }
    for (unsigned i = 0; i < 4u; ++i) {
      if (!printable(static_cast<char>(data[pos + i]))) {
        return Error::Malformed;
      }
    }
    const std::uint32_t size = get32(data + pos + 4);
    const std::uint64_t padded = static_cast<std::uint64_t>(size) + (size & 1u);
    if (padded > file_end - pos - 8u) {
      return Error::Malformed;
    }
    if ((size & 1u) != 0u && data[pos + 8 + size] != 0u) {
      return Error::Malformed;
    }
    if (std::memcmp(data + pos, "CRC ", 4) == 0) {
      if (size < 6u || pos + 8u + padded != file_end) {
        return Error::Malformed;  // the CRC chunk is last, and whole
      }
      const unsigned version = (static_cast<unsigned>(data[pos + 8]) << 8) | data[pos + 9];
      if ((version >> 8) != (kCrcVersion >> 8)) {
        return Error::Malformed;  // another major of the trailer: nothing here can say what it covers
      }
      if (crc32(data, pos) != get32(data + pos + 10)) {
        return Error::BadCrc;
      }
      crc_version_ = static_cast<std::uint16_t>(version);
      data_ = data;
      end_ = pos;
      file_end_ = file_end;
      pos_ = kHeaderBytes;
      return Error::None;
    }
    pos += 8u + static_cast<std::size_t>(padded);
  }
}

bool Reader::next(Chunk& out) noexcept {
  if (data_ == nullptr || pos_ >= end_) {
    return false;
  }
  std::memcpy(out.id, data_ + pos_, 4);
  out.size = get32(data_ + pos_ + 4);
  out.data = data_ + pos_ + 8;
  pos_ += 8u + out.size + (out.size & 1u);
  return true;
}

// ---- Fields -----------------------------------------------------------------------------------------------------------------------

std::uint8_t Fields::u8() noexcept {
  if (left_ < 1u) {
    ok_ = false;
    return 0;
  }
  --left_;
  return *p_++;
}

std::uint16_t Fields::u16() noexcept {
  const unsigned hi = u8();
  const unsigned lo = u8();
  return static_cast<std::uint16_t>((hi << 8) | lo);
}

std::uint32_t Fields::u32() noexcept {
  const std::uint32_t hi = u16();
  const std::uint32_t lo = u16();
  return (hi << 16) | lo;
}

float Fields::f32() noexcept {
  const std::uint32_t bits = u32();
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof v);
  return v;
}

void Fields::skip(std::size_t n) noexcept {
  if (n > left_) {
    ok_ = false;
    left_ = 0;
    return;
  }
  p_ += n;
  left_ -= n;
}

}  // namespace o3sdk::formats::chunk
