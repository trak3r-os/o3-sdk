// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal/screenshot_file.h (OS-SCREENSHOT): the BMP header's 66 bytes, the file names and the words of the strip. Pure.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/hal/save_guard.h>
#include <o3sdk/hal/screenshot_file.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace shot = o3sdk::hal::storage::shot;
namespace sv = o3sdk::hal::storage::save;

namespace {
std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint32_t le16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
}  // namespace

TEST_CASE("screenshot file: the BMP header is 66 bytes of a top-down 800 x 480 RGB565 image") {
  static_assert(shot::kHeaderBytes == 66 && shot::kPixelBytes == 768000 && shot::kFileBytes == 768066 && shot::kRowBytes == 1600);
  std::uint8_t h[shot::kHeaderBytes];
  std::memset(h, 0xEE, sizeof h);
  shot::bmp_header(h);
  CHECK(h[0] == 'B');
  CHECK(h[1] == 'M');
  CHECK(le32(h + 2) == 768066u);   // file size
  CHECK(le32(h + 6) == 0u);        // reserved
  CHECK(le32(h + 10) == 66u);      // where the pixels start
  CHECK(le32(h + 14) == 40u);      // BITMAPINFOHEADER
  CHECK(le32(h + 18) == 800u);
  CHECK(static_cast<std::int32_t>(le32(h + 22)) == -480);  // negative: the rows run top-down, as the coprocessor writes them
  CHECK(le16(h + 26) == 1u);
  CHECK(le16(h + 28) == 16u);
  CHECK(le32(h + 30) == 3u);       // BI_BITFIELDS
  CHECK(le32(h + 34) == 768000u);
  CHECK(le32(h + 46) == 0u);       // no palette
  CHECK(le32(h + 50) == 0u);
  CHECK(le32(h + 54) == 0xF800u);  // RGB565 masks
  CHECK(le32(h + 58) == 0x07E0u);
  CHECK(le32(h + 62) == 0x001Fu);
}

TEST_CASE("screenshot file: names SHOT_0001.bmp .. SHOT_9999.bmp, and the guard knows every one") {
  char n[shot::kNameCap];
  CHECK(shot::file_name(n, 1));
  CHECK(std::strcmp(n, "SHOT_0001.bmp") == 0);
  CHECK(shot::file_name(n, 7));
  CHECK(std::strcmp(n, "SHOT_0007.bmp") == 0);
  CHECK(shot::file_name(n, 9999));
  CHECK(std::strcmp(n, "SHOT_9999.bmp") == 0);
  CHECK_FALSE(shot::file_name(n, 0));
  CHECK(n[0] == '\0');
  CHECK_FALSE(shot::file_name(n, 10000));
  CHECK(n[0] == '\0');
  for (unsigned i = shot::kFirstNumber; i <= shot::kLastNumber; ++i) {
    char path[sv::kPathCap];
    NEED(shot::file_name(n, i));
    NEED(sv::join(path, sizeof path, sv::kScreenshotsDir, n));
    NEED(sv::classify(path) == sv::Place::Target);
  }
}

TEST_CASE("screenshot file: every refusal has words, and they fit a strip") {
  CHECK(std::strcmp(shot::refusal_text(shot::Refusal::None), "") == 0);
  for (const auto r : {shot::Refusal::NoCard, shot::Refusal::OnComputer, shot::Refusal::Unwritable, shot::Refusal::Busy, shot::Refusal::ConsoleShot, shot::Refusal::InProgress, shot::Refusal::FolderFull}) {
    const char* t = shot::refusal_text(r);
    CHECK(std::strncmp(t, "NO SCREENSHOT: ", 15) == 0);
    CHECK(std::strlen(t) > 15u);
    CHECK(std::strlen(t) < 48u);
  }
}
