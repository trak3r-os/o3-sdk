// SPDX-License-Identifier: GPL-3.0-or-later
// EveRaster's load on a scan line (peak_line_prims, peak_line_px: sim/include/o3sdk/sim/eve_raster.h): how many primitives, and how many box pixels,
// meet on one scan line of a frame. A bitmap counts its BITMAP_SIZE box, not its ink; RECTS, POINTS and LINES count their boxes. The fill-rate band
// (apps/fillrate) ties the counter to what the unit measured: the sprites and the pixels a line of the band carries.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/sim/eve_raster.h>

#include <band.h>  // apps/fillrate/src

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;
using o3sdk::sim::EveRaster;

namespace {

using Words = std::vector<std::uint32_t>;

bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

// A pixel position in the vertex format of the chip's default (1/16 px).
std::uint32_t v(int x, int y) { return dl::vertex2f(x * 16, y * 16); }

// Handle 1 as a 16 x 16 L1 glyph at address 0 (all zero: no ink at all), BITMAPS begun.
Words glyphs_begin() {
  return {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 2, 16),
          dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 16, 16), dl::begin(Primitive::Bitmaps)};
}

}  // namespace

TEST_CASE("line load: nothing drawn is no load, and a clear is not a primitive") {
  EveRaster r;
  CHECK(r.peak_line_prims() == 0u);
  CHECK(r.peak_line_px() == 0u);
  REQUIRE(run(r, {dl::clear_color_rgb(1, 2, 3), dl::clear(true, true, true)}));
  CHECK(r.last_frame_peak_line_prims() == 0u);
  CHECK(r.last_frame_peak_line_px() == 0u);
}

TEST_CASE("line load: ten overlapping glyphs on a row are ten primitives and ten boxes, ink or not") {
  Words w = glyphs_begin();
  for (int i = 0; i < 10; ++i) {
    w.push_back(v(100 + 3 * i, 50));  // 3 px apart: they overlap, the chip fetches each whole box on all 16 rows
  }
  w.push_back(dl::end());
  EveRaster r;
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_prims() == 10u);
  CHECK(r.last_frame_peak_line_px() == 160u);
  CHECK(r.peak_line_prims() == 10u);
  CHECK(r.peak_line_px() == 160u);
  // A lighter frame changes the last frame's figures and leaves the run's peak.
  Words one = glyphs_begin();
  one.push_back(v(10, 10));
  one.push_back(dl::end());
  REQUIRE(run(r, one));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 16u);
  CHECK(r.peak_line_prims() == 10u);
  CHECK(r.peak_line_px() == 160u);
  // A frame the raster refuses (no DISPLAY) is in neither.
  Words refused = glyphs_begin();
  for (int i = 0; i < 20; ++i) {
    refused.push_back(v(10 + i, 10));
  }
  CHECK_FALSE(r.render(refused.data(), refused.size()));
  CHECK(r.peak_line_prims() == 10u);
  CHECK(r.last_frame_peak_line_prims() == 1u);
}

TEST_CASE("line load: glyphs on different rows do not add; the busiest row counts") {
  Words w = glyphs_begin();
  for (int i = 0; i < 4; ++i) {
    w.push_back(v(10 + 20 * i, 100));  // rows 100..115: four
  }
  w.push_back(v(10, 108));             // rows 108..123: a fifth over rows 108..115
  w.push_back(dl::end());
  EveRaster r;
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_prims() == 5u);
  CHECK(r.last_frame_peak_line_px() == 80u);
}

TEST_CASE("line load: a box is clipped to the scissor and the screen") {
  EveRaster r;
  Words w = glyphs_begin();
  w.insert(w.begin(), {dl::scissor_xy(108, 0), dl::scissor_size(4, 480)});
  w.push_back(v(100, 50));  // x 100..115, 4 px of it inside the scissor
  w.push_back(dl::end());
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 4u);
  Words edge = glyphs_begin();
  edge.push_back(v(790, 470));  // 10 px wide and 10 rows left of the 16 x 16 box
  edge.push_back(v(-8, 470));   // 8 px wide on the same rows
  edge.push_back(dl::end());
  REQUIRE(run(r, edge));
  CHECK(r.last_frame_peak_line_prims() == 2u);
  CHECK(r.last_frame_peak_line_px() == 18u);
  Words off = glyphs_begin();
  off.push_back(v(900, 50));  // entirely off the screen: not on any line
  off.push_back(dl::end());
  REQUIRE(run(r, off));
  CHECK(r.last_frame_peak_line_prims() == 0u);
}

TEST_CASE("line load: a 16 px bitmap with the SIZE_H of an 800 px one left set is 528 px, which is what the chip fetches") {
  EveRaster r;
  Words w = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 2, 16),
             dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 800, 16), dl::bitmap_size_h(800 >> 9, 0),
             dl::begin(Primitive::Bitmaps), v(0, 10), dl::end()};
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_px() == 800u);
  // BITMAP_SIZE for 16 px alone does not clear the high bits: 512 + 16.
  w = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 2, 16),
       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 800, 16), dl::bitmap_size_h(800 >> 9, 0),
       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 16, 16), dl::begin(Primitive::Bitmaps), v(0, 10), dl::end()};
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 528u);
  // The six-word bind (SIZE_H written too) is what makes it 16.
  w = {dl::bitmap_handle(1), dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 2, 16),
       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 800, 16), dl::bitmap_size_h(800 >> 9, 0),
       dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 16, 16), dl::bitmap_size_h(0, 0), dl::begin(Primitive::Bitmaps), v(0, 10),
       dl::end()};
  REQUIRE(run(r, w));
  CHECK(r.last_frame_peak_line_px() == 16u);
}

TEST_CASE("line load: RECTS, POINTS, LINES and LINE_STRIP count their bounding boxes") {
  EveRaster r;
  // RECTS at LINE_WIDTH 16 (1 px): the rectangle grown by 1 px, 7 x 4 here, as the raster fills it.
  REQUIRE(run(r, {dl::line_width(16), dl::begin(Primitive::Rects), v(10, 20), v(15, 22), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 7u);
  // POINTS of radius 5 px at (100, 100): the box is 10 x 10.
  REQUIRE(run(r, {dl::point_size(80), dl::begin(Primitive::Points), v(100, 100), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 10u);
  // A point of radius 0 draws nothing and counts nothing.
  REQUIRE(run(r, {dl::point_size(0), dl::begin(Primitive::Points), v(100, 100), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 0u);
  // LINES, half-width 1 px, horizontal from x 10 to 20 on row 20: the box is x 9..20 (12 px wide: 11 px of length and the cap), rows 19..20.
  REQUIRE(run(r, {dl::line_width(16), dl::begin(Primitive::Lines), v(10, 20), v(20, 20), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 1u);
  CHECK(r.last_frame_peak_line_px() == 12u);
  // A LINE_STRIP of three vertices is two segments: on a row they both reach, two primitives.
  REQUIRE(run(r, {dl::line_width(16), dl::begin(Primitive::LineStrip), v(10, 20), v(20, 20), v(30, 20), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 2u);
  CHECK(r.last_frame_peak_line_px() == 12u + 12u);
  // One vertex draws no segment.
  REQUIRE(run(r, {dl::line_width(16), dl::begin(Primitive::LineStrip), v(10, 20), dl::end()}));
  CHECK(r.last_frame_peak_line_prims() == 0u);
}

// ---- The fill-rate band: the counter against what apps/fillrate says a line carries --------------------------------------------------

namespace {

struct VectorSink {
  Words words;
  void operator()(std::uint32_t w) { words.push_back(w); }
};

// The band the app draws (compose() inside the renderer's raw section: LINE_WIDTH 8), on an otherwise empty frame.
Words band_frame(const fillrate::Layers& n, unsigned cell_w) {
  VectorSink sink;
  sink.words.push_back(dl::line_width(8));
  fillrate::compose(sink, n, cell_w);
  return sink.words;
}

}  // namespace

TEST_CASE("line load: the fill-rate band gives the sprites and the pixels a line of it carries, for every kind, K and cell width") {
  const fillrate::Kind kinds[] = {fillrate::Kind::L8Nearest, fillrate::Kind::L8Bilinear, fillrate::Kind::L4, fillrate::Kind::Rect,
                                  fillrate::Kind::Mix};
  const unsigned ks[] = {1, 2, 3, 5, 8, 16, 32};
  static_assert(sizeof(kinds) / sizeof(kinds[0]) == fillrate::kKindCount, "every kind");
  for (const fillrate::Kind kind : kinds) {
    for (const unsigned k : ks) {
      for (const unsigned cell_w : fillrate::kCellWidths) {
        const fillrate::Layers n = fillrate::layers_for(kind, k);
        CAPTURE(static_cast<unsigned>(kind));
        CAPTURE(k);
        CAPTURE(cell_w);
        EveRaster r;
        REQUIRE(run(r, band_frame(n, cell_w)));
        CHECK(r.last_frame_peak_line_prims() == fillrate::sprites_per_line(n, cell_w));
        CHECK(r.last_frame_peak_line_px() == k * fillrate::kBandW);
        CHECK(r.last_frame_peak_line_px() == fillrate::px_per_line(n));
        CHECK(r.peak_line_prims() == fillrate::sprites_per_line(n, cell_w));
      }
    }
  }
}

TEST_CASE("line load: the band sizes of the unit's fill-rate edges, as the counter reads them") {
  // The edges were measured on the unit (o3-303 docs/hw/2026-10-30-fillrate.md, quoted by its docs/SKIN-PLAN.md section 4.4). This checks
  // that the counter reads those bands as the plan counts them; it does not check the unit's limit.
  // L8 clean at 208 primitives and 20,800 px at cell 100: K = 26 layers of 8 cells.
  EveRaster r;
  const fillrate::Layers l8 = fillrate::layers_for(fillrate::Kind::L8Nearest, 26);
  REQUIRE(run(r, band_frame(l8, 100)));
  CHECK(r.last_frame_peak_line_prims() == 208u);
  CHECK(r.last_frame_peak_line_px() == 20800u);
  // L8 clean at 224 primitives and 11,200 px at cell 50: K = 14 layers of 16 cells.
  const fillrate::Layers l8b = fillrate::layers_for(fillrate::Kind::L8Nearest, 14);
  REQUIRE(run(r, band_frame(l8b, 50)));
  CHECK(r.last_frame_peak_line_prims() == 224u);
  CHECK(r.last_frame_peak_line_px() == 11200u);
  // MIX clean at 128 primitives and 6,400 px at cell 50: K = 8.
  const fillrate::Layers mix = fillrate::layers_for(fillrate::Kind::Mix, 8);
  REQUIRE(run(r, band_frame(mix, 50)));
  CHECK(r.last_frame_peak_line_prims() == 128u);
  CHECK(r.last_frame_peak_line_px() == 6400u);
}
