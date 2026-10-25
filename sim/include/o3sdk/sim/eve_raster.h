// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Software interpreter of the FT81x display-list subset the UI emits (docs/specs/OS-001-walking-skeleton.md,
// extended by OS-003a: points, line strips, edge strips, VERTEX_TRANSLATE, BLEND_FUNC and antialiased LINES /
// LINE_STRIP / POINTS), producing an 800x480 RGBA framebuffer. The pixels are what the display-list words mean,
// not what a particular FT812 batch shows: the fidelity gaps, and the antialiasing coverage model, are listed in
// sim/README.md. Host only.
//
// Every word is decoded from its 32-bit encoding (eve/dl.h, FT81X Series Programmer's Guide v1.2). A word
// outside the subset, a malformed list or an access outside RAM_G fails the frame loudly (render() returns
// false and error() says why); nothing is silently skipped.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o3sdk::sim {

// One bitmap draw, as recorded for the legibility sweep (sim/legibility.h): which bitmap, where, in which colour, under which scissor, and
// which of its pixels have ink (a non-zero coverage), with the picture under each one. A transformed bitmap (the boot logo) is not recorded.
struct GlyphDraw {
  std::uint8_t handle = 0;
  std::uint16_t cell = 0;
  std::uint32_t source = 0;           // BITMAP_SOURCE: the RAM_G address of cell 0, which tells a face from a tile or an icon
  std::int32_t x = 0, y = 0;          // the drawn rectangle's top-left corner, in screen pixels (may lie outside the screen)
  std::uint16_t w = 0, h = 0;         // BITMAP_SIZE: the drawn rectangle
  std::uint8_t r = 0, g = 0, b = 0, a = 0;                                 // COLOR_RGB, COLOR_A
  std::uint32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;  // the scissor in force
  std::int32_t ink_x0 = 0, ink_y0 = 0, ink_x1 = 0, ink_y1 = 0;             // the bounding box of all the ink, end exclusive (empty: x0 >= x1)
  std::uint32_t ink_total = 0;        // pixels with ink, wherever they fall
  std::uint32_t ink_first = 0;        // the pixels with ink inside the scissor and the screen: ink_visible entries of glyph_ink(), from here
  std::uint32_t ink_visible = 0;
};

// A visible pixel of a recorded bitmap: the framebuffer index (y * kWidth + x), the colour that was under it before the bitmap was blended, and which
// recorded bitmap's ink that was (an index of glyph_log(), -1: none: a fill, or nothing, so a panel drawn in between hides the ink of an earlier bitmap).
struct GlyphInk {
  std::uint32_t pixel;
  std::uint8_t r, g, b;
  std::int32_t under;
};

class EveRaster {
 public:
  static constexpr int kWidth = 800;
  static constexpr int kHeight = 480;
  static constexpr std::size_t kFramebufferBytes = static_cast<std::size_t>(kWidth) * kHeight * 4;
  static constexpr std::size_t kRamGBytes = 1u << 20;  // FT81x RAM_G: 1 MiB (hal::kRamGBytes, asserted equal in raster_display.h)

  EveRaster();

  // Executes one display list (up to and including DISPLAY) into a fresh frame: every frame starts from
  // black and the default graphics context, so a list without CLEAR shows black where it does not draw.
  // On success the frame replaces the visible one and true is returned; on error the visible frame is
  // unchanged, false is returned and error() describes the first problem.
  bool render(const std::uint32_t* words, std::size_t count);

  const std::string& error() const noexcept { return error_; }

  // The visible frame: kWidth x kHeight RGBA8, row-major, alpha always 255.
  const std::uint8_t* rgba() const noexcept { return front_.data(); }
  std::size_t frames() const noexcept { return frames_; }

  // RAM_G, where bitmap data (the font) lives. Persists across frames; zero-filled at construction.
  std::uint8_t* ram_g() noexcept { return ram_g_.data(); }
  const std::uint8_t* ram_g() const noexcept { return ram_g_.data(); }

  // Copies `count` bytes to RAM_G at `addr`; false (nothing written) if they do not lie inside it. The visible
  // frame is not redrawn: the new bytes show from the next render().
  bool write_ram_g(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count);

  // The bitmap log (OS-E2E-5, the legibility sweep). Off by default, so a plain run draws as fast as before. With
  // record_glyphs(n), n > 0, every untransformed bitmap draw whose BITMAP_SOURCE is below RAM_G address n (the UI's faces, tiles
  // and icons end at ui::kRamGEnd; the wave lanes lie above) is logged by the next render() calls. Each render() starts a fresh log:
  // glyph_log() and glyph_ink() describe the last list drawn, in draw order. record_glyphs(0) turns it off and clears it.
  void record_glyphs(std::uint32_t ram_g_below);
  const std::vector<GlyphDraw>& glyph_log() const noexcept { return glyph_log_; }
  const std::vector<GlyphInk>& glyph_ink() const noexcept { return glyph_ink_; }
  // Per framebuffer pixel: the index of the recorded bitmap whose ink is on the picture there (the last thing drawn on it was that bitmap's ink), -1 if
  // anything else was drawn last or nothing was. Empty while the log is off.
  const std::vector<std::int32_t>& ink_owner() const noexcept { return ink_owner_; }

 private:
  enum class Prim : std::uint8_t { None, Bitmaps, Points, Lines, LineStrip, EdgeStripA, EdgeStripB, Rects };

  // The graphics context that SAVE_CONTEXT / RESTORE_CONTEXT push and pop (guide section 4.1, Table 5): colour,
  // clear colour, line width, point size, blend function, vertex format and translation, scissor, cell, the current
  // bitmap handle and the bitmap transform (OS-003f). CLEAR_COLOR_A is not in the subset (clears are opaque).
  struct Context {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;  // COLOR_RGB, COLOR_A
    std::uint8_t clear_r = 0, clear_g = 0, clear_b = 0;  // CLEAR_COLOR_RGB
    std::uint32_t line_width = 16;                    // 1/16 px
    std::uint32_t point_size = 16;                    // 1/16 px, the radius
    std::uint32_t blend_src = 2, blend_dst = 4;       // BLEND_FUNC: SRC_ALPHA, ONE_MINUS_SRC_ALPHA (Table 8)
    std::int32_t translate_x = 0, translate_y = 0;    // VERTEX_TRANSLATE_X / _Y, 1/16 px
    std::uint32_t vertex_frac = 4;                    // VERTEX_FORMAT
    std::uint32_t scissor_x = 0, scissor_y = 0;
    std::uint32_t scissor_w = 2048, scissor_h = 2048;
    std::uint32_t cell = 0;
    std::uint32_t handle = 0;                         // BITMAP_HANDLE: which handle BITMAP_* and VERTEX2F use
    // BITMAP_TRANSFORM_A..F as the raw fields (guide section 4.12-4.17): A, B, D, E signed 8.8 (17 bits), C and F signed
    // 15.8 (24 bits); the identity is A = E = 256. A bitmap pixel at (x, y) shows the source pixel
    // (A x + B y + C, D x + E y + F) / 256, floored (draw_bitmap).
    std::int32_t xf_a = 256, xf_b = 0, xf_c = 0, xf_d = 0, xf_e = 256, xf_f = 0;
  };

  // One bitmap handle's state. BITMAP_LAYOUT / BITMAP_SIZE set the low bits and the _H commands the high
  // bits; both persist until set again. The parameters are not part of the saved context (guide section 4.1);
  // which handle is current is.
  struct Handle {
    std::uint32_t source = 0;
    std::uint32_t format = 0;
    std::uint32_t stride_lo = 0, stride_hi = 0;
    std::uint32_t layout_h_lo = 0, layout_h_hi = 0;
    std::uint32_t wrap_x = 0, wrap_y = 0;
    std::uint32_t width_lo = 0, width_hi = 0;
    std::uint32_t height_lo = 0, height_hi = 0;
    std::uint32_t filter = 0;  // BITMAP_SIZE bit 20: NEAREST (0) or BILINEAR (1)
  };

  bool exec(std::uint32_t word);
  bool vertex(std::int32_t x_q4, std::int32_t y_q4);
  bool fail(const char* what, std::uint32_t word);

  // Vertices are 1/16 px after VERTEX_TRANSLATE. `coverage` is how much of each pixel the shape covers (255 inside,
  // a bitmap sample, or an antialiased edge); it is scaled by COLOR_A and blended with the BLEND_FUNC.
  void fill_pixels(int x0, int y0, int x1, int y1, std::uint8_t coverage);  // [x0,x1) x [y0,y1), clipped
  void store_pixels(int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b);  // CLEAR
  bool clip(int& x0, int& y0, int& x1, int& y1) const;  // to the scissor and the screen; false if nothing is left
  void draw_rect_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1);
  bool draw_capsule_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t radius_q4,
                       bool collect = false);
  bool add_edge_segment_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1);
  void flush_strip();
  void discard_strip();
  bool draw_bitmap(int px, int py, std::uint32_t handle, std::uint32_t cell);

  std::uint32_t record_below_ = 0;      // record_glyphs(): 0 off
  std::vector<GlyphDraw> glyph_log_;
  std::vector<GlyphInk> glyph_ink_;
  std::vector<std::int32_t> ink_owner_;
  bool in_glyph_ = false;               // a recorded bitmap is filling its own ink: that is not a cover
  void release_ink(int x0, int y0, int x1, int y1);  // a fill covers the ink of the recorded bitmaps in [x0,x1) x [y0,y1)

  std::vector<std::uint8_t> work_;   // frame being drawn
  std::vector<std::uint8_t> front_;  // last good frame
  std::vector<std::uint8_t> ram_g_;
  std::string error_;
  std::size_t frames_ = 0;

  // Interpreter state, reset at the start of every frame except `handles_` and RAM_G.
  Context ctx_;
  Context stack_[4];
  unsigned depth_ = 0;
  Prim prim_ = Prim::None;
  bool have_first_ = false;
  std::int32_t first_x_ = 0, first_y_ = 0;  // pending vertex of a RECTS / LINES pair or a strip, 1/16 px
  // A strip (LINE_STRIP, EDGE_STRIP_A/B) is collected and blended once at END / the next BEGIN / DISPLAY, with the
  // context of its first vertex, so segments that overlap are not blended twice. An edge strip keeps, per pixel
  // column, the last row to fill (_A) or the first (_B); a line strip keeps, per pixel, the OR of the sample masks
  // of its segments (sample_mask() in eve_raster.cpp), so a pixel is covered as the union of the capsules covers it.
  // See sim/README.md.
  std::vector<std::int32_t> strip_bound_;
  std::vector<std::uint64_t> line_mask_;     // kWidth x kHeight, all zero between strips
  std::vector<std::uint32_t> line_touched_;  // the indices of line_mask_ that are not zero
  Context strip_ctx_;
  bool strip_active_ = false;
  bool strip_line_ = false;   // a LINE_STRIP rather than an edge strip
  bool strip_above_ = false;  // EDGE_STRIP_A (fills up to row 0) rather than _B
  int strip_lo_ = 0, strip_hi_ = -1;  // edge strip: columns touched
  bool done_ = false;
  std::size_t word_index_ = 0;
  Handle handles_[32];
};

}  // namespace o3sdk::sim
