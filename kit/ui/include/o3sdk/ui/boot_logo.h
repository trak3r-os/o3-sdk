// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The boot logo's bitmap (OS-003f): an FT81x bitmap in flash that the boot screen (ui/boot_screen.h) draws centred on black. What
// the draw code needs to know about it (size, format, stride, scale, palette, tint) is DATA, in the BootLogo that boot_logo()
// returns, not a constant of one picture; the limits every logo keeps are the constants below.
//
//   * The default is the maintainer's O3 logo (logo/o3.png at the root of o3-sdk). tools/logo_convert.py turns it into
//     src/boot_logo_data.cpp, which is committed; the ctest `logo_convert_fresh` regenerates it and fails when it differs, so the
//     two cannot drift. The default defines boot_logo() WEAK.
//   * An app gives its own with o3sdk_app(<name> ... LOGO <png> [LOGO_COLOR 0xRRGGBB]) (docs/FRAMEWORK.md): the build runs the same
//     converter on that PNG into the app's build directory and the generated file defines boot_logo() STRONG, so the linker takes
//     it and not the default: one definition in an image, no duplicate symbol, nothing else to register.
//   * Grey logos are L1, L2 or L4 alpha masks drawn through COLOR_RGB (`color`, white by default: a tint). A logo with colour is a
//     PALETTED565 bitmap, an 8-bit index a pixel into a palette of at most 256 RGB565 colours that follows the index bitmap in the
//     same blob and is drawn in one pass with PALETTE_SOURCE.
//   * The generated file static_asserts boot_logo_valid() and that the limits below are the converter's own: a logo over a limit
//     fails the build, and so does a header that disagrees with the tool.
//
// Its place in RAM_G is ui/plot/ram_g_map.h (kRamGBootLogo, kBootLogoMaxBytes of it); the picture is shown scaled by the integer
// `scale` with BITMAP_TRANSFORM and nearest sampling.

#include <o3sdk/eve/dl.h>

#include <cstdint>

namespace o3sdk::ui {

// The limits. Drawn size (native x scale): the panel's width, and 420 px of its 480 so that the line under the logo (boot_screen.h)
// fits. Bytes: everything uploaded, the bitmap, its padding to a word and the palette; the RAM_G region reserves this much, which
// is also the most flash a logo costs.
inline constexpr unsigned kBootLogoMaxDrawW = 800;
inline constexpr unsigned kBootLogoMaxDrawH = 420;
inline constexpr unsigned kBootLogoMaxScale = 16;  // BITMAP_TRANSFORM's coefficient 256 / scale stays at least 16
inline constexpr unsigned kBootLogoMaxBytes = 64 * 1024;
inline constexpr unsigned kBootLogoPaletteEntries = 256;
inline constexpr unsigned kBootLogoPaletteBytes = kBootLogoPaletteEntries * 2;  // RGB565
inline constexpr std::uint32_t kBootLogoWhite = 0xFFFFFF;

struct BootLogo {
  const std::uint8_t* bytes;     // word aligned (the generated array is alignas(4)): the bitmap, then the palette (palette_offset)
  std::uint32_t size;            // bytes uploaded: the bitmap, its padding to a word, the palette; a whole number of words
  std::uint16_t width;           // native pixels
  std::uint16_t height;
  std::uint16_t stride;          // bytes a row (BITMAP_LAYOUT): the first pixel in the high bits; L1 / L2 / L4 / PALETTED565 only
  eve::BitmapFormat format;
  std::uint8_t scale;            // integer enlargement, 1..kBootLogoMaxScale
  std::uint32_t palette_offset;  // byte offset of the 256-entry RGB565 palette in `bytes` (Paletted565 only, else 0)
  std::uint32_t color;           // 0xRRGGBB: COLOR_RGB at draw time for a grey logo; a palette is drawn as it is
};

// Bits a pixel of a format a logo may have; 0 for any other.
constexpr unsigned boot_logo_bits(eve::BitmapFormat format) noexcept {
  switch (format) {
    case eve::BitmapFormat::L1:
      return 1;
    case eve::BitmapFormat::L2:
      return 2;
    case eve::BitmapFormat::L4:
      return 4;
    case eve::BitmapFormat::Paletted565:
      return 8;
    default:
      return 0;
  }
}

// True when `l` keeps every limit above and is consistent with itself: a known format, the stride of its width, the drawn size
// on the panel, the BITMAP_LAYOUT fields (stride under 1024 bytes, height under 512 rows) and the blob exactly the bitmap padded
// to a word plus its palette, at most kBootLogoMaxBytes. (BITMAP_SIZE's drawn width of 512 and more takes BITMAP_SIZE_H; the
// boot screen emits it.)
constexpr bool boot_logo_valid(const BootLogo& l) noexcept {
  const unsigned bits = boot_logo_bits(l.format);
  const unsigned w = l.width, h = l.height, stride = l.stride, scale = l.scale;
  if (l.bytes == nullptr || bits == 0 || w == 0 || h == 0 || scale == 0 || scale > kBootLogoMaxScale) {
    return false;
  }
  if (stride != (w * bits + 7u) / 8u || stride >= 1024u || h >= 512u) {
    return false;
  }
  if (w * scale > kBootLogoMaxDrawW || h * scale > kBootLogoMaxDrawH) {
    return false;
  }
  const std::uint32_t bitmap = (stride * h + 3u) / 4u * 4u;
  const bool paletted = l.format == eve::BitmapFormat::Paletted565;
  return l.size <= kBootLogoMaxBytes && l.palette_offset == (paletted ? bitmap : 0u) &&
         l.size == bitmap + (paletted ? kBootLogoPaletteBytes : 0u) && l.color <= 0xFFFFFFu;
}

// The logo of this image: the one the app's LOGO option generated, else the default. Defined by a generated file (above).
BootLogo boot_logo() noexcept;

}  // namespace o3sdk::ui
