// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/platform/host/png.h>
#include <o3sdk/platform/host/sha256.h>
#include <o3sdk/sim/eve_raster.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace o3sdk::platform::host;
namespace sim = o3sdk::sim;

namespace {
// The byte pattern the reference digests below were computed from (Python hashlib): (i * 7 + 3) & 255.
std::vector<std::uint8_t> pattern(std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 255);
  }
  return v;
}
std::string hex_of(const std::vector<std::uint8_t>& v) { return sha256_hex(v.data(), v.size()); }
}  // namespace

TEST_CASE("sha256: FIPS 180-4 test vectors") {
  CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(sha256_hex(two_block.data(), two_block.size()) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  CHECK(sha256_hex(million.data(), million.size()) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: padding boundaries (55, 56, 57, 63, 64, 65 bytes and two blocks)") {
  CHECK(hex_of(pattern(1)) == "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5");
  CHECK(hex_of(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
  CHECK(hex_of(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
  CHECK(hex_of(pattern(57)) == "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73");
  CHECK(hex_of(pattern(63)) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
  CHECK(hex_of(pattern(64)) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
  CHECK(hex_of(pattern(65)) == "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e");
  CHECK(hex_of(pattern(119)) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
  CHECK(hex_of(pattern(120)) == "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5");
  CHECK(hex_of(pattern(128)) == "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6");
}

TEST_CASE("sha256: feeding in pieces gives the one-shot digest") {
  const std::vector<std::uint8_t> data = pattern(1000);
  for (std::size_t piece : {1u, 3u, 63u, 64u, 65u, 500u}) {
    Sha256 h;
    for (std::size_t at = 0; at < data.size(); at += piece) {
      h.update(data.data() + at, std::min(piece, data.size() - at));
    }
    std::uint8_t digest[32];
    h.finish(digest);
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (std::uint8_t b : digest) {
      hex.push_back(kHex[b >> 4]);
      hex.push_back(kHex[b & 15]);
    }
    CHECK(hex == hex_of(data));
  }
}

TEST_CASE("png: signature, IHDR and determinism") {
  // 3 x 2 RGBA
  const std::uint8_t pixels[3 * 2 * 4] = {255, 0, 0,   255, 0,   255, 0,   255, 0,  0,  255, 255,
                                          10,  20, 30, 255, 200, 100, 50,  255, 1,  2,  3,   255};
  std::vector<std::uint8_t> a, b;
  CHECK(encode_png_rgba(pixels, 3, 2, a));
  CHECK(encode_png_rgba(pixels, 3, 2, b));
  CHECK(a == b);
  CHECK(a.size() > 57);  // signature + IHDR + at least one IDAT + IEND
  if (a.size() <= 57) {
    return;
  }
  const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CHECK(std::equal(signature, signature + 8, a.begin()));
  CHECK(std::string(a.begin() + 12, a.begin() + 16) == "IHDR");
  const auto be32 = [&](std::size_t at) {
    return (std::uint32_t{a[at]} << 24) | (std::uint32_t{a[at + 1]} << 16) | (std::uint32_t{a[at + 2]} << 8) | a[at + 3];
  };
  CHECK(be32(16) == 3u);  // width
  CHECK(be32(20) == 2u);  // height
  CHECK(a[24] == 8);      // bit depth
  CHECK(a[25] == 6);      // colour type: RGBA
  CHECK(std::string(a.end() - 8, a.end() - 4) == "IEND");
}

TEST_CASE("png: bad arguments are refused and write_png_rgba round-trips to a file") {
  std::vector<std::uint8_t> out{1, 2, 3};
  const std::uint8_t px[4] = {0, 0, 0, 255};
  CHECK_FALSE(encode_png_rgba(nullptr, 1, 1, out));
  CHECK(out.empty());
  CHECK_FALSE(encode_png_rgba(px, 0, 1, out));
  CHECK_FALSE(encode_png_rgba(px, 1, -1, out));
  CHECK_FALSE(write_png_rgba(nullptr, px, 1, 1));
  CHECK_FALSE(write_png_rgba("/nonexistent-dir-o3sdk/x.png", px, 1, 1));

  const std::string path = (std::filesystem::temp_directory_path() / "o3sdk_png_test.png").string();
  CHECK(write_png_rgba(path.c_str(), px, 1, 1));
  std::FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f != nullptr);
  if (f != nullptr) {
    std::uint8_t head[4] = {};
    CHECK(std::fread(head, 1, 4, f) == 4u);
    std::fclose(f);
    CHECK(head[1] == 'P');
  }
  std::remove(path.c_str());
}

// ---- PNG round trip -------------------------------------------------------------------------------------
// The PNGs are what people look at, so they must hold the pixels that were hashed. This decodes what the encoder
// wrote with a small independent reader: CRC-32 of every chunk, zlib header and Adler-32, an inflate for the
// fixed-Huffman and stored blocks that stb_image_write emits (RFC 1951; dynamic blocks are reported, not decoded),
// and all five scanline filters.
namespace {

struct BitReader {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t pos = 0;  // in bits
  bool ok = true;
  unsigned bit() {
    if (pos >= size * 8) {
      ok = false;
      return 0;
    }
    const unsigned b = (data[pos >> 3] >> (pos & 7)) & 1u;
    ++pos;
    return b;
  }
  unsigned bits(unsigned n) {  // least significant bit first, as deflate packs numbers
    unsigned v = 0;
    for (unsigned i = 0; i < n; ++i) {
      v |= bit() << i;
    }
    return v;
  }
};

constexpr unsigned kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr unsigned kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr unsigned kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr unsigned kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Literal/length symbol of the fixed code (RFC 1951 section 3.2.6): Huffman codes are packed most significant
// bit first. -1: not a valid code.
int fixed_symbol(BitReader& br) {
  unsigned code = 0;
  for (int i = 0; i < 7; ++i) {
    code = (code << 1) | br.bit();
  }
  if (code <= 23) {
    return static_cast<int>(256 + code);  // 0000000..0010111: 256..279
  }
  code = (code << 1) | br.bit();
  if (code >= 48 && code <= 191) {
    return static_cast<int>(code - 48);  // 00110000..10111111: 0..143
  }
  if (code >= 192 && code <= 199) {
    return static_cast<int>(280 + code - 192);  // 11000000..11000111: 280..287
  }
  code = (code << 1) | br.bit();
  return code >= 400 ? static_cast<int>(144 + code - 400) : -1;  // 110010000..111111111: 144..255
}

bool inflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
  BitReader br{data, size};
  for (;;) {
    const unsigned final_block = br.bit();
    const unsigned type = br.bits(2);
    if (type == 0) {  // stored
      br.pos = (br.pos + 7) & ~std::size_t{7};
      const std::size_t at = br.pos >> 3;
      if (at + 4 > size) {
        return false;
      }
      const std::size_t len = data[at] | (std::size_t{data[at + 1]} << 8);
      if (at + 4 + len > size) {
        return false;
      }
      out.insert(out.end(), data + at + 4, data + at + 4 + len);
      br.pos += (4 + len) * 8;
    } else if (type == 1) {  // fixed Huffman
      for (;;) {
        const int sym = fixed_symbol(br);
        if (!br.ok || sym < 0) {
          return false;
        }
        if (sym < 256) {
          out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
          break;
        } else {
          const unsigned li = static_cast<unsigned>(sym - 257);
          if (li > 28) {
            return false;
          }
          const unsigned length = kLenBase[li] + br.bits(kLenExtra[li]);
          unsigned di = 0;
          for (int i = 0; i < 5; ++i) {
            di = (di << 1) | br.bit();  // distance codes are 5 bits, most significant first
          }
          if (di > 29) {
            return false;
          }
          const unsigned distance = kDistBase[di] + br.bits(kDistExtra[di]);
          if (!br.ok || distance > out.size()) {
            return false;
          }
          for (unsigned i = 0; i < length; ++i) {
            out.push_back(out[out.size() - distance]);
          }
        }
      }
    } else {
      return false;  // dynamic Huffman: not emitted by stb_image_write, not decoded here
    }
    if (final_block) {
      return br.ok;
    }
  }
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

std::uint32_t adler32(const std::uint8_t* p, std::size_t n) {
  std::uint32_t a = 1, b = 0;
  for (std::size_t i = 0; i < n; ++i) {
    a = (a + p[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

// Decodes an 8-bit RGBA PNG; returns false (and says why through CHECK) when anything is not as expected.
bool decode_png(const std::vector<std::uint8_t>& png, int& width, int& height, std::vector<std::uint8_t>& rgba) {
  const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (png.size() < 8 || !std::equal(signature, signature + 8, png.begin())) {
    return false;
  }
  std::vector<std::uint8_t> idat;
  bool have_header = false;
  for (std::size_t pos = 8; pos + 12 <= png.size();) {
    const std::uint32_t length = be32(&png[pos]);
    if (pos + 12 + length > png.size() || crc32(&png[pos + 4], 4 + length) != be32(&png[pos + 8 + length])) {
      return false;  // truncated or corrupted chunk
    }
    const std::string kind(png.begin() + static_cast<std::ptrdiff_t>(pos + 4), png.begin() + static_cast<std::ptrdiff_t>(pos + 8));
    const std::uint8_t* body = &png[pos + 8];
    if (kind == "IHDR") {
      if (length != 13 || body[8] != 8 || body[9] != 6 || body[10] != 0 || body[11] != 0 || body[12] != 0) {
        return false;  // not 8-bit RGBA, deflate, adaptive filtering, no interlace
      }
      width = static_cast<int>(be32(body));
      height = static_cast<int>(be32(body + 4));
      have_header = true;
    } else if (kind == "IDAT") {
      idat.insert(idat.end(), body, body + length);
    }
    pos += 12 + length;
  }
  if (!have_header || idat.size() < 6 || (idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 != 0) {
    return false;
  }
  std::vector<std::uint8_t> raw;
  if (!inflate(idat.data() + 2, idat.size() - 6, raw) || be32(&idat[idat.size() - 4]) != adler32(raw.data(), raw.size())) {
    return false;
  }
  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  if (raw.size() != static_cast<std::size_t>(height) * (stride + 1)) {
    return false;
  }
  rgba.assign(static_cast<std::size_t>(height) * stride, 0);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t filter = raw[static_cast<std::size_t>(y) * (stride + 1)];
    const std::uint8_t* in = &raw[static_cast<std::size_t>(y) * (stride + 1) + 1];
    std::uint8_t* row = &rgba[static_cast<std::size_t>(y) * stride];
    const std::uint8_t* up = y > 0 ? row - stride : nullptr;
    for (std::size_t i = 0; i < stride; ++i) {
      const unsigned a = i >= 4 ? row[i - 4] : 0u;
      const unsigned b = up != nullptr ? up[i] : 0u;
      const unsigned c = (up != nullptr && i >= 4) ? up[i - 4] : 0u;
      unsigned predictor = 0;
      if (filter == 1) {
        predictor = a;
      } else if (filter == 2) {
        predictor = b;
      } else if (filter == 3) {
        predictor = (a + b) / 2;
      } else if (filter == 4) {
        const int p = static_cast<int>(a + b) - static_cast<int>(c);
        const int pa = std::abs(p - static_cast<int>(a)), pb = std::abs(p - static_cast<int>(b)), pc = std::abs(p - static_cast<int>(c));
        predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      } else if (filter != 0) {
        return false;
      }
      row[i] = static_cast<std::uint8_t>(in[i] + predictor);
    }
  }
  return true;
}

}  // namespace

TEST_CASE("png: the decoder used here agrees with CRC-32 and Adler-32 reference values") {
  CHECK(crc32(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xCBF43926u);
  CHECK(adler32(reinterpret_cast<const std::uint8_t*>("Wikipedia"), 9) == 0x11E60398u);
}

TEST_CASE("png: encoded pixels decode back exactly, whatever filters and matches the encoder chose") {
  // Smooth gradients, flat runs and noise in one picture, so every filter type and long matches turn up.
  const int w = 61, h = 47;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h * 4));
  std::uint32_t seed = 12345;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = &rgba[static_cast<std::size_t>((y * w + x) * 4)];
      seed = seed * 1664525u + 1013904223u;
      const unsigned noise = seed >> 24;
      const int band = y / 12;
      p[0] = static_cast<std::uint8_t>(band == 0 ? x * 4 : (band == 1 ? 200 : (band == 2 ? y * 5 : noise)));
      p[1] = static_cast<std::uint8_t>(band == 0 ? y * 5 : (band == 1 ? 40 : (band == 2 ? x * 3 : noise)));
      p[2] = static_cast<std::uint8_t>(band == 3 ? x ^ y : (band == 1 ? 40 : noise / 4));
      p[3] = 255;
    }
  }
  std::vector<std::uint8_t> png;
  CHECK(encode_png_rgba(rgba.data(), w, h, png));
  int dw = 0, dh = 0;
  std::vector<std::uint8_t> back;
  CHECK(decode_png(png, dw, dh, back));
  CHECK((dw == w && dh == h));
  CHECK(back == rgba);

  png[png.size() / 2] ^= 0x10;  // and the decoder does notice damage (a chunk CRC no longer matches)
  CHECK_FALSE(decode_png(png, dw, dh, back));
}
