// SPDX-License-Identifier: GPL-3.0-or-later
// The Kelly-Lochbaum tube against physics (README.md): a uniform tube resonates at odd quarter wavelengths, two joined tubes put F1 and F2
// where the area ratio says (this fixes the sign of k and which end is which), and the lattice stays passive when k changes in time.
#include <doctest/doctest.h>

#include "tract_probe.h"

#include <o3sdk/extras/tract/tube.h>

#include <cmath>
#include <cstdint>

using namespace o3sdk::extras::tract;

TEST_CASE("tract tube: a uniform tube of 22 sections resonates at 501, 1503 and 2506 Hz") {
  const Tube t = probe::two_tube(22, 22, 1.0, 1.0, 0.9f, 0.9f);
  const std::vector<double> f = probe::formants(t, 22050.0);
  REQUIRE(f.size() >= 3);
  const double want[3] = {501.1, 1503.4, 2505.7};
  for (int i = 0; i < 3; ++i) CHECK(std::fabs(f[static_cast<std::size_t>(i)] / want[i] - 1.0) < 0.02);
}

TEST_CASE("tract tube: the same tube of 11 sections at fs/4 has the same formants") {
  const Tube t = probe::two_tube(11, 11, 1.0, 1.0, 0.9f, 0.9f);
  const std::vector<double> f = probe::formants(t, 11025.0);
  REQUIRE(f.size() >= 2);
  const double want[2] = {501.1, 1503.4};
  for (int i = 0; i < 2; ++i) CHECK(std::fabs(f[static_cast<std::size_t>(i)] / want[i] - 1.0) < 0.02);
}

TEST_CASE("tract tube: a narrow back and a wide front raise F1, a wide back and a narrow front lower it") {
  {
    const Tube t = probe::two_tube(22, 11, 1.0, 7.0);
    const std::vector<double> f = probe::formants(t, 22050.0);
    REQUIRE(f.size() >= 2);
    CHECK(f[0] > 650.0);
    CHECK(f[1] < 1400.0);
  }
  {
    const Tube t = probe::two_tube(22, 11, 8.0, 1.0);
    const std::vector<double> f = probe::formants(t, 22050.0);
    REQUIRE(f.size() >= 2);
    CHECK(f[0] < 400.0);
    CHECK(f[1] > 1700.0);
  }
}

namespace {
// A deterministic generator (an LCG): a float in [-1, 1).
struct Noise {
  std::uint32_t s = 12345u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
  }
};
}  // namespace

// The one-multiplier lattice is passive while k stands still (|k| < 1, ends < 1), but not for any motion of k: redrawing every k at random
// every 8 samples makes it grow without bound (measured: 1e36 within seconds). What the processor needs is the motion it makes: the
// diameters approach random targets (the whole tract changing, areas from 0.03^2 to 1.5^2) through a one-pole of 5 ms, the fastest
// GLIDE; the targets are redrawn every 256 samples (11.6 ms at 22,050 Hz), the worst a player can ask for, and the ends are at the least
// loss (RES 1: 0.97, and 0.99 at the limit of set_ends).
TEST_CASE("tract tube: ten seconds of the fastest tract motion stay finite and bounded") {
  struct Cfg { unsigned n; float tau; };   // tau: the 5 ms one-pole, in tract samples
  for (const Cfg c : {Cfg{22u, 110.0f}, Cfg{11u, 55.0f}}) {
    for (float r : {0.97f, 0.99f}) {
      Tube t;
      t.set_size(c.n);
      t.set_ends(r, r);
      Noise noise;
      float d[Tube::kMaxSections], tg[Tube::kMaxSections];
      for (unsigned j = 0; j < c.n; ++j) d[j] = tg[j] = 0.8f;
      const float a = 1.0f - std::exp(-8.0f / c.tau);
      const unsigned total = 220500;  // 10 s at 22,050 Hz
      float peak = 0.0f;
      for (unsigned i = 0; i < total; ++i) {
        if ((i & 255u) == 0) {
          for (unsigned j = 0; j < c.n; ++j) tg[j] = 0.03f + 1.47f * (0.5f * noise.next() + 0.5f);
        }
        if ((i & 7u) == 0) {
          for (unsigned j = 0; j < c.n; ++j) d[j] += (tg[j] - d[j]) * a;
          t.set_diameters(d);
        }
        const float y = t.tick(0.5f * noise.next());
        REQUIRE(std::isfinite(y));
        const float m = std::fabs(y);
        peak = m > peak ? m : peak;
      }
      CAPTURE(c.n);
      CAPTURE(r);
      CHECK(peak < 10.0f);
    }
  }
}

TEST_CASE("tract tube: with k standing still the tube decays whatever k is") {
  Noise noise;
  for (unsigned n : {22u, 11u}) {
    Tube t;
    t.set_size(n);
    t.set_ends(0.99f, 0.99f);
    for (unsigned j = 0; j + 1 < n; ++j) t.set_reflection(j, 0.6f * noise.next());
    for (unsigned i = 0; i < 64; ++i) t.tick(noise.next());   // excite
    float first = 0.0f, last = 0.0f;
    for (unsigned i = 0; i < 200000; ++i) {
      const float m = std::fabs(t.tick(0.0f));
      if (i < 2000) first = std::fmax(first, m);
      if (i >= 198000) last = std::fmax(last, m);
    }
    REQUIRE(first > 0.0f);
    CHECK(last < 1e-3f * first);   // it decayed (a pole outside the circle would have grown)
  }
}

TEST_CASE("tract tube: sanitize clears a state that is not finite and keeps a healthy one") {
  Tube t = probe::two_tube(22, 22, 1.0, 1.0);
  t.tick(1.0f);
  CHECK_FALSE(t.sanitize());
  t.inject(3, 1e7f);
  CHECK(t.sanitize());
  CHECK(t.tick(0.0f) == 0.0f);
}
