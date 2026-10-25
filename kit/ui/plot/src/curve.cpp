// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/curve.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/components.h>

namespace o3sdk::ui::plot {

namespace {

namespace dl = eve::dl;
using eve::Primitive;

// Mapped points are clamped inside int16 and VERTEX2F's +-1023 px (+-16368 in 1/16 px), a margin beyond the panel's 800 px: a
// value far off the axis (a -200 dB floor) only tilts the segment that leaves the plot, which the scissor cuts off.
constexpr float kClampQ4 = 16000.0f;
std::int16_t to_q4(float v) noexcept {
  return static_cast<std::int16_t>(v < -kClampQ4 ? -kClampQ4 : (v > kClampQ4 ? kClampQ4 : v));
}

float value_y(const CurveSeries& s, std::uint32_t i) noexcept {
  return s.y != nullptr ? s.y[i] : static_cast<float>(s.y_q15[i]) * (1.0f / 32768.0f);
}
float value_x(const CurveSeries& s, std::uint32_t i) noexcept {
  if (s.x != nullptr) {
    return s.x[i];
  }
  return s.n < 2 ? s.x_lo : s.x_lo + (s.x_hi - s.x_lo) * (static_cast<float>(i) / static_cast<float>(s.n - 1));
}

// Where a data point lands, in 1/16 px: the centre of the pixel column / row of its axis position.
void map_point(const Rect& area, const AxisSpec& xa, const AxisSpec& ya, float x, float y, std::int16_t& qx,
               std::int16_t& qy) noexcept {
  const float tx = axis_t(xa, x), ty = axis_t(ya, y);
  qx = to_q4(static_cast<float>(area.x) * 16.0f + 8.0f + tx * static_cast<float>(area.w - 1) * 16.0f);
  qy = to_q4(static_cast<float>(area.y) * 16.0f + 8.0f + (1.0f - ty) * static_cast<float>(area.h - 1) * 16.0f);
}

// The series as points in the scratch: all of them when they fit, else (more points than columns) one a pixel column, the
// one that lies farthest from the previous in y so that a spike is not lost. x must rise. Returns how many.
unsigned map_series(const Rect& area, const AxisSpec& xa, const AxisSpec& ya, const CurveSeries& s,
                    CurveScratch& sc) noexcept {
  const bool reduce = s.n > kCurveMaxPoints || s.n > static_cast<std::uint32_t>(area.w);
  unsigned m = 0;
  if (!reduce) {
    for (std::uint32_t i = 0; i < s.n; ++i) {
      map_point(area, xa, ya, value_x(s, i), value_y(s, i), sc.px[m], sc.py[m]);
      ++m;
    }
    return m;
  }
  bool have = false;
  std::int16_t cand_x = 0, cand_y = 0;
  int column = 0;
  int last_y = 0;
  for (std::uint32_t i = 0; i < s.n; ++i) {
    std::int16_t qx, qy;
    map_point(area, xa, ya, value_x(s, i), value_y(s, i), qx, qy);
    const int col = qx >> 4;
    if (have && col != column) {  // the column ends: its candidate becomes a point
      if (m >= kCurveMaxPoints) {
        return m;
      }
      sc.px[m] = cand_x;
      sc.py[m] = cand_y;
      last_y = cand_y;
      ++m;
      have = false;
    }
    if (!have) {
      cand_x = qx;
      cand_y = qy;
      column = col;
      have = true;
    } else {
      const int d_new = qy > last_y ? qy - last_y : last_y - qy;
      const int d_old = cand_y > last_y ? cand_y - last_y : last_y - cand_y;
      if (d_new >= d_old) {
        cand_x = qx;
        cand_y = qy;
      }
    }
    if (m == 0 && have) {
      last_y = cand_y;  // before the first column ends there is nothing to be far from
    }
  }
  if (have && m < kCurveMaxPoints) {
    sc.px[m] = cand_x;
    sc.py[m] = cand_y;
    ++m;
  }
  return m;
}

// Squared distance, in (1/16 px)^2, of point `p` from the chord `a`-`b`.
float chord_error2(const std::int16_t* px, const std::int16_t* py, unsigned a, unsigned b, unsigned p) noexcept {
  const float ax = px[a], ay = py[a];
  const float dx = static_cast<float>(px[b]) - ax, dy = static_cast<float>(py[b]) - ay;
  const float ex = static_cast<float>(px[p]) - ax, ey = static_cast<float>(py[p]) - ay;
  const float len2 = dx * dx + dy * dy;
  if (len2 < 0.5f) {
    return ex * ex + ey * ey;  // a closed chord: the distance to its point
  }
  const float cross = dx * ey - dy * ex;
  return cross * cross / len2;
}

struct Segment {
  std::uint16_t lo;
  std::uint16_t hi;
  std::uint16_t far;  // the interior point farthest from the chord
  float err2;
};

Segment measure(const std::int16_t* px, const std::int16_t* py, unsigned lo, unsigned hi) noexcept {
  Segment s{static_cast<std::uint16_t>(lo), static_cast<std::uint16_t>(hi), static_cast<std::uint16_t>(lo), 0.0f};
  for (unsigned p = lo + 1; p < hi; ++p) {
    const float e = chord_error2(px, py, lo, hi, p);
    if (e > s.err2) {
      s.err2 = e;
      s.far = static_cast<std::uint16_t>(p);
    }
  }
  return s;
}

void scissor(Renderer& r, const Rect& a) noexcept {
  r.raw(dl::scissor_xy(static_cast<std::uint32_t>(a.x < 0 ? 0 : a.x), static_cast<std::uint32_t>(a.y < 0 ? 0 : a.y)));
  r.raw(dl::scissor_size(static_cast<std::uint32_t>(a.w < 0 ? 0 : a.w), static_cast<std::uint32_t>(a.h < 0 ? 0 : a.h)));
}

}  // namespace

unsigned simplify_polyline(const std::int16_t* px, const std::int16_t* py, unsigned n, unsigned max_keep,
                           float tolerance_q4, std::uint16_t* keep) noexcept {
  if (n == 0) {
    return 0;
  }
  if (n == 1 || max_keep < 2) {
    keep[0] = 0;
    return 1;
  }
  max_keep = max_keep > kCurveMaxVertices ? kCurveMaxVertices : max_keep;
  Segment segs[kCurveMaxVertices];  // the stretches between kept points that still have interior points
  unsigned ns = 0;
  unsigned m = 2;
  keep[0] = 0;
  keep[1] = static_cast<std::uint16_t>(n - 1);
  if (n > 2) {
    segs[ns++] = measure(px, py, 0, n - 1);
  }
  const float tol2 = tolerance_q4 * tolerance_q4;
  while (m < max_keep && ns > 0) {
    unsigned worst = 0;
    for (unsigned i = 1; i < ns; ++i) {
      worst = segs[i].err2 > segs[worst].err2 ? i : worst;
    }
    if (segs[worst].err2 <= tol2) {
      break;  // every stretch is within the tolerance
    }
    const Segment s = segs[worst];
    // keep the farthest point: insert it among the kept ones, in order
    unsigned at = m;
    while (at > 0 && keep[at - 1] > s.far) {
      keep[at] = keep[at - 1];
      --at;
    }
    keep[at] = s.far;
    ++m;
    // the stretch splits in two; a half with no interior point is done
    segs[worst] = segs[ns - 1];
    --ns;
    if (s.far - s.lo > 1) {
      segs[ns++] = measure(px, py, s.lo, s.far);
    }
    if (s.hi - s.far > 1) {
      segs[ns++] = measure(px, py, s.far, s.hi);
    }
  }
  return m;
}

void draw_axes(Renderer& r, const Rect& area, const AxisSpec& xa, const AxisSpec& ya, const CurveStyle& style) noexcept {
  if (area.w < 2 || area.h < 2) {
    return;
  }
  const Theme& theme = r.theme();
  Tick xt[kCurveMaxTicks], yt[kCurveMaxTicks];
  const unsigned nx = make_ticks(xa, static_cast<unsigned>(area.w) / 56u, xt, kCurveMaxTicks);
  const unsigned ny = make_ticks(ya, static_cast<unsigned>(area.h) / 28u, yt, kCurveMaxTicks);
  const auto x_px = [&](float v) {
    return area.x + static_cast<int>(axis_t(xa, v) * static_cast<float>(area.w - 1) + 0.5f);
  };
  const auto y_px = [&](float v) {
    return area.y + static_cast<int>((1.0f - axis_t(ya, v)) * static_cast<float>(area.h - 1) + 0.5f);
  };
  // the grid, then the frame over its ends, then the reference line of a dB axis
  for (unsigned i = 0; i < nx; ++i) {
    r.rule(x_px(xt[i].value), area.y, area.h, Axis::Vertical, style.grid);
  }
  for (unsigned i = 0; i < ny; ++i) {
    if (!(ya.kind == AxisKind::Db && yt[i].value == 0.0f)) {
      r.rule(area.x, y_px(yt[i].value), area.w, Axis::Horizontal, style.grid);
    }
  }
  r.frame({area.x - 1, area.y - 1, area.w + 2, area.h + 2}, style.frame);
  if (ya.kind == AxisKind::Db && ya.lo < 0.0f && ya.hi > 0.0f) {
    r.rule(area.x, y_px(0.0f), area.w, Axis::Horizontal, style.reference);
  }
  // Labels, in the Micro face: every tick when there is room, else the major ones.
  const FontGeometry micro = theme.geometry(Face::Micro);
  const bool all_x = nx * 28u <= static_cast<unsigned>(area.w);
  const bool all_y = ny * 14u <= static_cast<unsigned>(area.h);
  char text[kLabelBuffer];
  for (unsigned i = 0; i < nx; ++i) {
    if (all_x || xt[i].major) {
      if (format_tick(xa.kind, xt[i].value, text) > kCurveLabelChars) {
        continue;  // wider than the room a label has (the ticks are 56 px apart): not drawn
      }
      r.text(x_px(xt[i].value) - r.text_width(text, Face::Micro) / 2, area.y + area.h + 3 + 1, text, style.label, Face::Micro);
    }
  }
  for (unsigned i = 0; i < ny; ++i) {
    if (all_y || yt[i].major) {
      if (format_tick(ya.kind, yt[i].value, text) > kCurveLabelChars) {
        continue;
      }
      const int y = y_px(yt[i].value);
      r.text(area.x - 4 - r.text_width(text, Face::Micro), caps_y(theme, Face::Micro, y - micro.cap_h / 2, micro.cap_h), text,
             style.label, Face::Micro);
    }
  }
}

unsigned draw_series(Renderer& r, const Rect& area, const AxisSpec& xa, const AxisSpec& ya, const CurveSeries& series,
                     bool fill, const CurveStyle& style, CurveScratch& scratch) noexcept {
  if (area.w < 2 || area.h < 2 || series.n < 2 || (series.y == nullptr && series.y_q15 == nullptr)) {
    return 0;
  }
  const unsigned m = map_series(area, xa, ya, series, scratch);
  if (m < 2) {
    return 0;
  }
  unsigned max_keep = style.max_vertices < 2 ? 2u : style.max_vertices;
  max_keep = max_keep > kCurveMaxVertices ? kCurveMaxVertices : max_keep;
  const unsigned k = simplify_polyline(scratch.px, scratch.py, m, max_keep, style.tolerance_px * 16.0f, scratch.keep);
  r.begin_raw();
  scissor(r, area);
  if (fill) {
    r.raw(dl::color_a(style.fill_alpha));
    r.raw(r.color_word(style.fill));
    r.raw(dl::line_width(8));
    r.raw(dl::begin(Primitive::EdgeStripB));
    for (unsigned i = 0; i < k; ++i) {
      r.raw(dl::vertex2f(scratch.px[scratch.keep[i]], scratch.py[scratch.keep[i]]));
    }
    r.raw(dl::end());
    r.raw(dl::color_a(255));
  }
  r.raw(r.color_word(style.line));
  r.raw(dl::line_width(style.line_width_q4));
  r.raw(dl::begin(Primitive::LineStrip));
  for (unsigned i = 0; i < k; ++i) {
    r.raw(dl::vertex2f(scratch.px[scratch.keep[i]], scratch.py[scratch.keep[i]]));
  }
  r.raw(dl::end());
  r.end_raw();
  return k;
}

void draw_handles(Renderer& r, const Rect& area, const AxisSpec& xa, const AxisSpec& ya, const CurveHandle* handles,
                  unsigned count, const CurveStyle& style) noexcept {
  if (handles == nullptr || count == 0 || area.w < 2 || area.h < 2) {
    return;
  }
  count = count > kCurveMaxHandles ? kCurveMaxHandles : count;
  const std::uint32_t radius = style.handle_radius;
  std::int16_t qx[kCurveMaxHandles], qy[kCurveMaxHandles];
  for (unsigned i = 0; i < count; ++i) {
    map_point(area, xa, ya, handles[i].x, handles[i].y, qx[i], qy[i]);
  }
  const int grow = static_cast<int>(radius) + 2;  // a handle on the plot's edge is whole
  r.begin_raw();
  scissor(r, {area.x - grow, area.y - grow, area.w + 2 * grow, area.h + 2 * grow});
  r.raw(r.color_word(style.handle_ring));  // the rings first, so that no ring covers another handle
  r.raw(dl::point_size((radius + 2u) * 16u));
  r.raw(dl::begin(Primitive::Points));
  for (unsigned i = 0; i < count; ++i) {
    r.raw(dl::vertex2f(qx[i], qy[i]));
  }
  r.raw(dl::end());
  r.raw(r.color_word(style.handle));
  r.raw(dl::point_size(radius * 16u));
  r.raw(dl::begin(Primitive::Points));
  for (unsigned i = 0; i < count; ++i) {
    if (!handles[i].active) {
      r.raw(dl::vertex2f(qx[i], qy[i]));
    }
  }
  r.raw(dl::end());
  r.raw(r.color_word(style.handle_active));
  r.raw(dl::point_size(radius * 16u + 16u));  // the active one a pixel bigger
  r.raw(dl::begin(Primitive::Points));
  for (unsigned i = 0; i < count; ++i) {
    if (handles[i].active) {
      r.raw(dl::vertex2f(qx[i], qy[i]));
    }
  }
  r.raw(dl::end());
  r.end_raw();
}

}  // namespace o3sdk::ui::plot
