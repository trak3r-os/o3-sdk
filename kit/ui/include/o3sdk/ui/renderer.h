// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/font.h>
#include <o3sdk/ui/style.h>
#include <o3sdk/ui/theme.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui {

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

enum class Axis : std::uint8_t { Horizontal, Vertical };

// Immediate-mode drawing on top of eve::DisplayListBuilder: every call appends display-list words to the
// frame being built and nothing else; there is no retained state between frames. It emits only the words of the
// toolkit subset (ADR 0010 section 2): no coprocessor widgets, no CMD_TEXT. Main loop only, no heap.
//
// Frame:  Renderer r(builder, theme(ThemeId::Amiga)); if (r.begin(Color::Pane)) { r.panel(...); r.text(...); r.end(); }
//
// The colours are tokens (style.h); the renderer looks each one up in the theme it was made with. Faces are roles
// (font.h): Display, Data, Label, Micro, which the theme maps to compiled faces.
//
// Look (ADR 0001): flat fills, 1 px borders, no gradients, no rounded corners. Rectangles are RECTS at
// LINE_WIDTH 8 (half a pixel) with their vertices on the centres of the first and last pixel covered: the FT81x
// grows a RECTS primitive by the line width on every side and rounds its corners (Programmer's Guide section
// 2.5.4), so with these numbers every edge falls on a pixel boundary, simulator and device agree on all but the
// four corner pixels of a rectangle, and borders and rules are exactly 1 px. LINES are not used.
//
// Display-list budget: RAM_DL holds 2048 words. A glyph or a tile costs one word (VERTEX2II) plus one COLOR_RGB word
// per colour change; a hex-pair tile is two glyphs, a note tile three, an icon one, for the same one word (ADR 0010
// section 5). The renderer skips redundant COLOR_RGB/CELL/BITMAP_HANDLE/BEGIN words, so callers should group draws by
// colour. When the frame overflows, the builder drops the remaining words and end() aborts it: nothing is committed,
// the previous frame stays visible, and end() returns false.
//
// Reaching x >= 512. VERTEX2II carries x in 9 bits. The renderer keeps a horizontal origin (VERTEX_TRANSLATE_X) that
// it moves, a few words at a time, when the next bitmap would not fit: callers always pass screen coordinates.
// [A] That VERTEX_TRANSLATE_X moves VERTEX2II is not stated in the guide (sim/README.md, Calibration); use_translate()
// switches the renderer to the alternative that needs nothing of it (BITMAP_HANDLE + CELL + VERTEX2F: 1,502 words
// against 1,193 for the pattern view's worst case). It draws the same pixels (tests/pattern_view_test.cpp) and fits the
// budget (tests/budget_test.cpp), so it is a plan B that can be switched to without a redesign should hardware say otherwise.
class Renderer {
 public:
  // Longest string text() will draw; the DL cannot hold more glyphs anyway.
  static constexpr std::size_t kMaxTextChars = 128;

  Renderer(eve::DisplayListBuilder& builder, const Theme& theme) noexcept : b_(builder), theme_(&theme) {}

  const Theme& theme() const noexcept { return *theme_; }

  // Starts the frame (false: the sink is busy, skip drawing), clears to `clear`, selects LINE_WIDTH 8 and binds every
  // bitmap handle the toolkit uses (four faces, the tile faces and the icons; ui::upload_assets() has put them in
  // RAM_G, see assets.h).
  bool begin(Color clear) noexcept;
  // Closes the frame; true only if every word fitted and the sink committed the list.
  bool end() noexcept;

  void panel(const Rect& r, Color fill) noexcept;       // flat filled rectangle
  void frame(const Rect& r, Color border) noexcept;     // 1 px border, inside `r`, no fill
  // 1 px line of `length` pixels starting at (x, y), going right or down.
  void rule(int x, int y, int length, Axis axis, Color c) noexcept;
  // The strong selected-row highlight: a Sel bar the width of `r` (the one highlight of ADR 0001).
  void highlight_row(const Rect& r) noexcept;

  // Text with its top-left corner at (x, y). Spaces are skipped (no word), characters outside the face
  // become '?'. Draws at most kMaxTextChars characters.
  void text(int x, int y, const char* s, Color c, Face face = Face::Data) noexcept;
  // Text on a character grid: cell (col, row) of a grid whose top-left is (origin_x, origin_y) and whose
  // cells are the face's glyph cell.
  void text_grid(int origin_x, int origin_y, int col, int row, const char* s, Color c,
                 Face face = Face::Data) noexcept;
  // Width of `s` in pixels in `face` (every character advances one cell, spaces included).
  int text_width(const char* s, Face face) const noexcept;

  // Tiles and icons: one word each (ADR 0010 section 5). (x, y) is the top-left corner of the tile.
  // `face` is the size: the Data face (the default) draws the full tile sets, the Label (8x16) and Micro (6x12) faces the smaller ones of the zoomed pattern grid (OS-ZOOM; 4 words to bind a set,
  // in the first frame call that needs it). A value or cell a smaller set does not hold (a hex pair from 0x80, a note from C10, any hex pair in Micro) is drawn as glyphs of the face: 2 or 3 words.
  void hex_tile(int x, int y, unsigned value, Color c, Face face = Face::Data) noexcept;   // two hex digits, value 0..255
  void note_tile(int x, int y, unsigned cell, Color c, Face face = Face::Data) noexcept;   // "C-4", "---", "OFF", "CUT": cell 0..kNoteTiles-1
  // OS-AUDIT E-PAT-03: the 3-letter name of FX `type` as one Micro tile (18 x 12; 1 word, 4 more to bind the set in the first frame call that needs it). False and nothing drawn for a type without a name (assets.h kFxTiles).
  bool fx_name_tile(int x, int y, unsigned type, Color c) noexcept;
  void icon(int x, int y, Icon icon, Color c) noexcept;

  // Whether bitmaps past x = 511 use VERTEX_TRANSLATE_X (the default). Call before begin().
  void use_translate(bool on) noexcept { translate_ = on; }

  // ---- Raw sections (OS-003d, ui::plot) ----------------------------------------------------------------------------
  // A plot emits words the renderer has no call for (LINE_STRIP, EDGE_STRIP, POINTS, COLOR_A, a scissor, wave bitmaps)
  // inside a raw section, which keeps the renderer's own caches honest. begin_raw() ends the open primitive and emits
  // SAVE_CONTEXT, and VERTEX_TRANSLATE_X 0 if the renderer had moved its origin, so that raw vertices are plain screen
  // coordinates. end_raw() ends the primitive the section left open and emits RESTORE_CONTEXT: colour, alpha, line width,
  // point size, blend function, scissor and translation come back as they were; the renderer forgets its cached colour,
  // bitmap handle and cell (the section may have changed them) and takes its origin back. The renderer's own calls
  // (panel, text, ...) may be used inside a section, a translucent panel being the usual case (COLOR_A, then panel()).
  // Overhead: SAVE_CONTEXT and RESTORE_CONTEXT, an END if a primitive of the renderer's was open, and the translation word:
  // kRawOverheadWords = 4 at most (a section that draws rectangles through the renderer's own calls leaves a primitive open
  // that end_raw() closes: one more), which every plot's declared maximum includes. Sections do not nest (the chip's context stack
  // is four deep and the inner one would lose the origin the outer one saved), and raw() belongs inside one (outside it would
  // change a chip state the caches are not told of): both are programming errors, caught by a check that aborts in debug and
  // in host builds (the MK66's release build leaves it out).
  static constexpr unsigned kRawOverheadWords = 4;
  void begin_raw() noexcept;
  void end_raw() noexcept;
  void raw(std::uint32_t word) noexcept;
  bool in_raw() const noexcept { return in_raw_; }
  // COLOR_RGB of a token, and COLOR_A, as words for raw(); the renderer's colour cache is not told (end_raw forgets it).
  std::uint32_t color_word(Color c) const noexcept;
  // The display behind the builder: bitmap data for a plot goes to RAM_G with Display::upload(), which the contract allows
  // inside an open frame (hal/display.h). A plot's own job runner normally does it between frames.
  hal::Display& display() noexcept { return b_.sink(); }

  bool ok() const noexcept { return b_.ok(); }
  // Words emitted so far in this frame (DISPLAY not counted).
  std::size_t words() const noexcept { return b_.size(); }

 private:
  void emit(std::uint32_t word) noexcept { (void)b_.emit(word); }  // overflow is recorded by the builder
  void use(eve::Primitive prim) noexcept;
  void set_color(Color c) noexcept;
  void fill(int x0, int y0, int x1, int y1) noexcept;
  void bind(std::uint8_t handle, std::uint32_t ram_g, unsigned stride, unsigned cell_w, unsigned cell_h) noexcept;
  void bitmap(std::uint8_t handle, unsigned cell, int x, int y) noexcept;
  bool place(int x) noexcept;  // makes `x` reachable by VERTEX2II; false if it cannot be

  eve::DisplayListBuilder& b_;
  const Theme* theme_;
  bool has_prim_ = false;
  eve::Primitive prim_ = eve::Primitive::Rects;
  bool has_color_ = false;
  Color color_ = Color::Pane;
  // State of the VERTEX2F path (BITMAP_HANDLE / CELL registers); VERTEX2II draws forget it, see bitmap().
  bool has_handle_ = false;
  std::uint8_t handle_ = 0;
  bool has_cell_ = false;
  std::uint8_t cell_ = 0;
  bool notes_hi_bound_ = false;  // the second note-tile handle is bound by the first tile that needs it (4 words), once a frame
  bool small_hex_bound_ = false;      // OS-ZOOM: handle kHandleSmallHex is bound to the Label hex pairs
  std::uint8_t small_notes_ = 0xFF;   // ... and kHandleSmallNotes to this SmallTiles set (0xFF none); both once a frame, not remembered inside a raw section
  void bind_small(SmallTiles set) noexcept;
  int origin_ = 0;  // VERTEX_TRANSLATE_X in whole pixels; every x the renderer emits is relative to it
  int raw_origin_ = 0;  // origin_ at begin_raw(): the context restore brings it back
  bool in_raw_ = false;
  bool translate_ = true;
};

}  // namespace o3sdk::ui
