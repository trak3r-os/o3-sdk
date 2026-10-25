// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-SCREENSHOT: the file Shift + Power writes, as pure functions the device and the simulator share. A BMP, 16 bits a pixel (RGB565, BI_BITFIELDS), top-down (a negative height), 800 x 480:
// a fixed 66 byte header and the 768,000 bytes the FT812 coprocessor's CMD_SNAPSHOT2 gives in RGB565, row by row, little endian, exactly as they come: no conversion. The name is
// Screenshots/SHOT_nnnn.bmp (save_guard.h), the next free number, never overwritten. The words of the strip are here too, so the device and the simulator say the same.

#include <cstddef>
#include <cstdint>

namespace o3sdk::hal::storage::shot {

inline constexpr std::uint32_t kWidth = 800;
inline constexpr std::uint32_t kHeight = 480;
inline constexpr std::uint32_t kRowBytes = kWidth * 2u;           // 1,600
inline constexpr std::uint32_t kPixelBytes = kRowBytes * kHeight;  // 768,000
inline constexpr std::uint32_t kHeaderBytes = 66;                  // 14 file header + 40 info header + 12 colour masks
inline constexpr std::uint32_t kFileBytes = kHeaderBytes + kPixelBytes;
inline constexpr unsigned kFirstNumber = 1;
inline constexpr unsigned kLastNumber = 9999;
inline constexpr std::size_t kNameCap = 16;  // "SHOT_0001.bmp" and its NUL

namespace detail {
constexpr void put16(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
constexpr void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  put16(p, v);
  put16(p + 2, v >> 16);
}
}  // namespace detail

// The header, 66 bytes: "BM", the file size, the data offset 66; BITMAPINFOHEADER (size 40, width 800, height -480 = top-down, 1 plane, 16 bits, BI_BITFIELDS = 3, 768,000 image bytes, 2835 pixels
// a metre both ways, no palette); the masks R 0xF800, G 0x07E0, B 0x001F.
constexpr void bmp_header(std::uint8_t* out) noexcept {  // kHeaderBytes bytes
  for (std::size_t i = 0; i < kHeaderBytes; ++i) {
    out[i] = 0;
  }
  out[0] = 'B';
  out[1] = 'M';
  detail::put32(out + 2, kFileBytes);
  detail::put32(out + 10, kHeaderBytes);
  detail::put32(out + 14, 40);
  detail::put32(out + 18, kWidth);
  detail::put32(out + 22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(kHeight)));
  detail::put16(out + 26, 1);
  detail::put16(out + 28, 16);
  detail::put32(out + 30, 3);
  detail::put32(out + 34, kPixelBytes);
  detail::put32(out + 38, 2835);
  detail::put32(out + 42, 2835);
  detail::put32(out + 54, 0xF800u);
  detail::put32(out + 58, 0x07E0u);
  detail::put32(out + 62, 0x001Fu);
}

// "SHOT_0007.bmp" for n 1..9999; false (out left an empty string) outside that.
constexpr bool file_name(char (&out)[kNameCap], unsigned n) noexcept {
  out[0] = '\0';
  if (n < kFirstNumber || n > kLastNumber) {
    return false;
  }
  const char head[] = "SHOT_";
  std::size_t k = 0;
  for (; head[k] != '\0'; ++k) {
    out[k] = head[k];
  }
  for (unsigned scale = 1000; scale != 0; scale /= 10u) {
    out[k++] = static_cast<char>('0' + (n / scale) % 10u);
  }
  const char tail[] = ".bmp";
  for (std::size_t i = 0; tail[i] != '\0'; ++i) {
    out[k++] = tail[i];
  }
  out[k] = '\0';
  return true;
}

// Why a screenshot is not taken (the strip says NO SCREENSHOT: <text>), and the words of the success and the failure.
enum class Refusal : std::uint8_t { None, NoCard, OnComputer, Unwritable, Busy, ConsoleShot, InProgress, FolderFull };

constexpr const char* refusal_text(Refusal r) noexcept {
  switch (r) {
    case Refusal::None: return "";
    case Refusal::NoCard: return "NO SCREENSHOT: NO CARD";
    case Refusal::OnComputer: return "NO SCREENSHOT: CARD ON THE COMPUTER";
    case Refusal::Unwritable: return "NO SCREENSHOT: CARD NOT WRITABLE";
    case Refusal::Busy: return "NO SCREENSHOT: CARD BUSY";
    case Refusal::ConsoleShot: return "NO SCREENSHOT: CONSOLE SHOT RUNNING";
    case Refusal::InProgress: return "NO SCREENSHOT: ONE IS BEING SAVED";
    case Refusal::FolderFull: return "NO SCREENSHOT: FOLDER FULL";
  }
  return "";
}
inline constexpr char kFailedText[] = "SCREENSHOT FAILED";
inline constexpr char kSavedPrefix[] = "SCREENSHOT SAVED: ";  // + the file's name in upper case

}  // namespace o3sdk::hal::storage::shot
