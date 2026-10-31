// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/canvas.h>

#include <cstdlib>

// A misuse of the Canvas (a call outside a section, a Renderer call between two of its own, an invalid desc) is a programming error: it aborts in a
// debug build and in every host build, and costs nothing in the MK66's release build (NDEBUG on an ARM target), as the Renderer's raw-section check.
#if !defined(NDEBUG) || !defined(__arm__)
#define O3SDK_CANVAS_CHECK(cond) \
  do {                          \
    if (!(cond)) {              \
      std::abort();             \
    }                           \
  } while (false)
#else
#define O3SDK_CANVAS_CHECK(cond) \
  do {                          \
  } while (false)
#endif

namespace o3sdk::ui::plot {

namespace {

namespace dl = eve::dl;

// VERTEX2F holds 15 signed bits in 1/16 px (default VERTEX_FORMAT): -1024..1023 px.
constexpr int kMinQ4 = -16384;
constexpr int kMaxQ4 = 16383;
constexpr std::int32_t clamp_q4(std::int64_t v) noexcept { return static_cast<std::int32_t>(v < kMinQ4 ? kMinQ4 : (v > kMaxQ4 ? kMaxQ4 : v)); }

// VERTEX2II takes unsigned 9-bit pixel coordinates (guide section 4.48).
constexpr int kMaxVertex2ii = 511;
constexpr unsigned kMaxCell = 127;

// The Renderer's origin rule (renderer.cpp): when x leaves the 9-bit window the origin moves so that x lands 256..319 px into a window of 512, a multiple of 64.
constexpr int kOriginReach = 256;
constexpr int kOriginStep = 64;

constexpr unsigned kMaxLineWidth = 4095;  // LINE_WIDTH is 12 bits
constexpr unsigned kMaxPointSize = 8191;  // POINT_SIZE is 13 bits
constexpr unsigned kRectLineWidth = 8;    // the Renderer's RECTS at half a pixel: every edge on a pixel boundary (renderer.h)
constexpr unsigned kScissorMax = 2048;

constexpr std::uint32_t u32(int v) noexcept { return static_cast<std::uint32_t>(v); }

}  // namespace

void Canvas::check() const noexcept { O3SDK_CANVAS_CHECK(in_section_ && r_.words() == words_); }

void Canvas::open() noexcept {
  O3SDK_CANVAS_CHECK(!in_section_);
  r_.begin_raw();
  in_section_ = true;
  // What the section starts with: the Renderer's state (LINE_WIDTH 8, COLOR_A 255, the screen for a scissor, origin 0 after begin_raw) and nothing else known.
  has_prim_ = false;
  has_color_ = false;
  alpha_ = 255;
  has_point_size_ = false;
  line_width_ = kRectLineWidth;
  origin_ = 0;
  scissor_x_ = scissor_y_ = 0;
  scissor_w_ = scissor_h_ = kScissorMax;
  handle_set_ = false;
  has_cell_ = false;
  sync();
}

void Canvas::close() noexcept {
  check();
  r_.end_raw();
  in_section_ = false;
  has_bound_ = false;  // RESTORE_CONTEXT may have undone the bind (canvas.h)
  sync();
}

void Canvas::use(eve::Primitive prim) noexcept {
  if (has_prim_ && prim_ == prim) {
    return;
  }
  emit(dl::begin(prim));  // no END before it: see canvas.h
  prim_ = prim;
  has_prim_ = true;
}

void Canvas::style(Rgb rgb, std::uint8_t alpha) noexcept {
  if (!has_color_ || color_.r != rgb.r || color_.g != rgb.g || color_.b != rgb.b) {
    emit(dl::color_rgb(rgb.r, rgb.g, rgb.b));
    color_ = rgb;
    has_color_ = true;
  }
  if (alpha != alpha_) {
    emit(dl::color_a(alpha));
    alpha_ = alpha;
  }
}

void Canvas::set_line_width(unsigned w_q4) noexcept {
  if (w_q4 != line_width_) {
    emit(dl::line_width(w_q4));
    line_width_ = w_q4;
  }
}

void Canvas::bind(const BitmapDesc& d) noexcept {
  check();
  O3SDK_CANVAS_CHECK(bitmap_desc_valid(d));
  if (has_bound_ && bound_ == d) {
    return;
  }
  emit(dl::bitmap_handle(kHandleApp));
  emit(dl::bitmap_source(d.ram_g));
  emit(dl::bitmap_layout(d.format, d.stride, d.cell_h));
  emit(dl::bitmap_layout_h(u32(d.stride >> 10), u32(d.cell_h >> 9)));  // always: the high bits of the bind before must not survive
  emit(dl::bitmap_size(eve::Filter::Nearest, d.wrap_x, d.wrap_y, d.draw_w, d.draw_h));
  emit(dl::bitmap_size_h(u32(d.draw_w >> 9), u32(d.draw_h >> 9)));
  bound_ = d;
  has_bound_ = true;
  handle_set_ = true;  // BITMAP_HANDLE is current; the CELL register is not known
  has_cell_ = false;
  sync();
}

// Makes `x` reachable by VERTEX2II, moving the origin if need be. False when it cannot be (off the left of the panel, or translation switched off).
bool Canvas::place(int x) noexcept {
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

void Canvas::sprite(unsigned cell, int x, int y, Rgb rgb, std::uint8_t alpha) noexcept {
  check();
  cell = cell > kMaxCell ? kMaxCell : cell;
  O3SDK_CANVAS_CHECK(!has_bound_ || std::uint64_t{bound_.ram_g} + std::uint64_t{cell + 1u} * bound_.stride * bound_.cell_h <=
                                        std::uint64_t{kRamGApp} + kRamGAppBytes);
  style(rgb, alpha);
  use(eve::Primitive::Bitmaps);
  if (y >= 0 && y <= kMaxVertex2ii && place(x)) {
    emit(dl::vertex2ii(u32(x - origin_), u32(y), kHandleApp, cell));
    // As the Renderer: whether VERTEX2II also loads the BITMAP_HANDLE / CELL registers is not stated in the guide, so the VERTEX2F path does not rely on its earlier values after one.
    handle_set_ = false;
    has_cell_ = false;
  } else {
    if (!handle_set_) {
      emit(dl::bitmap_handle(kHandleApp));
      handle_set_ = true;
      has_cell_ = false;
    }
    if (!has_cell_ || cell_ != cell) {
      emit(dl::cell(cell));
      cell_ = cell;
      has_cell_ = true;
    }
    emit(dl::vertex2f(clamp_q4(std::int64_t{x - origin_} * 16), clamp_q4(std::int64_t{y} * 16)));
  }
  sync();
}

void Canvas::point(int cx_q4, int cy_q4, unsigned radius_q4, Rgb rgb, std::uint8_t alpha) noexcept {
  check();
  radius_q4 = radius_q4 > kMaxPointSize ? kMaxPointSize : radius_q4;
  style(rgb, alpha);
  if (!has_point_size_ || point_size_ != radius_q4) {
    emit(dl::point_size(radius_q4));
    point_size_ = radius_q4;
    has_point_size_ = true;
  }
  use(eve::Primitive::Points);
  emit(dl::vertex2f(clamp_q4(std::int64_t{cx_q4} - std::int64_t{origin_} * 16), clamp_q4(cy_q4)));
  sync();
}

void Canvas::line(int x0_q4, int y0_q4, int x1_q4, int y1_q4, unsigned w_q4, Rgb rgb, std::uint8_t alpha) noexcept {
  check();
  style(rgb, alpha);
  set_line_width(w_q4 > kMaxLineWidth ? kMaxLineWidth : w_q4);
  use(eve::Primitive::Lines);
  emit(dl::vertex2f(clamp_q4(std::int64_t{x0_q4} - std::int64_t{origin_} * 16), clamp_q4(y0_q4)));
  emit(dl::vertex2f(clamp_q4(std::int64_t{x1_q4} - std::int64_t{origin_} * 16), clamp_q4(y1_q4)));
  sync();
}

// RECTS grows by LINE_WIDTH on every side and rounds its corners to that radius, so the two vertices are the rect's corners moved in by it.
void Canvas::box(const Rect& r, unsigned radius_q4, Rgb rgb, std::uint8_t alpha) noexcept {
  check();
  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  const unsigned half_short = 8u * static_cast<unsigned>(r.w < r.h ? r.w : r.h);  // half the shorter side in 1/16 px (the rect is below 2,048 px wide and high)
  unsigned lw = radius_q4 > half_short ? half_short : radius_q4;
  lw = lw < kRectLineWidth ? kRectLineWidth : lw;
  lw = lw > kMaxLineWidth ? kMaxLineWidth : lw;
  style(rgb, alpha);
  set_line_width(lw);
  use(eve::Primitive::Rects);
  const std::int64_t inset = static_cast<std::int64_t>(lw);
  const std::int64_t ox = std::int64_t{origin_} * 16;
  emit(dl::vertex2f(clamp_q4(std::int64_t{r.x} * 16 - ox + inset), clamp_q4(std::int64_t{r.y} * 16 + inset)));
  emit(dl::vertex2f(clamp_q4((std::int64_t{r.x} + r.w) * 16 - ox - inset), clamp_q4((std::int64_t{r.y} + r.h) * 16 - inset)));
  sync();
}

void Canvas::rect(const Rect& r, Rgb rgb, std::uint8_t alpha) noexcept { box(r, kRectLineWidth, rgb, alpha); }

void Canvas::round_rect(const Rect& r, unsigned radius_q4, Rgb rgb, std::uint8_t alpha) noexcept { box(r, radius_q4, rgb, alpha); }

void Canvas::set_scissor(unsigned x, unsigned y, unsigned w, unsigned h) noexcept {
  if (x != scissor_x_ || y != scissor_y_) {
    emit(dl::scissor_xy(x, y));
    scissor_x_ = x;
    scissor_y_ = y;
  }
  if (w != scissor_w_ || h != scissor_h_) {
    emit(dl::scissor_size(w, h));
    scissor_w_ = w;
    scissor_h_ = h;
  }
  sync();
}

void Canvas::scissor(const Rect& r) noexcept {
  check();
  // SCISSOR_XY is unsigned and 11 bits: what lies left of or above the screen is cut off the rect, and a rect past the screen's far edge keeps its origin on the screen.
  const std::int64_t x0 = r.x < 0 ? 0 : (r.x > 2047 ? 2047 : r.x);
  const std::int64_t y0 = r.y < 0 ? 0 : (r.y > 2047 ? 2047 : r.y);
  const std::int64_t x1 = std::int64_t{r.x} + (r.w > 0 ? r.w : 0);
  const std::int64_t y1 = std::int64_t{r.y} + (r.h > 0 ? r.h : 0);
  const std::int64_t w = x1 > x0 ? x1 - x0 : 0;
  const std::int64_t h = y1 > y0 ? y1 - y0 : 0;
  set_scissor(static_cast<unsigned>(x0), static_cast<unsigned>(y0), static_cast<unsigned>(w > kScissorMax ? kScissorMax : w),
              static_cast<unsigned>(h > kScissorMax ? kScissorMax : h));
}

void Canvas::scissor_reset() noexcept {
  check();
  set_scissor(0, 0, kScissorMax, kScissorMax);
}

}  // namespace o3sdk::ui::plot
