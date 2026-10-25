// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/scope.h>

#include <o3sdk/eve/dl.h>

#include <cmath>

namespace o3sdk::ui::plot {

namespace {

namespace dl = eve::dl;
using eve::Primitive;

constexpr std::uint8_t kAlpha[Scope::kGenerations] = {255, 128, 68, 34};
constexpr unsigned kRingSegments = 32;
constexpr float kTwoPi = 6.28318530717959f;

std::int16_t clamp16(int v) noexcept { return static_cast<std::int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

}  // namespace

void Scope::clear() noexcept {
  head_ = 0;
  count_ = 0;
  for (unsigned g = 0; g < kGenerations; ++g) {
    n_[g] = 0;
  }
}

void Scope::push(const std::int16_t* lr, std::size_t frames, ScopeMode mode) noexcept {
  if (lr == nullptr || frames < 2) {
    return;
  }
  head_ = count_ == 0 ? 0u : (head_ + 1u) % kGenerations;
  count_ = count_ < kGenerations ? count_ + 1u : kGenerations;
  const std::size_t n = frames < kMaxPoints ? frames : kMaxPoints;
  n_[head_] = static_cast<std::uint8_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    // a uniform stride over the block: point i is the frame at i * frames / n, so the trace keeps its shape
    const std::size_t f = i * frames / n;
    const int l = lr[2 * f], rr = lr[2 * f + 1];
    if (mode == ScopeMode::LeftRight) {
      x_[head_][i] = static_cast<std::int16_t>(l);
      y_[head_][i] = static_cast<std::int16_t>(rr);
    } else {  // mid / side: x = (R - L) / 2, y = (L + R) / 2, so that mono is vertical
      x_[head_][i] = clamp16((rr - l) / 2);
      y_[head_][i] = clamp16((l + rr) / 2);
    }
  }
}

unsigned Scope::points(unsigned age, const std::int16_t*& x, const std::int16_t*& y) const noexcept {
  if (age >= count_) {
    x = nullptr;
    y = nullptr;
    return 0;
  }
  const unsigned slot = (head_ + kGenerations - age) % kGenerations;
  x = x_[slot];
  y = y_[slot];
  return n_[slot];
}

void Scope::draw(Renderer& r, const Rect& area, const ScopeStyle& style) const noexcept {
  const int side = area.w < area.h ? area.w : area.h;
  if (side < 8) {
    return;
  }
  const Rect sq{area.x + (area.w - side) / 2, area.y + (area.h - side) / 2, side, side};
  // The graticule: a frame, the crosshair, ticks in the corners' diagonals are left out (the ring does it).
  r.frame({sq.x - 1, sq.y - 1, sq.w + 2, sq.h + 2}, style.graticule);
  r.rule(sq.x + sq.w / 2, sq.y, sq.h, Axis::Vertical, style.graticule);
  r.rule(sq.x, sq.y + sq.h / 2, sq.w, Axis::Horizontal, style.graticule);
  const float cx = static_cast<float>(sq.x) + static_cast<float>(sq.w) * 0.5f;
  const float cy = static_cast<float>(sq.y) + static_cast<float>(sq.h) * 0.5f;
  const float radius = static_cast<float>(side) * 0.5f - 1.0f;  // full scale is the edge of the square
  r.begin_raw();
  r.raw(dl::scissor_xy(static_cast<std::uint32_t>(sq.x), static_cast<std::uint32_t>(sq.y)));
  r.raw(dl::scissor_size(static_cast<std::uint32_t>(sq.w), static_cast<std::uint32_t>(sq.h)));
  // the unit ring: a polygon of 32 segments, in Q4, at full scale along the axes
  r.raw(r.color_word(style.axes));
  r.raw(dl::line_width(8));
  r.raw(dl::begin(Primitive::LineStrip));
  for (unsigned i = 0; i <= kRingSegments; ++i) {
    const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kRingSegments);
    r.raw(dl::vertex2f(static_cast<std::int32_t>((cx + radius * std::cos(a)) * 16.0f),
                       static_cast<std::int32_t>((cy + radius * std::sin(a)) * 16.0f)));
  }
  r.raw(dl::end());
  // the trails, oldest first so that the newest lies on top
  r.raw(r.color_word(style.trace));
  const std::uint32_t crisp = style.lines ? style.line_width_q4 : style.dot_radius_q4;
  const auto size = [&](std::uint32_t v) { r.raw(style.lines ? dl::line_width(v) : dl::point_size(v)); };
  const auto strip = [&](unsigned age) {
    const std::int16_t *px, *py;
    const unsigned stored = points(age, px, py);
    const unsigned n = stored < kAgePoints[age] ? stored : kAgePoints[age];  // an older block is drawn with fewer points
    r.raw(dl::begin(style.lines ? Primitive::LineStrip : Primitive::Points));
    for (unsigned i = 0; i < n; ++i) {
      const unsigned k = i * stored / n;  // a uniform stride over the stored points
      r.raw(dl::vertex2f(static_cast<std::int32_t>((cx + static_cast<float>(px[k]) * (1.0f / 32768.0f) * radius) * 16.0f),
                         static_cast<std::int32_t>((cy - static_cast<float>(py[k]) * (1.0f / 32768.0f) * radius) * 16.0f)));
    }
    r.raw(dl::end());
  };
  size(crisp);
  for (unsigned i = count_; i > 0; --i) {
    const unsigned age = i - 1u;
    if (age == 0 && style.glow) {  // a faint wide copy under the newest block
      r.raw(dl::color_a(0x34));
      size(crisp * 3u);
      strip(0);
      size(crisp);
    }
    r.raw(dl::color_a(kAlpha[age]));
    strip(age);
  }
  r.end_raw();
}

}  // namespace o3sdk::ui::plot
