// SPDX-License-Identifier: GPL-3.0-or-later
// The chunk container of an app's own files (formats/chunk_file.h, docs/formats/o3sdk-chunk-file.md): the bytes it writes, the checks open() makes before anything is
// decoded (every byte flipped, every cut), the tolerant walk of unknown chunks, and the full-buffer behaviour of the writer.
#include <doctest/doctest.h>

#include <o3sdk/formats/chunk_file.h>
#include <o3sdk/formats/crc32.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace chunk = o3sdk::formats::chunk;

namespace {

std::vector<std::uint8_t> sample_file() {
  std::vector<std::uint8_t> buf(256);
  chunk::Writer w(buf.data(), buf.size(), "TEST");
  REQUIRE(w.begin("ABCD"));
  REQUIRE(w.u16(0x0102));
  REQUIRE(w.u32(0xDEADBEEFu));
  REQUIRE(w.f32(1.5f));
  REQUIRE(w.end());
  REQUIRE(w.begin("ODD "));  // an odd payload: a pad byte
  REQUIRE(w.u8(7));
  REQUIRE(w.end());
  REQUIRE(w.chunk("EMPT", nullptr, 0));
  const std::size_t n = w.finish();
  REQUIRE(n != 0);
  buf.resize(n);
  return buf;
}

}  // namespace

TEST_CASE("chunk: the bytes of a file are the documented ones") {
  const std::vector<std::uint8_t> f = sample_file();
  const std::uint8_t expect_head[] = {'F', 'O', 'R', 'M', 0, 0, 0, 0, 'T', 'E', 'S', 'T', 'A', 'B', 'C', 'D', 0, 0, 0, 10, 0x01, 0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x3F, 0xC0, 0, 0};
  REQUIRE(f.size() > sizeof expect_head);
  for (std::size_t i = 0; i < sizeof expect_head; ++i) {
    if (i < 4 || i >= 8) {
      CAPTURE(i);
      CHECK(f[i] == expect_head[i]);
    }
  }
  // FORM's length counts what follows it; ODD has a pad byte (length 1, two bytes); EMPT has none; CRC last.
  const std::uint32_t length = (std::uint32_t{f[4]} << 24) | (std::uint32_t{f[5]} << 16) | (std::uint32_t{f[6]} << 8) | f[7];
  CHECK(length + 8u == f.size());
  CHECK(length % 2u == 0u);
  const std::size_t odd = 12 + 8 + 10;
  CHECK(std::memcmp(&f[odd], "ODD ", 4) == 0);
  CHECK(f[odd + 7] == 1);
  CHECK(f[odd + 8] == 7);
  CHECK(f[odd + 9] == 0);  // the pad
  CHECK(std::memcmp(&f[f.size() - 14], "CRC ", 4) == 0);
  const std::uint32_t crc = o3sdk::formats::crc32(f.data(), f.size() - 14);
  const std::size_t c = f.size() - 14 + 8;
  CHECK(f[c] == 0x01);
  CHECK(f[c + 1] == 0x00);
  CHECK(((std::uint32_t{f[c + 2]} << 24) | (std::uint32_t{f[c + 3]} << 16) | (std::uint32_t{f[c + 4]} << 8) | f[c + 5]) == crc);
}

TEST_CASE("chunk: a file reads back, chunk by chunk, with its fields") {
  const std::vector<std::uint8_t> f = sample_file();
  chunk::Reader r;
  REQUIRE(r.open(f.data(), f.size(), "TEST") == chunk::Error::None);
  CHECK(r.file_size() == f.size());
  CHECK(r.crc_version() == 0x0100);
  chunk::Chunk c{};
  REQUIRE(r.next(c));
  CHECK(chunk::id_is(c, "ABCD"));
  CHECK(c.size == 10);
  chunk::Fields in(c);
  CHECK(in.u16() == 0x0102);
  CHECK(in.u32() == 0xDEADBEEFu);
  CHECK(in.f32() == 1.5f);
  CHECK(in.ok());
  CHECK(in.left() == 0);
  (void)in.u8();  // past the end: 0 and not ok
  CHECK_FALSE(in.ok());
  REQUIRE(r.next(c));
  CHECK(chunk::id_is(c, "ODD "));
  CHECK(c.size == 1);
  CHECK(c.data[0] == 7);
  REQUIRE(r.next(c));
  CHECK(chunk::id_is(c, "EMPT"));
  CHECK(c.size == 0);
  CHECK_FALSE(r.next(c));  // the CRC chunk is not one of them
  r.rewind();
  REQUIRE(r.next(c));
  CHECK(chunk::id_is(c, "ABCD"));
}

TEST_CASE("chunk: a container that is wrong in any byte is refused before a chunk is read") {
  const std::vector<std::uint8_t> f = sample_file();
  const std::size_t minor = f.size() - 14 + 9;  // the trailer's minor version is the one byte nothing checks: a newer minor is read
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (i == minor) {
      continue;
    }
    std::vector<std::uint8_t> g = f;
    g[i] ^= 0x01;
    chunk::Reader r;
    CAPTURE(i);
    CHECK(r.open(g.data(), g.size(), "TEST") != chunk::Error::None);
    chunk::Chunk c{};
    CHECK_FALSE(r.next(c));
  }
  for (std::size_t n = 0; n < f.size(); ++n) {  // every cut is a Truncated (or a refusal), never an Ok
    chunk::Reader r;
    CAPTURE(n);
    CHECK(r.open(f.data(), n, "TEST") != chunk::Error::None);
  }
  chunk::Reader r;
  CHECK(r.open(f.data(), f.size(), "TSET") == chunk::Error::BadMagic);
  CHECK(r.open(nullptr, 0, "TEST") == chunk::Error::Truncated);
}

TEST_CASE("chunk: bytes after the FORM are ignored, a newer trailer minor is read, a newer major is refused") {
  std::vector<std::uint8_t> f = sample_file();
  f.push_back(0xAA);
  f.push_back(0xBB);
  chunk::Reader r;
  CHECK(r.open(f.data(), f.size(), "TEST") == chunk::Error::None);
  CHECK(r.file_size() == f.size() - 2);

  // A CRC chunk of version 1.1 with two extra bytes: rebuilt by hand with its CRC.
  std::vector<std::uint8_t> g(f.begin(), f.end() - 2 - 14);
  const std::uint8_t trailer_head[] = {'C', 'R', 'C', ' ', 0, 0, 0, 8, 0x01, 0x01, 0, 0, 0, 0, 0xAB, 0xCD};
  const std::size_t at = g.size();
  g.insert(g.end(), trailer_head, trailer_head + sizeof trailer_head);
  const std::uint32_t crc = o3sdk::formats::crc32(g.data(), at);
  g[at + 10] = static_cast<std::uint8_t>(crc >> 24);
  g[at + 11] = static_cast<std::uint8_t>(crc >> 16);
  g[at + 12] = static_cast<std::uint8_t>(crc >> 8);
  g[at + 13] = static_cast<std::uint8_t>(crc);
  const std::uint32_t length = static_cast<std::uint32_t>(g.size() - 8);
  g[4] = static_cast<std::uint8_t>(length >> 24);
  g[5] = static_cast<std::uint8_t>(length >> 16);
  g[6] = static_cast<std::uint8_t>(length >> 8);
  g[7] = static_cast<std::uint8_t>(length);
  // (the FORM length changed, which the CRC covers: recompute it)
  const std::uint32_t crc2 = o3sdk::formats::crc32(g.data(), at);
  g[at + 10] = static_cast<std::uint8_t>(crc2 >> 24);
  g[at + 11] = static_cast<std::uint8_t>(crc2 >> 16);
  g[at + 12] = static_cast<std::uint8_t>(crc2 >> 8);
  g[at + 13] = static_cast<std::uint8_t>(crc2);
  CHECK(r.open(g.data(), g.size(), "TEST") == chunk::Error::None);
  CHECK(r.crc_version() == 0x0101);

  g[at + 8] = 0x02;  // a newer major of the trailer
  const std::uint32_t crc3 = o3sdk::formats::crc32(g.data(), at);
  g[at + 10] = static_cast<std::uint8_t>(crc3 >> 24);
  g[at + 11] = static_cast<std::uint8_t>(crc3 >> 16);
  g[at + 12] = static_cast<std::uint8_t>(crc3 >> 8);
  g[at + 13] = static_cast<std::uint8_t>(crc3);
  CHECK(r.open(g.data(), g.size(), "TEST") == chunk::Error::Malformed);
}

TEST_CASE("chunk: a pad byte that is not zero, a chunk past the end and an odd FORM length are Malformed") {
  std::vector<std::uint8_t> f = sample_file();
  const std::size_t odd = 12 + 8 + 10;
  auto reseal = [](std::vector<std::uint8_t>& g) {  // the CRC of what is before the trailer, so only the shape is wrong
    const std::size_t at = g.size() - 14;
    const std::uint32_t crc = o3sdk::formats::crc32(g.data(), at);
    g[at + 10] = static_cast<std::uint8_t>(crc >> 24);
    g[at + 11] = static_cast<std::uint8_t>(crc >> 16);
    g[at + 12] = static_cast<std::uint8_t>(crc >> 8);
    g[at + 13] = static_cast<std::uint8_t>(crc);
  };
  chunk::Reader r;
  std::vector<std::uint8_t> g = f;
  g[odd + 9] = 1;
  reseal(g);
  CHECK(r.open(g.data(), g.size(), "TEST") == chunk::Error::Malformed);
  g = f;
  g[12 + 4 + 3] = 0xFF;  // ABCD claims 255 bytes
  reseal(g);
  CHECK(r.open(g.data(), g.size(), "TEST") == chunk::Error::Malformed);
  g = f;
  g[odd] = 0x07;  // an id that is not printable
  reseal(g);
  CHECK(r.open(g.data(), g.size(), "TEST") == chunk::Error::Malformed);
}

TEST_CASE("chunk: the writer refuses what does not fit and finish() then says 0") {
  std::vector<std::uint8_t> buf(64);
  chunk::Writer w(buf.data(), buf.size(), "FULL");
  REQUIRE(w.begin("DATA"));
  std::uint8_t block[16] = {};
  CHECK(w.bytes(block, sizeof block));
  CHECK_FALSE(w.bytes(block, 40));  // would leave no room for the CRC chunk
  CHECK_FALSE(w.ok());
  CHECK_FALSE(w.u8(1));
  CHECK(w.finish() == 0);

  chunk::Writer small(buf.data(), 20, "TINY");  // less than the header and the trailer
  CHECK_FALSE(small.ok());
  CHECK(small.finish() == 0);

  chunk::Writer exact(buf.data(), buf.size(), "FITS");
  REQUIRE(exact.begin("DATA"));
  CHECK(exact.bytes(block, 16));
  CHECK(exact.end());
  CHECK(exact.finish() == 12 + 8 + 16 + 14);
  chunk::Reader r;
  CHECK(r.open(buf.data(), 12 + 8 + 16 + 14, "FITS") == chunk::Error::None);

  chunk::Writer misuse(buf.data(), buf.size(), "MISU");
  CHECK_FALSE(misuse.u8(1));  // no chunk open
  CHECK(misuse.finish() == 0);
  chunk::Writer unclosed(buf.data(), buf.size(), "OPEN");
  REQUIRE(unclosed.begin("DATA"));
  CHECK(unclosed.finish() == 0);  // a chunk still open
}

TEST_CASE("chunk: fields stop at the end of a payload") {
  const std::uint8_t p[3] = {1, 2, 3};
  chunk::Fields f(p, sizeof p);
  CHECK(f.u16() == 0x0102);
  CHECK(f.ok());
  CHECK(f.u16() == 0x0300);  // one byte, then the missing one is 0
  CHECK_FALSE(f.ok());
  chunk::Fields g(p, sizeof p);
  g.skip(4);
  CHECK_FALSE(g.ok());
  CHECK(g.left() == 0);
}
