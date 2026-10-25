// SPDX-License-Identifier: GPL-3.0-or-later
//
// The simulator's bitmap transform (OS-003f, guide sections 2.5.5 and 4.12-4.17): scaling, flipping, transposing and
// shifting a bitmap with BITMAP_TRANSFORM_A-F, nearest sampling at the pixel centre, the L2 format, and what is saved
// with the graphics context. Every expectation is worked out by hand from the matrix x' = A x + B y + C, y' = D x + E y + F
// (8.8 and 15.8 fixed point), not from the code under test.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/sim/eve_raster.h>

#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;
using o3sdk::sim::EveRaster;

namespace {

using Words = std::vector<std::uint32_t>;

std::uint32_t px(const EveRaster& r, int x, int y) {
  const std::uint8_t* p = r.rgba() + (static_cast<std::size_t>(y) * EveRaster::kWidth + static_cast<std::size_t>(x)) * 4;
  return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}

// The grey of the pixel (a white bitmap on black: the alpha, repeated).
unsigned grey(const EveRaster& r, int x, int y) { return px(r, x, y) & 0xFF; }

bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

constexpr std::uint32_t kWhite = 0xFFFFFF;

// A handle 1 bitmap: `format`, `stride` bytes a row, `rows` rows, drawn `w` x `h`, at RAM_G 0.
Words setup(BitmapFormat format, unsigned stride, unsigned rows, unsigned w, unsigned h, Wrap wrap = Wrap::Border,
            Filter filter = Filter::Nearest) {
  return {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(format, stride, rows),
          dl::bitmap_size(filter, wrap, wrap, w, h)};
}

Words concat(Words a, const Words& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// The left edge of every run of equal values along row `y` from x0 to x1, with the value: {start, value}.
struct Run {
  int start;
  unsigned value;
};
std::vector<Run> runs(const EveRaster& r, int y, int x0, int x1) {
  std::vector<Run> out;
  for (int x = x0; x < x1; ++x) {
    const unsigned g = grey(r, x, y);
    if (out.empty() || out.back().value != g) {
      out.push_back({x, g});
    }
  }
  return out;
}

}  // namespace

TEST_CASE("transform: the identity (the default, or 256 / 0 written out) draws exactly what an untransformed bitmap does") {
  EveRaster r, r2;
  const std::uint8_t data[] = {0xA5, 0x3C};  // L1 8 x 2
  std::memcpy(r.ram_g(), data, 2);
  std::memcpy(r2.ram_g(), data, 2);
  const Words draw = concat(setup(BitmapFormat::L1, 1, 2, 8, 2), {dl::begin(Primitive::Bitmaps), dl::vertex2f(160, 320)});
  CHECK(run(r, draw));
  Words explicit_identity = {dl::bitmap_transform_a(256), dl::bitmap_transform_b(0), dl::bitmap_transform_c(0),
                             dl::bitmap_transform_d(0), dl::bitmap_transform_e(256), dl::bitmap_transform_f(0)};
  CHECK(run(r2, concat(explicit_identity, draw)));
  CHECK(std::memcmp(r.rgba(), r2.rgba(), EveRaster::kFramebufferBytes) == 0);
  CHECK(grey(r, 10, 20) == 255);  // 0xA5 = 1010 0101: pixel 0 set
  CHECK(grey(r, 11, 20) == 0);
  CHECK(grey(r, 12, 20) == 255);
}

TEST_CASE("transform: A = E = 128 doubles the bitmap (the guide's example, p.108), nearest, every source pixel 2 x 2") {
  EveRaster r;
  const std::uint8_t data[] = {0x90, 0x60};  // L1 4 x 2 used (stride 1): row 0 = 1001, row 1 = 0110
  std::memcpy(r.ram_g(), data, 2);
  const Words w = concat(concat(setup(BitmapFormat::L1, 1, 2, 8, 4), {dl::bitmap_transform_a(128), dl::bitmap_transform_e(128)}),
                         {dl::begin(Primitive::Bitmaps), dl::vertex2f(16 * 100, 16 * 50)});
  CHECK(run(r, w));
  // columns 100..107, rows 50..53: source x = dx / 2, source y = dy / 2; the layout is 8 wide, so source columns 4..7 are 0
  const char* expect[4] = {"11000011", "11000011", "00111100", "00111100"};
  for (int dy = 0; dy < 4; ++dy) {
    for (int dx = 0; dx < 8; ++dx) {
      CHECK_MESSAGE(grey(r, 100 + dx, 50 + dy) == (expect[dy][dx] == '1' ? 255u : 0u), "at " << dx << "," << dy);
    }
  }
  // the same picture at 4 pixels a source pixel (A = E = 64) reaches 32 x 8
  CHECK(run(r, concat(concat(setup(BitmapFormat::L1, 1, 2, 32, 8), {dl::bitmap_transform_a(64), dl::bitmap_transform_e(64)}),
                      {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)})));
  CHECK(grey(r, 3, 3) == 255);
  CHECK(grey(r, 4, 3) == 0);
  CHECK(grey(r, 12, 3) == 255);  // source pixel 3 of row 0 (1001) is set: columns 12..15
  CHECK(grey(r, 12, 0) == 255);
  CHECK(grey(r, 15, 3) == 255);
  CHECK(grey(r, 16, 0) == 0);    // source column 4 is beyond the data (BORDER, the layout is 8 wide: the byte's pixel 4 is 0)
  CHECK(grey(r, 4, 4) == 255);   // row 1, source x 1: 0110
  CHECK(grey(r, 0, 4) == 0);
}

TEST_CASE("transform: a scale of 1/3 with the nearest 8.8 value, 85, is a block of 3 px except two: block 42 is 4 px, the last is 2") {
  // A 124-pixel L8 row whose pixel k has the value k + 1, drawn 372 wide at A = 85 (0.332 against 0.3333). The source pixel of
  // destination x is floor((x + 1/2) * 85 / 256); it steps to k at x = ceil(256 k / 85 - 1/2) = ceil(3.0118 k - 1/2): 3 6 9 ...
  // 126 (k = 42, as 3 k), then 130 (k = 43: 129.006 -> 130) and on, one later than 3 k. So block 42 spans x = 126..129 (4 px)
  // and every later one starts a pixel late; the last, k = 123, starts at ceil(369.9) = 370 and the picture ends at 372 (2 px).
  EveRaster r;
  for (unsigned k = 0; k < 124; ++k) {
    r.ram_g()[k] = static_cast<std::uint8_t>(k + 1);
  }
  const Words w = concat(concat(setup(BitmapFormat::L8, 124, 1, 372, 1), {dl::bitmap_transform_a(85)}),
                         {dl::begin(Primitive::Bitmaps), dl::vertex2f(16 * 214, 16 * 10)});
  // E stays 256: one source row, drawn 1 px high
  CHECK(run(r, w));
  const std::vector<Run> rs = runs(r, 10, 214, 214 + 372);
  CHECK(rs.size() == 124);
  bool ordered = true;
  for (std::size_t i = 0; i < rs.size(); ++i) {
    ordered = ordered && rs[i].value == i + 1;
  }
  CHECK(ordered);
  CHECK(rs[0].start == 214);
  CHECK(rs[42].start == 214 + 126);
  CHECK(rs[43].start == 214 + 130);   // block 42 is 4 px
  CHECK(rs[123].start == 214 + 370);  // block 123 is 2 px: the rest of the row ends at 372
  for (std::size_t i = 0; i + 1 < rs.size(); ++i) {
    const int width = rs[i + 1].start - rs[i].start;
    CHECK_MESSAGE(width == (i == 42 ? 4 : 3), "block " << i << " is " << width << " px");
  }
  CHECK(grey(r, 214 + 371, 10) == 124);
  CHECK(grey(r, 214 + 372, 10) == 0);  // nothing beyond BITMAP_SIZE
}

TEST_CASE("transform: B and D swap the axes, a negative A mirrors, C and F shift the source (15.8, in source pixels)") {
  EveRaster r;
  const std::uint8_t data[] = {0xC0, 0x80, 0x00};  // L1 4 x 3: rows 1100, 1000, 0000 (stride 1)
  std::memcpy(r.ram_g(), data, 3);
  const Words image = setup(BitmapFormat::L1, 1, 3, 4, 4);
  const Words draw = {dl::begin(Primitive::Bitmaps), dl::vertex2f(16 * 20, 16 * 20)};
  const auto at = [&](int dx, int dy) { return grey(r, 20 + dx, 20 + dy) != 0; };

  // transposed: x' = y, y' = x (A = 0, B = 256, D = 256, E = 0): pixel (dx, dy) shows source (dy, dx)
  CHECK(run(r, concat(concat(image, {dl::bitmap_transform_a(0), dl::bitmap_transform_b(256), dl::bitmap_transform_d(256),
                                     dl::bitmap_transform_e(0)}), draw)));
  // the source has (0,0), (1,0) and (0,1) set; the picture shows source (dy, dx) at (dx, dy): (0,0), (0,1) and (1,0)
  CHECK((at(0, 0) && at(0, 1) && at(1, 0)));
  CHECK((!at(1, 1) && !at(2, 0) && !at(0, 2) && !at(0, 3)));

  // mirrored in x: A = -256 with C = 4 * 256 gives u = floor(4 - (x + 1/2)) = 3 - x for x = 0..3
  CHECK(run(r, concat(concat(image, {dl::bitmap_transform_a(-256), dl::bitmap_transform_c(4 * 256)}), draw)));
  CHECK((!at(0, 0) && !at(1, 0) && at(2, 0) && at(3, 0)));  // row 0 = 1100 reversed: 0011
  CHECK((!at(0, 1) && !at(1, 1) && !at(2, 1) && at(3, 1)));  // 1000 -> 0001
  CHECK((!at(0, 2) && !at(3, 2)));

  // shifted: C = 2 * 256 moves the source 2 px to the left on the screen (the pixel at x shows source x + 2), F = 256 one row up
  CHECK(run(r, concat(concat(image, {dl::bitmap_transform_c(2 * 256), dl::bitmap_transform_f(256)}), draw)));
  CHECK((!at(0, 0) && !at(1, 0) && !at(2, 0)));  // row 1 of the source (1000), from column 2: nothing
  CHECK(run(r, concat(concat(image, {dl::bitmap_transform_c(-256)}), draw)));
  CHECK((!at(0, 0) && at(1, 0) && at(2, 0) && !at(3, 0)));  // row 0 (1100) one pixel to the right; u = -1 is BORDER: transparent
}

TEST_CASE("transform: outside the layout a BORDER bitmap is transparent and a REPEAT one wraps, in both axes") {
  EveRaster r;
  const std::uint8_t data[] = {0x80, 0x00};  // L1 8 x 2: only the top-left source pixel
  std::memcpy(r.ram_g(), data, 2);
  const Words scale2 = {dl::bitmap_transform_a(128), dl::bitmap_transform_e(128)};
  const Words draw = {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)};
  // 32 x 8 drawn at x2: the 8 x 2 source covers 16 x 4; beyond it BORDER shows nothing, REPEAT repeats the source
  CHECK(run(r, concat(concat(setup(BitmapFormat::L1, 1, 2, 32, 8, Wrap::Border), scale2), draw)));
  int lit = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 32; ++x) {
      lit += grey(r, x, y) != 0 ? 1 : 0;
    }
  }
  CHECK(lit == 4);  // the one source pixel, 2 x 2
  CHECK(run(r, concat(concat(setup(BitmapFormat::L1, 1, 2, 32, 8, Wrap::Repeat), scale2), draw)));
  lit = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 32; ++x) {
      lit += grey(r, x, y) != 0 ? 1 : 0;
    }
  }
  CHECK(lit == 4 * 2 * 2);  // 2 repeats across (16 px each), 2 down (4 px each), each 2 x 2
  CHECK(grey(r, 16, 0) == 255);
  CHECK(grey(r, 0, 4) == 255);
  CHECK(grey(r, 16, 4) == 255);
  // a negative source coordinate wraps from the far side
  CHECK(run(r, concat(concat(setup(BitmapFormat::L1, 1, 2, 8, 2, Wrap::Repeat), {dl::bitmap_transform_c(-256)}), draw)));
  CHECK(grey(r, 1, 0) == 255);  // source x = 1 - 1 = 0
  CHECK(grey(r, 0, 0) == 0);    // x = -1 wraps to 7: unset
}

TEST_CASE("transform: L2 is two bits a pixel, the first in bits 7-6, alpha 0, 85, 170, 255; L4 and L8 scale the same way") {
  EveRaster r;
  const std::uint8_t l2[] = {0x1B, 0xE4};  // 00 01 10 11 / 11 10 01 00
  std::memcpy(r.ram_g(), l2, 2);
  CHECK(run(r, concat(setup(BitmapFormat::L2, 1, 2, 4, 2), {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)})));
  CHECK(grey(r, 0, 0) == 0);
  CHECK(grey(r, 1, 0) == 85);
  CHECK(grey(r, 2, 0) == 170);
  CHECK(grey(r, 3, 0) == 255);
  CHECK(grey(r, 0, 1) == 255);
  CHECK(grey(r, 3, 1) == 0);
  // scaled by 2 in x only
  CHECK(run(r, concat(concat(setup(BitmapFormat::L2, 1, 2, 8, 2), {dl::bitmap_transform_a(128)}),
                      {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)})));
  CHECK(grey(r, 2, 0) == 85);
  CHECK(grey(r, 3, 0) == 85);
  CHECK(grey(r, 6, 0) == 255);
  CHECK(grey(r, 7, 0) == 255);
  // L4: the high nibble is the first pixel
  const std::uint8_t l4[] = {0x3F};
  std::memcpy(r.ram_g(), l4, 1);
  CHECK(run(r, concat(concat(setup(BitmapFormat::L4, 1, 1, 6, 1), {dl::bitmap_transform_a(128)}),
                      {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)})));
  CHECK(grey(r, 0, 0) == 3 * 17);
  CHECK(grey(r, 1, 0) == 3 * 17);
  CHECK(grey(r, 2, 0) == 255);
  CHECK(grey(r, 3, 0) == 255);
  CHECK(grey(r, 4, 0) == 0);  // the third nibble of a one-byte row: past the data
}

TEST_CASE("transform: it applies to VERTEX2II bitmaps, to COLOR_RGB and COLOR_A, and is clipped by the scissor") {
  EveRaster r;
  const std::uint8_t data[] = {0xFF};
  std::memcpy(r.ram_g(), data, 1);
  const Words scaled = concat(setup(BitmapFormat::L1, 1, 1, 64, 1), {dl::bitmap_transform_a(32)});  // 8x: 64 px for the 8 source pixels
  Words w = concat(scaled, {dl::color_rgb(255, 0, 0), dl::begin(Primitive::Bitmaps), dl::vertex2ii(10, 10, 1, 0)});
  CHECK(run(r, w));
  CHECK(px(r, 10, 10) == 0xFF0000);
  CHECK(px(r, 73, 10) == 0xFF0000);  // the 8-pixel bitmap, 64 px wide
  CHECK(px(r, 74, 10) == 0);
  // the scissor cuts the scaled picture, not the source
  w = concat({dl::scissor_xy(20, 0), dl::scissor_size(4, 100)}, w);
  CHECK(run(r, w));
  CHECK(px(r, 19, 10) == 0);
  CHECK(px(r, 20, 10) == 0xFF0000);
  CHECK(px(r, 23, 10) == 0xFF0000);
  CHECK(px(r, 24, 10) == 0);
  // COLOR_A scales the alpha of a transformed bitmap too
  CHECK(run(r, concat(concat(scaled, {dl::color_a(128)}), {dl::begin(Primitive::Bitmaps), dl::vertex2ii(10, 10, 1, 0)})));
  CHECK(grey(r, 10, 10) == 128);
}

TEST_CASE("transform: it is part of the graphics context, and every frame starts from the identity") {
  EveRaster r;
  const std::uint8_t data[] = {0xFF};
  std::memcpy(r.ram_g(), data, 1);
  const Words image = setup(BitmapFormat::L1, 1, 1, 16, 1);  // 16 px drawn for 8 source pixels: untransformed, half of it is empty
  const Words draw = {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0), dl::end()};
  // saved, changed, restored: back to 1:1; the draw inside the section is 2:1
  Words w = concat(image, {dl::save_context(), dl::bitmap_transform_a(128)});
  w = concat(concat(w, draw), {dl::restore_context()});
  w = concat(w, {dl::begin(Primitive::Bitmaps), dl::vertex2f(16 * 100, 0), dl::end()});
  CHECK(run(r, w));
  CHECK(grey(r, 15, 0) == 255);   // the section's draw: 8 source pixels over 16
  CHECK(grey(r, 16, 0) == 0);
  CHECK(grey(r, 107, 0) == 255);  // after the restore: 8 px
  CHECK(grey(r, 108, 0) == 0);
  // the other coefficients are saved too, and an extra restore loads the defaults
  w = concat(image, {dl::bitmap_transform_c(256), dl::bitmap_transform_f(512), dl::save_context(), dl::bitmap_transform_c(0),
                     dl::restore_context(), dl::restore_context()});
  w = concat(w, {dl::bitmap_handle(1), dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0), dl::end()});  // (the defaults include handle 0)
  CHECK(run(r, w));
  CHECK(grey(r, 0, 0) == 255);  // defaults: C = F = 0 (a shifted picture would have lost its first pixel)
  CHECK(grey(r, 7, 0) == 255);
  // a frame starts at the identity, whatever the last one set
  CHECK(run(r, concat(concat(image, {dl::bitmap_transform_a(512)}), draw)));
  CHECK(run(r, concat(image, draw)));
  CHECK(grey(r, 7, 0) == 255);
  CHECK(grey(r, 8, 0) == 0);
}

TEST_CASE("transform: bilinear filtering with a transform is refused loudly; with the identity it is the nearest bitmap as before") {
  EveRaster r;
  const std::uint8_t data[] = {0xFF};
  std::memcpy(r.ram_g(), data, 1);
  const Words draw = {dl::begin(Primitive::Bitmaps), dl::vertex2f(0, 0)};
  CHECK_FALSE(run(r, concat(concat(setup(BitmapFormat::L1, 1, 1, 8, 1, Wrap::Border, Filter::Bilinear), {dl::bitmap_transform_a(128)}), draw)));
  CHECK(r.error().find("BILINEAR") != std::string::npos);
  CHECK(run(r, concat(setup(BitmapFormat::L1, 1, 1, 8, 1, Wrap::Border, Filter::Bilinear), draw)));
  CHECK(grey(r, 0, 0) == 255);
  // a format outside the subset is still refused
  CHECK_FALSE(run(r, concat(concat(setup(static_cast<BitmapFormat>(7), 1, 1, 8, 1), {dl::bitmap_transform_a(128)}), draw)));
  CHECK(r.error().find("L1/L2/L4/L8") != std::string::npos);
}
