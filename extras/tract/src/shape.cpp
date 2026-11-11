// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/shape.h>

namespace o3sdk::extras::tract {

namespace {

inline float clamp01(float v) noexcept { return v >= 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f); }   // a NaN lands on 0

// (1 - u^2)^2 for |u| < 1, else 0: a smooth bump with no cosine.
inline float bump(float u) noexcept {
  const float q = 1.0f - u * u;
  return q > 0.0f ? q * q : 0.0f;
}

inline float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

}  // namespace

Shape vowel_shape(float v, const Shape* a) noexcept {
  const float p = clamp01(v) * static_cast<float>(kVowelCount - 1);
  unsigned i = static_cast<unsigned>(p);
  if (i > kVowelCount - 2) i = kVowelCount - 2;
  const float t = p - static_cast<float>(i);
  return {lerp(a[i].tongue, a[i + 1].tongue, t), lerp(a[i].height, a[i + 1].height, t), lerp(a[i].lips, a[i + 1].lips, t)};
}

Shape vowel_shape(float v) noexcept { return vowel_shape(v, kVowelShapes); }

void diameters(const Shape& s, const float closure[3], unsigned n, float* d) noexcept { diameters(s, closure, n, d, kAreaModel); }

void diameters(const Shape& s, const float closure[3], unsigned n, float* d, const AreaModel& m) noexcept {
  const float tongue = clamp01(s.tongue), height = clamp01(s.height), lips = clamp01(s.lips);
  const float c = m.tongue_c0 + m.tongue_c1 * tongue;
  const float h = m.depth * height;
  const float inv_w = 1.0f / m.width, inv_sw = 1.0f / m.side_width;
  const float lip_d = m.lip_base + m.lip_span * lips;
  const float inv_ramp = 1.0f / (m.ramp_end - m.larynx_end);
  const float inv_n = 1.0f / static_cast<float>(n);
  for (unsigned i = 0; i < n; ++i) {
    const float x = (static_cast<float>(i) + 0.5f) * inv_n;
    float d0 = m.open_d;
    if (x < m.larynx_end) d0 = m.larynx_d;
    else if (x < m.ramp_end) d0 = m.larynx_d + (m.open_d - m.larynx_d) * (x - m.larynx_end) * inv_ramp;
    float v = d0 - h * bump((x - c) * inv_w) +
              m.side_gain * h * (bump((x - c - m.side_off) * inv_sw) + bump((x - c + m.side_off) * inv_sw));
    if (x > m.lip_x - m.lip_len * (1.0f - lips) && lip_d < v) v = lip_d;
    d[i] = v;
  }
  // Closures: lips at the last section, tip at x = 0.85, back at x = 0.55.
  const unsigned at[3] = {n - 1, static_cast<unsigned>(0.85f * static_cast<float>(n)), static_cast<unsigned>(0.55f * static_cast<float>(n))};
  for (unsigned j = 0; j < 3; ++j) {
    const float cl = clamp01(closure[j]);
    if (cl <= 0.0f) continue;
    const unsigned at_j = at[j] < n ? at[j] : n - 1;
    d[at_j] = lerp(d[at_j], kMinDiameter, cl);
    if (at_j > 0) d[at_j - 1] = lerp(d[at_j - 1], kMinDiameter, 0.5f * cl);
    if (at_j + 1 < n) d[at_j + 1] = lerp(d[at_j + 1], kMinDiameter, 0.5f * cl);
  }
  for (unsigned i = 0; i < n; ++i)
    if (!(d[i] >= kMinDiameter)) d[i] = kMinDiameter;
}

}  // namespace o3sdk::extras::tract
