// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/plot_math.h>

#include <o3sdk/core/fast_math.h>

#include <cmath>
#include <cstdint>

namespace o3sdk::ui::plot {

namespace {

constexpr float kPi = 3.14159265358979f;

// The largest integer value at or below / the smallest at or above x, for |x| below 2^23 (a float at or above it is an integer).
float ceil_small(float x) noexcept {
  if (!(std::fabs(x) < 8388608.0f)) {
    return x;
  }
  const float t = static_cast<float>(static_cast<int>(x));
  return t < x ? t + 1.0f : t;
}

// A range the arithmetic below can work with: finite ends, hi above lo, a finite width (-3e38 .. 3e38 overflows to infinity
// and would turn every tick step into one), and on a log axis a positive lo and a finite ratio. Anything else maps to 0 and has
// no ticks, instead of looping on an infinite or NaN bound.
bool usable(const AxisSpec& a) noexcept {
  if (!std::isfinite(a.lo) || !std::isfinite(a.hi) || !(a.hi > a.lo) || !std::isfinite(a.hi - a.lo)) {
    return false;
  }
  if (a.kind == AxisKind::LogFreq) {
    return a.lo > 0.0f && std::isfinite(a.hi / a.lo) && a.hi / a.lo > 1.0f;
  }
  return true;
}

// Appends `n` (>= 0) and returns the new length: decimal digits of a non-negative integer.
unsigned put_uint(unsigned long v, char* out, unsigned n) noexcept {
  char tmp[12];
  unsigned t = 0;
  do {
    tmp[t++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0 && t < sizeof(tmp));
  while (t > 0) {
    out[n++] = tmp[--t];
  }
  return n;
}

// 1, 2, 5 or 10 times the power of ten at or below x: the smallest of them that is at least x. x is a finite number above
// zero (the caller's range is usable() and its count positive); the two loops are bounded all the same, so that no x, a
// denormal or a huge one, can keep them going.
float nice_up(float x) noexcept {
  if (!(x > 0.0f) || !std::isfinite(x)) {
    return 1.0f;
  }
  float p = 1.0f;
  for (int i = 0; i < 90 && p > x; ++i) {
    p *= 0.1f;
  }
  for (int i = 0; i < 90 && p * 10.0f <= x; ++i) {
    p *= 10.0f;
  }
  const float steps[4] = {1.0f, 2.0f, 5.0f, 10.0f};
  for (const float s : steps) {
    if (p * s >= x * 0.999f) {
      return p * s;
    }
  }
  return p * 10.0f;
}

}  // namespace

float axis_t(const AxisSpec& axis, float v) noexcept {
  if (!usable(axis)) {
    return 0.0f;
  }
  if (axis.kind == AxisKind::LogFreq) {
    if (v <= 0.0f) {
      return -1.0f;  // off the low end
    }
    return log2_fast(v / axis.lo) / log2_fast(axis.hi / axis.lo);
  }
  return (v - axis.lo) / (axis.hi - axis.lo);
}

float axis_value(const AxisSpec& axis, float t) noexcept {
  if (!usable(axis)) {
    return axis.lo;
  }
  if (axis.kind == AxisKind::LogFreq) {
    return axis.lo * exp2_fast(t * log2_fast(axis.hi / axis.lo));
  }
  return axis.lo + t * (axis.hi - axis.lo);
}

float db_from_amp(float amplitude) noexcept {
  return amplitude > 1.0e-10f ? 6.020599913f * log2_fast(amplitude) : kMinDb;  // 20 log10 a = 20 log10(2) log2 a
}
float amp_from_db(float db) noexcept { return exp2_fast(db * 0.1660964047f); }  // 10^(db/20) = 2^(db log2(10) / 20)
float db_from_power(float power) noexcept { return power > 1.0e-20f ? 3.010299957f * log2_fast(power) : kMinDb; }

// The libm-free exp2 / log2 live in core (core/fast_math.h) since OS-002d, where the parameter curves use them too.
float exp2_fast(float x) noexcept { return core::exp2_fast(x); }
float log2_fast(float x) noexcept { return core::log2_fast(x); }

float biquad_mag2(const Biquad& f, float phi) noexcept {
  const float bs = f.b0 + f.b1 + f.b2;
  const float as = 1.0f + f.a1 + f.a2;
  const float num = bs * bs - 4.0f * (f.b0 * f.b1 + 4.0f * f.b0 * f.b2 + f.b1 * f.b2) * phi + 16.0f * f.b0 * f.b2 * phi * phi;
  const float den = as * as - 4.0f * (f.a1 + 4.0f * f.a2 + f.a1 * f.a2) * phi + 16.0f * f.a2 * phi * phi;
  if (!(den > 1.0e-12f)) {
    return 0.0f;  // a pole on the unit circle: no finite answer, draw it as a floor
  }
  return num > 0.0f ? num / den : 0.0f;  // rounding can leave a hair below zero where the response is a null
}
float biquad_db(const Biquad& f, float phi) noexcept { return db_from_power(biquad_mag2(f, phi)); }

void response_basis(const AxisSpec& freq_axis, float sample_rate, unsigned n, float* phi) noexcept {
  if (n == 0 || phi == nullptr) {
    return;
  }
  const float scale = kPi / sample_rate;  // w / 2 = pi f / fs
  for (unsigned i = 0; i < n; ++i) {
    const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
    const float s = std::sin(axis_value(freq_axis, t) * scale);
    phi[i] = s * s;
  }
}

void biquad_response_db(const Biquad& f, const float* phi, unsigned n, float* out_db) noexcept {
  for (unsigned i = 0; i < n; ++i) {
    out_db[i] = biquad_db(f, phi[i]);
  }
}

unsigned make_ticks(const AxisSpec& axis, unsigned max_count, Tick* out, unsigned cap) noexcept {
  unsigned n = 0;
  if (!usable(axis) || out == nullptr || cap == 0 || max_count == 0) {
    return 0;
  }
  const float eps = 1.0e-4f * (axis.hi - axis.lo);
  if (axis.kind == AxisKind::LogFreq) {
    float base = 1.0f;  // the power of ten at or below lo (the loops are bounded: a float has fewer than 90 decades)
    for (int i = 0; i < 90 && base > axis.lo; ++i) {
      base *= 0.1f;
    }
    for (int i = 0; i < 90 && base * 10.0f <= axis.lo * 1.0001f; ++i) {
      base *= 10.0f;
    }
    // A decade at most every third of the count leaves room for the 2s and 5s as minor ticks.
    const bool minors = ceil_small(log2_fast(axis.hi / axis.lo) * 0.3010299957f) * 3.0f <= static_cast<float>(max_count);
    for (int decade = 0; decade < 90 && base <= axis.hi * 1.0001f && n < cap; ++decade, base *= 10.0f) {
      const float mult[3] = {1.0f, 2.0f, 5.0f};
      for (unsigned m = 0; m < (minors ? 3u : 1u) && n < cap; ++m) {
        const float v = base * mult[m];
        if (v >= axis.lo * 0.9999f && v <= axis.hi * 1.0001f) {
          out[n++] = Tick{v, m == 0};
        }
      }
    }
    return n;
  }
  const float range = axis.hi - axis.lo;
  float step;
  if (axis.kind == AxisKind::Db) {
    const float steps[7] = {1.0f, 2.0f, 3.0f, 6.0f, 12.0f, 24.0f, 48.0f};
    step = steps[6];
    for (const float s : steps) {
      if (range / s <= static_cast<float>(max_count)) {
        step = s;
        break;
      }
    }
  } else {
    step = nice_up(range / static_cast<float>(max_count));
  }
  const float first = ceil_small((axis.lo - eps) / step);
  if (!(std::fabs(first) < 8388608.0f) || !(step > 0.0f) || !std::isfinite(step)) {
    return 0;  // 2^23 steps from zero: a float cannot count them one by one
  }
  for (float k = first; n < cap; k += 1.0f) {
    const float v = k * step;
    if (v > axis.hi + eps) {
      break;
    }
    const long index = static_cast<long>(k);
    const bool major = axis.kind == AxisKind::Db ? (v > -eps && v < eps) || (index % 2 == 0) : (index % 2 == 0);
    out[n++] = Tick{v > -eps && v < eps ? 0.0f : v, major};
  }
  return n;
}

unsigned format_value(float v, char* out) noexcept {
  unsigned n = 0;
  if (!(v == v)) {
    v = 0.0f;  // NaN
  }
  if (v < 0.0f) {
    out[n++] = '-';
    v = -v;
  }
  if (v >= 99999.995f) {  // not a number a label holds ("-99999.99" is nine characters, and the conversion below must not overflow)
    out[0] = '\0';
    return 0;
  }
  // two decimals, rounded half up; the integer part carries
  const unsigned long hundredths = static_cast<unsigned long>(v * 100.0f + 0.5f);
  const unsigned long whole = hundredths / 100u;
  const unsigned frac = static_cast<unsigned>(hundredths % 100u);
  n = put_uint(whole, out, n);
  if (frac != 0) {
    out[n++] = '.';
    out[n++] = static_cast<char>('0' + frac / 10u);
    if (frac % 10u != 0) {
      out[n++] = static_cast<char>('0' + frac % 10u);
    }
  }
  out[n] = '\0';
  return n;
}

unsigned format_db(float db, char* out) noexcept {
  unsigned n = 0;
  if (!(db == db)) {
    db = 0.0f;  // NaN
  }
  if (db >= 999.5f || db <= -999.5f) {  // beyond "+999" / "-999"
    out[0] = '\0';
    return 0;
  }
  const long r = static_cast<long>(db < 0.0f ? db - 0.5f : db + 0.5f);
  if (r > 0) {
    out[n++] = '+';
  } else if (r < 0) {
    out[n++] = '-';
  }
  n = put_uint(static_cast<unsigned long>(r < 0 ? -r : r), out, n);
  out[n] = '\0';
  return n;
}

unsigned format_hz(float hz, char* out) noexcept {
  unsigned n = 0;
  if (!(hz == hz) || hz < 0.0f) {
    hz = 0.0f;  // NaN or negative: no frequency
  }
  if (hz >= 999950.0f) {  // beyond "999.9k"
    out[0] = '\0';
    return 0;
  }
  if (hz >= 999.5f) {
    // tenths of a kHz, rounded
    const unsigned long tenths = static_cast<unsigned long>(hz / 100.0f + 0.5f);
    n = put_uint(tenths / 10u, out, 0);
    if (tenths % 10u != 0) {
      out[n++] = '.';
      out[n++] = static_cast<char>('0' + tenths % 10u);
    }
    out[n++] = 'k';
  } else {
    n = put_uint(static_cast<unsigned long>(hz + 0.5f), out, 0);
  }
  out[n] = '\0';
  return n;
}

unsigned format_tick(AxisKind kind, float v, char* out) noexcept {
  switch (kind) {
    case AxisKind::LogFreq:
      return format_hz(v, out);
    case AxisKind::Db:
      return format_db(v, out);
    case AxisKind::Linear:
      break;
  }
  return format_value(v, out);
}

}  // namespace o3sdk::ui::plot
