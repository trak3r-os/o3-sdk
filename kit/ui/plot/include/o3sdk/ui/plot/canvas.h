// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app's bitmaps and shapes. Bitmap handle 0 is the app's: it holds the bitmaps an app uploads into its own RAM_G region (ram_g_map.h,
// kRamGApp; kit/app app_assets.h puts them there at boot), and the kit leaves it alone. Every other handle the kit draws with (assets.h: the
// glyph faces 1..4 and the tiles 5..8, 12..14; ram_g_map.h: the wave lanes 9, 10 and the boot logo 11) is named there and differs from it, and 15
// is the coprocessor's. A glyph face's handle is generated (font_convert.py, 1 + the position in ui::Font), so tests/handle_app_test.cpp checks
// the four. The simulator counts every list that touches handle 0 (sim::EveRaster::handle_app_uses()); a kit app that has no Canvas reads zero.
//
// Canvas is the app's way to draw with it: sprites from handle 0 and the shapes the Renderer has no call for (discs, lines, rounded boxes, alpha),
// in one raw section of a Renderer (renderer.h, "Raw sections") the way the wave view draws. It is a layer over the Renderer's public API; the
// Renderer is not touched. Text stays outside the section, through Renderer::text() (nothing is re-implemented here).
//
// Words (the budget a skinned frame is measured against; docs/FRAMEWORK.md, "The app's bitmaps and shapes"): open() and close() are SAVE_CONTEXT and RESTORE_CONTEXT, 2 words, and
// begin_raw() adds END and VERTEX_TRANSLATE_X 0 (0 to 2) when the Renderer had a primitive open or a moved origin; the Renderer's next call after
// close() re-emits its COLOR_RGB and BEGIN (0 to 2), which end_raw() made it forget. Inside: a bind is 6 words; a sprite 1 word (VERTEX2II; 3 on
// the VERTEX2F path); a point 1; a line 2; a rect or a round_rect 2; plus the state each call changes (COLOR_RGB, COLOR_A, POINT_SIZE,
// LINE_WIDTH, BEGIN, SCISSOR_XY and _SIZE, VERTEX_TRANSLATE_X), each emitted only when it differs from the section's last, so draw by layer
// and colour. No END is emitted between primitives: of the four the Canvas uses none holds anything back until END (the strips do).

#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/plot/ram_g_map.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui::plot {

inline constexpr std::uint8_t kHandleApp = 0;

static_assert(kHandleApp != kHandleHexLo && kHandleApp != kHandleHexHi && kHandleApp != kHandleNotes && kHandleApp != kHandleIcons &&
                  kHandleApp != kHandleWaveLane0 && kHandleApp != kHandleWaveLane1 && kHandleApp != kHandleBootLogo &&
                  kHandleApp != kHandleNotesHi && kHandleApp != kHandleSmallHex && kHandleApp != kHandleSmallNotes && kHandleApp != 15,
              "the app's handle is none of the kit's, and not the coprocessor's");

// A bitmap in the app's RAM_G region, drawn through kHandleApp.
struct BitmapDesc {
  std::uint32_t ram_g;         // absolute RAM_G address, a multiple of 4
  eve::BitmapFormat format;    // L1, L2, L4 or L8
  std::uint16_t stride;        // bytes a row, < 4096
  std::uint16_t cell_w, cell_h;  // one cell; cells follow each other (stride * cell_h bytes)
  std::uint16_t draw_w, draw_h;  // drawn size, 1..2047: the cell's, or wider with Wrap::Repeat
  eve::Wrap wrap_x = eve::Wrap::Border;
  eve::Wrap wrap_y = eve::Wrap::Border;
};

constexpr bool operator==(const BitmapDesc& a, const BitmapDesc& b) noexcept {
  return a.ram_g == b.ram_g && a.format == b.format && a.stride == b.stride && a.cell_w == b.cell_w && a.cell_h == b.cell_h && a.draw_w == b.draw_w &&
         a.draw_h == b.draw_h && a.wrap_x == b.wrap_x && a.wrap_y == b.wrap_y;
}
constexpr bool operator!=(const BitmapDesc& a, const BitmapDesc& b) noexcept { return !(a == b); }

// Pixels in a byte of `f`: 8, 4, 2 or 1 for L1, L2, L4 and L8; 0 for a format a BitmapDesc cannot have.
constexpr unsigned bitmap_pixels_per_byte(eve::BitmapFormat f) noexcept {
  switch (f) {
    case eve::BitmapFormat::L1: return 8;
    case eve::BitmapFormat::L2: return 4;
    case eve::BitmapFormat::L4: return 2;
    case eve::BitmapFormat::L8: return 1;
    default: return 0;
  }
}

// True when `d` can be drawn: one of the four formats, `ram_g` aligned and the first cell inside the app's region (kRamGApp, kRamGAppBytes), the
// row wide enough for the cell, the
// sizes in the registers' range (stride below 4,096, cell height and drawn size below 2,048, none zero), and Wrap::Repeat only on an axis the
// chip can wrap: x needs a power-of-two cell width that is also the whole row (stride x pixels a byte: the chip wraps at the row), y a
// power-of-two cell height. An app static_asserts its descs with it; Canvas::bind() checks it.
constexpr bool bitmap_desc_valid(const BitmapDesc& d) noexcept {
  const unsigned ppb = bitmap_pixels_per_byte(d.format);
  if (ppb == 0 || d.ram_g % 4u != 0 || d.stride == 0 || d.stride >= 4096u || d.cell_w == 0 || d.cell_h == 0 || d.cell_h >= 2048u) {
    return false;
  }
  if (d.draw_w == 0 || d.draw_w >= 2048u || d.draw_h == 0 || d.draw_h >= 2048u) {
    return false;
  }
  const unsigned row_px = unsigned{d.stride} * ppb;
  if (row_px < d.cell_w || d.ram_g < kRamGApp || std::uint64_t{d.ram_g} + std::uint64_t{d.stride} * d.cell_h > std::uint64_t{kRamGApp} + kRamGAppBytes) {
    return false;
  }
  const auto pow2 = [](unsigned v) { return v != 0 && (v & (v - 1u)) == 0; };
  if (d.wrap_x == eve::Wrap::Repeat && !(pow2(d.cell_w) && row_px == d.cell_w)) {
    return false;
  }
  if (d.wrap_y == eve::Wrap::Repeat && !pow2(d.cell_h)) {
    return false;
  }
  return true;
}

// Draws inside raw sections of `r`, after r.begin(). Between open() and close() only the Canvas emits words: a Renderer drawing call there would draw
// with state the Canvas changed behind the Renderer's private caches (colour, primitive, origin). A check, like the raw-section one, aborts debug and
// host builds when r.words() moved between two Canvas calls, when a call is made outside a section, when sections nest, or when a desc is not
// valid, or when a sprite's cell would end past the app's region; the MK66's release build leaves it out. Text goes outside, through
// Renderer::text(). One Canvas lives for one frame's draw(). Every cache lasts one section, the bound desc too: dl.h puts handle 0's properties
// outside the graphics context, but renderer.cpp counts on RESTORE_CONTEXT undoing a bind, and only the unit can say which (SU1), so each
// section binds again (6 words; one section a frame is the plan). Every vertex is in screen coordinates; the Canvas keeps its own origin (VERTEX_TRANSLATE_X) like the Renderer's.
class Canvas {
 public:
  explicit Canvas(Renderer& r) noexcept : r_(r) {}

  void open() noexcept;   // r.begin_raw(): SAVE_CONTEXT, plus END and VERTEX_TRANSLATE_X 0 when the Renderer had them pending
  void close() noexcept;  // r.end_raw(): RESTORE_CONTEXT brings back colour, alpha, line width, point size, scissor and translation

  // Six words, always: BITMAP_HANDLE, _SOURCE, _LAYOUT, _LAYOUT_H, _SIZE, _SIZE_H. Both _H words go out even when zero: a handle's properties persist
  // across binds and frames, so a bind that skipped them would inherit the high bits of the bind before. After the 800-wide texture, a small bitmap
  // would get a box 512 px wider: invisible in a PNG (Border wrap), but walked by the chip. Nothing when `d` (by value) is already bound in this section.
  void bind(const BitmapDesc& d) noexcept;
  // Cell 0..127 of the bitmap bound last, top-left at (x, y) in screen pixels, tinted by `rgb` at `alpha`: one VERTEX2II, moving the Canvas's own origin
  // for x > 511; BITMAP_HANDLE + CELL + VERTEX2F for x < 0 or y outside 0..511, or always after use_translate(false) (the Renderer's plan B, [A] in renderer.h).
  void sprite(unsigned cell, int x, int y, Rgb rgb, std::uint8_t alpha = 255) noexcept;
  // A disc of radius `radius_q4` (POINT_SIZE, at most 8191) centred on (cx_q4, cy_q4), all in 1/16 px.
  void point(int cx_q4, int cy_q4, unsigned radius_q4, Rgb rgb, std::uint8_t alpha = 255) noexcept;
  // A line from (x0_q4, y0_q4) to (x1_q4, y1_q4) in 1/16 px, round ends. `w_q4` is LINE_WIDTH, the distance from the line's centre to its edge (at most 4095).
  void line(int x0_q4, int y0_q4, int x1_q4, int y1_q4, unsigned w_q4, Rgb rgb, std::uint8_t alpha = 255) noexcept;
  // The Renderer's exact-pixel box (RECTS at LINE_WIDTH 8, as Renderer::panel draws it): the pixels [r.x, r.x + r.w) x [r.y, r.y + r.h). Nothing for an empty rect.
  void rect(const Rect& r, Rgb rgb, std::uint8_t alpha = 255) noexcept;
  // Corners of radius `radius_q4` (1/16 px), clamped to half the shorter side (radius_q4 <= 8 * min(w, h)) and raised to 8 (half a pixel, rect()'s corner);
  // covers `r` exactly (RECTS grows by LINE_WIDTH on every side, so the vertices are inset by it). Nothing for an empty rect.
  void round_rect(const Rect& r, unsigned radius_q4, Rgb rgb, std::uint8_t alpha = 255) noexcept;
  // Clips everything after it, until scissor_reset() or close() (RESTORE_CONTEXT brings the scissor back). An empty or off-screen rect clips everything.
  void scissor(const Rect& r) noexcept;
  void scissor_reset() noexcept;
  // Whether bitmaps past x = 511 use VERTEX_TRANSLATE_X (the default), as Renderer::use_translate(); may be changed between sections.
  void use_translate(bool on) noexcept { translate_ = on; }

 private:
  void check() const noexcept;
  void sync() noexcept { words_ = r_.words(); }
  void emit(std::uint32_t word) noexcept { r_.raw(word); }
  void use(eve::Primitive prim) noexcept;
  void style(Rgb rgb, std::uint8_t alpha) noexcept;
  void set_line_width(unsigned w_q4) noexcept;
  bool place(int x) noexcept;
  void box(const Rect& r, unsigned radius_q4, Rgb rgb, std::uint8_t alpha) noexcept;
  void set_scissor(unsigned x, unsigned y, unsigned w, unsigned h) noexcept;

  Renderer& r_;
  std::size_t words_ = 0;  // r_.words() after the Canvas's last call: a Renderer call in between moved it
  bool in_section_ = false;
  bool translate_ = true;
  // The section's: reset by open() and close().
  bool has_bound_ = false;
  BitmapDesc bound_{};
  bool has_prim_ = false;
  eve::Primitive prim_ = eve::Primitive::Bitmaps;
  bool has_color_ = false;
  Rgb color_{0, 0, 0};
  std::uint8_t alpha_ = 255;
  bool has_point_size_ = false;
  unsigned point_size_ = 0;
  unsigned line_width_ = 8;
  int origin_ = 0;  // VERTEX_TRANSLATE_X in whole pixels
  unsigned scissor_x_ = 0, scissor_y_ = 0, scissor_w_ = 2048, scissor_h_ = 2048;
  bool handle_set_ = false;  // BITMAP_HANDLE is kHandleApp (known), for the VERTEX2F path
  bool has_cell_ = false;
  unsigned cell_ = 0;
};

}  // namespace o3sdk::ui::plot
