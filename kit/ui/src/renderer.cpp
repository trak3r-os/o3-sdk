// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/renderer.h>

#include <cstdlib>
#include <cstring>

// A misuse of the raw sections (nesting them, emitting a raw word or ending one outside a section) is a programming error:
// it aborts in a debug build and in every host build, and costs nothing in the MK66's release build (NDEBUG on an ARM target).
#if !defined(NDEBUG) || !defined(__arm__)
#define O3SDK_RAW_CHECK(cond) \
  do {                       \
    if (!(cond)) {           \
      std::abort();          \
    }                        \
  } while (false)
#else
#define O3SDK_RAW_CHECK(cond) \
  do {                       \
  } while (false)
#endif

namespace o3sdk::ui {

namespace {

namespace dl = eve::dl;

// VERTEX2F holds 15 signed bits in 1/16 px (default VERTEX_FORMAT): -1024..1023 px.
constexpr int kMinPx = -1024;
constexpr int kMaxPx = 1023;
constexpr int clamp_px(int px) noexcept { return px < kMinPx ? kMinPx : (px > kMaxPx ? kMaxPx : px); }
// The top-left corner of pixel `px`: where a bitmap goes.
constexpr std::int32_t q4(int px) noexcept { return static_cast<std::int32_t>(clamp_px(px) * 16); }
// The centre of pixel `px`: where a rectangle's corner vertex goes, see kRectLineWidth.
constexpr std::int32_t centre_q4(int px) noexcept { return static_cast<std::int32_t>(clamp_px(px) * 16 + 8); }

constexpr std::uint32_t u32(int v) noexcept { return static_cast<std::uint32_t>(v); }

// Rectangles are RECTS with LINE_WIDTH 8 (half a pixel; the range is 0..4095, guide section 4.32) and their two
// vertices on the centres of the first and last pixel they cover. The chip grows a rectangle by the line width on
// every side (section 2.5.4), so the edges land exactly on pixel boundaries: whatever antialiasing the chip applies
// finds every edge pixel fully covered or not at all, and a 1 px border or rule is exactly 1 px. Only the four
// corner pixels of a rectangle are touched by the rounded corners (radius half a pixel), see sim/README.md.
constexpr std::uint32_t kRectLineWidth = 8;

// VERTEX2II takes unsigned 9-bit pixel coordinates (guide section 4.48).
constexpr int kMaxVertex2ii = 511;
constexpr bool fits_vertex2ii(int x, int y) noexcept {
  return x >= 0 && x <= kMaxVertex2ii && y >= 0 && y <= kMaxVertex2ii;
}

// When x leaves the 9-bit window the origin moves so that x lands 256..319 px into a window of 512: far enough from
// both ends that a run of neighbouring bitmaps (a grid column, a string) stays inside it, and a multiple of 64 so a
// few distinct origins serve the whole panel (an 800 px panel needs 0, 256 and 512).
constexpr int kOriginReach = 256;
constexpr int kOriginStep = 64;

}  // namespace

bool Renderer::begin(Color clear) noexcept {
  in_raw_ = false;  // a new frame: a section left open by the last one is gone with its context
  has_prim_ = false;
  has_color_ = false;
  has_handle_ = false;
  has_cell_ = false;
  notes_hi_bound_ = false;
  small_hex_bound_ = false;
  small_notes_ = 0xFF;
  origin_ = 0;  // every frame starts at VERTEX_TRANSLATE_X 0
  if (!b_.begin_frame()) {
    return false;
  }
  const Rgb& bg = theme_->rgb(clear);
  emit(dl::clear_color_rgb(bg.r, bg.g, bg.b));
  emit(dl::clear(true, true, true));
  emit(dl::line_width(kRectLineWidth));
  for (unsigned i = 0; i < kFontCount; ++i) {
    const FontFace& f = font_face(static_cast<Font>(i));
    bind(f.handle, f.ram_g_addr, f.stride, f.cell_w, f.cell_h);
  }
  for (unsigned i = 0; i < kTileSetCount; ++i) {
    const BitmapSet s = bitmap_set(static_cast<Tiles>(i));
    bind(s.handle, s.ram_g, s.stride, s.cell_w, s.cell_h);
  }
  return true;
}

void Renderer::bind(std::uint8_t handle, std::uint32_t ram_g, unsigned stride, unsigned cell_w,
                    unsigned cell_h) noexcept {
  emit(dl::bitmap_handle(handle));
  emit(dl::bitmap_source(ram_g));
  emit(dl::bitmap_layout(eve::BitmapFormat::L1, stride, cell_h));
  emit(dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, cell_w, cell_h));
}

void Renderer::begin_raw() noexcept {
  O3SDK_RAW_CHECK(!in_raw_);
  in_raw_ = true;
  if (has_prim_) {
    emit(dl::end());
    has_prim_ = false;
  }
  emit(dl::save_context());
  raw_origin_ = origin_;
  if (origin_ != 0) {
    emit(dl::vertex_translate_x(0));
    origin_ = 0;
  }
}

void Renderer::raw(std::uint32_t word) noexcept {
  O3SDK_RAW_CHECK(in_raw_);
  emit(word);
}

void Renderer::end_raw() noexcept {
  O3SDK_RAW_CHECK(in_raw_);
  in_raw_ = false;
  if (has_prim_) {
    emit(dl::end());
    has_prim_ = false;
  }
  emit(dl::restore_context());
  origin_ = raw_origin_;
  has_color_ = false;
  has_handle_ = false;
  has_cell_ = false;
}

std::uint32_t Renderer::color_word(Color c) const noexcept {
  const Rgb& v = theme_->rgb(c);
  return dl::color_rgb(v.r, v.g, v.b);
}

bool Renderer::end() noexcept {
  O3SDK_RAW_CHECK(!in_raw_);  // a section left open would leave its context on the chip's stack
  if (has_prim_) {
    emit(dl::end());
    has_prim_ = false;
  }
  return b_.end_frame();
}

void Renderer::use(eve::Primitive prim) noexcept {
  if (has_prim_ && prim_ == prim) {
    return;
  }
  if (has_prim_) {
    emit(dl::end());
  }
  emit(dl::begin(prim));
  prim_ = prim;
  has_prim_ = true;
}

void Renderer::set_color(Color c) noexcept {
  if (has_color_ && color_ == c) {
    return;
  }
  const Rgb& v = theme_->rgb(c);
  emit(dl::color_rgb(v.r, v.g, v.b));
  color_ = c;
  has_color_ = true;
}

// The pixels [x0, x1) x [y0, y1); callers pass x1 > x0 and y1 > y0. x is relative to the origin like every vertex.
void Renderer::fill(int x0, int y0, int x1, int y1) noexcept {
  use(eve::Primitive::Rects);
  emit(dl::vertex2f(centre_q4(x0 - origin_), centre_q4(y0)));
  emit(dl::vertex2f(centre_q4(x1 - 1 - origin_), centre_q4(y1 - 1)));
}

void Renderer::panel(const Rect& r, Color fill_color) noexcept {
  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  set_color(fill_color);
  fill(r.x, r.y, r.x + r.w, r.y + r.h);
}

void Renderer::frame(const Rect& r, Color border) noexcept {
  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  set_color(border);
  fill(r.x, r.y, r.x + r.w, r.y + 1);                    // top
  if (r.h > 1) {
    fill(r.x, r.y + r.h - 1, r.x + r.w, r.y + r.h);      // bottom
  }
  if (r.h > 2) {
    fill(r.x, r.y + 1, r.x + 1, r.y + r.h - 1);          // left
    if (r.w > 1) {
      fill(r.x + r.w - 1, r.y + 1, r.x + r.w, r.y + r.h - 1);  // right
    }
  }
}

void Renderer::rule(int x, int y, int length, Axis axis, Color c) noexcept {
  if (length <= 0) {
    return;
  }
  set_color(c);
  if (axis == Axis::Horizontal) {
    fill(x, y, x + length, y + 1);
  } else {
    fill(x, y, x + 1, y + length);
  }
}

void Renderer::highlight_row(const Rect& r) noexcept { panel(r, Color::Sel); }

// Makes `x` reachable by VERTEX2II, moving the origin if need be. False when it cannot be (off the left of the panel,
// or translation switched off): the caller then uses VERTEX2F.
bool Renderer::place(int x) noexcept {
  const int local = x - origin_;
  if (local >= 0 && local <= kMaxVertex2ii) {
    return true;
  }
  if (!translate_ || x < 0) {
    return false;
  }
  int origin = x - kOriginReach;
  origin = origin < 0 ? 0 : origin - origin % kOriginStep;
  emit(dl::vertex_translate_x(origin * 16));
  origin_ = origin;
  return true;
}

// One bitmap cell with its top-left corner at (x, y): VERTEX2II where x and y fit (it carries handle and cell, one
// word), else BITMAP_HANDLE + CELL + VERTEX2F (up to three words, the first two skipped when unchanged).
void Renderer::bitmap(std::uint8_t handle, unsigned cell, int x, int y) noexcept {
  use(eve::Primitive::Bitmaps);
  if (y >= 0 && y <= kMaxVertex2ii && place(x)) {
    emit(dl::vertex2ii(u32(x - origin_), u32(y), handle, cell));
    // Whether VERTEX2II also loads the BITMAP_HANDLE / CELL registers is not stated in the guide, so the
    // VERTEX2F path below does not rely on its own earlier values after one of these.
    has_handle_ = false;
    has_cell_ = false;
    return;
  }
  if (!has_handle_ || handle_ != handle) {
    emit(dl::bitmap_handle(handle));
    handle_ = handle;
    has_handle_ = true;
    has_cell_ = false;
  }
  if (!has_cell_ || cell_ != cell) {
    emit(dl::cell(cell));
    cell_ = static_cast<std::uint8_t>(cell);
    has_cell_ = true;
  }
  emit(dl::vertex2f(q4(x - origin_), q4(y)));
}

void Renderer::text(int x, int y, const char* s, Color c, Face face) noexcept {
  if (s == nullptr) {
    return;
  }
  const FontFace& f = font_face(theme_->font(face));
  bool color_set = false;  // only a string with something visible changes the colour
  for (std::size_t i = 0; i < kMaxTextChars && s[i] != '\0' && b_.ok(); ++i, x += f.cell_w) {
    unsigned ch = static_cast<std::uint8_t>(s[i]);
    if (ch == ' ') {
      continue;
    }
    if (ch < f.first_char || ch >= static_cast<unsigned>(f.first_char) + f.glyph_count) {
      ch = '?';
    }
    if (!color_set) {
      set_color(c);
      color_set = true;
    }
    bitmap(f.handle, ch - f.first_char, x, y);
  }
}

void Renderer::text_grid(int origin_x, int origin_y, int col, int row, const char* s, Color c, Face face) noexcept {
  const FontFace& f = font_face(theme_->font(face));
  text(origin_x + col * f.cell_w, origin_y + row * f.cell_h, s, c, face);
}

int Renderer::text_width(const char* s, Face face) const noexcept {
  if (s == nullptr) {
    return 0;
  }
  std::size_t n = std::strlen(s);
  if (n > kMaxTextChars) {
    n = kMaxTextChars;
  }
  return static_cast<int>(n) * font_face(theme_->font(face)).cell_w;
}

// Binds a smaller set (OS-ZOOM) the first time a frame draws from it. The bind ends on this handle, which is also the one the VERTEX2F path draws with next: bitmap() is told that and that the cell is not known.
void Renderer::bind_small(SmallTiles set) noexcept {
  const BitmapSet s = small_bitmap_set(set);
  bind(s.handle, s.ram_g, s.stride, s.cell_w, s.cell_h);
  handle_ = s.handle;
  has_handle_ = true;
  has_cell_ = false;
}

void Renderer::hex_tile(int x, int y, unsigned value, Color c, Face face) noexcept {
  value &= 0xFFu;
  const Font font = theme_->font(face);
  if (font != kTileFont) {
    if (font == kLabelTileFont && value < kSmallTiles) {
      set_color(c);
      if (!small_hex_bound_) {
        bind_small(SmallTiles::LabelHex);
        small_hex_bound_ = !in_raw_;  // a bind inside a raw section is undone by its RESTORE_CONTEXT: not remembered
      }
      bitmap(kHandleSmallHex, value, x, y);
      return;
    }
    static const char kDigits[] = "0123456789ABCDEF";
    const char pair[3] = {kDigits[value >> 4], kDigits[value & 0xFu], '\0'};
    text(x, y, pair, c, face);  // a value the smaller sets do not hold: its characters, as glyphs of the face
    return;
  }
  set_color(c);
  bitmap(value < kHexTilesPerHandle ? kHandleHexLo : kHandleHexHi, value % kHexTilesPerHandle, x, y);
}

void Renderer::note_tile(int x, int y, unsigned cell, Color c, Face face) noexcept {
  if (cell >= kNoteTiles) {
    cell = kNoteTileUnknown;
  }
  const Font font = theme_->font(face);
  if (font != kTileFont) {
    const bool label = font == kLabelTileFont;
    if ((label || font == kMicroTileFont) && cell < kSmallTiles) {
      const SmallTiles set = label ? SmallTiles::LabelNotes : SmallTiles::MicroNotes;
      set_color(c);
      if (small_notes_ != static_cast<std::uint8_t>(set)) {
        bind_small(set);
        small_notes_ = in_raw_ ? std::uint8_t{0xFF} : static_cast<std::uint8_t>(set);
      }
      bitmap(kHandleSmallNotes, cell, x, y);
      return;
    }
    char name[4];
    note_tile_text(cell, name);
    text(x, y, name, c, face);
    return;
  }
  set_color(c);
  if (cell < kNoteTilesPerHandle) {
    bitmap(kHandleNotes, cell, x, y);
    return;
  }
  if (!notes_hi_bound_) {
    // The tiles past the 128th (C10 and above is rare, and a frame without them pays nothing). The bind ends on this handle, which is also the
    // one the VERTEX2F path draws with next: bitmap() is told that and that the cell is not known.
    const BitmapSet s = notes_hi_set();
    bind(s.handle, s.ram_g, s.stride, s.cell_w, s.cell_h);
    notes_hi_bound_ = !in_raw_;  // a bind inside a raw section is undone by its RESTORE_CONTEXT: not remembered
    handle_ = s.handle;
    has_handle_ = true;
    has_cell_ = false;
  }
  bitmap(kHandleNotesHi, cell - kNoteTilesPerHandle, x, y);
}

bool Renderer::fx_name_tile(int x, int y, unsigned type, Color c) noexcept {
  char name[4];
  if (!fx_tile_text(type, name)) {
    return false;
  }
  set_color(c);
  if (small_notes_ != static_cast<std::uint8_t>(SmallTiles::MicroFx)) {
    bind_small(SmallTiles::MicroFx);
    small_notes_ = in_raw_ ? std::uint8_t{0xFF} : static_cast<std::uint8_t>(SmallTiles::MicroFx);
  }
  bitmap(kHandleSmallNotes, type, x, y);
  return true;
}

void Renderer::icon(int x, int y, Icon icon, Color c) noexcept {
  const unsigned cell = static_cast<unsigned>(icon);
  if (cell >= kIconCount) {
    return;
  }
  set_color(c);
  bitmap(kHandleIcons, cell, x, y);
}

}  // namespace o3sdk::ui
