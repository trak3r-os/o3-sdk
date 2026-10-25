// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FILLRATE, the band: what is drawn on the scan lines under test, how many display-list words that takes and how many pixels a line
// carries. Pure logic (no display, no renderer state), shared by the app and its test.
//
// The FT812 has no frame buffer: it walks the whole display list for every scan line and fills the part of each primitive that lies
// on that line, in about 2,112 chip clocks (PCLK 30 MHz, HCYCLE 1056). Its fill rate per pixel is not given in anything the kit cites
// and it has no underrun counter, so a line that takes longer only shows as a broken line. The band is rows kBandY .. kBandY+63 of
// the screen; every row of it carries the same load, the sum of K stacked full-width layers (800 px each) of one kind:
//
//   L8 NEAREST    an 800 x 64 L8 alpha mask, tinted (COLOR_RGB + COLOR_A), source-over blended
//   L8 BILINEAR   the same mask scaled by 1/0.75 with BITMAP_TRANSFORM and the BILINEAR filter (every pixel is four texels blended)
//   L4            an 800 x 64 L4 alpha mask (two pixels a byte), tinted
//   RECT          a filled, translucent rectangle
//   MIX           bitmaps and rectangles alternating (bitmaps rotate L8, L4, L8 BILINEAR): the skin's busiest line is 2,034 bitmap
//                 pixels and 2,174 primitive pixels, so K = 5 (2,400 + 1,600) and K = 6 (2,400 + 2,400) bracket it
//
// A second axis is the cell width: a layer is 800 / W sprites of W px side by side (W = 800: one), so the pixels a line carries stay
// K x 800 while the number of sprites and display-list words grows. Layer j of a bitmap kind reads the source from row j on, so no two
// layers fetch the same bytes on a line.
//
// compose() writes the words of the band once, through a sink: a counting sink gives band_words() (the clamp to the display-list
// budget), the renderer's raw() gives the frame. The count cannot drift from what is drawn.

#include <o3sdk/eve/dl.h>
#include <o3sdk/hal/display.h>
#include <o3sdk/ui/plot/ram_g_map.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace fillrate {

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;

// ---- What is drawn ------------------------------------------------------------------------------------------------------------

enum class Kind : std::uint8_t { L8Nearest, L8Bilinear, L4, Rect, Mix };
inline constexpr unsigned kKindCount = 5;
inline constexpr unsigned kMaxLayers = 32;

inline constexpr unsigned kBandW = static_cast<unsigned>(o3sdk::ui::kScreenW);  // every layer is full width: K x 800 px a line
inline constexpr unsigned kBandY = 200;                                        // the first row of the band
inline constexpr unsigned kBandH = 64;                                         // 200 .. 263
static_assert(kBandY + kBandH < static_cast<unsigned>(o3sdk::ui::kScreenH), "the band is on the screen");

// The cell widths (px) and the ballast steps (display-list words); every width divides kBandW.
inline constexpr unsigned kCellWidths[] = {800, 400, 200, 100, 50};
inline constexpr unsigned kCellWidthCount = sizeof(kCellWidths) / sizeof(kCellWidths[0]);
inline constexpr unsigned kBallastSteps[] = {0, 256, 512, 768, 1024, 1280};
inline constexpr unsigned kBallastCount = sizeof(kBallastSteps) / sizeof(kBallastSteps[0]);

constexpr bool widths_divide_band() noexcept {
  for (const unsigned w : kCellWidths) {
    if (kBandW % w != 0u) {
      return false;
    }
  }
  return true;
}
static_assert(widths_divide_band(), "a layer is a whole number of cells");

// How many layers of each kind a frame carries for `kind` and K.
struct Layers {
  unsigned l8n = 0;
  unsigned l8b = 0;
  unsigned l4 = 0;
  unsigned rect = 0;
  constexpr unsigned bitmaps() const noexcept { return l8n + l8b + l4; }
  constexpr unsigned total() const noexcept { return bitmaps() + rect; }
};

constexpr Layers layers_for(Kind kind, unsigned k) noexcept {
  Layers n;
  switch (kind) {
    case Kind::L8Nearest:
      n.l8n = k;
      break;
    case Kind::L8Bilinear:
      n.l8b = k;
      break;
    case Kind::L4:
      n.l4 = k;
      break;
    case Kind::Rect:
      n.rect = k;
      break;
    case Kind::Mix: {
      const unsigned bitmaps = (k + 1u) / 2u;  // bitmap first: K = 5 is three bitmaps and two rectangles
      n.rect = k / 2u;
      n.l8n = (bitmaps + 2u) / 3u;
      n.l4 = (bitmaps + 1u) / 3u;
      n.l8b = bitmaps / 3u;
      break;
    }
  }
  return n;
}

// The pixels a scan line of the band carries (every layer covers all 800 columns of every band row).
constexpr unsigned bitmap_px(const Layers& n) noexcept { return n.bitmaps() * kBandW; }
constexpr unsigned fill_px(const Layers& n) noexcept { return n.rect * kBandW; }
constexpr unsigned px_per_line(const Layers& n) noexcept { return n.total() * kBandW; }
// The sprites (bitmaps or rectangles) a scan line of the band meets.
constexpr unsigned sprites_per_line(const Layers& n, unsigned cell_w) noexcept { return n.total() * (kBandW / cell_w); }

// ---- The test bitmaps in RAM_G (ram_g_map.h: the end of the spare region) ------------------------------------------------------

// Layer j of a bitmap kind reads the source from row j on (BITMAP_SOURCE moves by one stride a layer), so the source has the band's
// 64 rows plus one per layer that can shift.
inline constexpr unsigned kSourceRows = kBandH + kMaxLayers;
inline constexpr std::uint32_t kL8Stride = kBandW;      // bytes a row
inline constexpr std::uint32_t kL4Stride = kBandW / 2;  // two pixels a byte
inline constexpr std::uint32_t kL8Bytes = kL8Stride * kSourceRows;
inline constexpr std::uint32_t kL4Bytes = kL4Stride * kSourceRows;
inline constexpr std::uint32_t kBitmapBytes = kL8Bytes + kL4Bytes;  // the L8, then the L4: one stream, uploaded in 4 KB chunks
inline constexpr std::uint32_t kL8Addr = o3sdk::ui::plot::kRamGApp;
inline constexpr std::uint32_t kL4Addr = kL8Addr + kL8Bytes;
static_assert(kBitmapBytes <= o3sdk::ui::plot::kRamGAppBytes, "the bitmaps fit the app's region of ram_g_map.h");
static_assert(kL8Addr % o3sdk::hal::kUploadAlign == 0 && kL4Addr % o3sdk::hal::kUploadAlign == 0 && kBitmapBytes % o3sdk::hal::kUploadAlign == 0 &&
                  kL8Stride % 4u == 0 && kL4Stride % 4u == 0,
              "the bitmaps and every row of them are on a word");
static_assert(kL8Stride < 1024 && kSourceRows < 512, "BITMAP_LAYOUT's stride and height fields hold them (no _H)");
static_assert(kBandW > 511, "the 800 px width needs BITMAP_SIZE_H");

// The bitmap handles of the band: 9, 10 and 11, which ram_g_map.h gives to the wave lanes and the boot logo; this app has neither, and
// binds its three at the start of every frame it draws a band.
inline constexpr std::uint8_t kHandleL8 = o3sdk::ui::plot::kHandleWaveLane0;
inline constexpr std::uint8_t kHandleL4 = o3sdk::ui::plot::kHandleWaveLane1;
inline constexpr std::uint8_t kHandleBilinear = o3sdk::ui::plot::kHandleBootLogo;

// The test pictures: diagonal triangle waves, so that every row differs from its neighbours (a repeated or shifted line breaks the
// stripes) and the alpha takes every value (an alpha-only shortcut would show).
constexpr unsigned triangle(unsigned phase, unsigned period) noexcept {  // 0 .. 255
  const unsigned half = period / 2u;
  const unsigned p = phase % period;
  return (p < half ? p : period - p) * 255u / half;
}
constexpr unsigned l8_pixel(unsigned x, unsigned y) noexcept { return triangle(x + 2u * y, 96u); }
constexpr unsigned l4_pixel(unsigned x, unsigned y) noexcept { return triangle(x + 3u * y, 64u) >> 4; }  // 0 .. 15

// Bytes [offset, offset + n) of the stream the two bitmaps make (L8 rows, then L4 rows with the left pixel in the high nibble).
inline void generate(std::uint8_t* out, std::size_t n, std::uint32_t offset) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t o = offset + static_cast<std::uint32_t>(i);
    if (o < kL8Bytes) {
      out[i] = static_cast<std::uint8_t>(l8_pixel(o % kL8Stride, o / kL8Stride));
    } else if (o < kBitmapBytes) {
      const std::uint32_t p = o - kL8Bytes;
      const unsigned x = (p % kL4Stride) * 2u;
      const unsigned y = p / kL4Stride;
      out[i] = static_cast<std::uint8_t>(l4_pixel(x, y) << 4 | l4_pixel(x + 1u, y));
    } else {
      out[i] = 0;
    }
  }
}

// ---- The words ----------------------------------------------------------------------------------------------------------------

inline constexpr std::uint8_t kLayerAlpha = 96;  // COLOR_A of every layer: translucent, so every layer blends with what is under it
inline constexpr std::int32_t kScale = 192;      // BITMAP_TRANSFORM_A and _E of the BILINEAR layers: 0.75 in 8.8, a 4/3 enlargement

// The simulator does not model BILINEAR with a BITMAP_TRANSFORM and refuses the frame (sim/src/eve_raster.cpp), so the host build
// draws the same words with NEAREST in the BITMAP_SIZE word: the list has the same length, only the filter bit differs.
#if defined(__arm__)
inline constexpr Filter kBilinearFilter = Filter::Bilinear;
inline constexpr bool kBilinearIsModelled = true;
#else
inline constexpr Filter kBilinearFilter = Filter::Nearest;
inline constexpr bool kBilinearIsModelled = false;
#endif

constexpr std::uint32_t tint_word(unsigned index) noexcept {
  constexpr std::uint8_t kTints[8][3] = {{0x3A, 0x7B, 0xD5}, {0xD5, 0x6B, 0x3A}, {0x4F, 0xC1, 0x7E}, {0xB0, 0x5A, 0xD0},
                                         {0xD0, 0xC0, 0x40}, {0x3F, 0xC0, 0xC8}, {0xD0, 0x50, 0x70}, {0x90, 0x90, 0xA0}};
  const auto& t = kTints[index % 8u];
  return dl::color_rgb(t[0], t[1], t[2]);
}

// One run of bitmap layers on one handle. VERTEX2F draws at (x, y) in 1/16 px with the handle and cell that BITMAP_HANDLE and CELL
// selected (VERTEX2II cannot reach x >= 512 without VERTEX_TRANSLATE_X, which the guide does not say moves it: renderer.h).
template <class Sink>
constexpr void bitmap_group(Sink& s, unsigned count, std::uint8_t handle, BitmapFormat format, std::uint32_t addr, std::uint32_t stride,
                            Filter filter, bool scaled, unsigned cell_w, unsigned& tint) noexcept {
  s(dl::bitmap_handle(handle));
  s(dl::bitmap_source(addr));
  s(dl::bitmap_layout(format, stride, kBandH));
  s(dl::bitmap_size(filter, Wrap::Border, Wrap::Border, cell_w, kBandH));
  s(dl::bitmap_size_h(cell_w >> 9, 0));
  s(dl::cell(0));
  if (scaled) {
    s(dl::bitmap_transform_a(kScale));
    s(dl::bitmap_transform_e(kScale));
  }
  s(dl::color_a(kLayerAlpha));
  s(dl::begin(Primitive::Bitmaps));
  for (unsigned j = 0; j < count; ++j) {
    s(dl::bitmap_source(addr + j * stride));
    s(tint_word(tint++));
    for (unsigned x = 0; x < kBandW; x += cell_w) {
      s(dl::vertex2f(static_cast<std::int32_t>(x * 16u), static_cast<std::int32_t>(kBandY * 16u)));
    }
  }
  s(dl::end());
}

// A run of rectangle layers: RECTS at the renderer's LINE_WIDTH 8, the vertices on the centres of the first and last pixel covered.
template <class Sink>
constexpr void rect_group(Sink& s, unsigned count, unsigned cell_w, unsigned& tint) noexcept {
  s(dl::color_a(kLayerAlpha));
  s(dl::begin(Primitive::Rects));
  for (unsigned j = 0; j < count; ++j) {
    s(tint_word(tint++));
    for (unsigned x = 0; x < kBandW; x += cell_w) {
      s(dl::vertex2f(static_cast<std::int32_t>(x * 16u + 8u), static_cast<std::int32_t>(kBandY * 16u + 8u)));
      s(dl::vertex2f(static_cast<std::int32_t>((x + cell_w - 1u) * 16u + 8u), static_cast<std::int32_t>((kBandY + kBandH - 1u) * 16u + 8u)));
    }
  }
  s(dl::end());
}

// The band inside a raw section (the renderer has done SAVE_CONTEXT; its RESTORE_CONTEXT takes the alpha and the transform back).
// `sink(word)` takes each word. The bitmaps that are not scaled come first and the BILINEAR group last, so the transform is set once.
template <class Sink>
constexpr void compose(Sink& s, const Layers& n, unsigned cell_w) noexcept {
  unsigned tint = 0;
  if (n.l8n != 0u) {
    bitmap_group(s, n.l8n, kHandleL8, BitmapFormat::L8, kL8Addr, kL8Stride, Filter::Nearest, false, cell_w, tint);
  }
  if (n.l4 != 0u) {
    bitmap_group(s, n.l4, kHandleL4, BitmapFormat::L4, kL4Addr, kL4Stride, Filter::Nearest, false, cell_w, tint);
  }
  if (n.rect != 0u) {
    rect_group(s, n.rect, cell_w, tint);
  }
  if (n.l8b != 0u) {
    bitmap_group(s, n.l8b, kHandleBilinear, BitmapFormat::L8, kL8Addr, kL8Stride, kBilinearFilter, true, cell_w, tint);
  }
}

struct CountSink {
  unsigned words = 0;
  constexpr void operator()(std::uint32_t) noexcept { ++words; }
};

// Words the band costs in a frame, the raw section's own overhead included (0 for no layers: the section is not opened).
constexpr unsigned band_words(const Layers& n, unsigned cell_w) noexcept {
  if (n.total() == 0u) {
    return 0;
  }
  CountSink c;
  compose(c, n, cell_w);
  return c.words + o3sdk::ui::Renderer::kRawOverheadWords;
}

// The most layers (up to `k`) of `kind` whose band fits in `avail` words: the clamp that keeps a frame inside the display list.
constexpr unsigned fit_layers(Kind kind, unsigned k, unsigned cell_w, unsigned avail) noexcept {
  while (k > 0u && band_words(layers_for(kind, k), cell_w) > avail) {
    --k;
  }
  return k;
}

// ---- The display list ---------------------------------------------------------------------------------------------------------

// A frame is the runtime's header, then this screen (the band, the ballast, the references, the text), then the runtime's soft-key row;
// Renderer::end() closes it with the END of the primitive the row leaves open and the builder with DISPLAY (kListClosingWords), and RAM_DL
// holds 2,048 words, so Renderer::words() may reach kWordsLimit. The band and the ballast take what the rest leaves: kTailWords bounds
// what the screen draws after them (the references and the text, at the longest strings: 333 words of text and about 50 of references,
// measured in the simulator), kKeysRowWords is the soft-key row, which is constant because the labels never change and exactly one key
// (the kind's) is in force; tests/fillrate_test.cpp counts it with the kit's own calls.
inline constexpr std::size_t kListClosingWords = 2;
inline constexpr std::size_t kWordsLimit = o3sdk::eve::kMaxDisplayListWords - kListClosingWords;
inline constexpr std::size_t kKeysRowWords = 87;
inline constexpr std::size_t kTailWords = 448;
inline constexpr const char* kSoftKeyLabels[8] = {"L8 NEAR", "L8 BILIN", "L4", "RECT", "MIX", "CELL W", "BALLAST", "K = 0"};
static_assert(kKeysRowWords + kTailWords < kWordsLimit / 2, "the band keeps most of the list");

}  // namespace fillrate
