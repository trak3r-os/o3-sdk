// SPDX-License-Identifier: GPL-3.0-or-later
// The area model and the fitted vowel table against Peterson and Barney's male averages (shape.h), at both tube sizes, and the closures.
#include <doctest/doctest.h>

#include "tract_probe.h"

#include <o3sdk/extras/tract/shape.h>
#include <o3sdk/extras/tract/tube.h>

#include <cmath>

using namespace o3sdk::extras::tract;

namespace {
constexpr double kTarget[kVowelCount][3] = {
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
const char* const kNames = "AEIOU";
}  // namespace

TEST_CASE("tract shape: the five vowels at 22 sections are within 12 % (F1, F2) and 20 % (F3; 40 % for U, see shape.h) of Peterson-Barney") {
  for (unsigned v = 0; v < kVowelCount; ++v) {
    CAPTURE(kNames[v]);
    const Tube t = probe::tube_for(vowel_shape(0.25f * static_cast<float>(v)), 22);
    const std::vector<double> f = probe::formants(t, 22050.0);
    REQUIRE(f.size() >= 3);
    for (unsigned i = 0; i < 3; ++i) {
      CAPTURE(i);
      CAPTURE(f[i]);
      CHECK(std::fabs(f[i] / kTarget[v][i] - 1.0) < (i < 2 ? 0.12 : (v == 4 ? 0.40 : 0.20)));
    }
  }
}

TEST_CASE("tract shape: the five vowels at 11 sections (Lite) are within 18 % in F1 and F2") {
  for (unsigned v = 0; v < kVowelCount; ++v) {
    CAPTURE(kNames[v]);
    const Tube t = probe::tube_for(vowel_shape(0.25f * static_cast<float>(v)), 11);
    const std::vector<double> f = probe::formants(t, 11025.0);
    REQUIRE(f.size() >= 2);
    for (unsigned i = 0; i < 2; ++i) {
      CAPTURE(i);
      CAPTURE(f[i]);
      CHECK(std::fabs(f[i] / kTarget[v][i] - 1.0) < 0.18);
    }
  }
}

TEST_CASE("tract shape: the vowel line passes through its anchors and stays between them") {
  for (unsigned v = 0; v < kVowelCount; ++v) {
    const Shape s = vowel_shape(0.25f * static_cast<float>(v));
    CHECK(s.tongue == doctest::Approx(kVowelShapes[v].tongue));
    CHECK(s.height == doctest::Approx(kVowelShapes[v].height));
    CHECK(s.lips == doctest::Approx(kVowelShapes[v].lips));
  }
  const Shape mid = vowel_shape(0.125f);
  CHECK(mid.tongue == doctest::Approx(0.5f * (kVowelShapes[0].tongue + kVowelShapes[1].tongue)));
  // out of range clamps to the ends
  CHECK(vowel_shape(-1.0f).tongue == doctest::Approx(kVowelShapes[0].tongue));
  CHECK(vowel_shape(9.0f).tongue == doctest::Approx(kVowelShapes[kVowelCount - 1].tongue));
}

TEST_CASE("tract shape: no section is narrower than kMinDiameter, a closure shuts its section and an open mouth is wide") {
  const float none[3] = {0, 0, 0};
  for (unsigned n : {22u, 11u}) {
    float d[Tube::kMaxSections];
    for (unsigned v = 0; v < kVowelCount; ++v) {
      diameters(vowel_shape(0.25f * static_cast<float>(v)), none, n, d);
      for (unsigned i = 0; i < n; ++i) CHECK(d[i] >= kMinDiameter);
    }
    const float lips[3] = {1, 0, 0}, tip[3] = {0, 1, 0}, back[3] = {0, 0, 1};
    diameters(vowel_shape(0.0f), lips, n, d);
    CHECK(d[n - 1] == doctest::Approx(kMinDiameter));
    diameters(vowel_shape(0.0f), tip, n, d);
    float mn = 10.0f;
    for (unsigned i = 0; i < n; ++i) mn = d[i] < mn ? d[i] : mn;
    CHECK(mn == doctest::Approx(kMinDiameter));
    diameters(vowel_shape(0.0f), back, n, d);
    mn = 10.0f;
    for (unsigned i = 0; i < n; ++i) mn = d[i] < mn ? d[i] : mn;
    CHECK(mn == doctest::Approx(kMinDiameter));
  }
}
