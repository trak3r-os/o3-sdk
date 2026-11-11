// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only measurement helpers of the tract's tests and of tools/fit_vowels.cpp (they use double and std::vector: never in an image).
// The magnitude response of a Tube from its impulse response (a plain DFT) and a peak picker.
#pragma once

#include <o3sdk/extras/tract/shape.h>
#include <o3sdk/extras/tract/tube.h>

#include <cmath>
#include <vector>

namespace o3sdk::extras::tract::probe {

// The impulse response of a copy of the tube: len samples.
inline std::vector<double> impulse_response(const Tube& tube, unsigned len) {
  Tube t = tube;
  t.reset();
  std::vector<double> h(len);
  for (unsigned i = 0; i < len; ++i) h[i] = static_cast<double>(t.tick(i == 0 ? 1.0f : 0.0f));
  return h;
}

// |H| at `bins` frequencies k * (fs / 2) / bins, k = 0 .. bins - 1: a DFT with a rotating phasor per bin.
inline std::vector<double> magnitude(const std::vector<double>& h, unsigned bins) {
  const double pi = 3.14159265358979323846;
  std::vector<double> m(bins);
  for (unsigned k = 0; k < bins; ++k) {
    const double w = pi * static_cast<double>(k) / static_cast<double>(bins);
    const double cw = std::cos(w), sw = std::sin(w);
    double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
    for (double v : h) {
      re += v * c;
      im -= v * s;
      const double nc = c * cw - s * sw;
      s = s * cw + c * sw;
      c = nc;
    }
    m[k] = std::sqrt(re * re + im * im);
  }
  return m;
}

// The frequencies [Hz] of the local maxima of `mag` (bins spaced fs_half / bins) above `floor_ratio` of the largest one, ascending, each
// refined by a parabola through the log magnitude of the peak and its neighbours.
inline std::vector<double> peaks(const std::vector<double>& mag, double fs_half, double floor_ratio = 0.01) {
  std::vector<double> out;
  double top = 0.0;
  for (double v : mag) top = v > top ? v : top;
  const double step = fs_half / static_cast<double>(mag.size());
  for (std::size_t k = 1; k + 1 < mag.size(); ++k) {
    if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > floor_ratio * top) {
      const double a = std::log(mag[k - 1]), b = std::log(mag[k]), c = std::log(mag[k + 1]);
      const double den = a - 2.0 * b + c;
      const double off = den < 0.0 ? 0.5 * (a - c) / den : 0.0;
      out.push_back((static_cast<double>(k) + off) * step);
    }
  }
  return out;
}

// The formants (peaks) of a tube at the tract rate fs_t.
inline std::vector<double> formants(const Tube& tube, double fs_t, unsigned len = 4096, unsigned bins = 2048) {
  return peaks(magnitude(impulse_response(tube, len), bins), fs_t * 0.5, 0.01);
}

// A tube of n sections for a shape and closures, ends rg and rl.
inline Tube tube_for(const Shape& s, unsigned n, float rg = 0.8f, float rl = 0.8f, const float* closure = nullptr) {
  const float none[3] = {0.0f, 0.0f, 0.0f};
  float d[Tube::kMaxSections];
  diameters(s, closure ? closure : none, n, d);
  Tube t;
  t.set_size(n);
  t.set_ends(rg, rl);
  t.set_diameters(d);
  return t;
}

// n sections, the first n1 of area a1 and the rest of area a2.
inline Tube two_tube(unsigned n, unsigned n1, double a1, double a2, float rg = 0.9f, float rl = 0.9f) {
  float d[Tube::kMaxSections];
  for (unsigned i = 0; i < n; ++i) d[i] = static_cast<float>(std::sqrt(i < n1 ? a1 : a2));
  Tube t;
  t.set_size(n);
  t.set_ends(rg, rl);
  t.set_diameters(d);
  return t;
}

}  // namespace o3sdk::extras::tract::probe
