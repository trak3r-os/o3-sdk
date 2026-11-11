// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only tool (it uses double and std::vector and never links into an image): fits the five vowel anchors (tongue, height, lips) of
// shape.h to Peterson and Barney's male formant averages and prints the table to freeze there.
//
//   c++ -O2 -std=c++17 -Iinclude -Itests tools/fit_vowels.cpp src/tube.cpp src/shape.cpp -o /tmp/fit_vowels && /tmp/fit_vowels [--tune]
//
// For each vowel: a coarse grid over the three numbers, then a pattern search, on a cost of the squared log ratios of F1, F2, F3 of the
// full-size tube (N = 22 at 22,050 Hz, ends 0.8) and of F1, F2 of the Lite one (N = 11 at 11,025 Hz). --tune also moves the area model's
// constants (coordinate descent, the five vowels re-fitted at each step). The printed values go into shape.h by hand.
#include <o3sdk/extras/tract/shape.h>
#include <o3sdk/extras/tract/tube.h>

#include "tract_probe.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace o3sdk::extras::tract;

namespace {

constexpr double kTarget[kVowelCount][3] = {
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
const char* const kName = "AEIOU";

AreaModel g_model = kAreaModel;

// The first three formants of a shape at n sections; the rate is 22,050 Hz at n = 22 and 11,025 Hz at n = 11 (the same length).
bool measure(const Shape& s, unsigned n, double out[3], unsigned len, unsigned bins, double fmax) {
  const double fs_t = (n >= 18) ? 22050.0 : 11025.0;
  const float none[3] = {0, 0, 0};
  float d[Tube::kMaxSections];
  diameters(s, none, n, d, g_model);
  Tube t;
  t.set_size(n);
  t.set_ends(0.8f, 0.8f);
  t.set_diameters(d);
  // the band 0..fmax only: bins at k * fmax / bins
  const std::vector<double> h = probe::impulse_response(t, len);
  std::vector<double> m(bins);
  const double pi = 3.14159265358979323846;
  for (unsigned k = 0; k < bins; ++k) {
    const double w = 2.0 * pi * (fmax * static_cast<double>(k) / static_cast<double>(bins)) / fs_t;
    const double cw = std::cos(w), sw = std::sin(w);
    double c = 1, sn = 0, re = 0, im = 0;
    for (double v : h) {
      re += v * c;
      im -= v * sn;
      const double nc = c * cw - sn * sw;
      sn = sn * cw + c * sw;
      c = nc;
    }
    m[k] = std::sqrt(re * re + im * im);
  }
  const std::vector<double> p = probe::peaks(m, fmax, 0.01);
  if (p.size() < 3) return false;
  for (int i = 0; i < 3; ++i) out[i] = p[static_cast<std::size_t>(i)];
  return true;
}

double cost(const Shape& s, int v, bool fine) {
  double f[3], g[3];
  const unsigned len = fine ? 2048 : 512, bins = fine ? 600 : 250;
  if (!measure(s, 22, f, len, bins, 5000.0)) return 1e9;
  double c = 0;
  const double w[3] = {1.0, 1.0, 0.5};
  for (int i = 0; i < 3; ++i) {
    const double e = std::log(f[i] / kTarget[v][i]);
    c += w[i] * e * e;
  }
  // Lite: N = 11, the fs/4 tube. Only F1 and F2 count.
  if (measure(s, 11, g, fine ? 1024 : 256, fine ? 400 : 160, 5000.0)) {
    for (int i = 0; i < 2; ++i) {
      const double e = std::log(g[i] / kTarget[v][i]);
      c += 0.5 * e * e;
    }
  } else {
    c += 1.0;
  }
  return c;
}

float clampf(double v) { return static_cast<float>(v < 0 ? 0 : (v > 1 ? 1 : v)); }

Shape fit_vowel(int v, Shape start, bool grid) {
  Shape best = start;
  double bc = cost(best, v, false);
  if (grid) {
    for (int a = 0; a <= 20; ++a)
      for (int b = 0; b <= 20; ++b)
        for (int c = 0; c <= 20; ++c) {
          const Shape s{static_cast<float>(a) * 0.05f, static_cast<float>(b) * 0.05f, static_cast<float>(c) * 0.05f};
          const double k = cost(s, v, false);
          if (k < bc) { bc = k; best = s; }
        }
  }
  for (double step = 0.04; step > 0.0006; step *= 0.6) {
    bool improved = true;
    while (improved) {
      improved = false;
      for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2) {
          Shape s = best;
          float* p = axis == 0 ? &s.tongue : (axis == 1 ? &s.height : &s.lips);
          *p = clampf(static_cast<double>(*p) + sign * step);
          const double k = cost(s, v, false);
          if (k < bc - 1e-12) { bc = k; best = s; improved = true; }
        }
    }
  }
  return best;
}

void report(const Shape* sh) {
  double tot = 0;
  for (int v = 0; v < 5; ++v) {
    double f[3], g[3];
    const bool ok = measure(sh[v], 22, f, 4096, 1200, 5000.0);
    const bool okl = measure(sh[v], 11, g, 4096, 1200, 5000.0);
    if (!ok) { std::printf("%c: fewer than 3 peaks\n", kName[v]); continue; }
    std::printf("%c  tongue %.3f height %.3f lips %.3f | Full F %4.0f %4.0f %4.0f (target %4.0f %4.0f %4.0f; %+.1f %+.1f %+.1f %%)", kName[v],
                static_cast<double>(sh[v].tongue), static_cast<double>(sh[v].height), static_cast<double>(sh[v].lips), f[0], f[1], f[2],
                kTarget[v][0], kTarget[v][1], kTarget[v][2], 100 * (f[0] / kTarget[v][0] - 1), 100 * (f[1] / kTarget[v][1] - 1),
                100 * (f[2] / kTarget[v][2] - 1));
    if (okl)
      std::printf(" | Lite F %4.0f %4.0f (%+.1f %+.1f %%)", g[0], g[1], 100 * (g[0] / kTarget[v][0] - 1), 100 * (g[1] / kTarget[v][1] - 1));
    std::printf("\n");
    tot += cost(sh[v], v, true);
  }
  std::printf("total cost %.5f\n", tot);
}

}  // namespace

int main(int argc, char** argv) {
  const bool tune = argc > 1 && std::strcmp(argv[1], "--tune") == 0;
  Shape sh[5];
  for (int v = 0; v < 5; ++v) sh[v] = fit_vowel(v, kVowelShapes[v], true);
  report(sh);
  if (tune) {
    float* fields[] = {&g_model.larynx_d, &g_model.open_d, &g_model.tongue_c0, &g_model.tongue_c1, &g_model.depth, &g_model.width,
                       &g_model.side_gain, &g_model.side_off, &g_model.side_width, &g_model.lip_base, &g_model.lip_span, &g_model.ramp_end, &g_model.lip_len, &g_model.lip_x};
    auto total = [&](Shape* s) {
      double t = 0;
      for (int v = 0; v < 5; ++v) { s[v] = fit_vowel(v, s[v], false); t += cost(s[v], v, false); }
      return t;
    };
    double best = total(sh);
    for (int round = 0; round < 8; ++round) {
      for (float* f : fields) {
        for (double fac : {0.9, 1.1}) {
          const float old = *f;
          Shape trial[5];
          std::memcpy(trial, sh, sizeof sh);
          *f = static_cast<float>(static_cast<double>(old) * fac);
          const double t = total(trial);
          if (t < best - 1e-9) { best = t; std::memcpy(sh, trial, sizeof sh); }
          else *f = old;
        }
      }
      std::printf("round %d cost %.5f\n", round, best);
    }
    std::printf("area model: {%.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff}\n",
                static_cast<double>(g_model.larynx_d), static_cast<double>(g_model.larynx_end), static_cast<double>(g_model.ramp_end),
                static_cast<double>(g_model.open_d), static_cast<double>(g_model.tongue_c0), static_cast<double>(g_model.tongue_c1),
                static_cast<double>(g_model.depth), static_cast<double>(g_model.width), static_cast<double>(g_model.side_gain),
                static_cast<double>(g_model.side_off), static_cast<double>(g_model.side_width), static_cast<double>(g_model.lip_x),
                static_cast<double>(g_model.lip_base), static_cast<double>(g_model.lip_span), static_cast<double>(g_model.lip_len));
    report(sh);
  }
  std::printf("kVowelShapes = {");
  for (int v = 0; v < 5; ++v)
    std::printf("%s{%.3ff, %.3ff, %.3ff}", v ? ", " : "", static_cast<double>(sh[v].tongue), static_cast<double>(sh[v].height),
                static_cast<double>(sh[v].lips));
  std::printf("}\n");
  return 0;
}
