// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The XY scope / Lissajous plot (ADR 0010 amendment 2): a stereo block as a trace in a square, left against right (or, as
// a goniometer, mid against side, which turns the picture 45 degrees so that a mono signal is a vertical line and a phase
// problem is a horizontal one), with a short phosphor trail: the last kGenerations blocks stay on screen, each dimmer than the
// one after (COLOR_A decaying 255, 128, 68, 34). A block is decimated to at most kMaxPoints points (a uniform stride, so the
// trace keeps its shape) when it arrives, and drawn with fewer the older it is (kAgePoints: 96, 48, 32, 32: a faint trail
// does not need more), so what is drawn is bounded whatever the block size: 208 vertices, the newest block's glow (96 more)
// and a graticule (a frame, a crosshair, a ring of 32 segments), POINTS or a LINE_STRIP per generation: under 400 words.
//
// It is fed from the audio -> main ring at the 20 to 30 Hz tier, never from the audio buffers (ADR 0010). The state is
// 4 x 96 points of 4 bytes. No heap. Main loop only.

#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui::plot {

enum class ScopeMode : std::uint8_t { LeftRight, MidSide };

struct ScopeStyle {
  Color trace = Color::Play;  // phosphor green
  Color graticule = Color::Empty;
  Color axes = Color::Empty;  // the unit ring
  bool lines = false;         // false: a dot a point, true: a line through them
  bool glow = true;           // a faint wide copy under the newest block: the phosphor's glow (one more strip of points)
  std::uint8_t dot_radius_q4 = 22;
  std::uint8_t line_width_q4 = 8;
};

class Scope {
 public:
  static constexpr unsigned kMaxPoints = 96;  // per generation, as stored; the newest is drawn with all of them
  static constexpr unsigned kGenerations = 4;
  // Points drawn of a generation of age 0 (the newest) .. 3: at most this many, a uniform stride over what is stored.
  static constexpr unsigned kAgePoints[kGenerations] = {96, 48, 32, 32};

  void clear() noexcept;
  // One block of `frames` interleaved stereo frames (L, R int16), a new generation; the oldest falls off. A block of less
  // than two frames is ignored.
  void push(const std::int16_t* lr, std::size_t frames, ScopeMode mode) noexcept;
  unsigned generations() const noexcept { return count_; }
  // Points of generation `age` (0 = the newest): x and y in -32768 .. 32767, y up.
  unsigned points(unsigned age, const std::int16_t*& x, const std::int16_t*& y) const noexcept;

  // Draws the graticule and the trails in `area` (the picture is a square of the smaller side, centred): full scale is the
  // edge of the square.
  void draw(Renderer& r, const Rect& area, const ScopeStyle& style) const noexcept;

 private:
  std::int16_t x_[kGenerations][kMaxPoints] = {};
  std::int16_t y_[kGenerations][kMaxPoints] = {};
  std::uint8_t n_[kGenerations] = {};
  unsigned head_ = 0;   // the slot of the newest generation
  unsigned count_ = 0;  // generations held
};

// Words Scope::draw adds at most: the graticule (a frame of four rules, the crosshair: 6 rules of two words and their
// colour), the raw section (2, its scissor 2, and 1 if the renderer had moved its origin), the ring (colour, width, BEGIN, 33
// vertices, END), the trace's colour and size, then per generation COLOR_A, BEGIN, its points and END, and the glow's strip
// for the newest (COLOR_A, size, BEGIN, the points, END, size: 5 and the points).
constexpr unsigned scope_max_words() noexcept {
  return (6u * 2u + 2u) + 5u + (4u + 33u) + 2u + (3u + Scope::kAgePoints[0]) + (3u + Scope::kAgePoints[1]) +
         (3u + Scope::kAgePoints[2]) + (3u + Scope::kAgePoints[3]) + (5u + Scope::kAgePoints[0]);
}

}  // namespace o3sdk::ui::plot
