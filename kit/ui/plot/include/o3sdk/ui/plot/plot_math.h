// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure math under the plots (ADR 0010 amendment 2): axis mapping (linear, logarithmic frequency, dB), decibel
// conversions, the magnitude response of a biquad, tick placement and number formatting for the tick labels. No drawing
// and no state: everything here is single precision, allocation-free and host-tested (tests/plot_math_test.cpp).

#include <cstddef>
#include <cstdint>

namespace o3sdk::ui::plot {

// What an axis measures. Linear and Db map the same way (the Db axis carries values already in decibels) and differ in how
// ticks are chosen and labelled; LogFreq maps the logarithm of the frequency.
enum class AxisKind : std::uint8_t { Linear, LogFreq, Db };

struct AxisSpec {
  AxisKind kind;
  float lo;  // value at the low end (> 0 for LogFreq)
  float hi;  // value at the high end (> lo)
};

// Position of `v` on the axis: 0 at `lo`, 1 at `hi`, not clamped. A degenerate axis (hi <= lo, or lo <= 0 on a log axis)
// maps everything to 0.
float axis_t(const AxisSpec& axis, float v) noexcept;
// The inverse: the value at position `t`.
float axis_value(const AxisSpec& axis, float t) noexcept;

// Base-2 logarithm and exponential in single precision without libm (powf, exp2f, log2f and log10f are about 7 KB of flash on the
// MK66 that nothing else of the plots needs; sinf and cosf, which the FFT and the engine use anyway, are kept). exp2_fast(x) is
// 2^x: the exponent by construction and 2^f of the fraction by a degree-7 Taylor series on [-1/2, 1/2], relative error under
// 1e-7 for x in [-126, 127] (0 below, 2^127 above, NaN gives 0). log2_fast(x) is the exponent plus 2 atanh((m - 1) / (m + 1)) / ln 2
// of the mantissa m in [1/sqrt 2, sqrt 2) by its series to z^9, error under 1e-6 plus the rounding of a float result (-126 for
// zero, a negative number, NaN or a denormal; 128 for infinity). Tested against libm in double precision.
float exp2_fast(float x) noexcept;
float log2_fast(float x) noexcept;

// Decibels. The floor, kMinDb, stands for silence (a zero or negative argument) so that no -infinity reaches a drawing.
inline constexpr float kMinDb = -200.0f;
float db_from_amp(float amplitude) noexcept;  // 20 log10(a)
float amp_from_db(float db) noexcept;         // 10^(db / 20)
float db_from_power(float power) noexcept;    // 10 log10(p)

// A biquad section, direct form: y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2] (a0 = 1).
struct Biquad {
  float b0, b1, b2, a1, a2;
};
// |H(e^jw)|^2 at an angular frequency w given as phi = sin^2(w / 2), 0 at DC and 1 at the Nyquist frequency. With
// z = e^-jw and cos w = 1 - 2 phi the squared magnitude of a polynomial p0 + p1 z + p2 z^2 is
//   (p0 + p1 + p2)^2 - 4 (p0 p1 + 4 p0 p2 + p1 p2) phi + 16 p0 p2 phi^2,
// which, unlike the textbook form in cos w and cos 2w, does not cancel catastrophically in single precision at low
// frequencies, where the plot of a filter spends most of its columns (a lowpass at 100 Hz of a 44.1 kHz system has
// 1 - cos w = 2e-6). For a fixed set of frequencies phi is computed once (response_basis()) and reused for every edit.
float biquad_mag2(const Biquad& f, float phi) noexcept;
float biquad_db(const Biquad& f, float phi) noexcept;
// phi = sin^2(w / 2), w = 2 pi f / sample_rate, for `n` frequencies spread evenly over the axis (n >= 2: the first at `lo`,
// the last at `hi`; logarithmically on a LogFreq axis).
void response_basis(const AxisSpec& freq_axis, float sample_rate, unsigned n, float* phi) noexcept;
// The response in dB at each of the `n` frequencies of a basis.
void biquad_response_db(const Biquad& f, const float* phi, unsigned n, float* out_db) noexcept;

// A tick: where it is and whether it carries a label.
struct Tick {
  float value;
  bool major;
};
// Ticks of an axis, at most `cap`, in rising order and inside [lo, hi]. `max_count` is the most the axis should show
// (the caller works it out from the room: a label every 30 px or so).
//   LogFreq  1, 2 and 5 times each power of ten; the 1s are major (a decade).
//   Db       steps of 1, 2, 3, 6, 12, 24 or 48 dB, whichever is the smallest that gives at most max_count; major at 0 dB
//            and at every other tick.
//   Linear   steps of 1, 2 or 5 times a power of ten likewise; major at every other tick.
unsigned make_ticks(const AxisSpec& axis, unsigned max_count, Tick* out, unsigned cap) noexcept;

// Tick labels, written without printf into `out` (at least kLabelBuffer = 10 characters, NUL terminated); each returns the
// length. A NaN is 0, and a value beyond what a label can hold gives the empty label (length 0: the plot draws none), so a
// label is at most 9 characters (format_value: "-99999.99"), 6 (format_hz: "999.9k") or 4 (format_db: "+999").
//   format_hz    20 -> "20", 100 -> "100", 1000 -> "1k", 2500 -> "2.5k", 20000 -> "20k"
//   format_db    -12 -> "-12", 0 -> "0", 6 -> "+6"
//   format_value 0.5 -> "0.5", 1 -> "1", -0.25 -> "-0.25" (at most two decimals, trailing zeros dropped)
inline constexpr unsigned kLabelBuffer = 10;
unsigned format_hz(float hz, char* out) noexcept;
unsigned format_db(float db, char* out) noexcept;
unsigned format_value(float v, char* out) noexcept;
unsigned format_tick(AxisKind kind, float v, char* out) noexcept;

}  // namespace o3sdk::ui::plot
