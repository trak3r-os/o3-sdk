// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/sim/eve_raster.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/plot/canvas.h>

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace o3sdk::sim {

namespace {

namespace op = eve::opcode;

constexpr std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {  // b > 0
  return a / b - ((a % b != 0 && a < 0) ? 1 : 0);
}
constexpr std::int32_t ceil_div(std::int32_t a, std::int32_t b) noexcept { return -floor_div(-a, b); }

constexpr std::int64_t floor_div64(std::int64_t a, std::int64_t b) noexcept {  // b > 0
  return a / b - ((a % b != 0 && a < 0) ? 1 : 0);
}
constexpr std::int64_t ceil_div64(std::int64_t a, std::int64_t b) noexcept { return -floor_div64(-a, b); }

constexpr std::int32_t sign_extend15(std::uint32_t v) noexcept {
  return static_cast<std::int32_t>(v & 0x7FFFu) - static_cast<std::int32_t>((v & 0x4000u) << 1);
}
constexpr std::int32_t sign_extend17(std::uint32_t v) noexcept {
  return static_cast<std::int32_t>(v & 0x1FFFFu) - static_cast<std::int32_t>((v & 0x10000u) << 1);
}
constexpr std::int32_t sign_extend24(std::uint32_t v) noexcept {
  return static_cast<std::int32_t>(v & 0xFFFFFFu) - static_cast<std::int32_t>((v & 0x800000u) << 1);
}

// floor(sqrt(n)), exactly, in integers (the digit-by-digit method): every inside/outside decision below stays in
// integer arithmetic, so the pixels do not depend on the compiler's floating point.
constexpr std::uint64_t isqrt(std::uint64_t n) noexcept {
  std::uint64_t root = 0, bit = std::uint64_t{1} << 62;
  while (bit > n) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// The set of points within `radius` of the segment p0-p1 (a capsule: the segment thickened by `radius`, with round
// ends; a point when p0 == p1, i.e. a disc). Coordinates are 1/16 px. The test is exact: no square root is
// taken. Beside the segment the condition |across| <= radius * |d| is decided with across^2 <= radius^2 * |d|^2,
// i.e. |across| <= isqrt(radius^2 * |d|^2) because across is an integer. A point exactly on the boundary is inside.
// With every coordinate below 2^16 (+-4096 px), |d|^2 < 2^35 and radius^2 * |d|^2 < 2^59 fit 64 bits.
class Capsule {
 public:
  Capsule(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1, std::int64_t radius)
      : x0_(x0), y0_(y0), dx_(x1 - x0), dy_(y1 - y0), r2_(radius * radius) {
    len2_ = dx_ * dx_ + dy_ * dy_;
    reach_ = static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(r2_ * len2_)));
  }

  bool contains(std::int64_t x, std::int64_t y) const noexcept {
    const std::int64_t rx = x - x0_, ry = y - y0_;
    const std::int64_t along = rx * dx_ + ry * dy_;
    if (along <= 0) {  // nearest to p0 (also the whole of a point)
      return rx * rx + ry * ry <= r2_;
    }
    if (along >= len2_) {  // nearest to p1
      const std::int64_t ex = rx - dx_, ey = ry - dy_;
      return ex * ex + ey * ey <= r2_;
    }
    const std::int64_t across = rx * dy_ - ry * dx_;
    return (across < 0 ? -across : across) <= reach_;
  }

 private:
  std::int64_t x0_, y0_, dx_, dy_;
  std::int64_t r2_;
  std::int64_t len2_ = 0;
  std::int64_t reach_ = 0;
};

// The 8 x 8 sample points of the pixel (px, py) that `shape` contains, at odd 1/16 px offsets (1, 3, .. 15): bit
// 8 * j + i is the sample in column i, row j. Coverage is their count, see coverage_of(). A strip ORs the masks of its
// segments, so it covers a pixel as the union of its capsules does.
// [A] the chip's own coverage is not known; this is the simulator's model, see sim/README.md.
std::uint64_t sample_mask(int px, int py, const Capsule& shape) noexcept {
  std::uint64_t mask = 0;
  for (int j = 0; j < 8; ++j) {
    for (int i = 0; i < 8; ++i) {
      if (shape.contains(px * 16 + 2 * i + 1, py * 16 + 2 * j + 1)) {
        mask |= std::uint64_t{1} << (8 * j + i);
      }
    }
  }
  return mask;
}

// The samples counted, scaled to 0..255 and rounded: (count * 255 + 32) / 64.
std::uint8_t coverage_of(std::uint64_t mask) noexcept {
  return static_cast<std::uint8_t>((std::bitset<64>(mask).count() * 255u + 32u) / 64u);
}

// A pixel centre is at most 8 * sqrt(2) = 11.3 sixteenths from any point of its pixel, so a pixel whose centre is
// farther than radius + 12 from the segment is not touched, and one whose centre is within radius - 12 is covered
// in full: the capsule grown or shrunk by 12 bounds the sampled one (a capsule is the segment's Minkowski sum
// with a disc). Only the pixels in between are sampled.
constexpr std::int64_t kPixelReach = 12;

// The vertices of the antialiased primitives stay below 2^16 sixteenths so that Capsule's arithmetic cannot overflow.
constexpr std::int32_t kVertexLimit = 65536;  // +-4096 px

constexpr std::uint8_t mul255(unsigned a, unsigned b) noexcept {  // a * b / 255, rounded
  return static_cast<std::uint8_t>((a * b + 127u) / 255u);
}

}  // namespace

EveRaster::EveRaster()
    : work_(kFramebufferBytes, 0), front_(kFramebufferBytes, 0), ram_g_(kRamGBytes, 0) {
  for (std::size_t i = 3; i < kFramebufferBytes; i += 4) {
    front_[i] = 255;
  }
}

bool EveRaster::fail(const char* what, std::uint32_t word) {
  if (error_.empty()) {
    char text[160];
    std::snprintf(text, sizeof(text), "display-list word #%zu 0x%08X: %s", word_index_, word, what);
    error_ = text;
  }
  return false;
}

void EveRaster::record_glyphs(std::uint32_t ram_g_below) {
  record_below_ = ram_g_below;
  glyph_log_.clear();
  glyph_ink_.clear();
  ink_owner_.assign(ram_g_below != 0 ? static_cast<std::size_t>(kWidth) * kHeight : 0, -1);
}

void EveRaster::release_ink(int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; ++y) {
    std::fill_n(ink_owner_.begin() + static_cast<std::ptrdiff_t>(y) * kWidth + x0, x1 - x0, -1);
  }
}

std::uint32_t EveRaster::digest_words(const std::uint32_t* words, std::size_t count) noexcept {
  std::uint32_t hash = kDigestBasis;
  for (std::size_t i = 0; i < count; ++i) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash = (hash ^ ((words[i] >> shift) & 0xFFu)) * 16777619u;
    }
  }
  return hash;
}

void EveRaster::count_line_load(const Context& c, int x0, int y0, int x1, int y1) {
  x0 = std::max({x0, static_cast<int>(c.scissor_x), 0});
  y0 = std::max({y0, static_cast<int>(c.scissor_y), 0});
  x1 = std::min({x1, static_cast<int>(c.scissor_x + c.scissor_w), kWidth});
  y1 = std::min({y1, static_cast<int>(c.scissor_y + c.scissor_h), kHeight});
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const auto width = static_cast<std::uint32_t>(x1 - x0);
  for (int y = y0; y < y1; ++y) {
    ++line_prims_[y];
    line_px_[y] += width;
  }
}

void EveRaster::count_capsule_box(const Context& c, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                                  std::uint32_t radius_q4) {
  const std::int64_t r = radius_q4;
  count_line_load(c, static_cast<int>(floor_div64(std::int64_t{std::min(x0, x1)} - r, 16)),
                  static_cast<int>(floor_div64(std::int64_t{std::min(y0, y1)} - r, 16)),
                  static_cast<int>(floor_div64(std::int64_t{std::max(x0, x1)} + r + 15, 16)),
                  static_cast<int>(floor_div64(std::int64_t{std::max(y0, y1)} + r + 15, 16)));
}

bool EveRaster::render(const std::uint32_t* words, std::size_t count) {
  error_.clear();
  std::fill(std::begin(line_prims_), std::end(line_prims_), 0u);
  std::fill(std::begin(line_px_), std::end(line_px_), 0u);
  glyph_log_.clear();
  glyph_ink_.clear();
  if (record_below_ != 0) {
    std::fill(ink_owner_.begin(), ink_owner_.end(), -1);
  }
  for (std::size_t i = 0; i < kFramebufferBytes; i += 4) {
    work_[i] = work_[i + 1] = work_[i + 2] = 0;
    work_[i + 3] = 255;
  }
  ctx_ = Context{};
  palette_source_ = 0;
  depth_ = 0;
  prim_ = Prim::None;
  have_first_ = false;
  discard_strip();  // a frame that failed half way through a strip leaves nothing behind
  done_ = false;
  for (word_index_ = 0; word_index_ < count && !done_; ++word_index_) {
    if (!exec(words[word_index_])) {
      return false;
    }
  }
  if (!done_) {
    word_index_ = count;
    return fail("list ends without DISPLAY", 0);
  }
  front_.swap(work_);
  ++frames_;
  last_digest_ = digest_words(words, count);
  last_line_prims_ = *std::max_element(std::begin(line_prims_), std::end(line_prims_));
  last_line_px_ = *std::max_element(std::begin(line_px_), std::end(line_px_));
  peak_line_prims_ = std::max(peak_line_prims_, last_line_prims_);
  peak_line_px_ = std::max(peak_line_px_, last_line_px_);
  return true;
}

bool EveRaster::write_ram_g(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) {
  if (count == 0) {
    return true;
  }
  if (bytes == nullptr || addr > kRamGBytes || count > kRamGBytes - addr) {
    return false;
  }
  std::memcpy(ram_g_.data() + addr, bytes, count);
  return true;
}

bool EveRaster::clip(int& x0, int& y0, int& x1, int& y1) const {
  const int sx0 = static_cast<int>(ctx_.scissor_x);
  const int sy0 = static_cast<int>(ctx_.scissor_y);
  x0 = std::max({x0, sx0, 0});
  y0 = std::max({y0, sy0, 0});
  x1 = std::min({x1, sx0 + static_cast<int>(ctx_.scissor_w), kWidth});
  y1 = std::min({y1, sy0 + static_cast<int>(ctx_.scissor_h), kHeight});
  return x0 < x1 && y0 < y1;
}

void EveRaster::store_pixels(int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  if (!clip(x0, y0, x1, y1)) {
    return;
  }
  if (record_below_ != 0) {
    release_ink(x0, y0, x1, y1);
  }
  for (int y = y0; y < y1; ++y) {
    std::uint8_t* px = &work_[(static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x0)) * 4];
    for (int x = x0; x < x1; ++x, px += 4) {
      px[0] = r;
      px[1] = g;
      px[2] = b;
    }
  }
}

// The pixel [x0,x1) x [y0,y1), clipped, with `coverage` of the current colour: source alpha = COLOR_A * coverage.
// BLEND_FUNC(src, dst): result = colour * src + destination * dst per channel, each product rounded to 8 bit and the sum
// held at 255 [A]. The source colour is COLOR_RGB itself, coverage only enters through the alpha, so a factor of
// ONE or ZERO ignores it. The default (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) is the source-over of the first golden
// scenarios, bit for bit. The destination alpha is not modelled (the picture is opaque), so DST_ALPHA factors
// are refused when BLEND_FUNC is executed.
void EveRaster::fill_pixels(int x0, int y0, int x1, int y1, std::uint8_t coverage) {
  if (coverage == 0 || !clip(x0, y0, x1, y1)) {
    return;
  }
  const std::uint8_t alpha = mul255(coverage, ctx_.a);
  // Only an opaque source-over cover hides the ink beneath it from the legibility judge; a translucent tint leaves the text readable through it.
  if (record_below_ != 0 && !in_glyph_ && alpha == 255 && ctx_.blend_src == 2 && ctx_.blend_dst == 4) {
    release_ink(x0, y0, x1, y1);
  }
  const std::size_t stride = static_cast<std::size_t>(kWidth) * 4;
  std::uint8_t* row = &work_[static_cast<std::size_t>(y0) * stride + static_cast<std::size_t>(x0) * 4];
  if (ctx_.blend_src == 2 && ctx_.blend_dst == 4) {  // source over, the default and nearly every pixel of the UI
    if (alpha == 0) {
      return;  // nothing changes
    }
    for (int y = y0; y < y1; ++y, row += stride) {
      std::uint8_t* px = row;
      if (alpha == 255) {
        for (int x = x0; x < x1; ++x, px += 4) {
          px[0] = ctx_.r;
          px[1] = ctx_.g;
          px[2] = ctx_.b;
        }
      } else {
        for (int x = x0; x < x1; ++x, px += 4) {
          px[0] = static_cast<std::uint8_t>(mul255(ctx_.r, alpha) + mul255(px[0], 255u - alpha));
          px[1] = static_cast<std::uint8_t>(mul255(ctx_.g, alpha) + mul255(px[1], 255u - alpha));
          px[2] = static_cast<std::uint8_t>(mul255(ctx_.b, alpha) + mul255(px[2], 255u - alpha));
        }
      }
    }
    return;
  }
  const auto factor = [alpha](std::uint32_t f) -> unsigned {  // 0..255; ZERO, ONE, SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    return f == 0 ? 0u : (f == 1 ? 255u : (f == 2 ? alpha : 255u - alpha));
  };
  const unsigned fs = factor(ctx_.blend_src), fd = factor(ctx_.blend_dst);
  const std::uint8_t rgb[3] = {ctx_.r, ctx_.g, ctx_.b};
  for (int y = y0; y < y1; ++y, row += stride) {
    std::uint8_t* px = row;
    for (int x = x0; x < x1; ++x, px += 4) {
      for (int c = 0; c < 3; ++c) {
        const unsigned v = mul255(rgb[c], fs) + mul255(px[c], fd);
        px[c] = static_cast<std::uint8_t>(v > 255u ? 255u : v);
      }
    }
  }
}

// RECTS: the rectangle between the two vertices, grown by the line width on every side (corner rounding is
// not modelled). A pixel is covered when its centre lies inside, borders included.
void EveRaster::draw_rect_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
  const std::int32_t w = static_cast<std::int32_t>(ctx_.line_width);
  const std::int32_t lo_x = std::min(x0, x1) - w, hi_x = std::max(x0, x1) + w;
  const std::int32_t lo_y = std::min(y0, y1) - w, hi_y = std::max(y0, y1) + w;
  const int bx0 = ceil_div(lo_x - 8, 16), by0 = ceil_div(lo_y - 8, 16);
  const int bx1 = floor_div(hi_x - 8, 16) + 1, by1 = floor_div(hi_y - 8, 16) + 1;
  count_line_load(ctx_, bx0, by0, bx1, by1);
  fill_pixels(bx0, by0, bx1, by1, 255);
}

// LINES, LINE_STRIP segments and POINTS: a capsule of `radius_q4` (the segment thickened on both sides, round ends;
// the half-width is LINE_WIDTH, the guide's "distance from the centre of the line to its boundary", p.130, and
// POINT_SIZE is the radius of a point, p.133). Antialiased with the 8 x 8 coverage model [A]. A radius of 0 draws
// nothing [A]: not even a sample that happens to lie on the line (the guide does not say). LINES and POINTS draw each
// shape at once; a LINE_STRIP segment (`collect`) only adds its sample masks to the strip, which flush_strip() blends
// once, so the joints of a strip are not covered twice.
bool EveRaster::draw_capsule_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                                std::uint32_t radius_q4, bool collect) {
  if (radius_q4 == 0) {
    return true;
  }
  for (const std::int32_t v : {x0, y0, x1, y1}) {
    if (v < -kVertexLimit || v >= kVertexLimit) {
      return fail("LINES / LINE_STRIP / POINTS vertex beyond +-4096 px is not modelled", 0);
    }
  }
  count_capsule_box(collect ? strip_ctx_ : ctx_, x0, y0, x1, y1, radius_q4);
  const std::int64_t r = radius_q4;
  const Capsule shape(x0, y0, x1, y1, r);
  const Capsule outer(x0, y0, x1, y1, r + kPixelReach);
  const Capsule inner(x0, y0, x1, y1, r >= kPixelReach ? r - kPixelReach : 0);
  const bool has_inner = r >= kPixelReach;
  // The pixels whose centre can lie within r + 12 of the segment, clipped to the scissor and the screen (a strip
  // is clipped by the scissor of its first vertex when it is blended, so here only to the screen).
  int px_lo = static_cast<int>(floor_div64(std::int64_t{std::min(x0, x1)} - r - kPixelReach - 8, 16));
  int px_hi = static_cast<int>(floor_div64(std::int64_t{std::max(x0, x1)} + r + kPixelReach - 8, 16)) + 1;
  int py_lo = static_cast<int>(floor_div64(std::int64_t{std::min(y0, y1)} - r - kPixelReach - 8, 16));
  int py_hi = static_cast<int>(floor_div64(std::int64_t{std::max(y0, y1)} + r + kPixelReach - 8, 16)) + 1;
  if (collect) {
    px_lo = std::max(px_lo, 0);
    py_lo = std::max(py_lo, 0);
    px_hi = std::min(px_hi, kWidth);
    py_hi = std::min(py_hi, kHeight);
    if (px_lo >= px_hi || py_lo >= py_hi) {
      return true;
    }
  } else if (!clip(px_lo, py_lo, px_hi, py_hi)) {
    return true;
  }
  for (int py = py_lo; py < py_hi; ++py) {
    for (int px = px_lo; px < px_hi; ++px) {
      const std::int64_t cx = px * 16 + 8, cy = py * 16 + 8;
      if (!outer.contains(cx, cy)) {
        continue;
      }
      const std::uint64_t mask = (has_inner && inner.contains(cx, cy)) ? ~std::uint64_t{0} : sample_mask(px, py, shape);
      if (collect) {
        if (mask != 0) {
          const std::size_t index = static_cast<std::size_t>(py) * kWidth + static_cast<std::size_t>(px);
          if (line_mask_[index] == 0) {
            line_touched_.push_back(static_cast<std::uint32_t>(index));
          }
          line_mask_[index] |= mask;
        }
      } else {
        fill_pixels(px, py, px + 1, py + 1, coverage_of(mask));
      }
    }
  }
  return true;
}

// EDGE_STRIP_A / _B: the area above / below the polyline to the edge of the screen (guide section 2.5.4, pp.22-23,
// the figures; the guide gives no text). Per segment, every pixel column whose centre lies between the two
// vertices (the ends included) is filled from the segment up to row 0 (A) or down to the last row (B), the
// segment's own line moved by LINE_WIDTH away from the fill, so that the line's own thickness is part of the fill [A]
// (the guide says only that LINE_WIDTH applies, p.130). The pixel-centre test is exact: the line's row at a column
// centre is a rational number, compared in integers, and a centre exactly on the boundary is inside. Edges are
// not antialiased: the guide's figure on p.22 shows a stair-stepped slope [A]. The columns are only recorded here;
// flush_strip() fills them.
bool EveRaster::add_edge_segment_q4(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
  for (const std::int32_t v : {x0, y0, x1, y1}) {
    if (v < -kVertexLimit || v >= kVertexLimit) {
      return fail("EDGE_STRIP vertex beyond +-4096 px is not modelled", 0);
    }
  }
  count_capsule_box(strip_ctx_, x0, y0, x1, y1, strip_ctx_.line_width);
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const bool above = strip_above_;
  const std::int64_t w = strip_ctx_.line_width;
  const std::int64_t dx = x1 - x0, dy = y1 - y0;
  const int c_lo = std::max(0, static_cast<int>(ceil_div64(x0 - 8, 16)));
  const int c_hi = std::min(kWidth - 1, static_cast<int>(floor_div64(x1 - 8, 16)));
  for (int c = c_lo; c <= c_hi; ++c) {
    const std::int64_t cx = c * 16 + 8;
    std::int64_t row;  // A: the last row filled; B: the first
    if (dx == 0) {     // a vertical segment on this very column: the strip hangs off its lower (A) / upper (B) end
      row = above ? floor_div64(std::int64_t{std::max(y0, y1)} + w - 8, 16)
                  : ceil_div64(std::int64_t{std::min(y0, y1)} - w - 8, 16);
    } else if (above) {  // 16 r + 8 <= y(cx) + w with y(cx) = y0 + (cx - x0) * dy / dx
      row = floor_div64((cx - x0) * dy + (y0 + w - 8) * dx, 16 * dx);
    } else {             // 16 r + 8 >= y(cx) - w
      row = ceil_div64((cx - x0) * dy + (y0 - w - 8) * dx, 16 * dx);
    }
    std::int32_t& bound = strip_bound_[static_cast<std::size_t>(c)];
    if (above) {
      bound = std::max(bound, static_cast<std::int32_t>(std::clamp<std::int64_t>(row, -1, kHeight - 1)));
    } else {
      bound = std::min(bound, static_cast<std::int32_t>(std::clamp<std::int64_t>(row, 0, kHeight)));
    }
  }
  if (c_lo <= c_hi) {
    strip_lo_ = std::min(strip_lo_, c_lo);
    strip_hi_ = std::max(strip_hi_, c_hi);
  }
  return true;
}

// Blends the strip collected so far, once, with the context of its first vertex (colour, COLOR_A, BLEND_FUNC, scissor).
void EveRaster::flush_strip() {
  if (!strip_active_) {
    return;
  }
  const Context live = ctx_;
  ctx_ = strip_ctx_;
  if (strip_line_) {
    for (const std::uint32_t index : line_touched_) {
      const int px = static_cast<int>(index % kWidth), py = static_cast<int>(index / kWidth);
      fill_pixels(px, py, px + 1, py + 1, coverage_of(line_mask_[index]));
    }
  } else {
    for (int c = strip_lo_; c <= strip_hi_; ++c) {
      const int bound = strip_bound_[static_cast<std::size_t>(c)];
      if (strip_above_) {
        fill_pixels(c, 0, c + 1, bound + 1, 255);  // rows 0..bound; bound -1 leaves the column empty
      } else {
        fill_pixels(c, bound, c + 1, kHeight, 255);  // rows bound..479; bound 480 leaves it empty
      }
    }
  }
  ctx_ = live;
  discard_strip();
}

// Forgets the strip being collected (after blending it, or at the start of a frame that may have failed half way).
void EveRaster::discard_strip() {
  for (const std::uint32_t index : line_touched_) {
    line_mask_[index] = 0;
  }
  line_touched_.clear();
  strip_active_ = false;
}

bool EveRaster::draw_bitmap(int px, int py, std::uint32_t handle, std::uint32_t cell) {
  if (handle == ui::plot::kHandleApp) {
    ++handle_app_uses_;
  }
  const Handle& h = handles_[handle & 31u];
  const std::uint32_t stride = (h.stride_hi << 10) | h.stride_lo;
  const std::uint32_t layout_h = (h.layout_h_hi << 9) | h.layout_h_lo;
  // BITMAP_SIZE: width and height of 0 mean 2048 (guide section 4.9).
  const std::uint32_t width = ((h.width_hi << 9) | h.width_lo) != 0 ? (h.width_hi << 9) | h.width_lo : 2048u;
  const std::uint32_t height = ((h.height_hi << 9) | h.height_lo) != 0 ? (h.height_hi << 9) | h.height_lo : 2048u;
  const bool paletted = h.format == 14;  // PALETTED565
  if (h.format != 1 && h.format != 2 && h.format != 3 && h.format != 17 && !paletted) {  // L1, L4, L8, L2
    return fail("bitmap format is not L1/L2/L4/L8/PALETTED565", h.format);
  }
  if (stride == 0 || layout_h == 0) {
    return fail("bitmap stride or layout height is zero", 0);
  }
  const std::uint64_t cell_bytes = std::uint64_t{stride} * layout_h;
  const std::uint64_t base = std::uint64_t{h.source} + std::uint64_t{cell} * cell_bytes;
  if (base + cell_bytes > kRamGBytes) {
    return fail("bitmap cell lies outside RAM_G", h.source);
  }
  // The chip fetches the whole BITMAP_SIZE box on every row it covers, ink or not (a transformed bitmap too: its box is the drawn rectangle).
  count_line_load(ctx_, px, py, px + static_cast<int>(width), py + static_cast<int>(height));
  const unsigned pixels_per_byte = h.format == 1 ? 8u : (h.format == 17 ? 4u : (h.format == 2 ? 2u : 1u));
  const std::uint32_t layout_w = stride * pixels_per_byte;
  // The alpha of source pixel (sx, sy) of the cell: L1 the bit (0 / 255, MSB leftmost), L2 two bits (x 85, leftmost in
  // bits 7-6, guide Figure 6 p.100), L4 a nibble (x 17, high nibble first), L8 the byte.
  const auto alpha_at = [&](std::uint32_t sx, std::uint32_t sy) -> unsigned {
    const std::uint8_t byte = ram_g_[static_cast<std::size_t>(base + std::uint64_t{sy} * stride + sx / pixels_per_byte)];
    switch (h.format) {
      case 1:
        return ((byte >> (7u - sx % 8u)) & 1u) ? 255u : 0u;
      case 17:
        return ((byte >> (6u - 2u * (sx % 4u))) & 3u) * 85u;
      case 2:
        return ((sx % 2u) == 0 ? byte >> 4 : byte & 0xFu) * 17u;
      default:
        return paletted ? 255u : byte;  // a palette entry is opaque (PALETTED565 has no alpha)
    }
  };
  // PALETTED565: the byte is an index into the 256 RGB565 entries (little endian) at PALETTE_SOURCE, drawn in one pass with its own
  // colour times COLOR_RGB. [A] The 5 and 6 bit channels widen by repeating their top bits (31 -> 255), and COLOR_RGB modulates
  // (255 leaves the entry as it is); which of the chip's roundings it really is, only a unit can say. paint() draws the source
  // pixel (sx, sy) at (x, y): an alpha mask through the current colour, or a paletted pixel through its entry.
  if (paletted && std::uint64_t{palette_source_} + 512u > kRamGBytes) {
    return fail("PALETTE_SOURCE: the 512-byte palette lies outside RAM_G", palette_source_);
  }
  const auto paint = [&](int x, int y, std::uint32_t sx, std::uint32_t sy) {
    const unsigned sample = alpha_at(sx, sy);
    if (sample == 0) {
      return;
    }
    if (!paletted) {
      fill_pixels(x, y, x + 1, y + 1, static_cast<std::uint8_t>(sample));
      return;
    }
    const std::uint8_t index = ram_g_[static_cast<std::size_t>(base + std::uint64_t{sy} * stride + sx)];
    const std::size_t entry = std::size_t{palette_source_} + 2u * index;
    const unsigned rgb565 = ram_g_[entry] | (unsigned{ram_g_[entry + 1]} << 8);
    const unsigned r5 = (rgb565 >> 11) & 31u, g6 = (rgb565 >> 5) & 63u, b5 = rgb565 & 31u;
    const Context keep = ctx_;
    ctx_.r = static_cast<std::uint8_t>(mul255(static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)), keep.r));
    ctx_.g = static_cast<std::uint8_t>(mul255(static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)), keep.g));
    ctx_.b = static_cast<std::uint8_t>(mul255(static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)), keep.b));
    fill_pixels(x, y, x + 1, y + 1, 255);
    ctx_ = keep;
  };
  const int sx0 = std::max(static_cast<int>(ctx_.scissor_x), 0);
  const int sy0 = std::max(static_cast<int>(ctx_.scissor_y), 0);
  const int sx1 = std::min(static_cast<int>(ctx_.scissor_x + ctx_.scissor_w), kWidth);
  const int sy1 = std::min(static_cast<int>(ctx_.scissor_y + ctx_.scissor_h), kHeight);

  const bool identity = ctx_.xf_a == 256 && ctx_.xf_b == 0 && ctx_.xf_c == 0 && ctx_.xf_d == 0 && ctx_.xf_e == 256 && ctx_.xf_f == 0;
  if (!identity) {
    // A transformed bitmap (OS-003f: the boot logo's integer scale). The drawn rectangle is BITMAP_SIZE wide and high; each of
    // its pixels shows the source pixel the matrix maps its centre to, floored: (A (dx + 1/2) + B (dy + 1/2) + C) / 256 and
    // (D (dx + 1/2) + E (dy + 1/2) + F) / 256, in integers (twice everything: the half pixel is exact). Outside the layout a BORDER bitmap is
    // transparent and a REPEAT one wraps. [A] that the chip samples at the pixel centre (the guide gives the matrix, not the
    // sampling point; for a scale by a power of two both conventions give the same pixels, for A = 85 they differ by at most
    // one column). NEAREST only: bilinear filtering is not modelled and fails loudly.
    if (h.filter != 0) {
      return fail("BITMAP_SIZE BILINEAR with a BITMAP_TRANSFORM is not modelled (NEAREST only)", h.filter);
    }
    const int tx0 = std::max(0, sx0 - px), tx1 = std::min(static_cast<int>(width), sx1 - px);
    const int ty0 = std::max(0, sy0 - py), ty1 = std::min(static_cast<int>(height), sy1 - py);
    for (int dy = ty0; dy < ty1; ++dy) {
      for (int dx = tx0; dx < tx1; ++dx) {
        const std::int64_t cx = 2 * std::int64_t{dx} + 1, cy = 2 * std::int64_t{dy} + 1;
        const std::int64_t u = floor_div64(ctx_.xf_a * cx + ctx_.xf_b * cy + 2 * std::int64_t{ctx_.xf_c}, 512);
        const std::int64_t v = floor_div64(ctx_.xf_d * cx + ctx_.xf_e * cy + 2 * std::int64_t{ctx_.xf_f}, 512);
        std::int64_t sx = u, sy = v;
        if (h.wrap_x) {
          sx = ((sx % layout_w) + layout_w) % layout_w;
        } else if (sx < 0 || sx >= layout_w) {
          continue;
        }
        if (h.wrap_y) {
          sy = ((sy % layout_h) + layout_h) % layout_h;
        } else if (sy < 0 || sy >= layout_h) {
          continue;
        }
        paint(px + dx, py + dy, static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
      }
    }
    return true;
  }

  // The part of the drawn rectangle that is visible: columns dx0..dx1, rows dy0..dy1 (exclusive ends). Beyond the
  // layout a BORDER bitmap is transparent, so only a REPEAT bitmap needs more than the layout's own size.
  const std::uint32_t reach_x = h.wrap_x ? width : std::min(width, layout_w);
  const std::uint32_t reach_y = h.wrap_y ? height : std::min(height, layout_h);
  if (record_below_ != 0 && h.source < record_below_) {
    // The bitmap log (record_glyphs): the same pixels, but walked over the whole drawn rectangle, so that the ink the scissor or the screen
    // edge cuts is counted (ink_total against ink_visible), and the colour under every visible ink pixel is kept before it is blended.
    GlyphDraw g;
    g.handle = static_cast<std::uint8_t>(handle & 31u);
    g.cell = static_cast<std::uint16_t>(cell);
    g.source = h.source;
    g.x = px;
    g.y = py;
    g.w = static_cast<std::uint16_t>(width);
    g.h = static_cast<std::uint16_t>(height);
    g.r = ctx_.r;
    g.g = ctx_.g;
    g.b = ctx_.b;
    g.a = ctx_.a;
    g.scissor_x = ctx_.scissor_x;
    g.scissor_y = ctx_.scissor_y;
    g.scissor_w = ctx_.scissor_w;
    g.scissor_h = ctx_.scissor_h;
    g.ink_first = static_cast<std::uint32_t>(glyph_ink_.size());
    const std::int32_t self = static_cast<std::int32_t>(glyph_log_.size());
    g.ink_x0 = g.ink_y0 = INT32_MAX;
    g.ink_x1 = g.ink_y1 = INT32_MIN;
    for (int dy = 0; dy < static_cast<int>(reach_y); ++dy) {
      const std::uint32_t sy = static_cast<std::uint32_t>(dy) % layout_h;
      for (int dx = 0; dx < static_cast<int>(reach_x); ++dx) {
        const unsigned sample = alpha_at(static_cast<std::uint32_t>(dx) % layout_w, sy);
        if (sample == 0) {
          continue;
        }
        const int x = px + dx, y = py + dy;
        ++g.ink_total;
        g.ink_x0 = std::min(g.ink_x0, x);
        g.ink_y0 = std::min(g.ink_y0, y);
        g.ink_x1 = std::max(g.ink_x1, x + 1);
        g.ink_y1 = std::max(g.ink_y1, y + 1);
        if (x >= sx0 && x < sx1 && y >= sy0 && y < sy1) {
          const std::size_t pixel = static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x);
          glyph_ink_.push_back({static_cast<std::uint32_t>(pixel), work_[pixel * 4], work_[pixel * 4 + 1], work_[pixel * 4 + 2], ink_owner_[pixel]});
          ++g.ink_visible;
          in_glyph_ = true;
          fill_pixels(x, y, x + 1, y + 1, static_cast<std::uint8_t>(sample));
          in_glyph_ = false;
          ink_owner_[pixel] = self;
        }
      }
    }
    if (g.ink_total == 0) {
      g.ink_x0 = g.ink_y0 = g.ink_x1 = g.ink_y1 = 0;
    }
    glyph_log_.push_back(g);
    return true;
  }
  const int dx0 = std::max(0, sx0 - px), dx1 = std::min(static_cast<int>(reach_x), sx1 - px);
  const int dy0 = std::max(0, sy0 - py), dy1 = std::min(static_cast<int>(reach_y), sy1 - py);
  for (int dy = dy0; dy < dy1; ++dy) {
    const std::uint32_t sy = static_cast<std::uint32_t>(dy) % layout_h;
    for (int dx = dx0; dx < dx1; ++dx) {
      paint(px + dx, py + dy, static_cast<std::uint32_t>(dx) % layout_w, sy);
    }
  }
  return true;
}

bool EveRaster::vertex(std::int32_t x_q4, std::int32_t y_q4) {
  switch (prim_) {
    case Prim::None:
      return fail("vertex outside BEGIN/END", 0);
    case Prim::Bitmaps:
      return draw_bitmap(floor_div(x_q4, 16), floor_div(y_q4, 16), ctx_.handle, ctx_.cell);
    case Prim::Points:
      return draw_capsule_q4(x_q4, y_q4, x_q4, y_q4, ctx_.point_size);
    case Prim::Rects:
    case Prim::Lines:
      if (!have_first_) {
        first_x_ = x_q4;
        first_y_ = y_q4;
        have_first_ = true;
      } else {
        have_first_ = false;
        if (prim_ == Prim::Rects) {
          draw_rect_q4(first_x_, first_y_, x_q4, y_q4);
        } else {
          return draw_capsule_q4(first_x_, first_y_, x_q4, y_q4, ctx_.line_width);
        }
      }
      return true;
    case Prim::LineStrip:
    case Prim::EdgeStripA:
    case Prim::EdgeStripB: {
      const bool had = have_first_;
      const std::int32_t from_x = first_x_, from_y = first_y_;
      first_x_ = x_q4;  // every vertex starts the next segment
      first_y_ = y_q4;
      have_first_ = true;
      if (!had) {  // a strip is blended once, in the context of its first vertex
        strip_ctx_ = ctx_;
        strip_active_ = true;
        strip_line_ = prim_ == Prim::LineStrip;
        strip_above_ = prim_ == Prim::EdgeStripA;
        if (strip_line_) {
          line_mask_.resize(static_cast<std::size_t>(kWidth) * kHeight, 0);
        } else {
          strip_bound_.assign(kWidth, strip_above_ ? -1 : kHeight);
          strip_lo_ = kWidth;
          strip_hi_ = -1;
        }
        return true;  // a strip needs two vertices to draw anything
      }
      return strip_line_ ? draw_capsule_q4(from_x, from_y, x_q4, y_q4, strip_ctx_.line_width, true)
                         : add_edge_segment_q4(from_x, from_y, x_q4, y_q4);
    }
  }
  return false;
}

bool EveRaster::exec(std::uint32_t word) {
  const std::uint32_t tag = word >> 30;
  if (tag == 1) {  // VERTEX2F: signed 15-bit x [29:15], y [14:0] in 2^-frac px, then VERTEX_TRANSLATE (1/16 px)
    const std::int32_t x = sign_extend15(word >> 15), y = sign_extend15(word);
    const std::int32_t scale = 16 >> ctx_.vertex_frac;  // frac <= 4, so 1, 2, 4, 8 or 16 Q4 units per step
    return vertex(x * scale + ctx_.translate_x, y * scale + ctx_.translate_y);
  }
  if (tag == 2) {  // VERTEX2II: unsigned pixels x [29:21], y [20:12], handle [11:7], cell [6:0]
    const std::int32_t x = static_cast<std::int32_t>((word >> 21) & 0x1FFu) * 16 + ctx_.translate_x;
    const std::int32_t y = static_cast<std::int32_t>((word >> 12) & 0x1FFu) * 16 + ctx_.translate_y;
    if (prim_ == Prim::Bitmaps) {  // own handle and cell; registers untouched
      return draw_bitmap(floor_div(x, 16), floor_div(y, 16), (word >> 7) & 31u, word & 127u);
    }
    return vertex(x, y);
  }
  if (tag == 3) {
    return fail("unknown display-list word (top bits 11)", word);
  }

  const std::uint32_t opcode = word >> 24;
  Handle& h = handles_[ctx_.handle];
  if (ctx_.handle == ui::plot::kHandleApp && (opcode == op::kBitmapSource || opcode == op::kBitmapLayout || opcode == op::kBitmapLayoutH ||
                                              opcode == op::kBitmapSize || opcode == op::kBitmapSizeH)) {
    ++handle_app_uses_;
  }
  switch (opcode) {
    case op::kDisplay:
      flush_strip();
      done_ = true;
      return true;
    case op::kBitmapSource:
      h.source = word & 0x3FFFFFu;
      return true;
    case op::kPaletteSource:
      palette_source_ = word & 0x3FFFFFu;
      return true;
    case op::kClearColorRgb:
      ctx_.clear_r = static_cast<std::uint8_t>(word >> 16);
      ctx_.clear_g = static_cast<std::uint8_t>(word >> 8);
      ctx_.clear_b = static_cast<std::uint8_t>(word);
      return true;
    case op::kColorRgb:
      ctx_.r = static_cast<std::uint8_t>(word >> 16);
      ctx_.g = static_cast<std::uint8_t>(word >> 8);
      ctx_.b = static_cast<std::uint8_t>(word);
      return true;
    case op::kBitmapHandle:
      ctx_.handle = word & 31u;
      return true;
    case op::kCell:
      ctx_.cell = word & 127u;
      return true;
    case op::kBitmapLayout:
      h.format = (word >> 19) & 31u;
      h.stride_lo = (word >> 9) & 1023u;
      h.layout_h_lo = word & 511u;
      return true;
    case op::kBitmapSize:
      h.filter = (word >> 20) & 1u;
      h.wrap_x = (word >> 19) & 1u;
      h.wrap_y = (word >> 18) & 1u;
      h.width_lo = (word >> 9) & 511u;
      h.height_lo = word & 511u;
      return true;  // the filter bit [20] only matters for a transformed bitmap (draw_bitmap)
    case op::kBitmapTransformA:
      ctx_.xf_a = sign_extend17(word);
      return true;
    case op::kBitmapTransformB:
      ctx_.xf_b = sign_extend17(word);
      return true;
    case op::kBitmapTransformC:
      ctx_.xf_c = sign_extend24(word);
      return true;
    case op::kBitmapTransformD:
      ctx_.xf_d = sign_extend17(word);
      return true;
    case op::kBitmapTransformE:
      ctx_.xf_e = sign_extend17(word);
      return true;
    case op::kBitmapTransformF:
      ctx_.xf_f = sign_extend24(word);
      return true;
    case op::kLineWidth:
      ctx_.line_width = word & 4095u;
      return true;
    case op::kPointSize:
      ctx_.point_size = word & 8191u;
      return true;
    case op::kBlendFunc: {
      const std::uint32_t src = (word >> 3) & 7u, dst = word & 7u;
      for (const std::uint32_t f : {src, dst}) {
        if (f == 3 || f == 5) {  // DST_ALPHA, ONE_MINUS_DST_ALPHA
          return fail("BLEND_FUNC with a destination-alpha factor is not modelled (the picture has no alpha)", word);
        }
        if (f > 5) {
          return fail("BLEND_FUNC factor must be 0..5 (guide Table 8)", word);
        }
      }
      ctx_.blend_src = src;
      ctx_.blend_dst = dst;
      return true;
    }
    case op::kVertexTranslateX:
      ctx_.translate_x = sign_extend17(word);
      return true;
    case op::kVertexTranslateY:
      ctx_.translate_y = sign_extend17(word);
      return true;
    case op::kColorA:
      ctx_.a = static_cast<std::uint8_t>(word);
      return true;
    case op::kScissorXy:
      ctx_.scissor_x = (word >> 11) & 2047u;
      ctx_.scissor_y = word & 2047u;
      return true;
    case op::kScissorSize:
      ctx_.scissor_w = (word >> 12) & 4095u;
      ctx_.scissor_h = word & 4095u;
      return true;
    case op::kBegin: {
      const std::uint32_t prim = word & 15u;
      flush_strip();
      have_first_ = false;
      switch (static_cast<eve::Primitive>(prim)) {
        case eve::Primitive::Bitmaps:
          prim_ = Prim::Bitmaps;
          break;
        case eve::Primitive::Points:
          prim_ = Prim::Points;
          break;
        case eve::Primitive::Lines:
          prim_ = Prim::Lines;
          break;
        case eve::Primitive::LineStrip:
          prim_ = Prim::LineStrip;
          break;
        case eve::Primitive::EdgeStripA:
          prim_ = Prim::EdgeStripA;
          break;
        case eve::Primitive::EdgeStripB:
          prim_ = Prim::EdgeStripB;
          break;
        case eve::Primitive::Rects:
          prim_ = Prim::Rects;
          break;
        default:
          return fail("BEGIN with a primitive outside the subset (BITMAPS, POINTS, LINES, LINE_STRIP, EDGE_STRIP_A/B, RECTS)", word);
      }
      return true;
    }
    case op::kEnd:
      flush_strip();
      prim_ = Prim::None;
      have_first_ = false;
      return true;
    case op::kSaveContext:
      if (depth_ == 4) {  // an extra push drops the oldest
        std::copy(stack_ + 1, stack_ + 4, stack_);
        --depth_;
      }
      stack_[depth_++] = ctx_;
      return true;
    case op::kRestoreContext:
      ctx_ = depth_ > 0 ? stack_[--depth_] : Context{};  // an extra pop loads the defaults
      return true;
    case op::kClear:
      if (word & 4u) {  // colour buffer; the stencil and tag buffers do not exist here
        store_pixels(0, 0, kWidth, kHeight, ctx_.clear_r, ctx_.clear_g, ctx_.clear_b);  // opaque, whatever BLEND_FUNC says
      }
      return true;
    case op::kVertexFormat:
      if ((word & 7u) > 4u) {
        return fail("VERTEX_FORMAT frac must be 0..4", word);
      }
      ctx_.vertex_frac = word & 7u;
      return true;
    case op::kBitmapLayoutH:
      h.stride_hi = (word >> 2) & 3u;
      h.layout_h_hi = word & 3u;
      return true;
    case op::kBitmapSizeH:
      h.width_hi = (word >> 2) & 3u;
      h.height_hi = word & 3u;
      return true;
    default:
      return fail("unsupported display-list command", word);
  }
}

}  // namespace o3sdk::sim
