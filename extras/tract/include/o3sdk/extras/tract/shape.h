// SPDX-License-Identifier: GPL-3.0-or-later
// The shape of the mouth: three numbers (tongue, height, lips), a table of five vowels, and the area function that turns a shape into the
// tube's diameters. The vowel line interpolates the three numbers, not the formant frequencies, so that between two vowels the mouth passes
// through shapes a mouth can hold; F3 and above stay where a real tract puts them.
//
// The five anchors and the area model's constants were fitted on the host (tools/fit_vowels.cpp) to Peterson and Barney's male formant
// averages (G. E. Peterson and H. L. Barney, JASA 24, 1952): A 730/1090/2440 Hz, E 530/1840/2480, I 270/2290/3010, O 570/840/2410, U 300/870/2240,
// in a tube of 22 sections at 22,050 Hz with the ends at 0.8, and are frozen here. A different tube length is another N: the area
// function is a function of x in [0, 1] sampled at N points.
//
// Fit result (Full, N = 22; F1 F2 F3 in Hz and the error against the target; Lite, N = 11, F1 F2 only):
//   A  729 1095 2436 (-0.1 +0.5 -0.2 %)   Lite 732 1081 (+0.2 -0.9 %)
//   E  532 1819 2753 (+0.4 -1.1 +11.0 %)  Lite 535 1812 (+0.9 -1.5 %)
//   I  267 2237 2927 (-1.1 -2.3 -2.8 %)   Lite 268 2193 (-0.8 -4.2 %)
//   O  569  830 2462 (-0.2 -1.2 +2.2 %)   Lite 565  894 (-1.0 +6.4 %)
//   U  301  862 2959 (+0.4 -0.9 +32.1 %)  Lite 303  873 (+1.2 +0.3 %)
// F1 and F2 are within 4.2 % (Full) and 6.4 % (Lite). The F3 of U misses by 32 %: a plain one-dimensional tube with F1 and F2 on the
// targets keeps its F3 near 3 kHz (weighting F3 higher in the fit moved F1 and F2 off the targets and did not bring it closer), so the
// test allows 40 % for that one formant; it is a known limit of the model, not of the fit.
#pragma once

namespace o3sdk::extras::tract {

struct Shape {
  float tongue;  // 0 back .. 1 front
  float height;  // 0 open .. 1 close
  float lips;    // 0 round .. 1 spread
};

// The constants of the area function (diameters in a unit where the open tract is 1.5, positions as a fraction of the tract's length).
struct AreaModel {
  float larynx_d;      // diameter of the larynx tube
  float larynx_end;    // x where the larynx tube ends and the ramp starts
  float ramp_end;      // x where the ramp reaches open_d
  float open_d;        // rest diameter of the open tract
  float tongue_c0;     // constriction centre: tongue_c0 + tongue_c1 * tongue
  float tongue_c1;
  float depth;         // constriction depth: depth * height
  float width;         // half width of the constriction's bump
  float side_gain;     // the tongue's volume widens both sides, at this fraction of the depth
  float side_off;      // ... at this distance from the centre
  float side_width;    // ... with this half width
  float lip_x;         // the lips start here
  float lip_base;      // the lip opening: lip_base + lip_span * lips
  float lip_span;
  float lip_len;       // rounded lips protrude: the opening starts lip_len * (1 - lips) earlier (a longer, narrow front tube)
};

inline constexpr AreaModel kAreaModel = {0.5400f, 0.1200f, 0.2500f, 1.5000f, 0.3000f, 0.5000f, 1.3500f, 0.3630f, 0.3500f, 0.4500f, 0.3500f, 0.8280f, 0.2500f, 1.6500f, 0.1100f};

inline constexpr float kMinDiameter = 0.03f;   // no section is narrower than this, closures included

inline constexpr unsigned kVowelCount = 5;     // A E I O U
inline constexpr Shape kVowelShapes[kVowelCount] = {
    {0.132f, 0.755f, 0.634f}, {0.584f, 0.352f, 0.739f}, {0.950f, 0.722f, 0.271f}, {0.126f, 0.818f, 0.340f}, {0.288f, 0.830f, 0.108f}};

// v in [0, 1] through A E I O U (a quarter each), piecewise linear between the anchors in Shape space; outside the range it is clamped.
Shape vowel_shape(float v) noexcept;
Shape vowel_shape(float v, const Shape* anchors) noexcept;   // the same with another table (the fit tool)

// The diameters of n sections (n <= 28) for a shape. closure[3] (each 0..1): lips (the last section), tip (x = 0.85), back (x = 0.55);
// a closure takes d towards kMinDiameter over its section and half as far over each neighbour. Every d >= kMinDiameter.
void diameters(const Shape& s, const float closure[3], unsigned n, float* d) noexcept;
void diameters(const Shape& s, const float closure[3], unsigned n, float* d, const AreaModel& m) noexcept;

}  // namespace o3sdk::extras::tract
