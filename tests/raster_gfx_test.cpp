// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-003a: the simulator's interpretation of the words added for the graphics toolkit (VERTEX_TRANSLATE, POINTS /
// POINT_SIZE, LINE_STRIP, EDGE_STRIP_A/B, BLEND_FUNC) and its antialiasing. Small cases whose expected pixels are
// derived by hand from sim/README.md (and checked with a Python replica of the coverage model); the property test at
// the end compares the antialiased capsules with an independently written reference.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/platform/host/sha256.h>
#include <o3sdk/sim/eve_raster.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::BlendFactor;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;
using o3sdk::sim::EveRaster;

namespace {

using Words = std::vector<std::uint32_t>;

constexpr std::uint32_t kBlack = 0, kWhite = 0xFFFFFF;

std::uint32_t px(const EveRaster& r, int x, int y) {
  const std::uint8_t* p = r.rgba() + (static_cast<std::size_t>(y) * EveRaster::kWidth + static_cast<std::size_t>(x)) * 4;
  return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}

int count(const EveRaster& r, std::uint32_t rgb) {
  int n = 0;
  for (int y = 0; y < EveRaster::kHeight; ++y) {
    for (int x = 0; x < EveRaster::kWidth; ++x) {
      n += px(r, x, y) == rgb ? 1 : 0;
    }
  }
  return n;
}

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

bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

// Vertex at the top-left corner of pixel (x, y), and at its centre (default VERTEX_FORMAT, 1/16 px).
std::uint32_t corner(int x, int y) { return dl::vertex2f(x * 16, y * 16); }
std::uint32_t centre(int x, int y) { return dl::vertex2f(x * 16 + 8, y * 16 + 8); }

void append(Words& to, const Words& more) { to.insert(to.end(), more.begin(), more.end()); }

// The pixels [x0, x1) x [y0, y1) in `rgb`: a RECTS with LINE_WIDTH 8 and its vertices on the pixel centres, the
// way the UI's renderer draws a rectangle.
Words fill(std::uint8_t r, std::uint8_t g, std::uint8_t b, int x0, int y0, int x1, int y1) {
  return {dl::color_rgb(r, g, b), dl::line_width(8), dl::begin(Primitive::Rects), centre(x0, y0), centre(x1 - 1, y1 - 1),
          dl::end()};
}

}  // namespace

// ---- VERTEX_TRANSLATE_X / _Y (guide section 4.50 p.148, 4.51 p.149) ----------------------------------------------

TEST_CASE("raster: VERTEX_TRANSLATE shifts every vertex by 1/16 px units, whatever VERTEX_FORMAT says") {
  EveRaster r;
  // A 4 x 2 rectangle at (10, 20), moved 10 px right and 2 px up.
  Words w = {dl::vertex_translate_x(160), dl::vertex_translate_y(-32)};
  append(w, fill(255, 255, 255, 10, 20, 14, 22));
  CHECK(run(r, w));
  CHECK(count(r, kWhite) == 8);
  CHECK(box_is(r, 20, 18, 24, 20, kWhite));
  // The translation is 1/16 px even when the vertices are whole pixels: frac 0, vertex (10, 20) + (5 px, 0).
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::line_width(8), dl::vertex_format(0), dl::vertex_translate_x(80),
                dl::begin(Primitive::Rects), dl::vertex2f(10, 20), dl::vertex2f(13, 21), dl::end()}));
  // The corners moved from (10, 20) .. (13, 21) to (15, 20) .. (18, 21); LINE_WIDTH 8 grows half a pixel each way,
  // so the covered centres are those within [14.5, 18.5] x [19.5, 21.5], the edges included.
  CHECK(px(r, 14, 20) == kWhite);  // 14.5 is on the grown edge: covered
  CHECK(px(r, 18, 20) == kWhite);
  CHECK(px(r, 13, 20) == kBlack);
  CHECK(px(r, 19, 20) == kBlack);
  // A half-pixel translation moves a rectangle by half a pixel: vertices on centres land on pixel boundaries,
  // and the grown edges at x = 10.0 .. 14.0 + 0.5 cover the centres 10.5..14.5 (the last on the edge): 5 columns.
  Words half = {dl::vertex_translate_x(8)};
  append(half, fill(255, 255, 255, 10, 20, 14, 21));
  CHECK(run(r, half));
  CHECK(box_is(r, 10, 20, 15, 21, kWhite));
  CHECK(px(r, 9, 20) == kBlack);
  CHECK(px(r, 15, 20) == kBlack);
}

TEST_CASE("raster: VERTEX_TRANSLATE also moves VERTEX2II, which reaches past x = 511 with it [A]") {
  EveRaster r;
  const std::uint8_t data[] = {0xAA, 0x55};  // 8 x 2: 10101010 / 01010101
  std::memcpy(r.ram_g(), data, 2);
  const Words setup = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 2),
                       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 2)};
  Words w = setup;
  w.push_back(dl::color_rgb(255, 255, 255));
  w.push_back(dl::begin(Primitive::Bitmaps));
  w.push_back(dl::vertex2ii(500, 20, 1, 0));  // x = 500
  w.push_back(dl::vertex_translate_x(16 * 100));
  w.push_back(dl::vertex_translate_y(-16 * 3));
  w.push_back(dl::vertex2ii(500, 20, 1, 0));  // x = 600, y = 17
  w.push_back(dl::vertex_translate_x(8));     // half a pixel: bitmaps floor to the whole pixel
  w.push_back(dl::vertex_translate_y(0));
  w.push_back(dl::vertex2ii(100, 40, 1, 0));
  w.push_back(dl::vertex_translate_x(-8));    // minus half a pixel: floor goes one pixel left
  w.push_back(dl::vertex2ii(100, 60, 1, 0));
  CHECK(run(r, w));
  for (int i = 0; i < 8; ++i) {
    CHECK(px(r, 500 + i, 20) == (i % 2 == 0 ? kWhite : kBlack));
    CHECK(px(r, 600 + i, 17) == (i % 2 == 0 ? kWhite : kBlack));
    CHECK(px(r, 100 + i, 40) == (i % 2 == 0 ? kWhite : kBlack));
    CHECK(px(r, 99 + i, 60) == (i % 2 == 0 ? kWhite : kBlack));
  }
  CHECK(px(r, 600 - 1, 17) == kBlack);
}

TEST_CASE("raster: VERTEX_TRANSLATE is part of the graphics context, reset at the start of every frame") {
  EveRaster r;
  // Saved at 0, set to (20, 0) px, restored: the rectangle drawn after the restore is not moved.
  Words w = {dl::save_context(), dl::vertex_translate_x(320), dl::restore_context()};
  append(w, fill(255, 255, 255, 10, 10, 12, 12));
  CHECK(run(r, w));
  CHECK(box_is(r, 10, 10, 12, 12, kWhite));
  CHECK(count(r, kWhite) == 4);
  // Saved at (20, 0), then changed to (40, 0): the restore brings back 20.
  w = {dl::vertex_translate_x(320), dl::save_context(), dl::vertex_translate_x(640), dl::restore_context()};
  append(w, fill(255, 255, 255, 10, 10, 12, 12));
  CHECK(run(r, w));
  CHECK(box_is(r, 30, 10, 32, 12, kWhite));
  CHECK(count(r, kWhite) == 4);
  // An extra RESTORE loads the defaults.
  w = {dl::vertex_translate_x(320), dl::restore_context()};
  append(w, fill(255, 255, 255, 10, 10, 12, 12));
  CHECK(run(r, w));
  CHECK(box_is(r, 10, 10, 12, 12, kWhite));
  // And the next frame starts from zero without a restore.
  CHECK(run(r, fill(255, 255, 255, 10, 10, 12, 12)));
  CHECK(box_is(r, 10, 10, 12, 12, kWhite));
  CHECK(count(r, kWhite) == 4);
  // The 17-bit field is signed: the extremes are -65536 (-4096 px) and 65535.
  w = {dl::vertex_translate_x(-65536), dl::vertex_translate_y(65535)};
  append(w, fill(255, 255, 255, 10, 10, 12, 12));
  CHECK(run(r, w));  // far off-screen: nothing visible, nothing wrong
  CHECK(count(r, kWhite) == 0);
}

// ---- POINTS and POINT_SIZE (guide section 4.36 p.133, 2.5.4 p.20-21) ---------------------------------------------

TEST_CASE("raster: POINTS are antialiased discs of radius POINT_SIZE / 16 round the vertex") {
  EveRaster r;
  // Radius 1 px round the pixel corner (50, 50): the four pixels round it hold 52 of 64 samples each,
  // (52 * 255 + 32) / 64 = 207 (hand count, see the Python replica); nothing else is touched.
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::point_size(16), dl::begin(Primitive::Points), corner(50, 50),
                dl::end()}));
  CHECK(px(r, 49, 49) == 0xCFCFCF);
  CHECK(px(r, 50, 49) == 0xCFCFCF);
  CHECK(px(r, 49, 50) == 0xCFCFCF);
  CHECK(px(r, 50, 50) == 0xCFCFCF);
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight - 4);
  // A radius of 0 draws nothing (no sample can lie on a centre of zero size).
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::point_size(0), dl::begin(Primitive::Points), corner(50, 50),
                dl::end()}));
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight);
  // The default size is 16 (1 px); several vertices draw several points; each takes the colour set before it.
  CHECK(run(r, {dl::color_rgb(255, 0, 0), dl::begin(Primitive::Points), corner(10, 10), dl::color_rgb(0, 255, 0),
                corner(30, 10), dl::end()}));
  CHECK(px(r, 9, 9) == 0xCF0000);
  CHECK(px(r, 29, 9) == 0x00CF00);
  // A big point is a disc: symmetric about its centre, and its area is pi r^2 to within the sampling error.
  // Centre of pixel (400, 240), radius 20 px (320 sixteenths).
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::point_size(320), dl::begin(Primitive::Points), centre(400, 240),
                dl::end()}));
  double area = 0;
  for (int y = 200; y < 280; ++y) {
    for (int x = 360; x < 440; ++x) {
      area += (px(r, x, y) & 0xFF) / 255.0;
      CHECK(px(r, x, y) == px(r, 800 - x, y));  // mirror about the centre of pixel 400: x <-> 800 - x
      CHECK(px(r, x, y) == px(r, x, 480 - y));
    }
  }
  CHECK(area == doctest::Approx(3.14159265358979 * 400.0).epsilon(0.005));
  CHECK(px(r, 400, 240) == kWhite);
  CHECK(px(r, 400, 219) == kBlack);  // 21 px up: outside
  // COLOR_A scales the coverage: half alpha halves the disc's pixels, 255 * 128 / 255 = 128, then blended.
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::color_a(128), dl::point_size(320), dl::begin(Primitive::Points),
                centre(400, 240), dl::end()}));
  CHECK(px(r, 400, 240) == 0x808080);
}

TEST_CASE("raster: POINT_SIZE is part of the graphics context; the scissor clips points; far points are refused") {
  EveRaster r;
  Words w = {dl::color_rgb(255, 255, 255), dl::point_size(160), dl::save_context(), dl::point_size(16),
             dl::restore_context(), dl::begin(Primitive::Points), centre(100, 100), dl::end()};
  CHECK(run(r, w));
  CHECK(px(r, 100, 91) == kWhite);  // 160 sixteenths = 10 px: the restored radius
  CHECK(px(r, 100, 80) == kBlack);
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::point_size(160), dl::scissor_xy(95, 95), dl::scissor_size(10, 10),
                dl::begin(Primitive::Points), centre(100, 100), dl::end()}));
  CHECK(count(r, kWhite) == 100);  // the 10 x 10 scissor, entirely inside the disc
  CHECK(box_is(r, 95, 95, 105, 105, kWhite));
  // Beyond +-4096 px the integer arithmetic of the model is not guaranteed: refused, never approximated.
  CHECK_FALSE(run(r, {dl::vertex_translate_x(65535), dl::begin(Primitive::Points), dl::vertex2f(16000, 0)}));
  CHECK(r.error().find("not modelled") != std::string::npos);
  CHECK(run(r, {dl::vertex_translate_x(65535 - 16000), dl::begin(Primitive::Points), dl::vertex2f(16000, 0)}));  // 4095 px
}

// ---- LINE_STRIP (guide section 2.5.4 p.22) -----------------------------------------------------------------------

TEST_CASE("raster: LINE_STRIP joins its vertices; one vertex draws nothing; BEGIN starts a new strip") {
  EveRaster r;
  const Words style = {dl::color_rgb(255, 255, 255), dl::line_width(8)};
  Words w = style;
  append(w, {dl::begin(Primitive::LineStrip), centre(10, 10), centre(20, 10), centre(20, 20), dl::end()});
  CHECK(run(r, w));
  CHECK(box_is(r, 11, 10, 20, 11, kWhite));   // the first leg: row 10, pixels 11..19
  CHECK(box_is(r, 20, 11, 21, 20, kWhite));   // the second leg: column 20, rows 11..19
  CHECK(px(r, 10, 10) == 0xE7E7E7);           // start cap: 58 of 64 samples (as for LINES)
  CHECK(px(r, 20, 20) == 0xE7E7E7);           // end cap
  // The corner pixel is under both end caps, 58 of 64 samples each (231); the strip covers it as the union of its
  // segments does: 61 samples, (61 * 255 + 32) / 64 = 243 (the exact area is 0.946 * 255 = 241). Drawn one segment
  // after the other the pixel would blend twice, 231 + 231 * (255 - 231) / 255 = 253: 5 % too heavy [A].
  CHECK(px(r, 20, 10) == 0xF3F3F3);
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight - (9 + 9 + 1 + 1 + 1));

  // Two vertices are exactly one LINES segment.
  EveRaster strip, lines;
  Words a = style, b = style;
  append(a, {dl::begin(Primitive::LineStrip), corner(30, 40), corner(90, 70), dl::end()});
  append(b, {dl::begin(Primitive::Lines), corner(30, 40), corner(90, 70), dl::end()});
  CHECK(run(strip, a));
  CHECK(run(lines, b));
  CHECK(std::memcmp(strip.rgba(), lines.rgba(), EveRaster::kFramebufferBytes) == 0);

  // A single vertex draws nothing, and BEGIN forgets the pending vertex of the strip before it.
  w = style;
  append(w, {dl::begin(Primitive::LineStrip), centre(10, 10), dl::end()});
  CHECK(run(r, w));
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight);
  w = style;
  append(w, {dl::begin(Primitive::LineStrip), centre(10, 10), dl::begin(Primitive::LineStrip), centre(40, 40),
             dl::end()});
  CHECK(run(r, w));
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight);
}

TEST_CASE("raster: a LINE_STRIP is blended once, so its joints and overlaps are not covered twice") {
  EveRaster r;
  const Words style = {dl::color_rgb(255, 255, 255), dl::color_a(128), dl::line_width(8)};
  // Translucent: the joint pixel is the union coverage (243) scaled by COLOR_A once, (243 * 128 + 127) / 255 = 122.
  Words w = style;
  append(w, {dl::begin(Primitive::LineStrip), centre(10, 10), centre(20, 10), centre(20, 20), dl::end()});
  CHECK(run(r, w));
  CHECK(px(r, 20, 10) == 0x7A7A7A);
  CHECK(px(r, 15, 10) == 0x808080);  // inside one segment: coverage 255, alpha 128
  CHECK(px(r, 20, 15) == 0x808080);
  // A strip that goes out and back along itself covers each pixel as the single segment does: no pixel is blended
  // twice. (Drawn as two separate segments the line would be 128 + 128 * 127 / 255 = 192 where they overlap.)
  EveRaster strip, single;
  Words out_and_back = style, once = style;
  append(out_and_back, {dl::begin(Primitive::LineStrip), centre(10, 10), centre(30, 10), centre(10, 10), dl::end()});
  append(once, {dl::begin(Primitive::LineStrip), centre(10, 10), centre(30, 10), dl::end()});
  CHECK(run(strip, out_and_back));
  CHECK(run(single, once));
  CHECK(std::memcmp(strip.rgba(), single.rgba(), EveRaster::kFramebufferBytes) == 0);
  CHECK(px(strip, 20, 10) == 0x808080);

  // The strip is blended with the context of its first vertex: a colour, alpha or width set after it changes nothing
  // for this strip [A], and a scissor of the first vertex clips it.
  w = {dl::color_rgb(255, 0, 0), dl::line_width(8), dl::scissor_xy(0, 0), dl::scissor_size(16, 480),
       dl::begin(Primitive::LineStrip), centre(10, 10), dl::color_rgb(0, 255, 0), dl::line_width(64),
       dl::scissor_size(800, 480), centre(30, 10), dl::end()};
  CHECK(run(r, w));
  CHECK(px(r, 12, 10) == 0xFF0000);  // red, 1 px thick, clipped at x = 16
  CHECK(px(r, 12, 9) == kBlack);
  CHECK(px(r, 12, 11) == kBlack);
  CHECK(px(r, 16, 10) == kBlack);
  CHECK(px(r, 15, 10) == 0xFF0000);

  // A frame that fails half way through a strip leaves nothing of it for the next frame.
  EveRaster fresh;
  Words failing = style;
  append(failing, {dl::begin(Primitive::LineStrip), centre(100, 100), centre(200, 150), 0x09000000u});
  CHECK_FALSE(run(r, failing));
  Words next = style;
  append(next, {dl::begin(Primitive::LineStrip), centre(300, 300), centre(310, 300), dl::end()});
  CHECK(run(r, next));
  CHECK(run(fresh, next));
  CHECK(std::memcmp(r.rgba(), fresh.rgba(), EveRaster::kFramebufferBytes) == 0);
}

TEST_CASE("raster: a radius of 0 draws no point, line or strip [A]; RECTS and edge strips at width 0 are meaningful") {
  EveRaster r;
  // Odd sixteenths, where sample points are: a zero-width line through them would otherwise light up a row of
  // samples. The guide does not say what a radius of 0 draws; the simulator draws nothing, on purpose.
  const Words style = {dl::color_rgb(255, 255, 255), dl::line_width(0), dl::point_size(0)};
  const auto drawn = [&](Words w) {
    Words all = style;
    append(all, w);
    CHECK(run(r, all));
    return count(r, kBlack) != EveRaster::kWidth * EveRaster::kHeight;
  };
  CHECK_FALSE(drawn({dl::begin(Primitive::Lines), dl::vertex2f(160, 329), dl::vertex2f(320, 329), dl::end()}));
  CHECK_FALSE(drawn({dl::begin(Primitive::LineStrip), dl::vertex2f(160, 329), dl::vertex2f(320, 329),
                     dl::vertex2f(320, 489), dl::end()}));
  CHECK_FALSE(drawn({dl::begin(Primitive::Points), dl::vertex2f(161, 329), dl::end()}));
  CHECK_FALSE(drawn({dl::begin(Primitive::Points), dl::vertex2f(160, 320), dl::end()}));
  // The same line at the smallest width, 1/16 px, does draw.
  Words thin = {dl::color_rgb(255, 255, 255), dl::line_width(1), dl::begin(Primitive::Lines), dl::vertex2f(160, 329),
                dl::vertex2f(320, 329), dl::end()};
  CHECK(run(r, thin));
  CHECK(count(r, kBlack) != EveRaster::kWidth * EveRaster::kHeight);
  // A rectangle at LINE_WIDTH 0 is exactly the pixels whose centre is between the vertices (edges included).
  CHECK(drawn({dl::begin(Primitive::Rects), centre(10, 10), centre(12, 11), dl::end()}));
  CHECK(box_is(r, 10, 10, 13, 12, kWhite));
  CHECK(count(r, kWhite) == 6);
  // And an edge strip at width 0 is filled (see the edge-strip test).
  CHECK(drawn({dl::begin(Primitive::EdgeStripA), corner(10, 20), corner(14, 20), dl::end()}));
  CHECK(count(r, kWhite) == 4 * 20);
}

// ---- EDGE_STRIP_A / _B (guide section 2.5.4 pp.22-23, figures) ---------------------------------------------------

TEST_CASE("raster: EDGE_STRIP_A and _B fill above / below the polyline, to the edge of the screen") {
  const auto strip = [](Primitive prim, std::uint32_t width, std::uint32_t a, std::uint32_t b, std::uint32_t c = 0) {
    Words w = {dl::color_rgb(255, 255, 255), dl::line_width(width), dl::begin(prim), a, b};
    if (c != 0) {
      w.push_back(c);
    }
    w.push_back(dl::end());
    return w;
  };
  EveRaster r;
  // A flat segment on the pixel boundary y = 20, from x = 10 to 14, LINE_WIDTH 0. The columns whose centre lies
  // between the vertices are 10..13 (centre 14.5 is beyond 14). Above: rows whose centre is at most 20, rows 0..19.
  CHECK(run(r, strip(Primitive::EdgeStripA, 0, corner(10, 20), corner(14, 20))));
  CHECK(box_is(r, 10, 0, 14, 20, kWhite));
  CHECK(count(r, kWhite) == 4 * 20);
  // Below: centres of at least 20: rows 20..479.
  CHECK(run(r, strip(Primitive::EdgeStripB, 0, corner(10, 20), corner(14, 20))));
  CHECK(box_is(r, 10, 20, 14, 480, kWhite));
  CHECK(count(r, kWhite) == 4 * 460);
  // LINE_WIDTH 8 (half a pixel) moves the boundary half a pixel away from the fill, so the line's own thickness is
  // part of the fill: above reaches row 20 (centre 20.5 <= 20 + 0.5), below starts at row 19.
  CHECK(run(r, strip(Primitive::EdgeStripA, 8, corner(10, 20), corner(14, 20))));
  CHECK(count(r, kWhite) == 4 * 21);
  CHECK(px(r, 10, 20) == kWhite);
  CHECK(px(r, 10, 21) == kBlack);
  CHECK(run(r, strip(Primitive::EdgeStripB, 8, corner(10, 20), corner(14, 20))));
  CHECK(count(r, kWhite) == 4 * 461);
  CHECK(px(r, 10, 19) == kWhite);
  CHECK(px(r, 10, 18) == kBlack);
  // A line on pixel centres: both ends are columns of their own (centres 10.5 and 14.5 lie on the vertices, which
  // count), and a centre exactly on the boundary is inside: row 20 (centre 20.5) is filled by A at LINE_WIDTH 0.
  CHECK(run(r, strip(Primitive::EdgeStripA, 0, centre(10, 20), centre(14, 20))));
  CHECK(box_is(r, 10, 0, 15, 21, kWhite));
  CHECK(count(r, kWhite) == 5 * 21);
  // A slope of one pixel per column from (10, 20) to (14, 24), LINE_WIDTH 0: at the centre of column 10 + k the line
  // is at y = 20.5 + k, so above fills rows 0..20 + k: 21 + 22 + 23 + 24 = 90 pixels, below rows 20 + k..479.
  CHECK(run(r, strip(Primitive::EdgeStripA, 0, corner(10, 20), corner(14, 24))));
  CHECK(count(r, kWhite) == 90);
  for (int k = 0; k < 4; ++k) {
    CHECK(px(r, 10 + k, 20 + k) == kWhite);
    CHECK(px(r, 10 + k, 21 + k) == kBlack);
  }
  CHECK(run(r, strip(Primitive::EdgeStripB, 0, corner(10, 20), corner(14, 24))));
  CHECK(count(r, kWhite) == 460 + 459 + 458 + 457);
  // The order of the vertices does not matter.
  EveRaster reversed;
  CHECK(run(r, strip(Primitive::EdgeStripA, 0, corner(10, 20), corner(14, 24))));
  CHECK(run(reversed, strip(Primitive::EdgeStripA, 0, corner(14, 24), corner(10, 20))));
  CHECK(std::memcmp(r.rgba(), reversed.rgba(), EveRaster::kFramebufferBytes) == 0);
  // Two segments: the strip is the union, each pixel filled once (here: translucent, so a double fill would show).
  Words two = {dl::color_rgb(255, 255, 255), dl::color_a(128), dl::line_width(0), dl::begin(Primitive::EdgeStripA),
               centre(10, 20), centre(12, 20), centre(14, 20), dl::end()};
  CHECK(run(r, two));
  CHECK(box_is(r, 10, 0, 15, 21, 0x808080));  // column 12 belongs to both segments, and is 128, not 192
}

TEST_CASE("raster: an edge strip is clipped by the scissor, filled at END or DISPLAY, in the context of its first vertex") {
  EveRaster r;
  Words w = {dl::color_rgb(255, 255, 255), dl::line_width(0), dl::scissor_xy(0, 10), dl::scissor_size(800, 5),
             dl::begin(Primitive::EdgeStripA), corner(10, 20), corner(14, 20), dl::end()};
  CHECK(run(r, w));
  CHECK(box_is(r, 10, 10, 14, 15, kWhite));  // only rows 10..14 are inside the scissor
  CHECK(count(r, kWhite) == 20);
  // No END: DISPLAY completes the strip.
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::line_width(0), dl::begin(Primitive::EdgeStripB), corner(10, 470),
                corner(14, 470)}));
  CHECK(count(r, kWhite) == 4 * 10);
  // A colour change after the first vertex does not recolour the strip [A]; a change before it does.
  CHECK(run(r, {dl::color_rgb(255, 0, 0), dl::line_width(0), dl::begin(Primitive::EdgeStripB), corner(10, 470),
                dl::color_rgb(0, 255, 0), corner(14, 470), dl::end()}));
  CHECK(px(r, 10, 475) == 0xFF0000);
  // A following primitive is not affected by the strip's state, and a second strip starts afresh.
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::line_width(0), dl::begin(Primitive::EdgeStripB), corner(10, 470),
                corner(14, 470), dl::begin(Primitive::EdgeStripB), corner(20, 475), corner(24, 475), dl::end()}));
  CHECK(px(r, 10, 475) == kWhite);
  CHECK(px(r, 20, 475) == kWhite);
  CHECK(px(r, 20, 474) == kBlack);  // the second strip starts at row 475, not 470
  CHECK(px(r, 12, 470) == kWhite);
  // VERTEX2II + VERTEX_TRANSLATE feed the strip like any vertex.
  CHECK(run(r, {dl::color_rgb(255, 255, 255), dl::line_width(0), dl::vertex_translate_x(16 * 100),
                dl::begin(Primitive::EdgeStripA), dl::vertex2ii(10, 20, 0, 0), dl::vertex2ii(14, 20, 0, 0), dl::end()}));
  CHECK(box_is(r, 110, 0, 114, 20, kWhite));
  CHECK(count(r, kWhite) == 4 * 20);
}

// ---- BLEND_FUNC (guide section 4.18 pp.114-115) ------------------------------------------------------------------

TEST_CASE("raster: BLEND_FUNC modes: source over, additive, ZERO, replace; DST_ALPHA is refused") {
  EveRaster r;
  const Words under = fill(100, 50, 25, 0, 0, 4, 4);
  const auto over = [&](BlendFactor s, BlendFactor d, std::uint8_t alpha, std::uint8_t cr, std::uint8_t cg,
                        std::uint8_t cb) {
    Words w = under;
    append(w, {dl::blend_func(s, d), dl::color_a(alpha)});
    append(w, fill(cr, cg, cb, 0, 0, 4, 4));
    return w;
  };
  using F = BlendFactor;
  // The default, written out, is the source-over of the existing goldens: 200 * 128 / 255 + 100 * 127 / 255.
  CHECK(run(r, over(F::SrcAlpha, F::OneMinusSrcAlpha, 128, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0x964B25);  // (100 + 50, 50 + 25, 25 + 12): the same as "colour and alpha blend source over"
  // ONE, ONE adds and holds at 255 (100 + 100 = 200, 50 + 200 = 250, 25 + 250 clamps).
  CHECK(run(r, over(F::One, F::One, 255, 100, 200, 250)));
  CHECK(px(r, 1, 1) == 0xC8FAFF);
  // ... and ignores COLOR_A: factors of ONE do not look at the alpha, even when it is 0.
  CHECK(run(r, over(F::One, F::One, 0, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0xFF964B);  // (100 + 200 clamps, 50 + 100, 25 + 50)
  // SRC_ALPHA, ONE: the source scaled by its alpha, added: 200 * 128 / 255 = 100, 50, 25.
  CHECK(run(r, over(F::SrcAlpha, F::One, 128, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0xC86432);  // (100 + 100, 50 + 50, 25 + 25)
  // ZERO, ONE_MINUS_SRC_ALPHA: only the destination, scaled by 127 / 255: 50, 25, 12 (rounded to nearest).
  CHECK(run(r, over(F::Zero, F::OneMinusSrcAlpha, 128, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0x32190C);
  // ONE, ZERO replaces the destination by the source colour, whatever the alpha.
  CHECK(run(r, over(F::One, F::Zero, 128, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0xC86432);
  // ZERO, ZERO is black.
  CHECK(run(r, over(F::Zero, F::Zero, 255, 200, 100, 50)));
  CHECK(px(r, 1, 1) == kBlack);
  CHECK(px(r, 4, 4) == kBlack);  // outside the rectangle: the frame's black
  // ZERO, SRC_ALPHA keeps the destination in proportion to the source alpha: 100 * 128 / 255 = 50, 25, 13.
  CHECK(run(r, over(F::Zero, F::SrcAlpha, 128, 200, 100, 50)));
  CHECK(px(r, 1, 1) == 0x32190D);

  // The destination alpha is not modelled (the picture is opaque): refused, and the old picture stays.
  const std::uint32_t before = px(r, 1, 1);
  for (const Words& bad : {Words{dl::blend_func(F::DstAlpha, F::Zero)}, Words{dl::blend_func(F::One, F::OneMinusDstAlpha)},
                           Words{0x0B000000u | (6u << 3)}, Words{0x0B000000u | 7u}}) {
    CHECK_FALSE(run(r, bad));
    CHECK(r.error().find("BLEND_FUNC") != std::string::npos);
    CHECK(px(r, 1, 1) == before);
  }
}

TEST_CASE("raster: BLEND_FUNC is part of the context, starts as the default, and CLEAR ignores it") {
  EveRaster r;
  Words w = {dl::blend_func(BlendFactor::One, BlendFactor::One), dl::save_context(),
             dl::blend_func(BlendFactor::Zero, BlendFactor::Zero), dl::restore_context()};
  append(w, fill(100, 100, 100, 0, 0, 2, 2));
  append(w, fill(100, 100, 100, 0, 0, 2, 2));
  CHECK(run(r, w));
  CHECK(px(r, 0, 0) == 0xC8C8C8);  // ONE, ONE came back from the restore: 100 + 100
  // The next frame starts with the default again.
  w = fill(100, 100, 100, 0, 0, 2, 2);
  append(w, fill(100, 100, 100, 0, 0, 2, 2));
  CHECK(run(r, w));
  CHECK(px(r, 0, 0) == 0x646464);
  // CLEAR writes the clear colour: it is not blended, whatever the function and COLOR_A.
  CHECK(run(r, {dl::clear_color_rgb(10, 20, 30), dl::clear(true, true, true), dl::blend_func(BlendFactor::One, BlendFactor::One),
                dl::clear(true, true, true)}));
  CHECK(px(r, 5, 5) == 0x0A141E);
  CHECK(px(r, 799, 479) == 0x0A141E);
}

TEST_CASE("raster: blending applies to antialiased edges and bitmaps; ONE ignores coverage [A]") {
  EveRaster r;
  const auto line = [](BlendFactor s, BlendFactor d) {
    return Words{dl::color_rgb(255, 255, 255), dl::blend_func(s, d), dl::line_width(8), dl::begin(Primitive::Lines),
                 corner(10, 20), corner(20, 20), dl::end()};
  };
  using F = BlendFactor;
  // Half covered pixels (rows 19 and 20, coverage 128) over a blue background (0, 0, 255).
  Words w = fill(0, 0, 255, 0, 0, 40, 40);
  append(w, line(F::SrcAlpha, F::OneMinusSrcAlpha));
  CHECK(run(r, w));
  CHECK(px(r, 15, 19) == 0x8080FF);  // 255 * 128 / 255 = 128 plus 255 * 127 / 255 = 127
  CHECK(px(r, 15, 20) == 0x8080FF);
  // ONE, ONE: the factor ONE does not depend on the coverage, so the edge pixels add the whole colour [A].
  w = fill(0, 0, 100, 0, 0, 40, 40);
  append(w, line(F::One, F::One));
  CHECK(run(r, w));
  CHECK(px(r, 15, 19) == 0xFFFFFF);
  CHECK(px(r, 15, 18) == 0x000064);  // untouched
  // SRC_ALPHA, ONE: the alpha is COLOR_A * coverage, so the edge pixels add half the colour.
  w = fill(0, 0, 100, 0, 0, 40, 40);
  append(w, line(F::SrcAlpha, F::One));
  CHECK(run(r, w));
  CHECK(px(r, 15, 19) == 0x8080E4);  // (128, 128, 128 + 100)

  // Bitmaps: a transparent texel (alpha 0) is never drawn, whichever the function; the others are blended.
  const std::uint8_t data[] = {0x80};  // L1, 8 px: only the leftmost is set
  std::memcpy(r.ram_g(), data, 1);
  w = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 1, 1),
       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 1)};
  append(w, fill(10, 20, 30, 0, 0, 20, 4));
  append(w, {dl::color_rgb(100, 100, 100), dl::blend_func(BlendFactor::One, BlendFactor::One),
             dl::begin(Primitive::Bitmaps), dl::vertex2ii(2, 2, 1, 0), dl::end()});
  CHECK(run(r, w));
  CHECK(px(r, 2, 2) == 0x6E7882);  // 10 + 100, 20 + 100, 30 + 100
  CHECK(px(r, 3, 2) == 0x0A141E);  // the transparent texel left the background alone
}

// ---- A reference for the antialiased capsules --------------------------------------------------------------------

namespace {

// An independent statement of the coverage model of sim/README.md: 8 x 8 samples at odd sixteenths of each pixel, a
// sample is covered when its distance to the segment is at most the radius, decided with exact 128-bit
// squares (no square-root trick, no bounding shortcut), coverage = (count * 255 + 32) / 64.
struct Ref {
  long long x0, y0, x1, y1, r;
  bool covered(long long x, long long y) const {
    const long long dx = x1 - x0, dy = y1 - y0;
    const long long rx = x - x0, ry = y - y0;
    const long long along = rx * dx + ry * dy, len2 = dx * dx + dy * dy;
    const auto sq = [](long long v) { return static_cast<__int128>(v) * v; };
    if (along <= 0) {
      return sq(rx) + sq(ry) <= sq(r);
    }
    if (along >= len2) {
      return sq(x - x1) + sq(y - y1) <= sq(r);
    }
    const long long across = rx * dy - ry * dx;
    return sq(across) <= sq(r) * len2;
  }
  // Distance from a point to the segment, in sixteenths, in floating point. Only used to skip pixels that are
  // certainly empty (the margin of 16 is more than the 11.3 a pixel centre can be from its own corners).
  double distance(long long x, long long y) const {
    const double dx = static_cast<double>(x1 - x0), dy = static_cast<double>(y1 - y0);
    const double rx = static_cast<double>(x - x0), ry = static_cast<double>(y - y0);
    const double len2 = dx * dx + dy * dy;
    double t = len2 == 0 ? 0 : (rx * dx + ry * dy) / len2;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double ex = rx - t * dx, ey = ry - t * dy;
    return std::sqrt(ex * ex + ey * ey);
  }
  unsigned coverage(int px_, int py_) const {
    if (distance(px_ * 16LL + 8, py_ * 16LL + 8) > static_cast<double>(r) + 16.0) {
      return 0;
    }
    unsigned n = 0;
    for (int j = 0; j < 8; ++j) {
      for (int i = 0; i < 8; ++i) {
        n += covered(px_ * 16 + 2 * i + 1, py_ * 16 + 2 * j + 1) ? 1u : 0u;
      }
    }
    return (n * 255u + 32u) / 64u;
  }
};

struct Lcg {
  std::uint32_t s;
  std::uint32_t next() { return s = s * 1664525u + 1013904223u; }
  int range(int lo, int hi) { return lo + static_cast<int>((next() >> 8) % static_cast<std::uint32_t>(hi - lo + 1)); }
};

}  // namespace

TEST_CASE("raster property: antialiased capsules equal an independent reference, pixel for pixel") {
  Lcg rng{12345u};
  EveRaster r;
  int nonempty = 0;
  for (int n = 0; n < 40; ++n) {
    const bool point = n % 4 == 3;
    // Sixteenths; some cases start far outside the screen and some are tiny.
    const int span = n % 5 == 0 ? 12000 : 2400;
    Ref ref{rng.range(-1600, 12800 + 1600), rng.range(-1600, 7680 + 1600), 0, 0, rng.range(0, n % 7 == 0 ? 400 : 60)};
    ref.x1 = point ? ref.x0 : ref.x0 + rng.range(-span, span);
    ref.y1 = point ? ref.y0 : ref.y0 + rng.range(-span, span);
    if (n == 1) {  // a case with both vertices off-screen and a thin line crossing the screen
      ref = Ref{-3000, 1000, 15000, 6000, 12};
    }
    const std::int32_t lim = 15999;  // VERTEX_FORMAT 4: +-1024 px
    for (long long* v : {&ref.x0, &ref.y0, &ref.x1, &ref.y1}) {
      *v = *v < -lim ? -lim : (*v > lim ? lim : *v);
    }
    Words w = {dl::color_rgb(255, 255, 255), dl::line_width(static_cast<std::uint32_t>(ref.r)),
               dl::point_size(static_cast<std::uint32_t>(ref.r)),
               dl::begin(point ? Primitive::Points : Primitive::Lines),
               dl::vertex2f(static_cast<std::int32_t>(ref.x0), static_cast<std::int32_t>(ref.y0))};
    if (!point) {
      w.push_back(dl::vertex2f(static_cast<std::int32_t>(ref.x1), static_cast<std::int32_t>(ref.y1)));
    }
    CHECK(run(r, w));
    // Everything the reference covers is on screen with the same value; the picture holds nothing else.
    unsigned long long want_sum = 0, got_sum = 0;
    const long long lo_x = (std::min(ref.x0, ref.x1) - ref.r) / 16 - 2, hi_x = (std::max(ref.x0, ref.x1) + ref.r) / 16 + 2;
    const long long lo_y = (std::min(ref.y0, ref.y1) - ref.r) / 16 - 2, hi_y = (std::max(ref.y0, ref.y1) + ref.r) / 16 + 2;
    for (long long y = std::max<long long>(lo_y, 0); y <= std::min<long long>(hi_y, EveRaster::kHeight - 1); ++y) {
      for (long long x = std::max<long long>(lo_x, 0); x <= std::min<long long>(hi_x, EveRaster::kWidth - 1); ++x) {
        const unsigned want = ref.coverage(static_cast<int>(x), static_cast<int>(y));
        want_sum += want;
        if ((px(r, static_cast<int>(x), static_cast<int>(y)) & 0xFF) != want) {
          FAIL_CHECK("case " << n << " pixel (" << x << ", " << y << "): got " << (px(r, static_cast<int>(x), static_cast<int>(y)) & 0xFF)
                             << ", reference " << want);
          return;
        }
      }
    }
    for (int y = 0; y < EveRaster::kHeight; ++y) {
      for (int x = 0; x < EveRaster::kWidth; ++x) {
        got_sum += px(r, x, y) & 0xFF;
      }
    }
    CHECK(got_sum == want_sum);
    nonempty += want_sum > 0 ? 1 : 0;
  }
  CHECK(nonempty >= 20);  // most of the cases really draw something (the others lie off the screen)
}

// ---- Unit goldens: small scenes whose pixels are known by hand --------------------------------------------------

// Each scene is a few words whose picture is derived by hand in the cases above (the pixels are asserted there and
// again here for the facts), and whose whole framebuffer is pinned by its SHA-256, so that a stray pixel anywhere
// else shows up as well. A hash is the simulator's truth, not the chip's (sim/README.md): after a deliberate change
// of the model, look at the pictures first and then update the literals.
TEST_CASE("raster goldens: small hand-checkable scenes of the new primitives") {
  struct Scene {
    const char* name;
    Words words;
    const char* sha256;
  };
  const auto wh = [](Words w) {
    Words all = {dl::color_rgb(255, 255, 255), dl::line_width(8)};
    append(all, w);
    return all;
  };
  const Scene scenes[] = {
      {"translated rectangle: 4 x 2 at (20, 18)",
       [&] {
         Words w = {dl::vertex_translate_x(160), dl::vertex_translate_y(-32)};
         append(w, fill(255, 255, 255, 10, 20, 14, 22));
         return w;
       }(),
       "0a6db9039da299bd5e4d461b72387cfeda22bdc9c6cac9ce10c93831dd6d2882"},
      {"point of radius 1 px at a pixel corner: four pixels of 207",
       wh({dl::point_size(16), dl::begin(Primitive::Points), corner(50, 50), dl::end()}), "3abd629f7e64ff1f7519cef5f622fd73e767ad49511d0183db9c1e416ea266fa"},
      {"1 px line on a pixel boundary: two rows of 128",
       wh({dl::begin(Primitive::Lines), dl::vertex2f(160, 320), dl::vertex2f(320, 320), dl::end()}), "fcd79624f3fbaf1e5410f1f5722092757d31e797d6fee8c733c322fbecc3e163"},
      {"L-shaped line strip between pixel centres (the joint pixel is 243, blended once)",
       wh({dl::begin(Primitive::LineStrip), centre(10, 10), centre(20, 10), centre(20, 20), dl::end()}), "49c1b29ed45375d239c2a13a9ac3aacf7256550339846dbf53f189623acd303c"},
      {"edge strip above a slope: 21 + 22 + 23 + 24 pixels",
       wh({dl::line_width(0), dl::begin(Primitive::EdgeStripA), corner(10, 20), corner(14, 24), dl::end()}), "f759dec169b64776b920916933151d08e3d5eb4ba2e73aa5eec719de37d453c8"},
      {"additive blend over a rectangle",
       [&] {
         Words w = fill(100, 50, 25, 0, 0, 4, 4);
         append(w, {dl::blend_func(BlendFactor::One, BlendFactor::One)});
         append(w, fill(100, 200, 250, 0, 0, 4, 4));
         return w;
       }(),
       "e82d610d3753d98df1d6b3b95d9e5f0881d24214fcef24d486574c70994ab99e"},
  };
  EveRaster r;
  for (const Scene& scene : scenes) {
    CHECK(run(r, scene.words));
    const std::string hash = o3sdk::platform::host::sha256_hex(r.rgba(), EveRaster::kFramebufferBytes);
    CHECK_MESSAGE(hash == std::string(scene.sha256), scene.name << ": " << hash);
  }
  // The facts behind three of them, so a wrong hash cannot be "fixed" without reading the picture.
  CHECK(run(r, scenes[1].words));
  CHECK(px(r, 49, 49) == 0xCFCFCF);
  CHECK(count(r, kBlack) == EveRaster::kWidth * EveRaster::kHeight - 4);
  CHECK(run(r, scenes[2].words));
  CHECK(px(r, 15, 19) == 0x808080);
  CHECK(px(r, 15, 20) == 0x808080);
  CHECK(run(r, scenes[4].words));
  CHECK(count(r, kWhite) == 90);
}
