// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/meters.h>

#include <o3sdk/ui/components.h>
#include <o3sdk/ui/plot/plot_math.h>

#include <cmath>
#include <cstring>

namespace o3sdk::ui::plot {

namespace {

constexpr float kLog2To10 = 0.30102999566f;

std::uint16_t pos_of_db(float db, float floor_db, float ceil_db) noexcept {
  const float t = (db - floor_db) / (ceil_db - floor_db);
  const float v = t * 65535.0f + 0.5f;
  return v <= 0.0f ? 0 : (v >= 65535.0f ? 65535 : static_cast<std::uint16_t>(v));
}

}  // namespace

void DbScale::init(float floor_db, float ceil_db) noexcept {
  floor_ = floor_db;
  ceil_ = ceil_db > floor_db ? ceil_db : floor_db + 1.0f;
  for (unsigned i = 0; i < kEntries; ++i) {
    // the entry for exponent e = kMinExp + i / 16 and mantissa step m = i % 16 stands for the middle of its step:
    // (1 + (m + 0.5) / 16) * 2^e
    const int e = kMinExp + static_cast<int>(i / kStepsPerOctave);
    const float m = static_cast<float>(i % kStepsPerOctave) + 0.5f;
    const float db = 20.0f * kLog2To10 * (static_cast<float>(e) + log2_fast(1.0f + m / static_cast<float>(kStepsPerOctave)));
    lut_[i] = pos_of_db(db, floor_, ceil_);
  }
}

std::uint16_t DbScale::position(float amplitude) const noexcept {
  if (!(amplitude > 0.0f)) {
    return 0;
  }
  std::uint32_t bits;
  std::memcpy(&bits, &amplitude, sizeof(bits));  // IEEE 754 single: exponent in [30:23], mantissa in [22:0]
  const int e = static_cast<int>((bits >> 23) & 0xFFu) - 127;
  if (e < kMinExp) {
    return 0;
  }
  if (e > kMaxExp) {
    return lut_[kEntries - 1];  // above 2.0: the top step (the ceiling is at or below +6 dB)
  }
  const unsigned m = (bits >> (23 - 4)) & 0xFu;  // the top four bits of the mantissa
  return lut_[static_cast<unsigned>(e - kMinExp) * kStepsPerOctave + m];
}

std::uint16_t DbScale::position_db(float db) const noexcept { return pos_of_db(db, floor_, ceil_); }

void MeterState::update(const MeterReading& in) noexcept {
  const float peak_in = in.peak < 0.0f ? 0.0f : in.peak;
  const float rms_in = in.rms < 0.0f ? 0.0f : in.rms;
  rms_ += (rms_in - rms_) * (rms_in > rms_ ? kRmsAttack : 1.0f - kRmsRelease);
  peak_ = peak_in > peak_ * kPeakRelease ? peak_in : peak_ * kPeakRelease;
  if (peak_in >= hold_) {
    hold_ = peak_in;
    hold_calls_ = kHoldCalls;
  } else if (hold_calls_ > 0) {
    --hold_calls_;
  } else {
    hold_ *= kHoldFall;
    hold_ = hold_ < peak_ ? peak_ : hold_;
  }
  if (peak_in >= kClipLevel) {
    clip_calls_ = kClipHoldCalls;
  } else if (clip_calls_ > 0) {
    --clip_calls_;
  }
}

namespace {

// The pixels of a bar `length` of `extent` long for position `pos` (0..65535).
int len_of(std::uint16_t pos, int extent) noexcept {
  return static_cast<int>((static_cast<std::uint32_t>(pos) * static_cast<std::uint32_t>(extent) + 32768u) / 65535u);
}

// The rectangle of the bar part [from, to) of a meter's inside `in`, measured along its length from the zero end; `narrow`
// takes the middle half of its width (at least two pixels, or the whole width if it has less): the peak above the body.
Rect part(const Rect& in, Axis orientation, int from, int to, bool narrow = false) noexcept {
  if (orientation == Axis::Vertical) {  // grows upwards from the bottom
    int w = narrow ? in.w / 2 : in.w;
    w = narrow && w < 2 ? (in.w < 2 ? in.w : 2) : w;
    return {in.x + (in.w - w) / 2, in.y + in.h - to, w, to - from};
  }
  int h = narrow ? in.h / 2 : in.h;
  h = narrow && h < 2 ? (in.h < 2 ? in.h : 2) : h;
  return {in.x + from, in.y + (in.h - h) / 2, to - from, h};
}

}  // namespace

void draw_meters(Renderer& r, const Rect* rects, const MeterState* states, unsigned count, Axis orientation,
                 const DbScale& scale, const MeterStyle& style) noexcept {
  const int b = r.theme().metrics.border;
  const auto inside = [&](unsigned i) {
    return Rect{rects[i].x + b, rects[i].y + b, rects[i].w - 2 * b, rects[i].h - 2 * b};
  };
  const auto extent = [&](const Rect& in) { return orientation == Axis::Vertical ? in.h : in.w; };
  for (unsigned i = 0; i < count; ++i) {
    r.panel(rects[i], style.frame);
  }
  for (unsigned i = 0; i < count; ++i) {
    r.panel(inside(i), style.slot);
  }
  for (unsigned i = 0; i < count; ++i) {  // the RMS body
    const Rect in = inside(i);
    const int rms = len_of(scale.position(states[i].rms()), extent(in));
    if (rms > 0) {
      r.panel(part(in, orientation, 0, rms), style.rms);
    }
  }
  for (unsigned i = 0; i < count; ++i) {  // the peak, above the body
    const Rect in = inside(i);
    const int rms = len_of(scale.position(states[i].rms()), extent(in));
    const int peak = len_of(scale.position(states[i].peak()), extent(in));
    if (peak > rms) {
      r.panel(part(in, orientation, rms, peak, true), style.peak);
    }
  }
  for (unsigned i = 0; i < count; ++i) {  // the hold tick: two pixels on the hold level
    const Rect in = inside(i);
    const int len = extent(in);
    const int hold = len_of(scale.position(states[i].hold()), len);
    if (hold > 0) {
      const int to = hold < len ? hold + 1 : len;  // the tick sits on the hold level, its top pixel just past it
      r.panel(part(in, orientation, to >= 2 ? to - 2 : 0, to), style.hold);
    }
  }
  for (unsigned i = 0; i < count; ++i) {  // the clip lamp: the end of the bar (a pass of its own, so that the colours do not alternate)
    if (states[i].clipping()) {
      const Rect in = inside(i);
      const int len = extent(in);
      const int lamp = len >= 8 ? 3 : 1;
      r.panel(part(in, orientation, len - lamp, len), style.clip);
    }
  }
}

void draw_meter_scale(Renderer& r, const Rect& bar, Axis orientation, const DbScale& scale, const float* marks_db,
                      unsigned count, const MeterStyle& style) noexcept {
  const Theme& t = r.theme();
  const FontGeometry micro = t.geometry(Face::Micro);
  const int b = t.metrics.border;
  const Rect in{bar.x + b, bar.y + b, bar.w - 2 * b, bar.h - 2 * b};
  char text[kLabelBuffer];
  for (unsigned i = 0; i < count; ++i) {
    const std::uint16_t pos = scale.position_db(marks_db[i]);
    const int len = orientation == Axis::Vertical ? in.h : in.w;
    const int at = len_of(pos, len);
    const int clamped = at >= len ? len - 1 : at;
    if (orientation == Axis::Vertical) {
      const int y = in.y + in.h - 1 - clamped;
      r.rule(bar.x - 4, y, 3, Axis::Horizontal, style.mark);
    } else {
      const int x = in.x + clamped;
      r.rule(x, bar.y + bar.h + 1, 3, Axis::Vertical, style.mark);
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    const std::uint16_t pos = scale.position_db(marks_db[i]);
    const int len = orientation == Axis::Vertical ? in.h : in.w;
    const int at = len_of(pos, len);
    const int clamped = at >= len ? len - 1 : at;
    format_db(marks_db[i], text);
    if (orientation == Axis::Vertical) {
      const int y = in.y + in.h - 1 - clamped;
      r.text(bar.x - 6 - r.text_width(text, Face::Micro), caps_y(t, Face::Micro, y - micro.cap_h / 2, micro.cap_h), text,
             style.scale, Face::Micro);
    } else {
      const int x = in.x + clamped;
      r.text(x - r.text_width(text, Face::Micro) / 2, bar.y + bar.h + 5, text, style.scale, Face::Micro);
    }
  }
}

}  // namespace o3sdk::ui::plot
