// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The wave view (ADR 0010 amendment 2): draws one lane of a waveform into a Rect, in one of two modes chosen per call,
// then its marks.
//
//   Vector      two EDGE_STRIP_B fills over the min/max columns: the first, in the wave colour, from each column's maximum
//               down; the second, in the lane's background colour, from just under each column's minimum down. One vertex
//               a column on the pixel centres, so each fill is exact (the strips are clipped to the lane by a scissor):
//               the picture is the L1 bitmap's, pixel for pixel (tests/plot_wave_test.cpp). 2 words a column and a dozen
//               more; wider than `max_vertices` columns the vertices are grouped (a vertex per k columns, the min/max
//               of the group), which is what keeps the cost bounded whatever the width. Zoomed in below about one frame
//               a pixel (the view shows no more than 2 * max_vertices - 2 frames, 1 word a frame) it is a LINE_STRIP
//               through the frames themselves, at sub-pixel positions, with a dot on each frame once they are 6 px apart.
//   BitmapL1/L4 the lane's picture from a WaveBitmap (wave_bitmap.h): about 14 words whatever the width, redone by the job
//               runner when the view changes. The bitmap modes need the sound's samples only to build the picture.
//
// The marks (selection, loop, slices, playhead, zero line, clipping tint) are RECTS in theme tokens, drawn over the wave;
// translucent ones use COLOR_A in a raw section. The lane must be a flat fill of `background` (Pane): the vector mode's
// second fill paints over everything beneath it from the minima down, and the bitmap mode leaves what is under it alone.
//
// Every call is bounded in display-list words by the constants below, whatever the data: the tests measure the worst
// case of each against them. Main loop only; no heap.

#include <o3sdk/ui/plot/peak_cache.h>
#include <o3sdk/ui/plot/wave_bitmap.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui::plot {

enum class WaveMode : std::uint8_t { Vector, BitmapL1, BitmapL4 };

// The part of the sound a lane shows: `span` frames from `first` across the lane's width.
struct WaveWindow {
  Frame first = 0;
  Frame span = 1;
};

inline constexpr unsigned kWaveMaxSlices = 32;

struct WaveMarks {
  bool zero_line = false;  // a translucent hairline on the row of value 0
  bool clip_tint = false;  // a translucent band over the columns that reach full scale
  bool has_selection = false;
  Frame sel_begin = 0, sel_end = 0;
  bool has_loop = false;
  Frame loop_start = 0, loop_end = 0;
  bool has_playhead = false;
  Frame playhead = 0;
  unsigned slice_count = 0;  // at most kWaveMaxSlices are drawn
  Frame slices[kWaveMaxSlices] = {};
};

// The tokens of a wave lane. All are theme colours (nothing here names a value).
struct WaveStyle {
  Color wave = Color::Instr;
  Color background = Color::Pane;  // the vector mode's second fill: what the lane is flat-filled with
  Color zero = Color::Text;
  Color clip = Color::Accent;  // the one orange also means "this is clipping" (ADR 0001 caps the palette at 16 tokens)
  Color selection = Color::Sel;
  Color loop = Color::Fx;
  Color slice = Color::Text;
  Color playhead = Color::Play;
  Color dots = Color::Text;  // the frames of the zoomed-in line
  std::uint16_t max_vertices = 192;  // per strip of the envelope (2 words a column: 394 words for the two strips); the line may use twice as many
  std::uint8_t line_width_q4 = 8;    // the zoomed-in line, 1/16 px of half width: 8 = a 1 px line
};

// Upper bounds on the words one call adds to a frame (DISPLAY not counted), hard: they hold for any style (every colour a
// different token), with the renderer's origin moved and a primitive of its own open, in both translate modes (the tests build
// that worst case). A raw section costs END of the open primitive, SAVE_CONTEXT, VERTEX_TRANSLATE_X 0 and RESTORE_CONTEXT
// (Renderer::kRawOverheadWords = 4).
//   envelope  raw 4, scissor 2, 2 x (colour, BEGIN, <= max_vertices vertices, END)
//   line      raw 4, scissor 2, colour, LINE_WIDTH, BEGIN, <= 2 * max_vertices vertices, END, and the dots (colour, POINT_SIZE,
//             BEGIN, a vertex a frame, END) when the frames are 6 px apart
//   bitmap    raw 4, scissor 2, handle, source, layout, size, size_h, colour, BEGIN, CELL, vertex, END
constexpr unsigned wave_envelope_max_words(unsigned max_vertices) noexcept { return 4u + 2u + 2u * (3u + max_vertices) + 0u; }
constexpr unsigned wave_line_max_words(unsigned max_vertices, unsigned lane_width) noexcept {
  const unsigned frames = 2u * max_vertices - 2u;  // the longest view drawn as a line, in frames
  const unsigned dots = lane_width / 6u < frames ? lane_width / 6u : frames;
  return 4u + 2u + 4u + 2u * max_vertices + 1u + 4u + dots;
}
inline constexpr unsigned kWaveBitmapMaxWords = 4 + 2 + 5 + 5;
// The marks: the raw section (4, and the END that closes the rectangles drawn inside it), its scissor (2), the BEGIN of the
// rectangles, the translucent group (zero line: COLOR_A, colour, a rule; the clip bands: COLOR_A, colour, 2 words a run;
// the selection and the loop tint: COLOR_A, colour, a rectangle each), COLOR_A, and the opaque group (selection edges: a colour
// and two rules; loop: a colour, two rules and two flags; slices: a colour and 2 words each; playhead: a colour, a rule and
// its head).
inline constexpr unsigned kWaveMarksMaxWords = 4 + 1 + 2 + 1 + 4 + (2 + 2 * kMaxClipRuns) + 4 + 4 + 1 + (1 + 4) + (1 + 8) +
                                               (1 + 2 * kWaveMaxSlices) + (1 + 4);

// Words the lane's wave (not the marks) costs at most in `mode`.
constexpr unsigned wave_max_words(WaveMode mode, unsigned max_vertices, unsigned lane_width) noexcept {
  const unsigned env = wave_envelope_max_words(max_vertices), line = wave_line_max_words(max_vertices, lane_width);
  return mode == WaveMode::Vector ? (env > line ? env : line) : kWaveBitmapMaxWords;
}

// The window of a lane for the bitmap's key.
inline WaveKey wave_key(const PeakCache& cache, std::uint32_t generation, unsigned channel, const WaveWindow& w,
                        const Rect& lane, WaveDepth depth) noexcept {
  WaveKey k;
  k.cache = &cache;
  k.generation = generation;
  k.epoch = cache.epoch();
  k.first = w.first;
  k.span = w.span;
  k.width = static_cast<std::uint16_t>(lane.w);
  k.height = static_cast<std::uint16_t>(lane.h);
  k.channel = static_cast<std::uint8_t>(channel);
  k.depth = depth;
  return k;
}

// Draws the lane's waveform in `mode` and then its marks. `bitmap` and `bitmap_lane` (0 or 1: left or right) are used
// by the bitmap modes only: the call asks the bitmap for the lane's current view (WaveBitmap::request(), a compare when
// nothing changed; `generation` is the owner's edit counter) and draws the lane's last complete picture, which is the
// previous view's until the job runner has finished the new one. Returns false when nothing could be drawn: a bitmap lane
// without a picture yet, a lane wider than kWaveMaxWidth, an invalid cache or one that is not built() yet.
bool draw_wave(Renderer& r, const Rect& lane, const PeakCache& cache, unsigned channel, const WaveWindow& window,
               WaveMode mode, WaveBitmap* bitmap, unsigned bitmap_lane, std::uint32_t generation,
               const WaveMarks& marks, const WaveStyle& style) noexcept;

// The pieces, for callers that compose their own lane (and for the tests).
void draw_wave_vector(Renderer& r, const Rect& lane, const PeakCache& cache, unsigned channel, const WaveWindow& window,
                      const WaveStyle& style, ClipRun* clips, unsigned* clip_count) noexcept;
bool draw_wave_bitmap(Renderer& r, const Rect& lane, const WaveBitmap& bitmap, unsigned bitmap_lane,
                      const WaveStyle& style) noexcept;
void draw_wave_marks(Renderer& r, const Rect& lane, const WaveWindow& window, const WaveMarks& marks,
                     const WaveStyle& style, const ClipRun* clips, unsigned clip_count) noexcept;

}  // namespace o3sdk::ui::plot
