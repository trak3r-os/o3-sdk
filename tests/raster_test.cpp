// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/sim/eve_raster.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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

// Number of pixels whose colour is `rgb`.
int count(const EveRaster& r, std::uint32_t rgb) {
  int n = 0;
  for (int y = 0; y < EveRaster::kHeight; ++y) {
    for (int x = 0; x < EveRaster::kWidth; ++x) {
      n += px(r, x, y) == rgb ? 1 : 0;
    }
  }
  return n;
}

// Counts pixels of `rgb` inside [x0,x1) x [y0,y1) and returns whether the whole box is that colour.
bool box_is(const EveRaster& r, int x0, int y0, int x1, int y1, std::uint32_t rgb) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (px(r, x, y) != rgb) {
        return false;
      }
    }
  }
  return true;
}

// Appends DISPLAY and renders.
bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

constexpr std::uint32_t kRed = 0xFF0000, kGreen = 0x00FF00, kBlack = 0, kWhite = 0xFFFFFF;

// vertex in whole pixels at the default VERTEX_FORMAT (1/16 px)
std::uint32_t v(int x, int y) { return dl::vertex2f(x * 16, y * 16); }
// the same in half-pixel steps: v2(21, 41) is (10.5, 20.5)
std::uint32_t v16(int x16, int y16) { return dl::vertex2f(x16, y16); }

Words rect(std::uint32_t line_width, int x0, int y0, int x1, int y1) {
  return {dl::color_rgb(255, 0, 0), dl::line_width(line_width), dl::begin(Primitive::Rects), v(x0, y0), v(x1, y1),
          dl::end()};
}

}  // namespace

TEST_CASE("raster: a fresh raster shows black and every frame starts black") {
  EveRaster r;
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight);
  CHECK(run(r, {dl::clear_color_rgb(10, 20, 30), dl::clear(true, true, true)}));
  CHECK(px(r, 0, 0) == 0x0A141E);
  CHECK(px(r, 799, 479) == 0x0A141E);
  CHECK(count(r, 0x0A141E) == EveRaster::kWidth * EveRaster::kHeight);
  CHECK(r.rgba()[3] == 255);  // alpha is always opaque
  // The next frame does not inherit the picture, the clear colour or the colour state.
  CHECK(run(r, {dl::begin(Primitive::Rects), v(0, 0), v(10, 10), dl::end()}));
  CHECK(box_is(r, 0, 0, 10, 10, kWhite));  // default COLOR_RGB is white
  CHECK(px(r, 10, 10) == kWhite);          // and the default LINE_WIDTH of 16 grew the rectangle by 1 px
  CHECK(px(r, 11, 11) == kBlack);
  CHECK(r.frames() == 2u);
}

TEST_CASE("raster: RECTS at LINE_WIDTH 1 cover exactly the pixels between the vertices") {
  EveRaster r;
  CHECK(run(r, rect(1, 10, 20, 15, 22)));
  CHECK(count(r, kRed) == 10);
  CHECK(box_is(r, 10, 20, 15, 22, kRed));
  CHECK(px(r, 9, 20) == kBlack);
  CHECK(px(r, 15, 21) == kBlack);
  CHECK(px(r, 10, 19) == kBlack);
  CHECK(px(r, 14, 22) == kBlack);
  // Vertex order does not matter.
  EveRaster r2;
  CHECK(run(r2, rect(1, 15, 22, 10, 20)));
  CHECK(std::memcmp(r.rgba(), r2.rgba(), EveRaster::kFramebufferBytes) == 0);
}

TEST_CASE("raster: LINE_WIDTH grows RECTS by that much on every side (guide section 2.5.4)") {
  EveRaster r;
  CHECK(run(r, rect(16, 10, 20, 15, 22)));  // 1 px: the chip's default width
  CHECK(box_is(r, 9, 19, 16, 23, kRed));
  CHECK(count(r, kRed) == 7 * 4);
  CHECK(run(r, rect(80, 100, 100, 110, 110)));  // 5 px, the guide's own example width
  CHECK(box_is(r, 95, 95, 115, 115, kRed));
  CHECK(count(r, kRed) == 20 * 20);
  // Half a pixel: a pixel centre exactly on the grown edge counts as covered (the chip antialiases it to ~50 %).
  CHECK(run(r, rect(8, 10, 20, 15, 22)));
  CHECK(box_is(r, 9, 19, 16, 23, kRed));
}

TEST_CASE("raster: LINES are antialiased capsules of half-width LINE_WIDTH with round ends") {
  // Expected values are derived by hand from the coverage model of sim/README.md: 8 x 8 sample points per pixel at
  // odd sixteenths, alpha = (count * 255 + 32) / 64, and checked with an independent Python replica of the model.
  const auto line = [](std::uint32_t width, int ax16, int ay16, int bx16, int by16) {
    return Words{dl::color_rgb(255, 255, 255), dl::line_width(width), dl::begin(Primitive::Lines), v16(ax16, ay16),
                 v16(bx16, by16), dl::end()};
  };
  EveRaster r;
  // Centred on pixel centres: a 1 px band (half-width 8/16) whose edges fall on pixel boundaries is fully covered
  // along the segment, here row 20 from pixel 10 to 19 (centres 168 and 312 sixteenths).
  CHECK(run(r, line(8, 10 * 16 + 8, 20 * 16 + 8, 19 * 16 + 8, 20 * 16 + 8)));
  CHECK(box_is(r, 11, 20, 19, 21, kWhite));  // pixels 11..18: 64 of 64 samples
  CHECK(px(r, 10, 20) == 0xE7E7E7);          // the round end: 58 of 64 samples, (58 * 255 + 32) / 64 = 231
  CHECK(px(r, 19, 20) == 0xE7E7E7);          // and the other one, by symmetry
  CHECK(px(r, 9, 20) == kBlack);             // the cap reaches x = 10.0 only, a pixel boundary
  CHECK(px(r, 20, 20) == kBlack);
  CHECK(box_is(r, 8, 19, 22, 20, kBlack));   // nothing above or below
  CHECK(box_is(r, 8, 21, 22, 22, kBlack));
  // Vertical, the same by symmetry.
  CHECK(run(r, line(8, 30 * 16 + 8, 5 * 16 + 8, 30 * 16 + 8, 14 * 16 + 8)));
  CHECK(box_is(r, 30, 6, 31, 14, kWhite));
  CHECK(px(r, 30, 5) == 0xE7E7E7);
  CHECK(px(r, 30, 14) == 0xE7E7E7);
  CHECK(px(r, 29, 10) == kBlack);
  CHECK(px(r, 31, 10) == kBlack);
  // On a pixel boundary (y = 20.0) the band splits over two rows: 4 of 8 sample rows each, 32 of 64 = 128.
  CHECK(run(r, line(8, 10 * 16, 20 * 16, 20 * 16, 20 * 16)));
  for (int x = 10; x < 20; ++x) {
    CHECK(px(r, x, 19) == 0x808080);
    CHECK(px(r, x, 20) == 0x808080);
  }
  CHECK(px(r, 9, 19) == 0x343434);  // the cap: 13 of 64 samples, (13 * 255 + 32) / 64 = 52
  CHECK(px(r, 10, 19) == 0x808080);
  CHECK(box_is(r, 10, 18, 20, 19, kBlack));
  CHECK(box_is(r, 10, 21, 20, 22, kBlack));
  // A centre exactly on the boundary counts as covered, here a sample row on y = 321/16: the band of the segment
  // at y = 329/16 reaches 321 and 337 sixteenths, so row 20 (samples 321..335) is full and row 21 has its one
  // sample at 337.
  CHECK(run(r, line(8, 10 * 16, 329, 20 * 16, 329)));
  CHECK(px(r, 12, 19) == kBlack);
  CHECK(px(r, 12, 20) == kWhite);
  CHECK(px(r, 12, 21) == 0x202020);  // 8 of 64 samples: (8 * 255 + 32) / 64 = 32
  CHECK(px(r, 12, 22) == kBlack);
  // A zero-length segment is a disc of radius LINE_WIDTH, the same pixels as a point of that size.
  CHECK(run(r, line(16, 50 * 16, 50 * 16, 50 * 16, 50 * 16)));
  EveRaster p;
  CHECK(run(p, {dl::color_rgb(255, 255, 255), dl::point_size(16), dl::begin(Primitive::Points), v(50, 50), dl::end()}));
  CHECK(std::memcmp(r.rgba(), p.rgba(), EveRaster::kFramebufferBytes) == 0);
  CHECK(px(r, 49, 49) == 0xCFCFCF);  // a radius of one pixel round a pixel corner: 52 of 64 samples in each of 4 pixels
  CHECK(px(r, 50, 50) == 0xCFCFCF);
  CHECK(px(r, 48, 49) == kBlack);
  CHECK(px(r, 51, 50) == kBlack);
  // A wider line is wider: half-width 24/16 = 1.5 px is 3 px thick; rows 19..21 are full between the end caps.
  CHECK(run(r, line(24, 10 * 16 + 8, 20 * 16 + 8, 19 * 16 + 8, 20 * 16 + 8)));
  CHECK(box_is(r, 11, 19, 19, 22, kWhite));
  CHECK(px(r, 10, 19) != kWhite);  // the round end: this pixel corner is 1.58 px from the end point
  CHECK(px(r, 10, 19) != kBlack);
  CHECK(px(r, 9, 20) != kBlack);  // the cap reaches 1.5 px beyond the end
  CHECK(px(r, 7, 20) == kBlack);
}

TEST_CASE("raster: antialiased lines conserve area and do not depend on the direction or the axis") {
  // A line of half-width w and length L covers 2 w L + pi w^2 pixels of area (a capsule); the sum of the coverages
  // is that area to within the sampling error, at every angle. (The ends sit off the sample lattice: a 45 degree
  // line that is exactly in step with the 8 x 8 samples is 2.5 % heavy, see README.)
  const auto area = [](const EveRaster& r) {
    double sum = 0;
    for (int y = 0; y < EveRaster::kHeight; ++y) {
      for (int x = 0; x < EveRaster::kWidth; ++x) {
        sum += (px(r, x, y) & 0xFF) / 255.0;
      }
    }
    return sum;
  };
  const auto line = [](std::uint32_t width, int ax16, int ay16, int bx16, int by16) {
    return Words{dl::color_rgb(255, 255, 255), dl::line_width(width), dl::begin(Primitive::Lines), v16(ax16, ay16),
                 v16(bx16, by16), dl::end()};
  };
  EveRaster r, flipped;
  const int w16 = 20;  // 1.25 px
  const double w = w16 / 16.0;
  const struct {
    int ax, ay, bx, by;  // sixteenths
  } cases[] = {{103, 101, 703, 105}, {103, 101, 703, 223}, {103, 101, 701, 700}, {103, 101, 225, 697},
               {105, 101, 107, 701}, {3001, 2003, 5003, 3507}, {101, 707, 703, 111}};
  for (const auto& c : cases) {
    CHECK(run(r, line(w16, c.ax, c.ay, c.bx, c.by)));
    const double dx = (c.bx - c.ax) / 16.0, dy = (c.by - c.ay) / 16.0;
    const double want = 2.0 * w * std::sqrt(dx * dx + dy * dy) + 3.14159265358979 * w * w;
    CHECK(area(r) == doctest::Approx(want).epsilon(0.005));
    // Swapping the ends changes nothing...
    CHECK(run(flipped, line(w16, c.bx, c.by, c.ax, c.ay)));
    CHECK(std::memcmp(r.rgba(), flipped.rgba(), EveRaster::kFramebufferBytes) == 0);
    // ...and the picture of the transposed line is the transposed picture.
    CHECK(run(flipped, line(w16, c.ay, c.ax, c.by, c.bx)));
    for (int y = 0; y < 400; y += 7) {
      for (int x = 0; x < 400; x += 5) {
        CHECK(px(r, x, y) == px(flipped, y, x));
      }
    }
  }
  // The same 45-degree line, centred on pixel centres, spreads over three pixels per step: 231 + 60 + 60 of 255
  // (hand-derived exact coverages of the model; the exact areas are 233, 64, 64).
  CHECK(run(r, line(8, 12 * 16 + 8, 12 * 16 + 8, 19 * 16 + 8, 19 * 16 + 8)));
  CHECK(px(r, 14, 14) == 0xE7E7E7);
  CHECK(px(r, 15, 14) == 0x3C3C3C);
  CHECK(px(r, 14, 15) == 0x3C3C3C);
}

TEST_CASE("raster: lines are decided in exact integer arithmetic, not with a rounded square root") {
  // Segment from (0, -11/16) to (12/16, 1/16) with half-width 8/16: |d|^2 = 288, so |d| = 16.97 and the reach
  // w * |d| = 135.8. The sample at (1/16, 1/16) is 132/|d| = 7.78/16 px from the segment's line, inside the
  // half-width of 8/16 (squared: 132^2 = 17424 <= 8^2 * 288 = 18432). A test against w * floor(|d|) = 128 would
  // wrongly leave that sample out (and others like it). Pixel (0, 0) holds 26 samples of 64, alpha (26 * 255 + 32) / 64
  // = 104; the rounded test would find 23, alpha 92 (both counted by a Python replica of the model).
  EveRaster r;
  const Words w = {dl::color_rgb(255, 255, 255), dl::line_width(8), dl::begin(Primitive::Lines), dl::vertex2f(0, -11),
                   dl::vertex2f(12, 1), dl::end()};
  CHECK(run(r, w));
  CHECK(px(r, 0, 0) == 0x686868);
  // Swapping the two ends draws the same pixels.
  Words swapped = w;
  std::swap(swapped[3], swapped[4]);
  EveRaster r2;
  CHECK(run(r2, swapped));
  CHECK(std::memcmp(r.rgba(), r2.rgba(), EveRaster::kFramebufferBytes) == 0);

  // Vertices beyond +-4096 px (reachable with VERTEX_FORMAT 0) are refused, not approximated.
  const Words far = {dl::vertex_format(0), dl::begin(Primitive::Lines), dl::vertex2f(0, 0), dl::vertex2f(5000, 5)};
  CHECK_FALSE(run(r, far));
  CHECK(r.error().find("LINES") != std::string::npos);
  CHECK(r.error().find("not modelled") != std::string::npos);
  // Long lines inside the range stay exact: a diagonal across the screen, 1 px thick, centred on the pixel centres.
  // Every pixel of the diagonal has the same coverage (the line is periodic), 231 of 255 as in the short case.
  const Words diag = {dl::color_rgb(255, 255, 255), dl::line_width(8), dl::begin(Primitive::Lines),
                      dl::vertex2f(8, 8), dl::vertex2f(479 * 16 + 8, 479 * 16 + 8)};
  CHECK(run(r, diag));
  for (int d = 1; d < 478; d += 13) {
    CHECK(px(r, d, d) == 0xE7E7E7);
    CHECK(px(r, d + 1, d) == 0x3C3C3C);
  }
  CHECK(px(r, 240, 240) == 0xE7E7E7);
}

TEST_CASE("raster: colour and alpha blend source over") {
  EveRaster r;
  const Words base = {dl::color_rgb(200, 100, 50), dl::line_width(1), dl::begin(Primitive::Rects)};
  const auto with_alpha = [&](std::uint8_t a) {
    Words w = base;
    w.insert(w.begin() + 1, dl::color_a(a));
    w.push_back(v(0, 0));
    w.push_back(v(4, 4));
    return w;
  };
  CHECK(run(r, with_alpha(255)));
  CHECK(px(r, 1, 1) == 0xC86432);
  CHECK(run(r, with_alpha(0)));
  CHECK(px(r, 1, 1) == kBlack);  // nothing drawn
  CHECK(run(r, with_alpha(128)));
  CHECK(px(r, 1, 1) == 0x643219);  // (200,100,50) * 128/255 over black, rounded: (100, 50, 25)
  // Two half-transparent layers: dst = src * a + dst * (1 - a), rounded each time.
  Words twice = with_alpha(128);
  twice.push_back(v(0, 0));
  twice.push_back(v(4, 4));
  CHECK(run(r, twice));
  CHECK(px(r, 1, 1) == 0x964B25);  // 100 + 100*127/255 = 150, 50 + 50*127/255 = 75, 25 + 25*127/255 = 37
}

TEST_CASE("raster: scissor clips fills and clear, size 0 draws nothing") {
  EveRaster r;
  CHECK(run(r, {dl::scissor_xy(100, 100), dl::scissor_size(50, 40), dl::clear_color_rgb(0, 255, 0),
                dl::clear(true, true, true)}));
  CHECK(count(r, kGreen) == 50 * 40);
  CHECK(box_is(r, 100, 100, 150, 140, kGreen));

  Words w = {dl::scissor_xy(10, 10), dl::scissor_size(10, 10)};
  const Words rc = rect(1, 0, 0, 100, 100);
  w.insert(w.end(), rc.begin(), rc.end());
  CHECK(run(r, w));
  CHECK(count(r, kRed) == 100);
  CHECK(box_is(r, 10, 10, 20, 20, kRed));

  Words none = {dl::scissor_size(0, 0)};
  none.insert(none.end(), rc.begin(), rc.end());
  CHECK(run(r, none));
  CHECK(count(r, kRed) == 0);
}

TEST_CASE("raster: save and restore context, four deep; an extra restore loads the defaults") {
  EveRaster r;
  Words w;
  for (unsigned k = 1; k <= 5; ++k) {  // five pushes: the first (colour 10) is dropped
    w.push_back(dl::color_rgb(static_cast<std::uint8_t>(k * 10), 0, 0));
    w.push_back(dl::save_context());
  }
  w.push_back(dl::color_rgb(0, 0, 255));
  w.push_back(dl::line_width(1));  // LINE_WIDTH is part of the context: every restore brings 16 back
  w.push_back(dl::begin(Primitive::Rects));
  for (int i = 0; i < 5; ++i) {
    w.push_back(dl::restore_context());
    w.push_back(v(i * 20, 0));
    w.push_back(v(i * 20 + 5, 5));
  }
  CHECK(run(r, w));
  // Each rectangle is 5 px wide plus the restored width of 1 px on every side: x in [i*20-1, i*20+6).
  CHECK(px(r, 5, 2) == 0x320000);    // popped first: colour 50
  CHECK(px(r, 25, 2) == 0x280000);   // 40
  CHECK(px(r, 45, 2) == 0x1E0000);   // 30
  CHECK(px(r, 65, 2) == 0x140000);   // 20
  CHECK(px(r, 85, 2) == kWhite);     // the fifth restore finds nothing: defaults, not the dropped colour 10
  CHECK(px(r, 6, 2) == kBlack);      // and the width was not the 1 set before the restores
  CHECK(px(r, 86, 2) == kBlack);
}

TEST_CASE("raster: save and restore also hold the clear colour and the current bitmap handle") {
  EveRaster r;
  CHECK(run(r, {dl::clear_color_rgb(255, 0, 0), dl::save_context(), dl::clear_color_rgb(0, 255, 0),
                dl::restore_context(), dl::clear(true, true, true)}));
  CHECK(px(r, 100, 100) == kRed);  // the green set after the save is gone
  CHECK(run(r, {dl::clear_color_rgb(255, 0, 0), dl::save_context(), dl::clear_color_rgb(0, 255, 0),
                dl::clear(true, true, true)}));
  CHECK(px(r, 100, 100) == kGreen);  // control: without the restore it is the green

  // Handle 1 shows F0 (left four pixels), handle 2 shows 0F (right four).
  const std::uint8_t data[] = {0xF0, 0x0F};
  std::memcpy(r.ram_g(), data, 2);
  const auto config = [](std::uint32_t handle, std::uint32_t source) {
    return Words{dl::bitmap_handle(handle), dl::bitmap_source(source),
                 dl::bitmap_layout(BitmapFormat::L1, 1, 1),
                 dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 1)};
  };
  Words w = config(1, 0);
  const Words second = config(2, 1);
  w.insert(w.end(), second.begin(), second.end());
  w.push_back(dl::bitmap_handle(1));
  w.push_back(dl::save_context());
  w.push_back(dl::bitmap_handle(2));
  w.push_back(dl::restore_context());  // back to handle 1
  w.push_back(dl::begin(Primitive::Bitmaps));
  w.push_back(v(10, 10));
  CHECK(run(r, w));
  for (int i = 0; i < 8; ++i) {
    CHECK(px(r, 10 + i, 10) == (i < 4 ? kWhite : kBlack));
  }
  w.erase(w.begin() + 11);  // drop the restore...
  w.erase(w.begin() + 9);   // ...and the save: handle 2 stays current
  CHECK(run(r, w));
  for (int i = 0; i < 8; ++i) {
    CHECK(px(r, 10 + i, 10) == (i >= 4 ? kWhite : kBlack));
  }
}

TEST_CASE("raster: BITMAP_SIZE of 0 means 2048 (guide section 4.9)") {
  EveRaster r;
  const std::uint8_t data[] = {0x80, 0x01};  // 8 x 2: top-left pixel and bottom-right pixel
  std::memcpy(r.ram_g(), data, 2);
  const auto draw = [](Wrap wrap) {
    return Words{dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 2),
                 dl::bitmap_size(Filter::Nearest, wrap, wrap, 0, 0), dl::begin(Primitive::Bitmaps),
                 dl::vertex2ii(50, 50, 1, 0)};
  };
  // BORDER: 2048 x 2048 of which only the 8 x 2 of data is not transparent.
  CHECK(run(r, draw(Wrap::Border)));
  CHECK(count(r, kWhite) == 2);
  CHECK(px(r, 50, 50) == kWhite);
  CHECK(px(r, 57, 51) == kWhite);
  // REPEAT: the pattern tiles the whole visible area right of and below (50, 50): 750 x 430 pixels.
  CHECK(run(r, draw(Wrap::Repeat)));
  CHECK(count(r, kWhite) == 215 * 94 + 215 * 93);  // top-left pixel of every tile: 94 per even row, 93 per odd row
  CHECK(px(r, 50 + 744, 50) == kWhite);
  CHECK(px(r, 50 + 7 + 8 * 90, 51 + 2 * 100) == kWhite);
  CHECK(px(r, 49, 50) == kBlack);
  // The high bits (BITMAP_SIZE_H) extend the range: width 1 * 512 + 8 draws the tile 65 times across.
  Words wide = draw(Wrap::Repeat);
  wide.insert(wide.begin() + 4, dl::bitmap_size_h(1, 0));
  wide[3] = dl::bitmap_size(Filter::Nearest, Wrap::Repeat, Wrap::Repeat, 8, 2);
  CHECK(run(r, wide));
  CHECK(px(r, 50 + 8 * 64, 50) == kWhite);  // column 512 is the start of a tile
  // A zero stride or layout height is still refused.
  CHECK_FALSE(run(r, {dl::bitmap_handle(2), dl::bitmap_layout(BitmapFormat::L1, 0, 1),
                      dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 1, 1), dl::begin(Primitive::Bitmaps),
                      dl::vertex2ii(0, 0, 2, 0)}));
  CHECK(r.error().find("zero") != std::string::npos);
}

TEST_CASE("raster: bitmaps, L1 cells and the two ways to name them") {
  EveRaster r;
  // Two 8x2 cells (stride 1, height 2) at RAM_G 0: cell 0 = AA 55, cell 1 = F0 0F.
  const std::uint8_t data[] = {0xAA, 0x55, 0xF0, 0x0F};
  std::memcpy(r.ram_g(), data, sizeof(data));
  const Words setup = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 2),
                       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 2)};
  Words w = setup;
  w.push_back(dl::begin(Primitive::Bitmaps));
  w.push_back(dl::vertex2ii(10, 20, 1, 1));  // cell 1: F0 / 0F
  w.push_back(dl::cell(0));
  w.push_back(dl::bitmap_handle(1));
  w.push_back(v(100, 30));                   // VERTEX2F: cell 0 from the CELL register: AA / 55
  w.push_back(dl::vertex2ii(200, 40, 1, 1)); // VERTEX2II does not touch CELL...
  w.push_back(v(300, 50));                   // ...so this is still cell 0
  w.push_back(dl::end());
  CHECK(run(r, w));
  for (int i = 0; i < 8; ++i) {
    CHECK(px(r, 10 + i, 20) == (i < 4 ? kWhite : kBlack));
    CHECK(px(r, 10 + i, 21) == (i >= 4 ? kWhite : kBlack));
    CHECK(px(r, 100 + i, 30) == (i % 2 == 0 ? kWhite : kBlack));  // 0xAA: leftmost pixel is the top bit
    CHECK(px(r, 100 + i, 31) == (i % 2 == 1 ? kWhite : kBlack));
    CHECK(px(r, 300 + i, 50) == (i % 2 == 0 ? kWhite : kBlack));
  }
  CHECK(px(r, 18, 20) == kBlack);  // nothing beyond the 8 x 2 bitmap
  CHECK(px(r, 10, 22) == kBlack);

  // Bitmap handles persist across frames, like the chip's registers: this frame draws without setup.
  CHECK(run(r, {dl::color_rgb(255, 0, 0), dl::begin(Primitive::Bitmaps), dl::vertex2ii(0, 0, 1, 0)}));
  CHECK(px(r, 0, 0) == kRed);
  CHECK(px(r, 1, 0) == kBlack);
  CHECK(px(r, 2, 0) == kRed);
}

TEST_CASE("raster: bitmap alpha comes from the data and is scaled by COLOR_A; colour from COLOR_RGB") {
  EveRaster r;
  const std::uint8_t l4[] = {0x1F, 0x80};     // L4, 4 px: 1, 15, 8, 0
  const std::uint8_t l8[] = {0x40, 0xFF};     // L8, 2 px: 64, 255
  std::memcpy(r.ram_g(), l4, 2);
  std::memcpy(r.ram_g() + 16, l8, 2);
  Words w = {dl::bitmap_handle(2), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L4, 2, 1),
             dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 4, 1), dl::bitmap_handle(3),
             dl::bitmap_source(16), dl::bitmap_layout(BitmapFormat::L8, 2, 1),
             dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 2, 1), dl::begin(Primitive::Bitmaps),
             dl::vertex2ii(10, 10, 2, 0), dl::vertex2ii(20, 10, 3, 0), dl::color_a(128), dl::vertex2ii(30, 10, 3, 0)};
  CHECK(run(r, w));
  CHECK(px(r, 10, 10) == 0x111111);  // alpha 1 * 17 over black with white: 17
  CHECK(px(r, 11, 10) == kWhite);    // 15 * 17 = 255
  CHECK(px(r, 12, 10) == 0x888888);  // 8 * 17 = 136
  CHECK(px(r, 13, 10) == kBlack);
  CHECK(px(r, 20, 10) == 0x404040);  // L8 64
  CHECK(px(r, 21, 10) == kWhite);
  CHECK(px(r, 30, 10) == 0x202020);  // 64 * 128 / 255 = 32
  CHECK(px(r, 31, 10) == 0x808080);  // 255 * 128 / 255
}

TEST_CASE("raster: bitmaps wider than their data are transparent (BORDER) or repeat (REPEAT)") {
  EveRaster r;
  const std::uint8_t data[] = {0x80, 0x01};  // 8 x 2: top-left pixel and bottom-right pixel
  std::memcpy(r.ram_g(), data, 2);
  const auto draw = [&](Wrap wrap) {
    return Words{dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 2),
                 dl::bitmap_size(Filter::Nearest, wrap, wrap, 16, 4), dl::begin(Primitive::Bitmaps),
                 dl::vertex2ii(50, 50, 1, 0)};
  };
  CHECK(run(r, draw(Wrap::Border)));
  CHECK(count(r, kWhite) == 2);
  CHECK(px(r, 50, 50) == kWhite);
  CHECK(px(r, 57, 51) == kWhite);
  CHECK(run(r, draw(Wrap::Repeat)));
  CHECK(count(r, kWhite) == 8);  // 2 copies across, 2 down
  CHECK(px(r, 58, 50) == kWhite);
  CHECK(px(r, 50, 52) == kWhite);
  CHECK(px(r, 65, 53) == kWhite);
}

TEST_CASE("raster: bitmaps are clipped at the screen edge and by the scissor") {
  EveRaster r;
  const std::uint8_t data[] = {0xFF, 0xFF};
  std::memcpy(r.ram_g(), data, 2);
  const Words setup = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 2),
                       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 2),
                       dl::begin(Primitive::Bitmaps)};
  Words w = setup;
  w.push_back(v(-4, -1));     // 4 columns and 1 row off the top-left corner
  w.push_back(v(796, 479));   // 4 columns off the right edge, 1 row off the bottom
  CHECK(run(r, w));
  CHECK(count(r, kWhite) == 4 * 1 + 4 * 1);
  CHECK(box_is(r, 0, 0, 4, 1, kWhite));
  CHECK(box_is(r, 796, 479, 800, 480, kWhite));

  w = {dl::scissor_xy(52, 0), dl::scissor_size(2, 100)};
  w.insert(w.end(), setup.begin(), setup.end());
  w.push_back(dl::vertex2ii(50, 50, 1, 0));
  CHECK(run(r, w));
  CHECK(count(r, kWhite) == 2 * 2);
  CHECK(box_is(r, 52, 50, 54, 52, kWhite));
}

TEST_CASE("raster: VERTEX_FORMAT sets the units of VERTEX2F") {
  EveRaster r;
  Words w = {dl::color_rgb(255, 0, 0), dl::line_width(1), dl::vertex_format(0), dl::begin(Primitive::Rects),
             dl::vertex2f(10, 20), dl::vertex2f(15, 22)};  // whole pixels
  CHECK(run(r, w));
  CHECK(count(r, kRed) == 10);
  CHECK(box_is(r, 10, 20, 15, 22, kRed));
  w[2] = dl::vertex_format(2);  // quarter pixels
  w[4] = dl::vertex2f(40, 80);
  w[5] = dl::vertex2f(60, 88);
  CHECK(run(r, w));
  CHECK(box_is(r, 10, 20, 15, 22, kRed));
  CHECK(count(r, kRed) == 10);
  w[2] = dl::vertex_format(5);
  CHECK_FALSE(run(r, w));
  CHECK(r.error().find("VERTEX_FORMAT") != std::string::npos);
}

TEST_CASE("raster: words outside the subset or malformed lists fail loudly and keep the old picture") {
  EveRaster r;
  CHECK(run(r, {dl::clear_color_rgb(1, 2, 3), dl::clear(true, true, true)}));
  const std::uint32_t before = px(r, 5, 5);
  const std::size_t frames = r.frames();

  const auto refused = [&](Words w, const char* fragment) {
    const bool ok = run(r, std::move(w));
    CHECK_FALSE(ok);
    CHECK(r.error().find(fragment) != std::string::npos);
    CHECK(px(r, 5, 5) == before);  // the previous frame stays visible
    CHECK(r.frames() == frames);
  };
  refused({0x09000000u}, "unsupported");                                  // ALPHA_FUNC: outside the subset
  refused({0x0C000000u}, "unsupported");                                  // STENCIL_OP
  refused({0xC0000000u}, "unknown");                                      // the unused top-bit tag
  refused({dl::begin(Primitive::Bitmaps), dl::end(), v(1, 1)}, "outside BEGIN");
  refused({0x1F000005u}, "BEGIN");                                        // EDGE_STRIP_R
  refused({dl::bitmap_handle(1), dl::bitmap_layout(BitmapFormat::L1, 1, 1), dl::bitmap_size(Filter::Nearest,
           Wrap::Border, Wrap::Border, 1, 1), dl::bitmap_source(0x3FFFFF), dl::begin(Primitive::Bitmaps),
           dl::vertex2ii(0, 0, 1, 0)}, "RAM_G");
  refused({dl::bitmap_handle(1), dl::bitmap_layout(static_cast<BitmapFormat>(6), 1, 1),
           dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 1, 1), dl::begin(Primitive::Bitmaps),
           dl::vertex2ii(0, 0, 1, 0)}, "L1/L2/L4/L8");
  refused({dl::bitmap_handle(4), dl::begin(Primitive::Bitmaps), dl::vertex2ii(0, 0, 4, 0)}, "L1/L2/L4/L8");  // unconfigured

  // A list without DISPLAY is refused, one with words after DISPLAY ignores them.
  Words no_display = {dl::clear_color_rgb(9, 9, 9), dl::clear(true, true, true)};
  CHECK_FALSE(r.render(no_display.data(), no_display.size()));
  CHECK(r.error().find("DISPLAY") != std::string::npos);
  CHECK(px(r, 5, 5) == before);
  Words trailing = {dl::clear_color_rgb(9, 9, 9), dl::clear(true, true, true), dl::display(), 0x09000000u};
  CHECK(r.render(trailing.data(), trailing.size()));
  CHECK(px(r, 5, 5) == 0x090909);
  CHECK(r.error().empty());  // a good frame clears the last error
}
