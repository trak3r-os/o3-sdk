// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FT81x display-list word encoders (the subset the UI emits).
//
// Source of truth: "FT81X Series Programmers Guide", Version 1.2 (BRT_000031), chapter 4 "Display List
// Commands". Citations are "§section, p.page" of that document. Each word is 32 bits: the opcode in
// [31:24] (VERTEX2F/VERTEX2II use [31:30]) and the parameters in the low bits; reserved bits stay zero
// (§4.2, p.90).
//
// Out-of-range arguments are masked to their field, exactly as the vendor macros do, so no argument can
// leak into another field or into the opcode. Valid ranges are noted per encoder.
//
// Where this file and the guide's diagrams differ, it is on purpose and noted at the encoder:
//   * COLOR_RGB / CLEAR_COLOR_RGB (pp.126, 121): the diagrams label bits [15:8] "Blue" and [7:0] "Green".
//     That is a typo: the guide's own coprocessor text (§5.30 CMD_FGCOLOR, p.183: "Red is the most
//     significant 8 bits, blue is the least. So 0xff0000 is bright red") and the firmware's FT8xx.h
//     (the FT81x branch of the vendor's library) both put green in [15:8] and blue in [7:0]. We follow those.
//   * VERTEX2II (p.146): the Description repeats VERTEX2F's "-16384 to +16383"; the encoding and the
//     parameter list say x and y are unsigned 9-bit (0..511). We follow the encoding.
//   * BITMAP_LAYOUT (p.97): the text says "valid range 0 to 11" but Table 7 lists formats up to 17; the
//     field is 5 bits wide ([23:19]) and we mask to it.
//
// OS-003f added BITMAP_TRANSFORM_A-F (§4.12-§4.17, pp.108-113) for the boot logo's integer scale, and the L2 bitmap format
// (Table 7, p.97; layout Figure 6, p.100).
//
// OS-003a added BLEND_FUNC, POINT_SIZE, VERTEX_TRANSLATE_X/Y and the primitives POINTS, LINE_STRIP, EDGE_STRIP_A/B
// (Table 6). Their opcodes, field positions and masks agree with the guide and with the vendor library's header (FT81x branch); the test
// words were computed from those macros by a C program, as well as by hand.

#include <cstddef>
#include <cstdint>

namespace o3sdk::eve {

// RAM_DL holds at most 2048 commands (§4, p.89: 8 KiB, 4 bytes each).
inline constexpr std::size_t kMaxDisplayListWords = 2048;

namespace opcode {
// Opcode values: the high byte of each word, from the Encoding table of each command.
inline constexpr std::uint32_t kDisplay = 0x00;        // §4.29 p.127
inline constexpr std::uint32_t kBitmapSource = 0x01;   // §4.11 p.106
inline constexpr std::uint32_t kClearColorRgb = 0x02;  // §4.23 p.121
inline constexpr std::uint32_t kColorRgb = 0x04;       // §4.28 p.126
inline constexpr std::uint32_t kBitmapHandle = 0x05;   // §4.6  p.96
inline constexpr std::uint32_t kCell = 0x06;           // §4.20 p.117
inline constexpr std::uint32_t kBitmapLayout = 0x07;   // §4.7  p.97
inline constexpr std::uint32_t kBitmapSize = 0x08;     // §4.9  p.103
inline constexpr std::uint32_t kBlendFunc = 0x0B;      // §4.18 p.114
inline constexpr std::uint32_t kPointSize = 0x0D;      // §4.36 p.133
inline constexpr std::uint32_t kLineWidth = 0x0E;      // §4.32 p.130
inline constexpr std::uint32_t kColorA = 0x10;         // §4.26 p.124
inline constexpr std::uint32_t kBitmapTransformA = 0x15;  // §4.12 p.108
inline constexpr std::uint32_t kBitmapTransformB = 0x16;  // §4.13 p.109
inline constexpr std::uint32_t kBitmapTransformC = 0x17;  // §4.14 p.110
inline constexpr std::uint32_t kBitmapTransformD = 0x18;  // §4.15 p.111
inline constexpr std::uint32_t kBitmapTransformE = 0x19;  // §4.16 p.112
inline constexpr std::uint32_t kBitmapTransformF = 0x1A;  // §4.17 p.113
inline constexpr std::uint32_t kScissorXy = 0x1B;      // §4.41 p.138
inline constexpr std::uint32_t kScissorSize = 0x1C;    // §4.40 p.137
inline constexpr std::uint32_t kBegin = 0x1F;          // §4.5  p.94
inline constexpr std::uint32_t kEnd = 0x21;            // §4.30 p.128
inline constexpr std::uint32_t kSaveContext = 0x22;    // §4.39 p.136
inline constexpr std::uint32_t kRestoreContext = 0x23;  // §4.37 p.134
inline constexpr std::uint32_t kClear = 0x26;          // §4.21 p.118
inline constexpr std::uint32_t kVertexFormat = 0x27;   // §4.49 p.147
inline constexpr std::uint32_t kBitmapLayoutH = 0x28;  // §4.8  p.103
inline constexpr std::uint32_t kBitmapSizeH = 0x29;    // §4.10 p.105
inline constexpr std::uint32_t kVertexTranslateX = 0x2B;  // §4.50 p.148
inline constexpr std::uint32_t kVertexTranslateY = 0x2C;  // §4.51 p.149
// VERTEX2F and VERTEX2II are told apart by the two top bits [31:30], not by a full opcode byte.
inline constexpr std::uint32_t kVertex2fTag = 0x1;   // §4.47 p.145: [31:30] = 0b01
inline constexpr std::uint32_t kVertex2iiTag = 0x2;  // §4.48 p.146: [31:30] = 0b10
}  // namespace opcode

// Named parameter values for the supported subset.
enum class Primitive : std::uint32_t {  // §4.5 Table 6, p.94 (EDGE_STRIP_R = 5 and _L = 6 are not in the subset)
  Bitmaps = 1,
  Points = 2,
  Lines = 3,
  LineStrip = 4,
  EdgeStripA = 7,  // fills above the strip
  EdgeStripB = 8,  // fills below the strip
  Rects = 9,
};
enum class BitmapFormat : std::uint32_t {  // §4.7 Table 7, p.97
  L1 = 1,
  L4 = 2,
  L8 = 3,
  L2 = 17,  // two bits a pixel, the first pixel in bits 7-6 (Figure 6, p.100)
};
enum class Filter : std::uint32_t {  // §4.9, pp.103-104
  Nearest = 0,
  Bilinear = 1,
};
enum class Wrap : std::uint32_t {  // §4.9, p.104
  Border = 0,
  Repeat = 1,
};
enum class BlendFactor : std::uint32_t {  // §4.18 Table 8, p.114
  Zero = 0,
  One = 1,
  SrcAlpha = 2,
  DstAlpha = 3,
  OneMinusSrcAlpha = 4,
  OneMinusDstAlpha = 5,
};

namespace dl {

namespace detail {
// `value` masked to the bit range [hi:lo] and moved into place.
constexpr std::uint32_t field(std::uint32_t value, unsigned hi, unsigned lo) noexcept {
  return (value & ((2u << (hi - lo)) - 1u)) << lo;
}
constexpr std::uint32_t op(std::uint32_t opcode) noexcept { return opcode << 24; }
}  // namespace detail

// §4.29 DISPLAY, p.127. [31:24]=0x00, rest reserved. Ends the list; the chip ignores everything after.
constexpr std::uint32_t display() noexcept { return detail::op(opcode::kDisplay); }

// §4.23 CLEAR_COLOR_RGB, p.121. [31:24]=0x02, red [23:16], green [15:8], blue [7:0] (see the note above).
constexpr std::uint32_t clear_color_rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
  return detail::op(opcode::kClearColorRgb) | detail::field(red, 23, 16) | detail::field(green, 15, 8) |
         detail::field(blue, 7, 0);
}

// §4.21 CLEAR, p.118. [31:24]=0x26, c [2] color buffer, s [1] stencil buffer, t [0] tag buffer.
constexpr std::uint32_t clear(bool color, bool stencil, bool tag) noexcept {
  return detail::op(opcode::kClear) | detail::field(color ? 1u : 0u, 2, 2) | detail::field(stencil ? 1u : 0u, 1, 1) |
         detail::field(tag ? 1u : 0u, 0, 0);
}

// §4.28 COLOR_RGB, p.126. [31:24]=0x04, red [23:16], green [15:8], blue [7:0] (see the note above).
constexpr std::uint32_t color_rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
  return detail::op(opcode::kColorRgb) | detail::field(red, 23, 16) | detail::field(green, 15, 8) |
         detail::field(blue, 7, 0);
}

// §4.26 COLOR_A, p.124. [31:24]=0x10, alpha [7:0].
constexpr std::uint32_t color_a(std::uint8_t alpha) noexcept {
  return detail::op(opcode::kColorA) | detail::field(alpha, 7, 0);
}

// §4.5 BEGIN, p.94. [31:24]=0x1F, prim [3:0].
constexpr std::uint32_t begin(Primitive prim) noexcept {
  return detail::op(opcode::kBegin) | detail::field(static_cast<std::uint32_t>(prim), 3, 0);
}

// §4.30 END, p.128. [31:24]=0x21, rest reserved.
constexpr std::uint32_t end() noexcept { return detail::op(opcode::kEnd); }

// §4.47 VERTEX2F, p.145. [31:30]=0b01, X [29:15] and Y [14:0], both signed 15-bit in the units set by
// VERTEX_FORMAT (default 1/16 pixel). Valid range -16384..16383 in those units.
constexpr std::uint32_t vertex2f(std::int32_t x, std::int32_t y) noexcept {
  return (opcode::kVertex2fTag << 30) | detail::field(static_cast<std::uint32_t>(x), 29, 15) |
         detail::field(static_cast<std::uint32_t>(y), 14, 0);
}

// §4.48 VERTEX2II, p.146. [31:30]=0b10, x [29:21], y [20:12] (unsigned pixels, 0..511), handle [11:7]
// (0..31), cell [6:0] (0..127). handle and cell only matter while BEGIN(Bitmaps) is active.
constexpr std::uint32_t vertex2ii(std::uint32_t x, std::uint32_t y, std::uint32_t handle, std::uint32_t cell) noexcept {
  return (opcode::kVertex2iiTag << 30) | detail::field(x, 29, 21) | detail::field(y, 20, 12) |
         detail::field(handle, 11, 7) | detail::field(cell, 6, 0);
}

// §4.49 VERTEX_FORMAT, p.147. [31:24]=0x27, frac [2:0]: fractional bits of VERTEX2F coordinates, valid
// 0..4 (initial 4, i.e. 1/16 pixel).
constexpr std::uint32_t vertex_format(std::uint32_t frac) noexcept {
  return detail::op(opcode::kVertexFormat) | detail::field(frac, 2, 0);
}

// §4.50 VERTEX_TRANSLATE_X, p.148, and §4.51 VERTEX_TRANSLATE_Y, p.149. [31:24]=0x2B / 0x2C, offset [16:0]: a
// signed 17-bit offset in 1/16 pixel (initial 0, valid -65536..65535, i.e. +-4096 px) that the chip adds to the X
// / Y of vertices. [A] The guide gives the unit as 1/16 pixel ("signed x-coordinate in 1/16 pixel") but does not
// say whether VERTEX_FORMAT changes it, nor whether VERTEX2II is translated; the simulator takes it as always
// 1/16 pixel and applies it to both vertex forms. To be checked on hardware (sim/README.md, "Calibration").
constexpr std::uint32_t vertex_translate_x(std::int32_t x) noexcept {
  return detail::op(opcode::kVertexTranslateX) | detail::field(static_cast<std::uint32_t>(x), 16, 0);
}
constexpr std::uint32_t vertex_translate_y(std::int32_t y) noexcept {
  return detail::op(opcode::kVertexTranslateY) | detail::field(static_cast<std::uint32_t>(y), 16, 0);
}

// §4.32 LINE_WIDTH, p.130. [31:24]=0x0E, width [11:0] in 1/16 pixel (initial 16), valid 0..4095.
// It is the distance from the line centre to its edge. It also applies to RECTS (p.130), where it sets the
// corner curvature and grows the rectangle by that much on every side (§2.5.4, pp.21-22).
constexpr std::uint32_t line_width(std::uint32_t width) noexcept {
  return detail::op(opcode::kLineWidth) | detail::field(width, 11, 0);
}

// §4.36 POINT_SIZE, p.133. [31:24]=0x0D, size [12:0]: the point radius, centre to outermost drawn pixel, in 1/16
// pixel (initial 16), valid 0..8191 (0..511 pixels).
constexpr std::uint32_t point_size(std::uint32_t size) noexcept {
  return detail::op(opcode::kPointSize) | detail::field(size, 12, 0);
}

// §4.18 BLEND_FUNC, p.114. [31:24]=0x0B, src [5:3], dst [2:0]: the factors of `source * src + destination * dst`
// (initial SRC_ALPHA, ONE_MINUS_SRC_ALPHA). The guide's Table 8 values are 0..5; the field is 3 bits wide.
constexpr std::uint32_t blend_func(BlendFactor src, BlendFactor dst) noexcept {
  return detail::op(opcode::kBlendFunc) | detail::field(static_cast<std::uint32_t>(src), 5, 3) |
         detail::field(static_cast<std::uint32_t>(dst), 2, 0);
}

// §4.41 SCISSOR_XY, p.138. [31:24]=0x1B, x [21:11], y [10:0], unsigned pixels, valid 0..2047.
constexpr std::uint32_t scissor_xy(std::uint32_t x, std::uint32_t y) noexcept {
  return detail::op(opcode::kScissorXy) | detail::field(x, 21, 11) | detail::field(y, 10, 0);
}

// §4.40 SCISSOR_SIZE, p.137. [31:24]=0x1C, width [23:12], height [11:0], pixels, valid 0..2048
// (initial 2048; 0 draws nothing).
constexpr std::uint32_t scissor_size(std::uint32_t width, std::uint32_t height) noexcept {
  return detail::op(opcode::kScissorSize) | detail::field(width, 23, 12) | detail::field(height, 11, 0);
}

// §4.6 BITMAP_HANDLE, p.96. [31:24]=0x05, handle [4:0], valid 0..31 (initial 0).
constexpr std::uint32_t bitmap_handle(std::uint32_t handle) noexcept {
  return detail::op(opcode::kBitmapHandle) | detail::field(handle, 4, 0);
}

// §4.11 BITMAP_SOURCE, p.106. [31:24]=0x01, addr [21:0]: byte address in RAM_G, aligned to the format.
constexpr std::uint32_t bitmap_source(std::uint32_t addr) noexcept {
  return detail::op(opcode::kBitmapSource) | detail::field(addr, 21, 0);
}

// §4.7 BITMAP_LAYOUT, p.97. [31:24]=0x07, format [23:19], linestride [18:9] (bytes per row, low 10 bits),
// height [8:0] (rows, low 9 bits). Rows of 1024+ bytes or 512+ rows also need bitmap_layout_h().
constexpr std::uint32_t bitmap_layout(BitmapFormat format, std::uint32_t linestride, std::uint32_t height) noexcept {
  return detail::op(opcode::kBitmapLayout) | detail::field(static_cast<std::uint32_t>(format), 23, 19) |
         detail::field(linestride, 18, 9) | detail::field(height, 8, 0);
}

// §4.8 BITMAP_LAYOUT_H, p.103. [31:24]=0x28, linestride [3:2], height [1:0]: the two most significant
// bits of the 12-bit linestride and 11-bit height. The arguments are those 2-bit parts, already
// separated (the guide's example: a 1600-byte line is BITMAP_LAYOUT_H(1, ..) with BITMAP_LAYOUT(.., 576, ..)).
constexpr std::uint32_t bitmap_layout_h(std::uint32_t linestride_high, std::uint32_t height_high) noexcept {
  return detail::op(opcode::kBitmapLayoutH) | detail::field(linestride_high, 3, 2) | detail::field(height_high, 1, 0);
}

// §4.9 BITMAP_SIZE, p.103. [31:24]=0x08, filter [20], wrapx [19], wrapy [18], width [17:9], height [8:0]
// (drawn size in pixels, low 9 bits each; 0 means 2048 unless bitmap_size_h() says otherwise).
constexpr std::uint32_t bitmap_size(Filter filter, Wrap wrap_x, Wrap wrap_y, std::uint32_t width,
                                    std::uint32_t height) noexcept {
  return detail::op(opcode::kBitmapSize) | detail::field(static_cast<std::uint32_t>(filter), 20, 20) |
         detail::field(static_cast<std::uint32_t>(wrap_x), 19, 19) |
         detail::field(static_cast<std::uint32_t>(wrap_y), 18, 18) | detail::field(width, 17, 9) |
         detail::field(height, 8, 0);
}

// §4.10 BITMAP_SIZE_H, p.105. [31:24]=0x29, width [3:2], height [1:0]: the two most significant bits of
// the drawn width and height, already separated (see bitmap_layout_h).
constexpr std::uint32_t bitmap_size_h(std::uint32_t width_high, std::uint32_t height_high) noexcept {
  return detail::op(opcode::kBitmapSizeH) | detail::field(width_high, 3, 2) | detail::field(height_high, 1, 0);
}

// §4.12 BITMAP_TRANSFORM_A, p.108, §4.13 B (p.109), §4.15 D (p.111) and §4.16 E (p.112). [31:24]=0x15 / 0x16 / 0x18 / 0x19,
// [16:0] a signed 17-bit coefficient in 8.8 fixed point (initial A = E = 256 = 1.0, B = D = 0). The bitmap transform maps a
// pixel of the drawn bitmap to the source pixel it shows: x' = A x + B y + C, y' = D x + E y + F (§2.5.5, p.24), so A = 128 (0.5)
// "causes the bitmap to appear double width" (p.108). Part of the graphics context (§4.1). The argument is the raw field.
constexpr std::uint32_t bitmap_transform_a(std::int32_t a) noexcept {
  return detail::op(opcode::kBitmapTransformA) | detail::field(static_cast<std::uint32_t>(a), 16, 0);
}
constexpr std::uint32_t bitmap_transform_b(std::int32_t b) noexcept {
  return detail::op(opcode::kBitmapTransformB) | detail::field(static_cast<std::uint32_t>(b), 16, 0);
}
constexpr std::uint32_t bitmap_transform_d(std::int32_t d) noexcept {
  return detail::op(opcode::kBitmapTransformD) | detail::field(static_cast<std::uint32_t>(d), 16, 0);
}
constexpr std::uint32_t bitmap_transform_e(std::int32_t e) noexcept {
  return detail::op(opcode::kBitmapTransformE) | detail::field(static_cast<std::uint32_t>(e), 16, 0);
}
// §4.14 BITMAP_TRANSFORM_C, p.110, and §4.17 F, p.113. [31:24]=0x17 / 0x1A, [23:0] a signed 24-bit offset in 15.8 fixed point
// (initial 0), in source pixels.
constexpr std::uint32_t bitmap_transform_c(std::int32_t c) noexcept {
  return detail::op(opcode::kBitmapTransformC) | detail::field(static_cast<std::uint32_t>(c), 23, 0);
}
constexpr std::uint32_t bitmap_transform_f(std::int32_t f) noexcept {
  return detail::op(opcode::kBitmapTransformF) | detail::field(static_cast<std::uint32_t>(f), 23, 0);
}

// §4.20 CELL, p.117. [31:24]=0x06, cell [6:0] (0..127, initial 0): which cell of the bitmap VERTEX2F draws.
constexpr std::uint32_t cell(std::uint32_t cell_index) noexcept {
  return detail::op(opcode::kCell) | detail::field(cell_index, 6, 0);
}

// §4.39 SAVE_CONTEXT, p.136. [31:24]=0x22. Pushes the graphics context (4 levels; an extra push drops the
// oldest). Bitmap handle parameters are not part of the context (§4.1, p.89).
constexpr std::uint32_t save_context() noexcept { return detail::op(opcode::kSaveContext); }

// §4.37 RESTORE_CONTEXT, p.134. [31:24]=0x23. Pops the context; an extra pop loads the defaults.
constexpr std::uint32_t restore_context() noexcept { return detail::op(opcode::kRestoreContext); }

}  // namespace dl
}  // namespace o3sdk::eve
