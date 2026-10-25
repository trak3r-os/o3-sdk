// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Level meters (ADR 0010 amendment 2): peak and RMS bars with peak hold and a clip latch, on a dB scale, vertical or
// horizontal. Levels come from the audio -> main ring as linear amplitudes (peak and RMS of the last block, 1.0 = full
// scale); the meter state holds the ballistics at the 30 Hz tier. A bar is RECTS only, about 4 words with its share of the
// colour passes: the RMS body (Play, the full width of the bar), the peak above it as a narrower bar in the same colour (half
// the width, centred: told from the body by its shape, because the palette has no second green that contrasts with Play),
// a two-pixel hold tick (Text), and a clip lamp (Accent, the top of the bar) that stays lit for kClipHoldCalls updates after a
// sample reached full scale. The palette has no red (ADR 0001 caps it at 16 tokens): the one orange is the cursor's, the
// record-arm's and, by the maintainer's decision, the clip's.
//
// The dB scale is a LUT, DbScale: the amplitude's float bits index a table (16 steps an octave: 0.53 dB wide at the bottom of an
// octave, 0.28 at the top), generated at init
// with log10f into the object (480 bytes), so per frame a bar is a bit extraction and a read, never a logarithm.
// No heap. Main loop only.

#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui::plot {

class DbScale {
 public:
  static constexpr unsigned kStepsPerOctave = 16;
  static constexpr int kMinExp = -14;   // below 2^-14 (-84 dB) a level is the floor
  static constexpr int kMaxExp = 0;     // 2^0 .. 2^1 is the top octave: +0 .. +6 dB
  static constexpr unsigned kEntries = static_cast<unsigned>(kMaxExp - kMinExp + 1) * kStepsPerOctave;

  // The scale runs from floor_db (position 0) to ceil_db (position 65535), floor_db < ceil_db.
  void init(float floor_db, float ceil_db) noexcept;
  // Position of a linear amplitude on the scale, 0 .. 65535: 0 at or below the floor, 65535 at or above the ceiling. The
  // error is at most half a step: 0.27 dB (at the bottom of an octave), 0.14 dB at the top.
  std::uint16_t position(float amplitude) const noexcept;
  // Position of a level in dB (no table: for the marks of a scale).
  std::uint16_t position_db(float db) const noexcept;
  float floor_db() const noexcept { return floor_; }
  float ceil_db() const noexcept { return ceil_; }

 private:
  float floor_ = -60.0f;
  float ceil_ = 0.0f;
  std::uint16_t lut_[kEntries] = {};
};

// A reading of one block: the largest |sample| and the RMS, both as linear amplitudes (1.0 = full scale).
struct MeterReading {
  float peak = 0.0f;
  float rms = 0.0f;
};

class MeterState {
 public:
  // Per update (30 Hz): the peak bar falls with kPeakRelease a call, the RMS bar follows with kRmsAttack / kRmsRelease, a
  // louder peak is held for kHoldCalls and then falls by kHoldFall a call, a peak at full scale lights the clip lamp.
  static constexpr float kPeakRelease = 0.88f;
  static constexpr float kRmsAttack = 0.7f;
  static constexpr float kRmsRelease = 0.82f;
  static constexpr unsigned kHoldCalls = 30;
  static constexpr float kHoldFall = 0.97f;
  static constexpr unsigned kClipHoldCalls = 60;
  static constexpr float kClipLevel = 0.9999f;

  void update(const MeterReading& in) noexcept;
  void reset() noexcept { *this = MeterState{}; }
  void clear_clip() noexcept { clip_calls_ = 0; }

  float rms() const noexcept { return rms_; }
  float peak() const noexcept { return peak_; }
  float hold() const noexcept { return hold_; }
  bool clipping() const noexcept { return clip_calls_ > 0; }

 private:
  float rms_ = 0.0f;
  float peak_ = 0.0f;
  float hold_ = 0.0f;
  unsigned hold_calls_ = 0;
  unsigned clip_calls_ = 0;
};

struct MeterStyle {
  Color frame = Color::Empty;
  Color slot = Color::Pane;
  Color rms = Color::Play;
  Color peak = Color::Play;
  Color hold = Color::Text;
  Color clip = Color::Accent;
  Color scale = Color::TextDim;  // the labels
  Color mark = Color::Empty;     // the marks of the scale
};

// `count` meters in `rects` (each a bar of its own length and width, the level running from the bottom up for a Vertical
// bar and from the left for a Horizontal one), one `states` entry each, drawn in colour passes (all the frames, all the
// slots, ...) so that a bank costs 2 words a rectangle and 5 colour changes in all. A one-pixel frame is drawn round
// each rectangle; the bar is inside it.
void draw_meters(Renderer& r, const Rect* rects, const MeterState* states, unsigned count, Axis orientation,
                 const DbScale& scale, const MeterStyle& style) noexcept;

// The marks of a scale beside a bar: a three-pixel rule and a Micro-face label at each of `marks_db` (levels in dB). `bar`
// is the rectangle the marks line up with; they are drawn on its left (Vertical) or below it (Horizontal), outside it.
void draw_meter_scale(Renderer& r, const Rect& bar, Axis orientation, const DbScale& scale, const float* marks_db,
                      unsigned count, const MeterStyle& style) noexcept;

// Words draw_meters adds at most, for any colours of the style: six colour changes (frame, slot, body, peak, hold, lamp: each a
// pass of its own), the BEGIN of the rectangles, and for each meter a frame, a slot, a body, a peak, a hold tick and a lamp,
// two vertex words each. A bank of n meters is exactly this when every meter has every part and the tokens differ.
constexpr unsigned meters_max_words(unsigned count) noexcept { return 7u + 12u * count; }

// Words draw_meter_scale adds at most: the rules' colour and BEGIN, 2 words a rule, the labels' colour and, a label being at most
// kMeterLabelChars characters (format_db), 3 words a character (a glyph past x = 511 without VERTEX_TRANSLATE_X is a handle, a
// cell and a vertex; with it one word and, at most, one move of the origin).
inline constexpr unsigned kMeterLabelChars = 5;
constexpr unsigned meter_scale_max_words(unsigned marks) noexcept { return 3u + marks * (2u + 3u * kMeterLabelChars); }

}  // namespace o3sdk::ui::plot
