// SPDX-License-Identifier: GPL-3.0-or-later
// ui::plot::Canvas (kit/ui/plot canvas.h): the app's bitmaps and shapes in one raw section of a Renderer. Words first (what a bind, a section and each
// primitive cost, and which state each call emits only when it changed), then the boundary with the Renderer (origin, colour), the checks, and pixels
// through the simulator's raster (tint, L4 and L8 cells, the 800-wide REPEAT band, round_rect, the context a section leaves behind).
#include <doctest/doctest.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/eve/dl.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/eve_raster.h>
#include <o3sdk/sim/raster_display.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/plot/canvas.h>
#include <o3sdk/ui/plot/meters.h>
#include <o3sdk/ui/plot/ram_g_map.h>
#include <o3sdk/ui/renderer.h>
#include <o3sdk/ui/theme.h>

#include "support/aborts.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace dl = o3sdk::eve::dl;
using namespace o3sdk;
using eve::BitmapFormat;
using eve::Filter;
using eve::Primitive;
using eve::Wrap;
using test::aborts;
using ui::Color;
using ui::Rect;
using ui::Rgb;
using ui::plot::BitmapDesc;
using ui::plot::Canvas;
using ui::plot::kRamGApp;
using Words = std::vector<std::uint32_t>;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

// What Renderer::begin() emits before anything of the test: CLEAR_COLOR_RGB, CLEAR, LINE_WIDTH and four words for each of the eight handles.
constexpr std::size_t kPreamble = 3 + 4 * (ui::kFontCount + ui::kTileSetCount);

// The words a frame holds between the preamble and DISPLAY.
template <typename Fn>
Words body(Fn draw, std::size_t capacity = eve::kMaxDisplayListWords) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(capacity);
  eve::DisplayListBuilder builder(sink, storage.data(), capacity);
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  draw(r);
  CHECK(r.end());
  const Words& f = sink.last_frame();
  return Words(f.begin() + static_cast<std::ptrdiff_t>(kPreamble), f.end() - 1);
}

Words cat(std::initializer_list<Words> parts) {
  Words all;
  for (const Words& p : parts) {
    all.insert(all.end(), p.begin(), p.end());
  }
  return all;
}

unsigned count_op(const Words& w, std::uint32_t opcode) {
  unsigned n = 0;
  for (const std::uint32_t word : w) {
    n += (word >> 24) == opcode ? 1u : 0u;
  }
  return n;
}

constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kRed{200, 30, 30};
constexpr Rgb kBlue{30, 60, 220};

// An 800-wide texture band of a 256 x 1 tile (L4: 128 bytes a row, REPEAT in x), and a 16 px LED (L8 cells of 16 x 16).
constexpr BitmapDesc kBand{kRamGApp, BitmapFormat::L4, 128, 256, 1, 800, 1, Wrap::Repeat, Wrap::Border};
constexpr BitmapDesc kLed{kRamGApp + 4096, BitmapFormat::L8, 16, 16, 16, 16, 16};
// Four 2 x 2 L4 cells and four 4 x 2 L8 cells for the pixel tests.
constexpr BitmapDesc kTiny4{kRamGApp + 8192, BitmapFormat::L4, 1, 2, 2, 2, 2};
constexpr BitmapDesc kTiny8{kRamGApp + 8704, BitmapFormat::L8, 4, 4, 2, 4, 2};
static_assert(ui::plot::bitmap_desc_valid(kBand) && ui::plot::bitmap_desc_valid(kLed) && ui::plot::bitmap_desc_valid(kTiny4) && ui::plot::bitmap_desc_valid(kTiny8),
              "the test's descs are drawable");
static_assert(!ui::plot::bitmap_desc_valid(BitmapDesc{0, BitmapFormat::L8, 16, 16, 16, 16, 16}) &&
                  !ui::plot::bitmap_desc_valid(BitmapDesc{kRamGApp + ui::plot::kRamGAppBytes - 128, BitmapFormat::L8, 16, 16, 16, 16, 16}),
              "a desc outside the app's region, or whose first cell ends past it, is not drawable");

// The six words of a bind of `d`.
Words bind_words(const BitmapDesc& d) {
  return {dl::bitmap_handle(0),
          dl::bitmap_source(d.ram_g),
          dl::bitmap_layout(d.format, d.stride, d.cell_h),
          dl::bitmap_layout_h(d.stride >> 10, d.cell_h >> 9),
          dl::bitmap_size(Filter::Nearest, d.wrap_x, d.wrap_y, d.draw_w, d.draw_h),
          dl::bitmap_size_h(d.draw_w >> 9, d.draw_h >> 9)};
}

// ---- A simulated panel -------------------------------------------------------------------------------------------------------------------
constexpr std::uint8_t mul255(unsigned a, unsigned b) { return static_cast<std::uint8_t>((a * b + 127u) / 255u); }  // the simulator's, a * b / 255 rounded

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  // Bytes into RAM_G (word aligned, a multiple of four long).
  void put(std::uint32_t addr, const std::vector<std::uint8_t>& bytes) {
    alignas(4) std::uint8_t buf[4096] = {};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      buf[i] = bytes[i];
    }
    CHECK(display.upload(addr, buf, (bytes.size() + 3u) & ~std::size_t{3}) == hal::UploadResult::Ok);
  }
  template <typename Fn>
  bool draw(Fn fn, const ui::Theme& theme = amiga()) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(Color::Pane));
    fn(r);
    return r.end() && display.render_failures() == 0;
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  std::vector<std::uint8_t> image() const { return std::vector<std::uint8_t>(raster.rgba(), raster.rgba() + sim::EveRaster::kFramebufferBytes); }
};

std::uint32_t pane() {
  const Rgb& p = amiga().rgb(Color::Pane);
  return (std::uint32_t{p.r} << 16) | (std::uint32_t{p.g} << 8) | p.b;
}

// `colour` over `under` at the coverage `a` (0..255): the simulator's source-over.
std::uint32_t over(Rgb colour, std::uint32_t under, unsigned a) {
  const unsigned ur = (under >> 16) & 0xFFu, ug = (under >> 8) & 0xFFu, ub = under & 0xFFu;
  return (std::uint32_t{mul255(colour.r, a)} + mul255(ur, 255u - a)) << 16 | (std::uint32_t{mul255(colour.g, a)} + mul255(ug, 255u - a)) << 8 |
         (std::uint32_t{mul255(colour.b, a)} + mul255(ub, 255u - a));
}

}  // namespace

// ---- Words ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("canvas: a bind is six words, always, the two _H words included") {
  const Words w = body([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kBand);
    c.close();
  });
  CHECK(w == cat({{dl::save_context()}, bind_words(kBand), {dl::restore_context()}}));
  CHECK(w[1 + 5] == dl::bitmap_size_h(1, 0));  // 800 = 1 * 512 + 288: the high bit of the width

  // A row of 1,600 bytes needs the stride's high bit, and 800 x 600 drawn (64-row cells repeated down) the size's two; the app's region is too
  // small for a cell height of 512 or more, so that bit stays 0.
  constexpr BitmapDesc big{kRamGApp, BitmapFormat::L8, 1600, 1600, 64, 800, 600, Wrap::Border, Wrap::Repeat};
  static_assert(ui::plot::bitmap_desc_valid(big), "");
  const Words b = body([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(big);
    c.close();
  });
  CHECK(b[1 + 2] == dl::bitmap_layout(BitmapFormat::L8, 1600, 64));
  CHECK(b[1 + 3] == dl::bitmap_layout_h(1, 0));
  CHECK(b[1 + 4] == dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Repeat, 800, 600));
  CHECK(b[1 + 5] == dl::bitmap_size_h(1, 1));
}

TEST_CASE("canvas: after the 800-wide band, a 16 px LED's bind carries LAYOUT_H and SIZE_H zero") {
  const Words w = body([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kBand);
    c.bind(kLed);
    c.close();
  });
  REQUIRE(w.size() == 14u);
  CHECK(w[1 + 5] == dl::bitmap_size_h(1, 0));
  CHECK(w[7 + 3] == dl::bitmap_layout_h(0, 0));
  CHECK(w[7 + 5] == dl::bitmap_size_h(0, 0));
  CHECK(w == cat({{dl::save_context()}, bind_words(kBand), bind_words(kLed), {dl::restore_context()}}));
}

TEST_CASE("canvas: a rebind in the same section is elided; the next section and a new Canvas bind again") {
  const Words one = body([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    c.bind(kLed);
    c.close();
    c.open();
    c.bind(kLed);
    c.close();
  });
  CHECK(count_op(one, eve::opcode::kBitmapHandle) == 2u);  // RESTORE_CONTEXT may undo a bind (canvas.h): each section binds again
  CHECK(one.size() == 1u + 6u + 1u + 1u + 6u + 1u);  // SAVE, the bind, RESTORE; SAVE, the bind, RESTORE

  // Another desc is six words again, and going back to the first is too (the cache holds the last).
  const Words three = body([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    c.bind(kBand);
    c.bind(kLed);
    c.close();
  });
  CHECK(count_op(three, eve::opcode::kBitmapHandle) == 3u);

  const Words fresh = body([](ui::Renderer& r) {
    {
      Canvas c(r);
      c.open();
      c.bind(kLed);
      c.close();
    }
    Canvas again(r);
    again.open();
    again.bind(kLed);
    again.close();
  });
  CHECK(count_op(fresh, eve::opcode::kBitmapHandle) == 2u);
}

TEST_CASE("canvas: open() as the first draw of a frame is SAVE_CONTEXT only, close() RESTORE_CONTEXT only") {
  const Words w = body([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.close();
  });
  CHECK(w == Words{dl::save_context(), dl::restore_context()});
}

TEST_CASE("canvas: open() after the Renderer drew ends its primitive and zeroes its origin; the Renderer's next call re-emits what end_raw made it forget") {
  const Words w = body([](ui::Renderer& r) {
    r.panel({10, 20, 5, 5}, Color::Text);  // COLOR_RGB, BEGIN(RECTS), two vertices: a primitive open
    r.text(700, 40, "A", Color::Text);     // END, COLOR_RGB.., BEGIN(BITMAPS), VERTEX_TRANSLATE_X, the glyph: origin moved
    Canvas c(r);
    const std::size_t before = r.words();
    c.open();
    CHECK(r.words() - before == 3u);       // END, SAVE_CONTEXT, VERTEX_TRANSLATE_X 0
    c.close();
    CHECK(r.words() - before == 4u);       // RESTORE_CONTEXT
    const std::size_t after = r.words();
    r.panel({1, 1, 2, 2}, Color::Text);
    CHECK(r.words() - after == 4u);        // COLOR_RGB, BEGIN(RECTS) and the two vertices: the caches were dropped, and the origin is the Renderer's again
  });
  CHECK(count_op(w, eve::opcode::kSaveContext) == 1u);
  CHECK(count_op(w, eve::opcode::kRestoreContext) == 1u);
}

TEST_CASE("canvas: the cost of each primitive in words, measured") {
  const auto section = [](auto fn) {
    return body([&](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      fn(c);
      c.close();
    }).size();
  };
  const std::size_t empty = section([](Canvas&) {});
  CHECK(empty == 2u);  // SAVE_CONTEXT, RESTORE_CONTEXT: the per-frame overhead of one section when it is the frame's first draw (kRawOverheadWords = 4 at most)

  // Each kind: the first of a run (with its COLOR_RGB and BEGIN, and for a point its POINT_SIZE, for a shape its LINE_WIDTH when it differs), and the next of the same style.
  const std::size_t sprite1 = section([](Canvas& c) { c.bind(kLed); c.sprite(0, 10, 10, kWhite); }) - empty;
  const std::size_t sprite2 = section([](Canvas& c) { c.bind(kLed); c.sprite(0, 10, 10, kWhite); c.sprite(1, 30, 10, kWhite); }) - empty - sprite1;
  CHECK(sprite1 == 6u + 1u + 1u + 1u);  // bind, COLOR_RGB, BEGIN, VERTEX2II
  CHECK(sprite2 == 1u);
  const std::size_t point1 = section([](Canvas& c) { c.point(160, 160, 40, kWhite); }) - empty;
  const std::size_t point2 = section([](Canvas& c) { c.point(160, 160, 40, kWhite); c.point(320, 160, 40, kWhite); }) - empty - point1;
  CHECK(point1 == 4u);  // COLOR_RGB, POINT_SIZE, BEGIN, VERTEX2F
  CHECK(point2 == 1u);
  const std::size_t line1 = section([](Canvas& c) { c.line(0, 0, 160, 160, 12, kWhite); }) - empty;
  const std::size_t line2 = section([](Canvas& c) { c.line(0, 0, 160, 160, 12, kWhite); c.line(0, 16, 160, 32, 12, kWhite); }) - empty - line1;
  CHECK(line1 == 5u);  // COLOR_RGB, LINE_WIDTH, BEGIN, two vertices
  CHECK(line2 == 2u);
  const std::size_t rect1 = section([](Canvas& c) { c.rect({10, 10, 20, 20}, kWhite); }) - empty;
  const std::size_t rect2 = section([](Canvas& c) { c.rect({10, 10, 20, 20}, kWhite); c.rect({40, 10, 20, 20}, kWhite); }) - empty - rect1;
  CHECK(rect1 == 4u);  // COLOR_RGB, BEGIN (LINE_WIDTH is 8 already), two vertices
  CHECK(rect2 == 2u);
  const std::size_t round1 = section([](Canvas& c) { c.round_rect({10, 10, 20, 20}, 64, kWhite); }) - empty;
  const std::size_t round2 = section([](Canvas& c) { c.round_rect({10, 10, 20, 20}, 64, kWhite); c.round_rect({40, 10, 20, 20}, 64, kWhite); }) - empty - round1;
  CHECK(round1 == 5u);  // COLOR_RGB, LINE_WIDTH, BEGIN, two vertices
  CHECK(round2 == 2u);
}

TEST_CASE("canvas: each state is emitted only when it differs, and every cache but the bound desc lasts one section") {
  const auto section = [](auto fn) {
    return body([&](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      fn(c);
      c.close();
    });
  };
  // COLOR_RGB: unknown at open (first call always emits), then when the Rgb differs.
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed); c.rect({8, 0, 4, 4}, kRed); }), eve::opcode::kColorRgb) == 1u);
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed); c.rect({8, 0, 4, 4}, kBlue); c.rect({16, 0, 4, 4}, kRed); }), eve::opcode::kColorRgb) == 3u);
  CHECK(count_op(body([](ui::Renderer& r) {
          Canvas c(r);
          c.open();
          c.rect({0, 0, 4, 4}, kRed);
          c.close();
          c.open();
          c.rect({0, 0, 4, 4}, kRed);  // a new section: RESTORE_CONTEXT took the colour back
          c.close();
        }),
                 eve::opcode::kColorRgb) == 2u);
  // COLOR_A: 255 at open (the Renderer never emits it), so only a different alpha does; going back to 255 emits again.
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed); c.rect({8, 0, 4, 4}, kRed, 255); }), eve::opcode::kColorA) == 0u);
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed, 128); c.rect({8, 0, 4, 4}, kRed, 128); c.rect({16, 0, 4, 4}, kRed, 255); }), eve::opcode::kColorA) == 2u);
  // LINE_WIDTH: 8 at open, which is rect()'s; a round_rect or a line changes it, and rect() puts it back.
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed); }), eve::opcode::kLineWidth) == 0u);
  CHECK(count_op(section([](Canvas& c) { c.round_rect({0, 0, 40, 40}, 48, kRed); c.round_rect({50, 0, 40, 40}, 48, kRed); c.rect({0, 50, 4, 4}, kRed); }), eve::opcode::kLineWidth) == 2u);
  const Words lw = section([](Canvas& c) { c.line(0, 0, 160, 0, 8, kRed); });  // a line of width 8 is the section's own
  CHECK(count_op(lw, eve::opcode::kLineWidth) == 0u);
  // POINT_SIZE: unknown at open.
  CHECK(count_op(section([](Canvas& c) { c.point(0, 0, 16, kRed); c.point(32, 0, 16, kRed); c.point(64, 0, 32, kRed); }), eve::opcode::kPointSize) == 2u);
  // The primitive: BEGIN when the next call needs another one, and none while it is the same.
  CHECK(count_op(section([](Canvas& c) { c.rect({0, 0, 4, 4}, kRed); c.rect({8, 0, 4, 4}, kRed); c.point(0, 0, 16, kRed); c.point(16, 0, 16, kRed); c.rect({0, 8, 4, 4}, kRed); }), eve::opcode::kBegin) == 3u);
  // The origin: moved by the first sprite past x = 511 only, 256..319 px before it and in steps of 64; a sprite near by keeps it.
  const Words origin = section([](Canvas& c) { c.bind(kLed); c.sprite(0, 600, 10, kWhite); c.sprite(0, 700, 10, kWhite); c.sprite(0, 640, 10, kWhite); });
  CHECK(count_op(origin, eve::opcode::kVertexTranslateX) == 1u);
  CHECK(origin[1 + 6 + 1 + 1] == dl::vertex_translate_x(320 * 16));  // 600 - 256 = 344, down to a multiple of 64
  CHECK(origin[1 + 6 + 1 + 1 + 1] == dl::vertex2ii(600 - 320, 10, 0, 0));
  // Scissor: when it differs, and RESTORE_CONTEXT takes it back at close().
  const Words sc = section([](Canvas& c) { c.scissor({10, 20, 100, 50}); c.scissor({10, 20, 100, 50}); c.scissor({10, 20, 100, 60}); c.scissor_reset(); c.scissor_reset(); });
  CHECK(sc == Words{dl::save_context(), dl::scissor_xy(10, 20), dl::scissor_size(100, 50), dl::scissor_size(100, 60), dl::scissor_xy(0, 0), dl::scissor_size(2048, 2048),
                    dl::restore_context()});
  // The bound desc: the section's (see the rebind tests above).
}

TEST_CASE("canvas: a sprite is one VERTEX2II, or BITMAP_HANDLE + CELL + VERTEX2F off the 9-bit window or after use_translate(false)") {
  const auto section = [](bool translate, auto fn) {
    return body([&](ui::Renderer& r) {
      Canvas c(r);
      c.use_translate(translate);
      c.open();
      c.bind(kLed);
      fn(c);
      c.close();
    });
  };
  const std::size_t head = 1 + 6;  // SAVE, the bind
  const Words near = section(true, [](Canvas& c) { c.sprite(5, 100, 50, kWhite); });
  CHECK(near[head + 2] == dl::vertex2ii(100, 50, 0, 5));
  const Words neg = section(true, [](Canvas& c) { c.sprite(5, -4, 50, kWhite); });
  CHECK(neg[head + 2] == dl::cell(5));
  CHECK(neg[head + 3] == dl::vertex2f(-64, 800));
  CHECK(count_op(neg, eve::opcode::kBitmapHandle) == 1u);  // the bind's: the section's BITMAP_HANDLE is still the app's
  const Words low = section(true, [](Canvas& c) { c.sprite(0, 100, 600, kWhite); });
  CHECK(low[head + 3] == dl::vertex2f(1600, 9600));
  // Plan B: x past 511 without a translation: VERTEX2F at its own screen coordinate, the same cell word once for a run of the same cell.
  const Words plan_b = section(false, [](Canvas& c) { c.sprite(2, 600, 10, kWhite); c.sprite(2, 640, 10, kWhite); c.sprite(3, 660, 10, kWhite); });
  CHECK(count_op(plan_b, eve::opcode::kVertexTranslateX) == 0u);
  CHECK(count_op(plan_b, eve::opcode::kCell) == 2u);
  CHECK(count_op(plan_b, eve::opcode::kBitmapHandle) == 1u);  // set by the bind, known, not repeated
  CHECK(plan_b[head + 3] == dl::vertex2f(600 * 16, 160));
  // The cell is clamped to 127.
  const Words clamp = section(true, [](Canvas& c) { c.sprite(900, 10, 10, kWhite); });
  CHECK(clamp[head + 2] == dl::vertex2ii(10, 10, 0, 127));
}

// ---- The boundary with the Renderer ----------------------------------------------------------------------------------------------------------

TEST_CASE("canvas: Renderer::text() after a section that moved the Canvas's origin lands where it should, and so do the shapes after the sprite") {
  Panel reference;
  CHECK(reference.draw([](ui::Renderer& r) { r.text(620, 200, "HELLO", Color::Text); }));
  const std::vector<std::uint8_t> text_alone = reference.image();

  Panel panel;
  panel.put(kLed.ram_g, std::vector<std::uint8_t>(256, 255));
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    c.sprite(0, 600, 100, kRed);  // past x = 511: the Canvas moves its origin
    c.close();
    r.text(620, 200, "HELLO", Color::Text);  // the Renderer moves its own
  }));
  CHECK(panel.pixel(608, 108) == ((std::uint32_t{kRed.r} << 16) | (std::uint32_t{kRed.g} << 8) | kRed.b));  // the sprite is a 16 x 16 box at (600, 100)
  CHECK(panel.pixel(599, 108) == pane());
  CHECK(panel.pixel(616, 108) == pane());
  // The text's rows are the reference's, pixel for pixel (the sprite is far above them).
  const std::vector<std::uint8_t> now = panel.image();
  for (int y = 190; y < 230; ++y) {
    for (int x = 600; x < 700; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
      CHECK(now[i] == text_alone[i]);
      CHECK(now[i + 1] == text_alone[i + 1]);
      CHECK(now[i + 2] == text_alone[i + 2]);
    }
  }

  // Shapes drawn after a sprite that moved the origin land at their screen coordinates: the same picture as with the shapes drawn first.
  const auto shapes = [](Canvas& c) {
    c.rect({700, 300, 30, 12}, kBlue);
    c.round_rect({700, 330, 60, 30}, 96, kRed);
    c.point(740 * 16, 400 * 16, 5 * 16, kWhite);
    c.line(610 * 16, 300 * 16, 690 * 16, 380 * 16, 24, kWhite);
  };
  Panel first;
  first.put(kLed.ram_g, std::vector<std::uint8_t>(256, 255));
  CHECK(first.draw([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    shapes(c);
    c.sprite(0, 600, 100, kRed);
    c.close();
  }));
  Panel second;
  second.put(kLed.ram_g, std::vector<std::uint8_t>(256, 255));
  CHECK(second.draw([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    c.sprite(0, 600, 100, kRed);
    shapes(c);
    c.close();
  }));
  CHECK(first.image() == second.image());
  CHECK(second.pixel(715, 306) == ((std::uint32_t{kBlue.r} << 16) | (std::uint32_t{kBlue.g} << 8) | kBlue.b));
  CHECK(second.pixel(740, 400) == 0xFFFFFFu);
}

// ---- Checks ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("canvas: bitmap_desc_valid") {
  using ui::plot::bitmap_desc_valid;
  CHECK(bitmap_desc_valid(kBand));
  CHECK(bitmap_desc_valid(kLed));
  // REPEAT needs a power-of-two cell: 100 px is not one (the row is padded to 64 bytes of L4 = 128 px, which does not make it one either).
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L4, 50, 100, 1, 800, 1, Wrap::Repeat, Wrap::Border}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L4, 64, 100, 1, 800, 1, Wrap::Repeat, Wrap::Border}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 100, 100, 1, 800, 1, Wrap::Repeat, Wrap::Border}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 100, 16, 100, Wrap::Border, Wrap::Repeat}));  // y: 100 rows
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 128, 16, 128, Wrap::Border, Wrap::Repeat}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 64, 16, 1, 800, 1, Wrap::Repeat, Wrap::Border}));  // the chip wraps at the row (64 px), not at the cell (16)
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 4096, 800, 8, 800, 8}));  // the stride register is 12 bits
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 4095, 800, 8, 800, 8}));
  CHECK_FALSE(bitmap_desc_valid({0, BitmapFormat::L8, 16, 16, 16, 16, 16}));  // outside the app's region (the kit's tiles)
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::Paletted565, 16, 16, 16, 16, 16}));
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L4, 8, 16, 16, 16, 16}));  // 8 bytes of L4 are 16 px
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L1, 2, 16, 16, 16, 16}));
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L2, 4, 16, 16, 16, 16}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp + 2, BitmapFormat::L8, 16, 16, 16, 16, 16}));  // the source is on a word
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 16, 0, 16}));       // a drawn size of 0 means 2048 to the chip
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 16, 2048, 16}));
  CHECK(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 16, 2047, 16}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 16, 16, 0, 16, 16}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 0, 16, 16, 16, 16}));
  CHECK_FALSE(bitmap_desc_valid({kRamGApp, BitmapFormat::L8, 8, 16, 16, 16, 16}));  // the row holds 8 px, the cell is 16 wide
  CHECK_FALSE(bitmap_desc_valid({hal::kRamGBytes - 8, BitmapFormat::L8, 16, 16, 16, 16, 16}));  // the first cell leaves RAM_G
  CHECK(ui::plot::bitmap_pixels_per_byte(BitmapFormat::Paletted565) == 0u);
}

TEST_CASE("canvas: a Renderer call inside a section, a call outside one, nesting and an invalid desc trip the check") {
  CHECK(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      c.rect({0, 0, 4, 4}, kRed);
      r.panel({10, 10, 4, 4}, Color::Text);  // a Renderer call between two Canvas calls
      c.rect({8, 0, 4, 4}, kRed);
    });
  }));
  CHECK(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.rect({0, 0, 4, 4}, kRed);  // outside a section
    });
  }));
  CHECK(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      r.text(10, 10, "X", Color::Text);
      c.close();  // the Renderer moved r.words() before the close too
    });
  }));
  CHECK(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      c.open();  // sections do not nest
    });
  }));
  CHECK(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      c.bind({kRamGApp, BitmapFormat::Paletted565, 16, 16, 16, 16, 16});
    });
  }));
  // And a clean use does not.
  CHECK_FALSE(aborts([] {
    (void)body([](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      c.bind(kLed);
      c.sprite(0, 10, 10, kWhite);
      c.close();
      r.panel({10, 10, 4, 4}, Color::Text);
    });
  }));
}

// ---- Pixels ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("canvas pixels: an L4 2 x 2 cell tinted at alpha 128 gives the simulator's blend") {
  Panel panel;
  panel.put(kTiny4.ram_g, {0x0F, 0x84});  // row 0: 0, 15; row 1: 8, 4 (alpha = nibble * 17)
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kTiny4);
    c.sprite(0, 100, 50, kRed, 128);
    c.close();
  }));
  const unsigned sample[2][2] = {{0, 255}, {136, 68}};
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      CAPTURE(x);
      CAPTURE(y);
      const unsigned a = mul255(sample[y][x], 128);
      CHECK(panel.pixel(100 + x, 50 + y) == (a == 0 ? pane() : over(kRed, pane(), a)));
    }
  }
  CHECK(panel.pixel(102, 50) == pane());  // the box is 2 x 2
  CHECK(panel.pixel(100, 52) == pane());
}

TEST_CASE("canvas pixels: an L8 cell, the second cell of a bitmap, tinted at full alpha") {
  Panel panel;
  // Cell 0 is all zero; cell 1 (8 bytes on) has the 4 x 2 values below.
  const std::uint8_t values[8] = {0, 64, 128, 255, 255, 200, 100, 1};
  std::vector<std::uint8_t> bytes(16, 0);
  for (int i = 0; i < 8; ++i) {
    bytes[8 + static_cast<std::size_t>(i)] = values[i];
  }
  panel.put(kTiny8.ram_g, bytes);
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kTiny8);
    c.sprite(1, 300, 200, kBlue);
    c.sprite(0, 320, 200, kBlue);  // the empty cell: nothing
    c.close();
  }));
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      CAPTURE(x);
      CAPTURE(y);
      const unsigned a = values[y * 4 + x];
      CHECK(panel.pixel(300 + x, 200 + y) == (a == 0 ? pane() : over(kBlue, pane(), a)));
    }
  }
  CHECK(panel.pixel(321, 200) == pane());
}

TEST_CASE("canvas pixels: an 800-wide REPEAT band of a 256 tile, and the bind after it leaves no oversized box") {
  Panel panel;
  std::vector<std::uint8_t> tile(128);
  for (std::size_t i = 0; i < tile.size(); ++i) {
    tile[i] = static_cast<std::uint8_t>(((2u * i) % 16u) << 4 | ((2u * i + 1u) % 16u));  // pixel p has the alpha (p % 16) * 17
  }
  panel.put(kBand.ram_g, tile);
  panel.put(kLed.ram_g, std::vector<std::uint8_t>(256, 255));
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kBand);
    c.sprite(0, 0, 100, kWhite);
    c.close();
  }));
  int wrong = 0;
  for (int x = 0; x < 800; ++x) {
    const unsigned a = static_cast<unsigned>(x % 16) * 17u;
    wrong += panel.pixel(x, 100) != (a == 0 ? pane() : over(kWhite, pane(), a)) ? 1 : 0;
  }
  CHECK(wrong == 0);
  CHECK(panel.pixel(10, 99) == pane());
  CHECK(panel.pixel(10, 101) == pane());
  CHECK(panel.raster.last_frame_peak_line_px() == 800u);  // one 800 px box on the line (N9)
  CHECK(panel.raster.last_frame_peak_line_prims() == 1u);

  // The LED drawn after the band, rebound: a 16 px box on the line, not the 528 px (16 + the band's 512) a bind that left SIZE_H alone gets.
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kBand);
    c.sprite(0, 0, 100, kWhite);
    c.bind(kLed);
    c.sprite(0, 50, 300, kWhite);
    c.close();
  }));
  CHECK(panel.raster.last_frame_peak_line_px() == 800u);  // the band's line
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kBand);
    c.bind(kLed);
    c.sprite(0, 50, 300, kWhite);
    c.close();
  }));
  CHECK(panel.raster.last_frame_peak_line_px() == 16u);
  CHECK(panel.pixel(58, 308) == 0xFFFFFFu);
}

TEST_CASE("canvas pixels: rect() is the Renderer's box, pixel for pixel") {
  const Rect rects[] = {{10, 20, 30, 40}, {200, 200, 1, 1}, {300, 100, 1, 7}, {400, 50, 9, 1}, {790, 470, 10, 10}, {5, 5, 2, 2}};
  Panel with_renderer;
  CHECK(with_renderer.draw([&](ui::Renderer& r) {
    for (const Rect& rc : rects) {
      r.panel(rc, Color::Sel);
    }
  }));
  Panel with_canvas;
  const Rgb sel = amiga().rgb(Color::Sel);
  CHECK(with_canvas.draw([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    for (const Rect& rc : rects) {
      c.rect(rc, sel);
    }
    c.close();
  }));
  CHECK(with_canvas.image() == with_renderer.image());
}

TEST_CASE("canvas pixels: round_rect covers exactly its rect, also with a radius clamped") {
  const Rgb ink = kWhite;
  const auto drawn = [&](const Rect& rc, unsigned radius_q4) {
    auto panel = std::make_unique<Panel>();
    CHECK(panel->draw([&](ui::Renderer& r) {
      Canvas c(r);
      c.open();
      c.round_rect(rc, radius_q4, ink);
      c.close();
    }));
    return panel;
  };
  const auto bbox_ok = [&](const Panel& p, const Rect& rc) {
    int x0 = 800, y0 = 480, x1 = -1, y1 = -1;
    for (int y = 0; y < 480; ++y) {
      for (int x = 0; x < 800; ++x) {
        if (p.pixel(x, y) != pane()) {
          x0 = x < x0 ? x : x0;
          y0 = y < y0 ? y : y0;
          x1 = x > x1 ? x : x1;
          y1 = y > y1 ? y : y1;
        }
      }
    }
    return x0 == rc.x && y0 == rc.y && x1 == rc.x + rc.w - 1 && y1 == rc.y + rc.h - 1;
  };
  // The simulator draws RECTS as the box they grow to (sim/README.md: it does not round the corners), so what it can show of round_rect is its extent: the rect's
  // pixels and nothing outside, solid along every edge and inside. The corner's curve is the chip's (SU1).
  const Rect big{100, 100, 120, 60};
  const auto round = drawn(big, 8 * 16);  // 8 px corners
  CHECK(bbox_ok(*round, big));
  CHECK(round->pixel(160, 100) == 0xFFFFFFu);       // an edge's middle is solid
  CHECK(round->pixel(100, 130) == 0xFFFFFFu);
  CHECK(round->pixel(160, 130) == 0xFFFFFFu);       // the inside
  CHECK(round->pixel(219, 130) == 0xFFFFFFu);
  CHECK(round->pixel(160, 159) == 0xFFFFFFu);

  // A radius over half the shorter side is clamped to it: the same words as that radius, so the same picture, and still inside the rect.
  const Rect pill{300, 100, 100, 20};
  const auto clamped = drawn(pill, 4000);
  const auto exact = drawn(pill, 8u * 20u);
  CHECK(bbox_ok(*clamped, pill));
  CHECK(clamped->image() == exact->image());
  CHECK(clamped->pixel(350, 110) == 0xFFFFFFu);
  const Words lw_clamped = body([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.round_rect(pill, 4000, ink);
    c.close();
  });
  const Words lw_exact = body([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.round_rect(pill, 160, ink);
    c.close();
  });
  CHECK(lw_clamped == lw_exact);
  CHECK(count_op(lw_clamped, eve::opcode::kLineWidth) == 1u);
  // The words in order: the corner radius as LINE_WIDTH, then the two vertices, the rect's corners moved in by it.
  std::size_t at_lw = 0;
  while (at_lw < lw_clamped.size() && (lw_clamped[at_lw] >> 24) != eve::opcode::kLineWidth) {
    ++at_lw;
  }
  std::size_t at_v = 0;
  while (at_v < lw_clamped.size() && (lw_clamped[at_v] >> 30) != eve::opcode::kVertex2fTag) {
    ++at_v;
  }
  REQUIRE(at_lw + 2 < lw_clamped.size());
  REQUIRE(at_v + 1 < lw_clamped.size());
  CHECK(lw_clamped[at_lw] == dl::line_width(160));
  CHECK(lw_clamped[at_v] == dl::vertex2f(300 * 16 + 160, 100 * 16 + 160));
  CHECK(lw_clamped[at_v + 1] == dl::vertex2f(400 * 16 - 160, 120 * 16 - 160));
  // A radius below half a pixel is rect()'s corner (8): the same picture as rect().
  const Rect small{500, 100, 30, 30};
  const auto tiny = drawn(small, 0);
  Panel plain;
  CHECK(plain.draw([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.rect(small, ink);
    c.close();
  }));
  CHECK(tiny->image() == plain.image());
  CHECK(bbox_ok(plain, small));
}

TEST_CASE("canvas pixels: a section that ended at alpha 128 leaves the next draws their own pixels (meters stand for the wave view, which needs a built peak cache)") {
  ui::plot::DbScale scale;
  scale.init(-60.0f, 0.0f);
  ui::plot::MeterState state;
  state.update({0.5f, 0.3f});
  const Rect bar[1] = {{600, 40, 12, 120}};
  const auto meter = [&](ui::Renderer& r) { ui::plot::draw_meters(r, bar, &state, 1, ui::Axis::Vertical, scale, ui::plot::MeterStyle{}); };

  Panel alone;
  CHECK(alone.draw([&](ui::Renderer& r) { meter(r); }));
  Panel after;
  after.put(kLed.ram_g, std::vector<std::uint8_t>(256, 255));
  CHECK(after.draw([&](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.bind(kLed);
    c.rect({300, 300, 40, 40}, kRed, 128);  // the section ends with COLOR_A 128, a different line width and a moved origin
    c.round_rect({400, 300, 40, 40}, 160, kRed, 128);
    c.sprite(0, 700, 300, kRed, 128);
    c.close();
    meter(r);
  }));
  const std::vector<std::uint8_t> a = alone.image(), b = after.image();
  int differing = 0;
  for (int y = 30; y < 170; ++y) {
    for (int x = 590; x < 625; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
      differing += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ? 1 : 0;
    }
  }
  CHECK(differing == 0);
  // And the shapes the section drew are there, translucent.
  CHECK(after.pixel(320, 320) == over(kRed, pane(), 128));
}

TEST_CASE("canvas pixels: points, lines and a scissor") {
  Panel panel;
  CHECK(panel.draw([](ui::Renderer& r) {
    Canvas c(r);
    c.open();
    c.point(100 * 16 + 8, 100 * 16 + 8, 6 * 16, kWhite);             // a disc of radius 6 on the centre of pixel (100, 100)
    c.line(200 * 16, 100 * 16, 260 * 16, 100 * 16, 2 * 16, kRed);    // a horizontal line, 4 px thick, round ends
    c.scissor({300, 90, 20, 20});
    c.rect({280, 80, 60, 40}, kBlue);                                // clipped to the scissor
    c.scissor_reset();
    c.rect({400, 80, 4, 4}, kBlue);                                  // after the reset: not clipped
    c.close();
  }));
  CHECK(panel.pixel(100, 100) == 0xFFFFFFu);
  CHECK(panel.pixel(105, 100) == 0xFFFFFFu);
  CHECK(panel.pixel(100, 108) == pane());
  CHECK(panel.pixel(230, 100) == ((std::uint32_t{kRed.r} << 16) | (std::uint32_t{kRed.g} << 8) | kRed.b));
  CHECK(panel.pixel(230, 110) == pane());
  const std::uint32_t blue = (std::uint32_t{kBlue.r} << 16) | (std::uint32_t{kBlue.g} << 8) | kBlue.b;
  CHECK(panel.pixel(310, 100) == blue);
  CHECK(panel.pixel(290, 100) == pane());
  CHECK(panel.pixel(330, 100) == pane());
  CHECK(panel.pixel(401, 81) == blue);
}
