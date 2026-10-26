// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every expected word below was computed by hand from the bit layouts of the FT81X Series Programmers
// Guide v1.2 (BRT_000031), chapter 4, and cross-checked with an independent field-table script. They are
// literals on purpose: the encoders must never be tested against themselves.
#include <doctest/doctest.h>

#include <o3sdk/eve/dl.h>

#include <cstdint>
#include <ios>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::BitmapFormat;
using o3sdk::eve::BlendFactor;
using o3sdk::eve::Filter;
using o3sdk::eve::Primitive;
using o3sdk::eve::Wrap;

namespace {

constexpr std::uint32_t kAll = 0xFFFFFFFFu;
// (an array, not a braced list: int32_t is long on the ARM target and INT32_MAX is a long, -1 an int)
constexpr std::int32_t kExtremes[] = {INT32_MIN, INT32_MAX, -1, 0};

#define CHECK_WORD(expr, want)                                                                   \
  CHECK_MESSAGE((expr) == static_cast<std::uint32_t>(want),                                      \
                #expr << " gave 0x" << std::hex << (expr) << ", want 0x" << std::hex             \
                      << static_cast<std::uint32_t>(want))

}  // namespace

// The encoders are usable in constant expressions (they can be baked into tables at compile time).
static_assert(dl::display() == 0x00000000u, "DISPLAY");
static_assert(dl::color_rgb(255, 100, 50) == 0x04FF6432u, "COLOR_RGB guide example");
static_assert(dl::vertex2ii(50, 65, 31, 0x45) == 0x86441FC5u, "VERTEX2II guide example");
static_assert(dl::blend_func(BlendFactor::SrcAlpha, BlendFactor::Zero) == 0x0B000010u, "BLEND_FUNC guide example");
static_assert(dl::point_size(160) == 0x0D0000A0u, "POINT_SIZE guide example");

TEST_CASE("dl: DISPLAY, END, SAVE_CONTEXT, RESTORE_CONTEXT carry only their opcode") {
  CHECK_WORD(dl::display(), 0x00000000);  // §4.29 p.127
  CHECK_WORD(dl::end(), 0x21000000);      // §4.30 p.128
  CHECK_WORD(dl::save_context(), 0x22000000);     // §4.39 p.136
  CHECK_WORD(dl::restore_context(), 0x23000000);  // §4.37 p.134
}

TEST_CASE("dl: CLEAR_COLOR_RGB and COLOR_RGB put red/green/blue in [23:16]/[15:8]/[7:0]") {
  // Distinct bytes so a swapped channel cannot hide. The guide's diagrams (pp.121, 126) label the lower
  // two fields Blue/Green, which contradicts CMD_FGCOLOR (§5.30 p.183) and firmware FT8xx.h; see dl.h.
  CHECK_WORD(dl::clear_color_rgb(0x12, 0x34, 0x56), 0x02123456);  // §4.23 p.121
  CHECK_WORD(dl::clear_color_rgb(0, 0, 255), 0x020000FF);         // guide example "bright blue"
  CHECK_WORD(dl::color_rgb(0x12, 0x34, 0x56), 0x04123456);        // §4.28 p.126
  CHECK_WORD(dl::color_rgb(255, 100, 50), 0x04FF6432);            // guide example
}

TEST_CASE("dl: CLEAR flags are c [2], s [1], t [0]") {
  CHECK_WORD(dl::clear(true, false, false), 0x26000004);  // §4.21 p.118
  CHECK_WORD(dl::clear(false, true, false), 0x26000002);
  CHECK_WORD(dl::clear(false, false, true), 0x26000001);
  CHECK_WORD(dl::clear(true, true, true), 0x26000007);
  CHECK_WORD(dl::clear(false, false, false), 0x26000000);
}

TEST_CASE("dl: COLOR_A is the low byte") {
  CHECK_WORD(dl::color_a(128), 0x10000080);  // §4.26 p.124
  CHECK_WORD(dl::color_a(255), 0x100000FF);
  CHECK_WORD(dl::color_a(0), 0x10000000);
}

TEST_CASE("dl: BEGIN primitive values from Table 6") {
  CHECK_WORD(dl::begin(Primitive::Bitmaps), 0x1F000001);  // §4.5 p.94
  CHECK_WORD(dl::begin(Primitive::Lines), 0x1F000003);
  CHECK_WORD(dl::begin(Primitive::Rects), 0x1F000009);
  CHECK_WORD(dl::begin(Primitive::Points), 0x1F000002);
  CHECK_WORD(dl::begin(Primitive::LineStrip), 0x1F000004);
  CHECK_WORD(dl::begin(Primitive::EdgeStripA), 0x1F000007);
  CHECK_WORD(dl::begin(Primitive::EdgeStripB), 0x1F000008);
  CHECK_WORD(dl::begin(static_cast<Primitive>(5)), 0x1F000005);  // EDGE_STRIP_R: outside the subset, still encodable
  CHECK_WORD(dl::begin(static_cast<Primitive>(6)), 0x1F000006);  // EDGE_STRIP_L
}

TEST_CASE("dl: VERTEX2F is two signed 15-bit fields under tag 0b01") {
  CHECK_WORD(dl::vertex2f(160, 480), 0x405001E0);  // §4.47 p.145; guide LINE_WIDTH example (16*10, 16*30)
  CHECK_WORD(dl::vertex2f(0, 0), 0x40000000);
  CHECK_WORD(dl::vertex2f(-1, -1), 0x7FFFFFFF);
  CHECK_WORD(dl::vertex2f(-16, 16), 0x7FF80010);
  CHECK_WORD(dl::vertex2f(-16384, 16383), 0x60003FFF);  // both ends of the 15-bit range
  CHECK_WORD(dl::vertex2f(-1, 0), 0x7FFF8000);          // x only touches [29:15]
  CHECK_WORD(dl::vertex2f(0, -1), 0x40007FFF);          // y only touches [14:0]
}

TEST_CASE("dl: VERTEX2II fields x/y/handle/cell under tag 0b10") {
  CHECK_WORD(dl::vertex2ii(50, 65, 31, 0x45), 0x86441FC5);  // §4.48 p.146; guide BEGIN example
  CHECK_WORD(dl::vertex2ii(1, 2, 3, 4), 0x80202184);
  CHECK_WORD(dl::vertex2ii(511, 511, 31, 127), 0xBFFFFFFF);
  CHECK_WORD(dl::vertex2ii(kAll, 0, 0, 0), 0xBFE00000);  // each field isolated: x [29:21]
  CHECK_WORD(dl::vertex2ii(0, kAll, 0, 0), 0x801FF000);  // y [20:12]
  CHECK_WORD(dl::vertex2ii(0, 0, kAll, 0), 0x80000F80);  // handle [11:7]
  CHECK_WORD(dl::vertex2ii(0, 0, 0, kAll), 0x8000007F);  // cell [6:0]
}

TEST_CASE("dl: VERTEX_FORMAT frac [2:0]") {
  CHECK_WORD(dl::vertex_format(4), 0x27000004);  // §4.49 p.147, the initial value
  CHECK_WORD(dl::vertex_format(0), 0x27000000);
  CHECK_WORD(dl::vertex_format(kAll), 0x27000007);
}

TEST_CASE("dl: LINE_WIDTH width [11:0]") {
  CHECK_WORD(dl::line_width(80), 0x0E000050);  // §4.32 p.130; guide example
  CHECK_WORD(dl::line_width(16), 0x0E000010);
  CHECK_WORD(dl::line_width(4095), 0x0E000FFF);
  CHECK_WORD(dl::line_width(kAll), 0x0E000FFF);
}

TEST_CASE("dl: SCISSOR_XY x [21:11], y [10:0]; SCISSOR_SIZE width [23:12], height [11:0]") {
  CHECK_WORD(dl::scissor_xy(40, 30), 0x1B01401E);  // §4.41 p.138; guide example
  CHECK_WORD(dl::scissor_xy(2047, 2047), 0x1B3FFFFF);
  CHECK_WORD(dl::scissor_xy(kAll, 0), 0x1B3FF800);
  CHECK_WORD(dl::scissor_xy(0, kAll), 0x1B0007FF);
  CHECK_WORD(dl::scissor_size(80, 60), 0x1C05003C);  // §4.40 p.137; guide example
  CHECK_WORD(dl::scissor_size(2048, 2048), 0x1C800800);
  CHECK_WORD(dl::scissor_size(kAll, 0), 0x1CFFF000);
  CHECK_WORD(dl::scissor_size(0, kAll), 0x1C000FFF);
}

TEST_CASE("dl: POINT_SIZE size [12:0], the radius in 1/16 pixel") {
  // §4.36 p.133; the expected words are the vendor library's FT81x macro evaluated
  // by a C program, and equal the hand-derived 0x0D << 24 | size.
  CHECK_WORD(dl::point_size(160), 0x0D0000A0);   // guide example: "a width of 160, for a 10 pixel radius"
  CHECK_WORD(dl::point_size(760), 0x0D0002F8);   // guide example in §4.42 p.139 (STENCIL_FUNC)
  CHECK_WORD(dl::point_size(16), 0x0D000010);    // the initial value
  CHECK_WORD(dl::point_size(0), 0x0D000000);
  CHECK_WORD(dl::point_size(8191), 0x0D001FFF);  // the top of the valid range, 511 px
  CHECK_WORD(dl::point_size(kAll), 0x0D001FFF);  // masked to 13 bits: nothing leaks into the opcode
}

TEST_CASE("dl: BLEND_FUNC src [5:3], dst [2:0]") {
  // §4.18 p.114, Table 8: ZERO 0, ONE 1, SRC_ALPHA 2, DST_ALPHA 3, ONE_MINUS_SRC_ALPHA 4, ONE_MINUS_DST_ALPHA 5.
  CHECK_WORD(dl::blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha), 0x0B000014);  // the initial value
  CHECK_WORD(dl::blend_func(BlendFactor::SrcAlpha, BlendFactor::Zero), 0x0B000010);  // guide example, p.115
  CHECK_WORD(dl::blend_func(BlendFactor::Zero, BlendFactor::SrcAlpha), 0x0B000002);  // guide example, p.115
  CHECK_WORD(dl::blend_func(BlendFactor::One, BlendFactor::One), 0x0B000009);        // additive
  CHECK_WORD(dl::blend_func(BlendFactor::One, BlendFactor::Zero), 0x0B000008);
  CHECK_WORD(dl::blend_func(BlendFactor::Zero, BlendFactor::Zero), 0x0B000000);
  CHECK_WORD(dl::blend_func(BlendFactor::DstAlpha, BlendFactor::OneMinusDstAlpha), 0x0B00001D);
  CHECK_WORD(dl::blend_func(BlendFactor::OneMinusDstAlpha, BlendFactor::DstAlpha), 0x0B00002B);
  // Each field isolated, and masked to its 3 bits.
  CHECK_WORD(dl::blend_func(static_cast<BlendFactor>(kAll), BlendFactor::Zero), 0x0B000038);
  CHECK_WORD(dl::blend_func(BlendFactor::Zero, static_cast<BlendFactor>(kAll)), 0x0B000007);
}

TEST_CASE("dl: VERTEX_TRANSLATE_X / _Y offset [16:0], a signed 17-bit value in 1/16 pixel") {
  // §4.50 p.148 and §4.51 p.149 (opcodes 0x2B, 0x2C); the vendor library's macros evaluated as above.
  CHECK_WORD(dl::vertex_translate_x(0), 0x2B000000);
  CHECK_WORD(dl::vertex_translate_x(160), 0x2B0000A0);  // 10 px
  CHECK_WORD(dl::vertex_translate_x(1), 0x2B000001);
  CHECK_WORD(dl::vertex_translate_x(-1), 0x2B01FFFF);
  CHECK_WORD(dl::vertex_translate_x(-16), 0x2B01FFF0);  // -1 px
  CHECK_WORD(dl::vertex_translate_x(65535), 0x2B00FFFF);   // top of the 17-bit signed range
  CHECK_WORD(dl::vertex_translate_x(-65536), 0x2B010000);  // bottom of it
  CHECK_WORD(dl::vertex_translate_x(INT32_MAX), 0x2B01FFFF);  // masked to 17 bits
  CHECK_WORD(dl::vertex_translate_y(0), 0x2C000000);
  CHECK_WORD(dl::vertex_translate_y(160), 0x2C0000A0);
  CHECK_WORD(dl::vertex_translate_y(-1), 0x2C01FFFF);
  CHECK_WORD(dl::vertex_translate_y(-16), 0x2C01FFF0);
  CHECK_WORD(dl::vertex_translate_y(65535), 0x2C00FFFF);
  CHECK_WORD(dl::vertex_translate_y(-65536), 0x2C010000);
  CHECK_WORD(dl::vertex_translate_y(INT32_MIN), 0x2C000000);  // 2^31 has no bits in the low 17
}

TEST_CASE("dl: BITMAP_HANDLE handle [4:0], CELL cell [6:0]") {
  CHECK_WORD(dl::bitmap_handle(31), 0x0500001F);  // §4.6 p.96
  CHECK_WORD(dl::bitmap_handle(1), 0x05000001);
  CHECK_WORD(dl::bitmap_handle(kAll), 0x0500001F);
  CHECK_WORD(dl::cell(0x47), 0x06000047);  // §4.20 p.117
  CHECK_WORD(dl::cell(127), 0x0600007F);
  CHECK_WORD(dl::cell(kAll), 0x0600007F);
}

TEST_CASE("dl: BITMAP_SOURCE addr [21:0]") {
  CHECK_WORD(dl::bitmap_source(0x12345), 0x01012345);  // §4.11 p.106
  CHECK_WORD(dl::bitmap_source(128 * 16 + 32), 0x01000820);  // guide example
  CHECK_WORD(dl::bitmap_source(0x3FFFFF), 0x013FFFFF);
  CHECK_WORD(dl::bitmap_source(kAll), 0x013FFFFF);
}

TEST_CASE("dl: BITMAP_LAYOUT format [23:19], linestride [18:9], height [8:0]") {
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L8, 800, 480), 0x071E41E0);  // §4.7 p.97
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L1, 16, 8), 0x07082008);
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(7), 128, 64), 0x07390040);  // guide example, RGB565
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(0), 576, 480), 0x070481E0);  // guide example, ARGB1555
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(kAll), kAll, kAll), 0x07FFFFFF);
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(kAll), 0, 0), 0x07F80000);
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L1, 0, 0), 0x07080000);  // Table 7: L1 = 1, L4 = 2, L8 = 3
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L4, 0, 0), 0x07100000);
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L8, 0, 0), 0x07180000);
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(0), kAll, 0), 0x0707FE00);
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(0), 0, kAll), 0x070001FF);
}

TEST_CASE("dl: BITMAP_LAYOUT_H linestride [3:2], height [1:0]") {
  CHECK_WORD(dl::bitmap_layout_h(1, 0), 0x28000004);  // §4.8 p.103; guide example (a 1600-byte line)
  CHECK_WORD(dl::bitmap_layout_h(0, 2), 0x28000002);
  CHECK_WORD(dl::bitmap_layout_h(3, 3), 0x2800000F);
  CHECK_WORD(dl::bitmap_layout_h(kAll, kAll), 0x2800000F);
}

TEST_CASE("dl: BITMAP_SIZE filter [20], wrapx [19], wrapy [18], width [17:9], height [8:0]") {
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 64, 64), 0x08008040);  // §4.9 p.103
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 288, 480), 0x080241E0);  // guide example
  CHECK_WORD(dl::bitmap_size(Filter::Bilinear, Wrap::Border, Wrap::Repeat, 288, 480), 0x081641E0);
  CHECK_WORD(dl::bitmap_size(Filter::Bilinear, Wrap::Repeat, Wrap::Repeat, 511, 511), 0x081FFFFF);
  CHECK_WORD(dl::bitmap_size(Filter::Bilinear, Wrap::Border, Wrap::Border, 0, 0), 0x08100000);
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Repeat, Wrap::Border, 0, 0), 0x08080000);
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Repeat, 0, 0), 0x08040000);
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, kAll, 0), 0x0803FE00);
  CHECK_WORD(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 0, kAll), 0x080001FF);
}

TEST_CASE("dl: L2 is format 17 (Table 7 p.97)") {
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L2, 0, 0), 0x07880000);  // 17 << 19
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::L2, 31, 124), 0x07883E7C);  // a 124-pixel-wide L2 row is 31 bytes
  CHECK(static_cast<std::uint32_t>(BitmapFormat::L2) == 17u);
}

TEST_CASE("dl: BITMAP_TRANSFORM_A, B, D, E are signed 17-bit 8.8 coefficients in [16:0] (§4.12-§4.16 pp.108-112)") {
  CHECK_WORD(dl::bitmap_transform_a(256), 0x15000100);  // the initial value, 1.0
  CHECK_WORD(dl::bitmap_transform_a(128), 0x15000080);  // the guide's example: 0.5, the bitmap appears double width
  CHECK_WORD(dl::bitmap_transform_a(512), 0x15000200);  // 2.0, half width
  CHECK_WORD(dl::bitmap_transform_a(85), 0x15000055);   // 85 / 256, the nearest 8.8 value to a third
  CHECK_WORD(dl::bitmap_transform_a(-256), 0x1501FF00);  // 17-bit two's complement
  CHECK_WORD(dl::bitmap_transform_a(-1), 0x1501FFFF);
  CHECK_WORD(dl::bitmap_transform_a(65535), 0x1500FFFF);
  CHECK_WORD(dl::bitmap_transform_a(65536), 0x15010000);  // bit 16 is the sign bit: this is -65536
  CHECK_WORD(dl::bitmap_transform_b(0), 0x16000000);
  CHECK_WORD(dl::bitmap_transform_b(-2), 0x1601FFFE);
  CHECK_WORD(dl::bitmap_transform_d(0x123), 0x18000123);
  CHECK_WORD(dl::bitmap_transform_d(-300), 0x1801FED4);
  CHECK_WORD(dl::bitmap_transform_e(128), 0x19000080);  // the guide's example for E: double height
  CHECK_WORD(dl::bitmap_transform_e(512), 0x19000200);
  CHECK_WORD(dl::bitmap_transform_e(-256), 0x1901FF00);
  for (const std::int32_t v : kExtremes) {  // the argument never leaks into the opcode or a reserved bit
    CHECK_WORD(dl::bitmap_transform_a(v) & ~0x1501FFFFu, 0);
    CHECK_WORD(dl::bitmap_transform_b(v) & ~0x1601FFFFu, 0);
    CHECK_WORD(dl::bitmap_transform_d(v) & ~0x1801FFFFu, 0);
    CHECK_WORD(dl::bitmap_transform_e(v) & ~0x1901FFFFu, 0);
  }
}

TEST_CASE("dl: BITMAP_TRANSFORM_C and F are signed 24-bit 15.8 offsets in [23:0] (§4.14 p.110, §4.17 p.113)") {
  CHECK_WORD(dl::bitmap_transform_c(0), 0x17000000);
  CHECK_WORD(dl::bitmap_transform_c(256), 0x17000100);   // one source pixel
  CHECK_WORD(dl::bitmap_transform_c(-256), 0x17FFFF00);  // 24-bit two's complement
  CHECK_WORD(dl::bitmap_transform_c(0x7FFFFF), 0x177FFFFF);
  CHECK_WORD(dl::bitmap_transform_f(0), 0x1A000000);
  CHECK_WORD(dl::bitmap_transform_f(1280), 0x1A000500);  // five source pixels
  CHECK_WORD(dl::bitmap_transform_f(-1), 0x1AFFFFFF);
  for (const std::int32_t v : kExtremes) {
    CHECK_WORD(dl::bitmap_transform_c(v) & ~0x17FFFFFFu, 0);
    CHECK_WORD(dl::bitmap_transform_f(v) & ~0x1AFFFFFFu, 0);
  }
  // The six opcodes are consecutive, 0x15..0x1A.
  CHECK(o3sdk::eve::opcode::kBitmapTransformA == 0x15u);
  CHECK(o3sdk::eve::opcode::kBitmapTransformF == 0x1Au);
}

TEST_CASE("dl: PALETTE_SOURCE 0x2A, a 22-bit address; PALETTED565 is format 14") {
  CHECK_WORD(dl::palette_source(0x6F000), 0x2A06F000);
  CHECK_WORD(dl::palette_source(0xFFFFFFFFu), 0x2A3FFFFF);
  CHECK_WORD(dl::bitmap_layout(BitmapFormat::Paletted565, 86, 5), 0x07000000 | (14u << 19) | (86u << 9) | 5u);
}

TEST_CASE("dl: BITMAP_SIZE_H width [3:2], height [1:0]") {
  CHECK_WORD(dl::bitmap_size_h(1, 0), 0x29000004);  // §4.10 p.105; guide example (an 800-pixel width)
  CHECK_WORD(dl::bitmap_size_h(0, 1), 0x29000001);
  CHECK_WORD(dl::bitmap_size_h(3, 3), 0x2900000F);
  CHECK_WORD(dl::bitmap_size_h(kAll, kAll), 0x2900000F);
}

TEST_CASE("dl: the guide's 800x480 image example reassembles to the right size and stride") {
  // §4.11 p.107: ARGB1555, 800 px wide = 1600 bytes per line. Low 10 bits (576) go in BITMAP_LAYOUT, the
  // top two bits (1) in BITMAP_LAYOUT_H; the width 800 = 512 + 288 likewise.
  const std::uint32_t layout = dl::bitmap_layout(static_cast<BitmapFormat>(0), 1600u & 1023u, 480);
  const std::uint32_t layout_h = dl::bitmap_layout_h(1600u >> 10, 480u >> 9);
  const std::uint32_t size = dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 800u & 511u, 480);
  const std::uint32_t size_h = dl::bitmap_size_h(800u >> 9, 480u >> 9);
  CHECK_WORD(layout, 0x070481E0);
  CHECK_WORD(layout_h, 0x28000004);
  CHECK_WORD(size, 0x080241E0);
  CHECK_WORD(size_h, 0x29000004);
}

TEST_CASE("dl: opcodes stay in the top byte whatever the arguments") {
  // Worst-case arguments: every parameter all ones. Any leak into the opcode would change these words.
  struct Row {
    std::uint32_t word;
    std::uint32_t opcode;  // expected top byte
  };
  const Row rows[] = {
      {dl::clear_color_rgb(255, 255, 255), o3sdk::eve::opcode::kClearColorRgb},
      {dl::clear(true, true, true), o3sdk::eve::opcode::kClear},
      {dl::color_rgb(255, 255, 255), o3sdk::eve::opcode::kColorRgb},
      {dl::color_a(255), o3sdk::eve::opcode::kColorA},
      {dl::begin(static_cast<Primitive>(kAll)), o3sdk::eve::opcode::kBegin},
      {dl::vertex_format(kAll), o3sdk::eve::opcode::kVertexFormat},
      {dl::line_width(kAll), o3sdk::eve::opcode::kLineWidth},
      {dl::point_size(kAll), o3sdk::eve::opcode::kPointSize},
      {dl::blend_func(static_cast<BlendFactor>(kAll), static_cast<BlendFactor>(kAll)), o3sdk::eve::opcode::kBlendFunc},
      {dl::vertex_translate_x(-1), o3sdk::eve::opcode::kVertexTranslateX},
      {dl::vertex_translate_y(-1), o3sdk::eve::opcode::kVertexTranslateY},
      {dl::vertex_translate_x(INT32_MIN), o3sdk::eve::opcode::kVertexTranslateX},
      {dl::scissor_xy(kAll, kAll), o3sdk::eve::opcode::kScissorXy},
      {dl::scissor_size(kAll, kAll), o3sdk::eve::opcode::kScissorSize},
      {dl::bitmap_handle(kAll), o3sdk::eve::opcode::kBitmapHandle},
      {dl::bitmap_source(kAll), o3sdk::eve::opcode::kBitmapSource},
      {dl::bitmap_layout(static_cast<BitmapFormat>(kAll), kAll, kAll), o3sdk::eve::opcode::kBitmapLayout},
      {dl::bitmap_layout_h(kAll, kAll), o3sdk::eve::opcode::kBitmapLayoutH},
      {dl::bitmap_size(static_cast<Filter>(kAll), static_cast<Wrap>(kAll), static_cast<Wrap>(kAll), kAll, kAll),
       o3sdk::eve::opcode::kBitmapSize},
      {dl::bitmap_size_h(kAll, kAll), o3sdk::eve::opcode::kBitmapSizeH},
      {dl::palette_source(kAll), o3sdk::eve::opcode::kPaletteSource},
      {dl::cell(kAll), o3sdk::eve::opcode::kCell},
  };
  for (const Row& row : rows) {
    CHECK_MESSAGE((row.word >> 24) == row.opcode, "word 0x" << std::hex << row.word);
  }
  // The two vertex encoders keep their 2-bit tag (and never produce the other one).
  CHECK((dl::vertex2f(INT32_MIN, INT32_MIN) >> 30) == 0x1u);
  CHECK((dl::vertex2f(INT32_MAX, INT32_MAX) >> 30) == 0x1u);
  CHECK((dl::vertex2ii(kAll, kAll, kAll, kAll) >> 30) == 0x2u);
}

TEST_CASE("dl: reserved bits stay zero (§4.2 p.90)") {
  // Words with no parameters, and the unused bits of parameterised ones, are exactly the documented masks.
  CHECK_WORD(dl::clear(true, true, true) & ~0x26000007u, 0);
  CHECK_WORD(dl::color_a(255) & ~0x100000FFu, 0);
  CHECK_WORD(dl::begin(static_cast<Primitive>(kAll)) & ~0x1F00000Fu, 0);
  CHECK_WORD(dl::line_width(kAll) & ~0x0E000FFFu, 0);
  CHECK_WORD(dl::point_size(kAll) & ~0x0D001FFFu, 0);
  CHECK_WORD(dl::blend_func(static_cast<BlendFactor>(kAll), static_cast<BlendFactor>(kAll)) & ~0x0B00003Fu, 0);
  CHECK_WORD(dl::vertex_translate_x(-1) & ~0x2B01FFFFu, 0);
  CHECK_WORD(dl::vertex_translate_y(-1) & ~0x2C01FFFFu, 0);
  CHECK_WORD(dl::bitmap_handle(kAll) & ~0x0500001Fu, 0);
  CHECK_WORD(dl::bitmap_source(kAll) & ~0x013FFFFFu, 0);
  CHECK_WORD(dl::cell(kAll) & ~0x0600007Fu, 0);
  CHECK_WORD(dl::vertex_format(kAll) & ~0x27000007u, 0);
  CHECK_WORD(dl::bitmap_layout_h(kAll, kAll) & ~0x2800000Fu, 0);
  CHECK_WORD(dl::bitmap_size_h(kAll, kAll) & ~0x2900000Fu, 0);
  CHECK_WORD(dl::palette_source(kAll) & ~0x2A3FFFFFu, 0);  // the address is 22 bits
  CHECK_WORD(dl::scissor_xy(kAll, kAll) & ~0x1B3FFFFFu, 0);       // bits 23:22 are reserved
  CHECK_WORD(dl::bitmap_layout(static_cast<BitmapFormat>(kAll), kAll, kAll) & ~0x07FFFFFFu, 0);
  CHECK_WORD(dl::bitmap_size(static_cast<Filter>(kAll), static_cast<Wrap>(kAll), static_cast<Wrap>(kAll), kAll, kAll) &
                 ~0x081FFFFFu,
             0);  // bits 23:21 are reserved
}
