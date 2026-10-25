// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/wave_view.h>

#include <o3sdk/eve/dl.h>

namespace o3sdk::ui::plot {

namespace {

namespace dl = eve::dl;
using eve::Primitive;

// VERTEX2F holds 15 signed bits in 1/16 px (default VERTEX_FORMAT): +-1023 px. Plot vertices stay well inside.
constexpr std::int32_t kMaxQ4 = 16000;
constexpr std::int32_t clamp_q4(std::int64_t v) noexcept {
  return static_cast<std::int32_t>(v < -kMaxQ4 ? -kMaxQ4 : (v > kMaxQ4 ? kMaxQ4 : v));
}
std::uint32_t vtx(std::int64_t x_q4, std::int64_t y_q4) noexcept { return dl::vertex2f(clamp_q4(x_q4), clamp_q4(y_q4)); }

constexpr std::uint8_t kAlphaZero = 0x38;
constexpr std::uint8_t kAlphaClip = 0x78;
constexpr std::uint8_t kAlphaSelection = 0x58;
constexpr std::uint8_t kAlphaLoop = 0x28;
constexpr int kFlag = 6;  // loop flags and the playhead's head, px

void scissor(Renderer& r, const Rect& lane) noexcept {
  r.raw(dl::scissor_xy(static_cast<std::uint32_t>(lane.x < 0 ? 0 : lane.x), static_cast<std::uint32_t>(lane.y < 0 ? 0 : lane.y)));
  r.raw(dl::scissor_size(static_cast<std::uint32_t>(lane.w), static_cast<std::uint32_t>(lane.h)));
}

// The envelope: the min/max columns as two EDGE_STRIP_B fills (wave_view.h). A vertex stands for k columns when there
// are more than max_vertices of them; k = 1 draws every column exactly (vertices on the pixel centres: the strip's rule
// for a vertex row is "fill from this row down", sim/README.md).
void draw_envelope(Renderer& r, const Rect& lane, const PeakPair* peaks, unsigned cols, const WaveStyle& s) noexcept {
  unsigned used = cols;
  while (used > 0 && peaks[used - 1].empty()) {
    --used;  // columns past the end of the sound
  }
  if (used == 0) {
    return;
  }
  const unsigned max_v = s.max_vertices < 2 ? 2u : s.max_vertices;
  const unsigned k = (used + max_v - 1) / max_v;
  const unsigned groups = (used + k - 1) / k;
  const unsigned h = static_cast<unsigned>(lane.h);
  const std::int64_t y0 = static_cast<std::int64_t>(lane.y) * 16 + 8;

  const auto group = [&](unsigned g, int& mn, int& mx, unsigned& width) {
    const unsigned c0 = g * k;
    const unsigned c1 = c0 + k < used ? c0 + k : used;
    mn = 32767;
    mx = -32768;
    for (unsigned c = c0; c < c1; ++c) {
      if (!peaks[c].empty()) {
        mn = peaks[c].min < mn ? peaks[c].min : mn;
        mx = peaks[c].max > mx ? peaks[c].max : mx;
      }
    }
    width = c1 - c0;
  };
  const auto x_of = [&](unsigned g, unsigned width) {
    return static_cast<std::int64_t>(lane.x + static_cast<int>(g * k)) * 16 + static_cast<std::int64_t>(width) * 8;
  };

  r.begin_raw();
  scissor(r, lane);
  r.raw(r.color_word(s.wave));
  r.raw(dl::begin(Primitive::EdgeStripB));
  for (unsigned g = 0; g < groups; ++g) {
    int mn, mx;
    unsigned width;
    group(g, mn, mx, width);
    r.raw(vtx(x_of(g, width), y0 + static_cast<std::int64_t>(wave_row(mx, h)) * 16));
  }
  r.raw(dl::end());
  r.raw(r.color_word(s.background));
  r.raw(dl::begin(Primitive::EdgeStripB));
  for (unsigned g = 0; g < groups; ++g) {
    int mn, mx;
    unsigned width;
    group(g, mn, mx, width);
    r.raw(vtx(x_of(g, width), y0 + static_cast<std::int64_t>(wave_row(mn, h) + 1u) * 16));
  }
  r.raw(dl::end());
  r.end_raw();
}

// Below about a frame a pixel the lane draws the frames themselves: a LINE_STRIP at sub-pixel positions, and a dot on each
// frame once they are far enough apart to be told apart.
void draw_line(Renderer& r, const Rect& lane, const PeakCache& cache, unsigned channel, const WaveWindow& w,
               const WaveStyle& s) noexcept {
  const Frame frames = cache.frames();
  if (w.first >= frames) {
    return;
  }
  const Frame i_lo = w.first > 0 ? w.first - 1u : 0u;  // one frame each side keeps the line running into the lane's edges
  const std::uint64_t end = std::uint64_t{w.first} + w.span;
  const Frame i_hi = end < frames ? static_cast<Frame>(end) : frames - 1u;
  const unsigned h = static_cast<unsigned>(lane.h);
  const std::int64_t cols = lane.w;
  const std::int16_t* p = cache.samples();
  const unsigned ch = cache.channels();
  const auto x_q4 = [&](Frame i) {  // the centre of frame i, in the view's columns
    const std::int64_t d2 = 2 * static_cast<std::int64_t>(i) - 2 * static_cast<std::int64_t>(w.first) + 1;
    return static_cast<std::int64_t>(lane.x) * 16 + (d2 * cols * 16) / (2 * static_cast<std::int64_t>(w.span));
  };
  const auto y_q4 = [&](Frame i) {
    return static_cast<std::int64_t>(lane.y) * 16 + wave_y_q4(p[static_cast<std::size_t>(i) * ch + channel], h);
  };
  r.begin_raw();
  scissor(r, lane);
  r.raw(r.color_word(s.wave));
  r.raw(dl::line_width(s.line_width_q4));
  r.raw(dl::begin(Primitive::LineStrip));
  for (Frame i = i_lo; i <= i_hi; ++i) {
    r.raw(vtx(x_q4(i), y_q4(i)));
  }
  r.raw(dl::end());
  if (static_cast<std::uint64_t>(w.span) * 6u <= static_cast<std::uint64_t>(lane.w)) {
    r.raw(r.color_word(s.dots));
    r.raw(dl::point_size(40));  // radius 2.5 px
    r.raw(dl::begin(Primitive::Points));
    const Frame last = static_cast<Frame>(end - 1u) < frames ? static_cast<Frame>(end - 1u) : frames - 1u;
    for (Frame i = w.first; i <= last; ++i) {
      r.raw(vtx(x_q4(i), y_q4(i)));
    }
    r.raw(dl::end());
  }
  r.end_raw();
}

}  // namespace

void draw_wave_vector(Renderer& r, const Rect& lane, const PeakCache& cache, unsigned channel, const WaveWindow& window,
                      const WaveStyle& style, ClipRun* clips, unsigned* clip_count) noexcept {
  if (clip_count != nullptr) {
    *clip_count = 0;
  }
  if (!cache.valid() || !cache.built() || lane.w < 1 || lane.h < 1 || lane.w > static_cast<int>(kWaveMaxWidth) ||
      window.span == 0 || channel >= cache.channels()) {
    return;
  }
  const unsigned cols = static_cast<unsigned>(lane.w);
  const unsigned max_v = style.max_vertices < 2 ? 2u : style.max_vertices;
  if (window.span < 2u * cols && window.span + 2u <= 2u * max_v) {
    draw_line(r, lane, cache, channel, window, style);
    return;
  }
  PeakPair peaks[kWaveMaxWidth];
  cache.columns(channel, window.first, window.span, cols, peaks);
  if (clips != nullptr && clip_count != nullptr) {
    *clip_count = WaveBitmap::find_clips(peaks, cols, clips);
  }
  draw_envelope(r, lane, peaks, cols, style);
}

bool draw_wave_bitmap(Renderer& r, const Rect& lane, const WaveBitmap& bitmap, unsigned bitmap_lane,
                      const WaveStyle& style) noexcept {
  if (!bitmap.ready(bitmap_lane)) {
    return false;
  }
  const WaveKey& k = bitmap.shown_key(bitmap_lane);
  const unsigned w = k.width, h = k.height;
  r.begin_raw();
  scissor(r, lane);
  r.raw(dl::bitmap_handle(wave_handle(bitmap_lane)));
  r.raw(dl::bitmap_source(bitmap.shown_addr(bitmap_lane)));
  r.raw(dl::bitmap_layout(k.depth == WaveDepth::L1 ? eve::BitmapFormat::L1 : eve::BitmapFormat::L4,
                          bitmap.shown_stride(bitmap_lane), h));
  r.raw(dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, w, h));  // the low 9 bits of each
  if (w > 511 || h > 511) {
    r.raw(dl::bitmap_size_h(w >> 9, h >> 9));  // and the high two
  }
  r.raw(r.color_word(style.wave));
  r.raw(dl::begin(Primitive::Bitmaps));
  r.raw(dl::cell(0));  // VERTEX2F draws the CELL register's cell, which a tile drawn before this lane may have left anywhere
  r.raw(dl::vertex2f(clamp_q4(static_cast<std::int64_t>(lane.x) * 16), clamp_q4(static_cast<std::int64_t>(lane.y) * 16)));
  r.raw(dl::end());
  r.end_raw();
  return true;
}

void draw_wave_marks(Renderer& r, const Rect& lane, const WaveWindow& window, const WaveMarks& marks,
                     const WaveStyle& style, const ClipRun* clips, unsigned clip_count) noexcept {
  const unsigned cols = static_cast<unsigned>(lane.w < 0 ? 0 : lane.w);
  if (cols == 0 || lane.h < 1 || window.span == 0) {
    return;
  }
  const bool slices = marks.slice_count > 0;
  const bool any = marks.zero_line || (marks.clip_tint && clip_count > 0) || marks.has_selection || marks.has_loop ||
                   marks.has_playhead || slices;
  if (!any) {
    return;
  }
  // x of the column holding `f`, clamped to the lane: a mark outside the view sits on its edge
  const auto col = [&](Frame f) { return static_cast<int>(PeakCache::column_of(window.first, window.span, cols, f)); };
  const auto in_view = [&](Frame f) {
    return f >= window.first && static_cast<std::uint64_t>(f) < static_cast<std::uint64_t>(window.first) + window.span;
  };
  const int right = lane.x + lane.w;
  const auto x_of = [&](Frame f) {
    const int c = col(f);
    return lane.x + (c < lane.w ? c : lane.w - 1);
  };
  const auto span_rect = [&](Frame a, Frame b, Rect& out) {  // the lane's columns from frame a to frame b, inclusive
    if (a > b) {
      const Frame t = a;
      a = b;
      b = t;
    }
    if (static_cast<std::uint64_t>(b) < window.first ||
        a >= window.first + static_cast<std::uint64_t>(window.span)) {
      return false;  // wholly outside
    }
    const int x0 = lane.x + (a < window.first ? 0 : col(a));
    int x1 = lane.x + (in_view(b) ? col(b) : lane.w - 1);
    x1 = x1 < right ? x1 : right - 1;
    out = Rect{x0, lane.y, x1 - x0 + 1, lane.h};
    return x1 >= x0;
  };

  r.begin_raw();
  scissor(r, lane);
  // Translucent group, by token: the zero line, the clip bands, the selection and the loop tint.
  if (marks.zero_line) {
    r.raw(dl::color_a(kAlphaZero));
    r.rule(lane.x, lane.y + static_cast<int>(wave_zero_row(static_cast<unsigned>(lane.h))), lane.w, Axis::Horizontal,
           style.zero);
  }
  if (marks.clip_tint && clip_count > 0) {
    r.raw(dl::color_a(kAlphaClip));
    for (unsigned i = 0; i < clip_count; ++i) {
      r.panel({lane.x + clips[i].x0, lane.y, clips[i].x1 - clips[i].x0 + 1, lane.h}, style.clip);
    }
  }
  Rect loop_rect{0, 0, 0, 0};
  const bool loop_visible = marks.has_loop && span_rect(marks.loop_start, marks.loop_end, loop_rect);
  if (marks.has_selection) {
    Rect sel;
    if (span_rect(marks.sel_begin, marks.sel_end, sel)) {
      r.raw(dl::color_a(kAlphaSelection));
      r.panel(sel, style.selection);
    }
  }
  if (loop_visible) {
    r.raw(dl::color_a(kAlphaLoop));
    r.panel(loop_rect, style.loop);
  }
  r.raw(dl::color_a(255));
  // Opaque group: one-pixel rules, flags, the playhead.
  if (marks.has_selection) {
    const Frame a = marks.sel_begin < marks.sel_end ? marks.sel_begin : marks.sel_end;
    const Frame b = marks.sel_begin < marks.sel_end ? marks.sel_end : marks.sel_begin;
    if (in_view(a)) {
      r.rule(x_of(a), lane.y, lane.h, Axis::Vertical, style.selection);
    }
    if (in_view(b)) {
      r.rule(x_of(b), lane.y, lane.h, Axis::Vertical, style.selection);
    }
  }
  if (marks.has_loop) {
    const Frame a = marks.loop_start < marks.loop_end ? marks.loop_start : marks.loop_end;
    const Frame b = marks.loop_start < marks.loop_end ? marks.loop_end : marks.loop_start;
    if (in_view(a)) {  // the start's flag points inwards (right), the end's (left)
      r.rule(x_of(a), lane.y, lane.h, Axis::Vertical, style.loop);
      r.panel({x_of(a) + 1, lane.y, kFlag, kFlag}, style.loop);
    }
    if (in_view(b)) {
      r.rule(x_of(b), lane.y, lane.h, Axis::Vertical, style.loop);
      r.panel({x_of(b) - kFlag, lane.y, kFlag, kFlag}, style.loop);
    }
  }
  const unsigned n_slices = marks.slice_count < kWaveMaxSlices ? marks.slice_count : kWaveMaxSlices;
  for (unsigned i = 0; i < n_slices; ++i) {
    if (in_view(marks.slices[i])) {
      r.rule(x_of(marks.slices[i]), lane.y, lane.h, Axis::Vertical, style.slice);
    }
  }
  if (marks.has_playhead && in_view(marks.playhead)) {
    const int x = x_of(marks.playhead);
    r.rule(x, lane.y, lane.h, Axis::Vertical, style.playhead);
    r.panel({x - kFlag / 2, lane.y, kFlag + 1, kFlag - 2}, style.playhead);  // the head, centred on the line
  }
  r.end_raw();
}

bool draw_wave(Renderer& r, const Rect& lane, const PeakCache& cache, unsigned channel, const WaveWindow& window,
               WaveMode mode, WaveBitmap* bitmap, unsigned bitmap_lane, std::uint32_t generation, const WaveMarks& marks,
               const WaveStyle& style) noexcept {
  if (!cache.valid() || !cache.built() || lane.w < 1 || lane.h < 1 || lane.w > static_cast<int>(kWaveMaxWidth) ||
      lane.h > static_cast<int>(kWaveMaxHeight) || window.span == 0 || channel >= cache.channels()) {
    return false;
  }
  ClipRun clips[kMaxClipRuns];
  unsigned clip_count = 0;
  bool drawn = false;
  if (mode == WaveMode::Vector) {
    draw_wave_vector(r, lane, cache, channel, window, style, clips, &clip_count);
    drawn = true;
  } else if (bitmap != nullptr && bitmap_lane < kWaveLanes) {
    bitmap->request(bitmap_lane, wave_key(cache, generation, channel, window, lane,
                                          mode == WaveMode::BitmapL1 ? WaveDepth::L1 : WaveDepth::L4));
    drawn = draw_wave_bitmap(r, lane, *bitmap, bitmap_lane, style);
    clip_count = bitmap->shown_clip_count(bitmap_lane);
    for (unsigned i = 0; i < clip_count; ++i) {
      clips[i] = bitmap->shown_clips(bitmap_lane)[i];
    }
  }
  draw_wave_marks(r, lane, window, marks, style, clips, clip_count);
  return drawn;
}

}  // namespace o3sdk::ui::plot
