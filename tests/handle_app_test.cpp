// SPDX-License-Identifier: GPL-3.0-or-later
// Bitmap handle 0 is the app's (ui/plot/canvas.h, kHandleApp): none of the kit's handles, a glyph face's included, and EveRaster counts every use of it.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/font.h>
#include <o3sdk/ui/plot/canvas.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include <cstdint>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;
using o3sdk::sim::EveRaster;
namespace ui = o3sdk::ui;

namespace {

using Words = std::vector<std::uint32_t>;

bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

std::uint32_t v(int x, int y) { return dl::vertex2f(x * 16, y * 16); }

// The five parameter words that configure the current handle as a 16 x 16 L1 bitmap with address 0: SOURCE, LAYOUT, LAYOUT_H, SIZE, SIZE_H.
Words configure() {
  return {dl::bitmap_source(0), dl::bitmap_layout(BitmapFormat::L1, 2, 16), dl::bitmap_layout_h(0, 0),
          dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 16, 16), dl::bitmap_size_h(0, 0)};
}

void append(Words& to, const Words& more) { to.insert(to.end(), more.begin(), more.end()); }

}  // namespace

TEST_CASE("handle 0: the app's handle is none of the kit's, and not the coprocessor's") {
  static_assert(ui::plot::kHandleApp == 0, "the app's bitmaps are on handle 0");  // canvas.h holds the rest of the proof
  CHECK(ui::plot::kHandleApp != ui::kHandleHexLo);
  CHECK(ui::plot::kHandleApp != ui::kHandleNotes);
  CHECK(ui::plot::kHandleApp != ui::kHandleIcons);
  CHECK(ui::plot::kHandleApp != ui::plot::kHandleWaveLane0);
  CHECK(ui::plot::kHandleApp != ui::plot::kHandleBootLogo);
  CHECK(ui::plot::kHandleApp != 15);
}

TEST_CASE("handle 0: no glyph face is on it (the faces' handles are generated: 1 + the position in ui::Font)") {
  for (unsigned f = 0; f < ui::kFontCount; ++f) {
    CAPTURE(f);
    const ui::FontFace& face = ui::font_face(static_cast<ui::Font>(f));
    CHECK(face.handle != ui::plot::kHandleApp);
    CHECK(face.handle >= 1u);
    CHECK(face.handle <= 4u);
  }
}

TEST_CASE("handle 0: the raster counts the words that configure it and the bitmaps drawn with it, and nothing else") {
  EveRaster r;
  CHECK(r.handle_app_uses() == 0u);
  // Handle 1 configured and drawn, by VERTEX2F (the current handle) and VERTEX2II (its own): handle 0 is not used.
  Words w = {dl::bitmap_handle(1)};
  append(w, configure());
  append(w, {dl::begin(Primitive::Bitmaps), v(10, 10), dl::vertex2ii(30, 10, 1, 0), dl::end()});
  REQUIRE(run(r, w));
  CHECK(r.handle_app_uses() == 0u);
  // A frame starts on handle 0: the five words before any BITMAP_HANDLE configure it, and count.
  w.clear();
  append(w, configure());
  REQUIRE(run(r, w));
  CHECK(r.handle_app_uses() == 5u);
  // Explicitly on handle 0: the five again; a VERTEX2F draws with it, a VERTEX2II with handle 0 too; a VERTEX2II with handle 1 does not.
  w = {dl::bitmap_handle(1)};
  append(w, configure());
  append(w, {dl::bitmap_handle(0)});
  append(w, configure());
  append(w, {dl::begin(Primitive::Bitmaps), v(10, 10), dl::vertex2ii(30, 10, 0, 0), dl::vertex2ii(50, 10, 1, 0), dl::end()});
  REQUIRE(run(r, w));
  CHECK(r.handle_app_uses() == 5u + 5u + 2u);
  // Words of other kinds on handle 0 (CELL, the transform, the colour) are not uses; the context restores the handle that was saved.
  const std::size_t before = r.handle_app_uses();
  w = {dl::save_context(), dl::bitmap_handle(2)};
  append(w, configure());
  append(w, {dl::restore_context(), dl::cell(1), dl::bitmap_transform_a(256), dl::color_rgb(1, 2, 3)});
  REQUIRE(run(r, w));
  CHECK(r.handle_app_uses() == before);
  w = {dl::save_context(), dl::bitmap_handle(2), dl::restore_context(), dl::bitmap_source(64)};  // back on handle 0: one use
  REQUIRE(run(r, w));
  CHECK(r.handle_app_uses() == before + 1u);
}
