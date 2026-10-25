// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/spectrum.h>

#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/plot/plot_math.h>

#include <cmath>

namespace o3sdk::ui::plot {

namespace {

namespace dl = eve::dl;
using eve::Primitive;

constexpr float kTwoPi = 6.28318530717959f;

}  // namespace

bool Spectrum::init(const Config& c) noexcept {
  ready_ = false;
  if (c.bars < 1 || c.bars > kMaxBars || !fft_.init(c.fft_size) || !(c.sample_rate > 0.0f) ||
      !(c.f_lo >= bin_hz(c.fft_size, c.sample_rate)) || !(c.f_hi > c.f_lo) || c.f_hi > c.sample_rate * 0.5f ||
      !(c.ceil_db > c.floor_db)) {
    return false;
  }
  cfg_ = c;
  const unsigned n = c.fft_size;
  float sum = 0.0f;
  for (unsigned i = 0; i < n; ++i) {  // Hann, periodic
    window_[i] = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(n));
    sum += window_[i];
  }
  const float gain = 2.0f / (sum * 32768.0f);  // amplitude of a full-scale sine -> 1.0
  norm_ = gain * gain;
  const float log2_ratio = log2_fast(c.f_hi / c.f_lo);
  const float bin_hz = c.sample_rate / static_cast<float>(n);
  for (unsigned b = 0; b < c.bars; ++b) {
    const float f0 = c.f_lo * exp2_fast(log2_ratio * static_cast<float>(b) / static_cast<float>(c.bars));
    const float f1 = c.f_lo * exp2_fast(log2_ratio * static_cast<float>(b + 1u) / static_cast<float>(c.bars));
    unsigned k0 = static_cast<unsigned>(f0 / bin_hz + 0.5f);
    unsigned k1 = static_cast<unsigned>(f1 / bin_hz + 0.5f);
    k0 = k0 < 1u ? 1u : k0;                      // bin 0 is DC
    k1 = k1 > n / 2u ? n / 2u : k1;
    k0 = k0 > n / 2u - 1u ? n / 2u - 1u : k0;
    if (k1 <= k0) {
      k1 = k0 + 1u;
    }
    bin_lo_[b] = static_cast<std::uint16_t>(k0);
    bin_hi_[b] = static_cast<std::uint16_t>(k1);
    if (k1 - k0 <= 1u) {  // narrower than a bin: interpolate round the centre
      const float pos = 0.5f * (f0 + f1) / bin_hz;
      unsigned i0 = static_cast<unsigned>(pos);
      i0 = i0 < 1u ? 1u : (i0 > n / 2u - 1u ? n / 2u - 1u : i0);
      bin_lo_[b] = static_cast<std::uint16_t>(i0);
      bin_hi_[b] = static_cast<std::uint16_t>(i0 + 1u);
      const float frac = pos - static_cast<float>(i0);
      frac_[b] = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    } else {
      frac_[b] = 0.0f;
    }
  }
  ready_ = true;
  reset();
  return true;
}

void Spectrum::reset() noexcept {
  for (unsigned b = 0; b < kMaxBars; ++b) {
    raw_[b] = level_[b] = peak_[b] = cfg_.floor_db;
    hold_[b] = 0;
  }
}

float Spectrum::bar_center_hz(unsigned bar) const noexcept {
  if (!ready_ || bar >= cfg_.bars) {
    return 0.0f;
  }
  return cfg_.f_lo * exp2_fast(log2_fast(cfg_.f_hi / cfg_.f_lo) * (static_cast<float>(bar) + 0.5f) / static_cast<float>(cfg_.bars));
}

void Spectrum::analyze(const std::int16_t* frames, unsigned channels) noexcept {
  if (!ready_ || frames == nullptr) {
    return;
  }
  const unsigned n = cfg_.fft_size;
  if (channels == 2) {
    for (unsigned i = 0; i < n; ++i) {
      buf_[i] = 0.5f * static_cast<float>(static_cast<int>(frames[2u * i]) + static_cast<int>(frames[2u * i + 1u])) * window_[i];
    }
  } else {
    for (unsigned i = 0; i < n; ++i) {
      buf_[i] = static_cast<float>(frames[i]) * window_[i];
    }
  }
  fft_.forward(buf_, out_);
  const auto power = [&](unsigned k) {  // |X[k]|^2, k in 1 .. n/2 - 1
    return (out_[2u * k] * out_[2u * k] + out_[2u * k + 1u] * out_[2u * k + 1u]) * norm_;
  };
  for (unsigned b = 0; b < cfg_.bars; ++b) {
    float p;
    if (static_cast<unsigned>(bin_hi_[b] - bin_lo_[b]) <= 1u) {
      const unsigned i0 = bin_lo_[b];
      const unsigned i1 = i0 + 1u < n / 2u ? i0 + 1u : i0;
      p = power(i0) * (1.0f - frac_[b]) + power(i1) * frac_[b];
    } else {
      p = 0.0f;
      for (unsigned k = bin_lo_[b]; k < bin_hi_[b]; ++k) {
        const float pk = power(k);
        p = pk > p ? pk : p;
      }
    }
    float db = db_from_power(p);
    db = db < cfg_.floor_db ? cfg_.floor_db : (db > cfg_.ceil_db + 6.0f ? cfg_.ceil_db + 6.0f : db);
    raw_[b] = db;
    level_[b] += (db - level_[b]) * (db > level_[b] ? kAttack : kRelease);
    if (level_[b] >= peak_[b]) {
      peak_[b] = level_[b];
      hold_[b] = static_cast<std::uint8_t>(kHoldCalls);
    } else if (hold_[b] > 0) {
      --hold_[b];
    } else {
      peak_[b] -= kPeakFallDb;
      peak_[b] = peak_[b] < level_[b] ? level_[b] : peak_[b];
    }
  }
}

namespace {

// Pixels (0 .. area.h) of a level in dB.
int height_of(float db, const Spectrum::Config& c, int h) noexcept {
  const float t = (db - c.floor_db) / (c.ceil_db - c.floor_db);
  const float v = t * static_cast<float>(h) + 0.5f;
  return v <= 0.0f ? 0 : (v >= static_cast<float>(h) ? h : static_cast<int>(v));
}

}  // namespace

void draw_spectrum(Renderer& r, const Rect& area, const Spectrum& s, const SpectrumStyle& style) noexcept {
  if (!s.ready() || area.w < 1 || area.h < 1) {
    return;
  }
  const unsigned n = s.bars();
  const Spectrum::Config& c = s.config();
  const int bottom = area.y + area.h;
  // bar b spans the columns [x(b), x(b + 1)), less a pixel of gap when the bars are wide enough to spare it
  const auto x_of = [&](unsigned b) { return area.x + static_cast<int>(static_cast<std::int64_t>(area.w) * b / n); };
  const bool gap = area.w >= static_cast<int>(2u * n);
  if (style.shape == SpectrumShape::Bars) {
    for (unsigned b = 0; b < n; ++b) {
      const int h = height_of(s.level_db(b), c, area.h);
      if (h > 0) {
        r.panel({x_of(b), bottom - h, x_of(b + 1) - x_of(b) - (gap ? 1 : 0), h}, style.bar);
      }
    }
  } else {
    r.begin_raw();
    r.raw(dl::scissor_xy(static_cast<std::uint32_t>(area.x), static_cast<std::uint32_t>(area.y)));
    r.raw(dl::scissor_size(static_cast<std::uint32_t>(area.w), static_cast<std::uint32_t>(area.h)));
    for (int pass = 0; pass < 2; ++pass) {
      if (pass == 0) {
        r.raw(dl::color_a(style.fill_alpha));
      } else {
        r.raw(dl::color_a(255));
        r.raw(dl::line_width(12));
      }
      r.raw(r.color_word(style.bar));
      r.raw(dl::begin(pass == 0 ? Primitive::EdgeStripB : Primitive::LineStrip));
      for (unsigned b = 0; b < n; ++b) {
        const int h = height_of(s.level_db(b), c, area.h);
        const std::int32_t cx = (x_of(b) + x_of(b + 1)) * 8;  // the bar's centre, in 1/16 px
        r.raw(dl::vertex2f(cx, (bottom - h) * 16));
      }
      r.raw(dl::end());
    }
    r.end_raw();
  }
  if (style.caps) {
    for (unsigned b = 0; b < n; ++b) {
      const int h = height_of(s.peak_db(b), c, area.h);
      if (h > 0) {
        const int top = bottom - h;
        r.panel({x_of(b), top, x_of(b + 1) - x_of(b) - (gap ? 1 : 0), top + 2 <= bottom ? 2 : 1}, style.cap);
      }
    }
  }
}

}  // namespace o3sdk::ui::plot
