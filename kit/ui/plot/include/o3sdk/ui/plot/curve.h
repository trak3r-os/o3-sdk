// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The curve / XY plot (ADR 0010 amendment 2): envelopes, LFO shapes and filter responses. A series of up to
// kCurveMaxPoints points (float or Q15, evenly spaced or with explicit x) is mapped through two axes (linear,
// logarithmic frequency, dB), reduced to at most one point a pixel column and then to at most `max_vertices` vertices by
// Ramer-Douglas-Peucker (largest error first, so the budget is met with the points that matter), and drawn as an
// EDGE_STRIP_B translucent fill with a LINE_STRIP on top. Axes, a grid and tick labels (the Micro face) come from the same
// axis specs (plot_math.h); handles are POINTS. Every call is bounded in display-list words (curve_*_max_words()),
// whatever the data.
//
// Work memory is the caller's (CurveScratch, about 5 KB); no heap. Main loop only.

#include <o3sdk/ui/plot/plot_math.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui::plot {

inline constexpr unsigned kCurveMaxPoints = 1024;    // points of a series, after the reduction to one a pixel column
inline constexpr unsigned kCurveMaxVertices = 192;   // vertices a strip may have, whatever max_vertices says
inline constexpr unsigned kCurveMaxTicks = 16;       // per axis
inline constexpr unsigned kCurveMaxHandles = 16;
inline constexpr unsigned kCurveLabelChars = 6;      // a tick label is at most this wide (6 x 6 px): a longer one is not drawn

struct CurveStyle {
  Color line = Color::Instr;
  Color fill = Color::Instr;
  Color grid = Color::Empty;
  Color frame = Color::Empty;
  Color reference = Color::TextDim;  // the 0 dB line of a dB axis
  Color label = Color::TextDim;
  Color handle = Color::Text;
  Color handle_active = Color::Accent;  // the handle being dragged: the plot's cursor, so the one orange
  Color handle_ring = Color::Pane;      // a ring round a handle, so that it reads on a line
  std::uint8_t line_width_q4 = 12;      // half width of the line in 1/16 px: 12 = 1.5 px
  std::uint8_t fill_alpha = 0x40;
  std::uint8_t handle_radius = 5;       // px
  std::uint16_t max_vertices = 128;     // at most kCurveMaxVertices
  float tolerance_px = 0.35f;           // Ramer-Douglas-Peucker: the largest error a dropped point may have
};

// What to plot, in data coordinates. `x` may be null: the points are then evenly spaced from x_lo to x_hi. Values are
// floats (`y`) or Q15 (`y_q15`, value = q / 32768); if both are given `y` wins.
struct CurveSeries {
  const float* x = nullptr;
  const float* y = nullptr;
  const std::int16_t* y_q15 = nullptr;
  std::uint32_t n = 0;
  float x_lo = 0.0f;
  float x_hi = 1.0f;
};

struct CurveHandle {
  float x;
  float y;
  bool active;
};

struct CurveScratch {
  std::int16_t px[kCurveMaxPoints];  // the mapped points, screen px in 1/16
  std::int16_t py[kCurveMaxPoints];
  std::uint16_t keep[kCurveMaxVertices];
};

// The pure reduction, exposed for the tests: indices into the points (px, py in 1/16 px) of at most `max_keep` that
// approximate the polyline within `tolerance_q4` (1/16 px), always including the first and the last, in rising order.
// Returns how many. n >= 2, max_keep >= 2 and <= kCurveMaxVertices.
unsigned simplify_polyline(const std::int16_t* px, const std::int16_t* py, unsigned n, unsigned max_keep,
                           float tolerance_q4, std::uint16_t* keep) noexcept;

// The frame (a one-pixel border just outside `area`), the grid and the tick labels of a plot whose data is drawn in `area`.
// Labels sit below the frame (x axis) and left of it (y axis), so `area` needs about 24 px to its left and 16 below.
void draw_axes(Renderer& r, const Rect& area, const AxisSpec& x, const AxisSpec& y, const CurveStyle& style) noexcept;

// The series: the translucent fill (if `fill`) and the line, clipped to `area`. Returns the number of vertices drawn.
unsigned draw_series(Renderer& r, const Rect& area, const AxisSpec& x, const AxisSpec& y, const CurveSeries& series,
                     bool fill, const CurveStyle& style, CurveScratch& scratch) noexcept;

// Handles at data positions: a ring and a disc each, the active one in `handle_active`. At most kCurveMaxHandles.
void draw_handles(Renderer& r, const Rect& area, const AxisSpec& x, const AxisSpec& y, const CurveHandle* handles,
                  unsigned count, const CurveStyle& style) noexcept;

// Upper bounds on the words each call adds (DISPLAY not counted), hard: they hold for any style (every colour a different
// token), with the renderer's origin moved and a primitive of its own open, and in both translate modes (the tests build that
// worst case). A raw section costs Renderer::kRawOverheadWords = 4 on top of what is listed.
//   axes     the grid (a colour and BEGIN, 2 words a line, at most kCurveMaxTicks a side), the frame (a colour and four
//            rules), the 0 dB line (a colour and a rule), the labels (a colour and at most kCurveLabelChars glyphs of 3 words:
//            a glyph past x = 511 without VERTEX_TRANSLATE_X is a handle, a cell and a vertex). About 250 for the usual labels
//            of 4 characters in the translate mode; the bound is what the fallback mode of a 6-character label can take.
//   series   raw 4, scissor 2, COLOR_A, fill (colour, width, BEGIN, vertices, END), COLOR_A, line (colour, width, BEGIN,
//            vertices, END): 2 * vertices + 16
//   handles  raw 4, scissor 2, three passes (a colour, a size, BEGIN, END) with 2 vertices a handle between them
inline constexpr unsigned kCurveAxesMaxWords = (2 + 2 * 2 * kCurveMaxTicks) + (1 + 8) + (1 + 2) + (1 + 3 * kCurveLabelChars * 2 * kCurveMaxTicks);
constexpr unsigned curve_series_max_words(unsigned max_vertices) noexcept { return 2u * max_vertices + 16u; }
inline constexpr unsigned kCurveHandlesMaxWords = 4 + 2 + 3 * 4 + 2 * kCurveMaxHandles;

}  // namespace o3sdk::ui::plot
