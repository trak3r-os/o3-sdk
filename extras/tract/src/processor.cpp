// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/processor.h>

#include <o3sdk/extras/tract/shape.h>

#include <cmath>
#include <cstring>

namespace o3sdk::extras::tract {

namespace {

constexpr float kSettle = 2e-4f;       // a one-pole lands on its target when this close
constexpr float kHissOpen = 0.3f;      // a section narrower than this makes turbulence
constexpr float kWetMax = 8.0f;        // the wet signal is clamped here
constexpr float kTrimFloor = 1e-7f;    // the level trim holds while the source's power is under this (-70 dB)
constexpr float kTrimMin = 1.0f / 32.0f;   // -30 dB
constexpr float kTrimMax = 16.0f;          // +24 dB
constexpr float kEnvA = 0.02f;         // the follower of the source, per tract sample

inline float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// A parameter: NaN takes the default, anything else is clamped.
inline float sane(float v, float lo, float hi, float def) noexcept { return v != v ? def : clampf(v, lo, hi); }

// One pole towards t; lands on it when close. Returns true when v changed.
inline bool approach(float& v, float t, float a) noexcept {
  const float d = t - v;
  if (d == 0.0f) return false;
  if (d < kSettle && d > -kSettle) v = t;
  else v += d * a;
  return true;
}

// A linear ramp towards t of at most `step` per call.
inline bool ramp(float& v, float t, float step) noexcept {
  const float d = t - v;
  if (d == 0.0f) return false;
  if (d <= step && d >= -step) v = t;
  else v += d > 0.0f ? step : -step;
  return true;
}

}  // namespace

Processor::Processor(Quality q) noexcept
    : quality_(q), group_(q == Quality::Full ? 2u : 4u), n0_(q == Quality::Full ? 22u : 11u), n_pending_(q == Quality::Full ? 22u : 11u) {
  prepare(44100.0f);
}

unsigned Processor::size_to_sections(int steps) const noexcept {
  const int s = steps < -2 ? -2 : (steps > 2 ? 2 : steps);
  const int per = quality_ == Quality::Full ? 2 : 1;
  return static_cast<unsigned>(static_cast<int>(n0_) - per * s);
}

void Processor::apply_ends() noexcept {
  const float r = 0.60f + 0.37f * res_;
  tube_.set_ends(r, r);
  lvl_t_ = kMakeup * std::pow(1.0f / (1.0f - r), 1.35f);   // the middle of what the vowels need (the trim does the rest)
}

void Processor::prepare(float sample_rate) noexcept {
  fs_ = sane(sample_rate, 8000.0f, 192000.0f, 44100.0f);
  clos_step_ = static_cast<float>(kPeriod) / (0.012f * fs_);
  mix_step_ = 1.0f / (0.005f * fs_);
  dip_step_ = 1.0f / (0.0025f * fs_);
  set_glide_ms(glide_ms_);
  agc_a_ = 1.0f - std::exp(-static_cast<float>(group_) / (0.06f * fs_));   // 60 ms, per tract sample
  reset();
}

void Processor::wake() noexcept {
  tube_.reset();
  down_[0].reset();
  down_[1].reset();
  up_[0].reset();
  up_[1].reset();
  std::memset(queue_, 0, sizeof queue_);
  phase_ = 0;
  ctl_ = 0;
  env_ = 0.0f;
  ed_ = ew_ = 0.0f;
  trim_ = 1.0f;
  dip_ = 1.0f;
  dip_state_ = 0;
  tube_.set_size(n_pending_);
  vowel_ = vowel_t_;
  for (unsigned i = 0; i < 3; ++i) {
    bend_[i] = bend_t_[i];
    clos_[i] = clos_t_[i];
  }
  lvl_ = lvl_t_;
  apply_ends();
  lvl_ = lvl_t_;
  seal_ = 1.0f - std::fmax(clos_[0], std::fmax(clos_[1], clos_[2]));
  gain_ = lvl_ * trim_ * seal_;
  shape_dirty_ = true;
  dormant_ = false;
}

void Processor::reset() noexcept {
  mix_ = mix_t_;
  rng_ = 1u;
  wake();
}

void Processor::set_vowel(float v) noexcept { vowel_t_ = sane(v, 0.0f, 1.0f, 0.0f); }

void Processor::set_bend(float tongue, float jaw, float lips) noexcept {
  bend_t_[0] = sane(tongue, -1.0f, 1.0f, 0.0f);
  bend_t_[1] = sane(jaw, -1.0f, 1.0f, 0.0f);
  bend_t_[2] = sane(lips, -1.0f, 1.0f, 0.0f);
}

void Processor::set_closure(unsigned which, bool on) noexcept {
  if (which < 3) clos_t_[which] = on ? 1.0f : 0.0f;
}

void Processor::set_glide_ms(float ms) noexcept {
  glide_ms_ = sane(ms, 5.0f, 500.0f, 20.0f);
  glide_a_ = 1.0f - std::exp(-static_cast<float>(kPeriod) / (glide_ms_ * 0.001f * fs_));
}

void Processor::set_size(int steps) noexcept { n_pending_ = size_to_sections(steps); }

void Processor::set_res(float r) noexcept {
  res_ = sane(r, 0.0f, 1.0f, 0.5f);
  apply_ends();
}

void Processor::set_hiss(float h) noexcept {
  hiss_ = sane(h, 0.0f, 1.0f, 0.0f);
  shape_dirty_ = true;
}

void Processor::set_mix(float m) noexcept {
  const float v = sane(m, 0.0f, 1.0f, 1.0f);
  mix_t_ = v < 1e-3f ? 0.0f : v;
}

void Processor::update_shape() noexcept {
  Shape s = vowel_shape(vowel_);
  s.tongue += bend_[0];
  s.height -= bend_[1];
  s.lips += bend_[2];
  const unsigned n = tube_.size();
  diameters(s, clos_, n, diam_);
  tube_.set_diameters(diam_);
  float dmin = diam_[0];
  unsigned j = 0;
  for (unsigned i = 1; i < n; ++i) {
    if (diam_[i] < dmin) {
      dmin = diam_[i];
      j = i;
    }
  }
  hiss_j_ = j;
  hiss_amt_ = (hiss_ > 0.0f && dmin < kHissOpen) ? hiss_ * (kHissOpen - dmin) * (1.0f / kHissOpen) : 0.0f;
  shape_dirty_ = false;
}

void Processor::control() noexcept {
  bool moved = shape_dirty_;
  moved |= approach(vowel_, vowel_t_, glide_a_);
  for (unsigned i = 0; i < 3; ++i) {
    moved |= approach(bend_[i], bend_t_[i], glide_a_);
    moved |= ramp(clos_[i], clos_t_[i], clos_step_);
  }
  approach(lvl_, lvl_t_, 0.25f);
  // A closed tube still rings (a sealed cavity has a pressure gain), so the output is shut by the closure itself: linear in the largest one.
  seal_ = 1.0f - std::fmax(clos_[0], std::fmax(clos_[1], clos_[2]));
  bool still = true;   // no closure on or moving: the level trim may follow
  for (unsigned i = 0; i < 3; ++i) still = still && clos_[i] == 0.0f && clos_t_[i] == 0.0f;
  if (still && ed_ > kTrimFloor && ew_ > 0.0f) {
    const float g = std::sqrt(ed_ / ew_) / lvl_;
    trim_ += (clampf(g, kTrimMin, kTrimMax) - trim_) * 0.1f;
  }
  gain_ = lvl_ * trim_ * seal_;   // the wet gain but for the size dip, once per step
  if (ed_ < 1e-20f) ed_ = 0.0f;
  if (ew_ < 1e-20f) ew_ = 0.0f;
  if (dip_state_ == 0 && n_pending_ != tube_.size()) dip_state_ = 1;
  if (moved) update_shape();
  if (tube_.sanitize()) {
    down_[0].reset();
    down_[1].reset();
    up_[0].reset();
    up_[1].reset();
    std::memset(queue_, 0, sizeof queue_);
    env_ = 0.0f;
    ed_ = ew_ = 0.0f;
  }
  if (!(env_ < 1e6f) || env_ < 1e-15f) env_ = 0.0f;
}

void Processor::process(const float* in, float* out, std::uint32_t n) noexcept {
  if (n == 0) return;
  if (bypassed()) {
    if (out != in) std::memcpy(out, in, static_cast<std::size_t>(n) * sizeof(float));
    dormant_ = true;
    return;
  }
  if (dormant_) wake();

  const bool lite = quality_ == Quality::Lite;
  const unsigned gmask = group_ - 1u;
  unsigned p = phase_;   // in a register for the block
  for (std::uint32_t i = 0; i < n; ++i) {
    if (ctl_ == 0) {
      control();
      ctl_ = kPeriod;
    }
    --ctl_;
    const float x = in[i];

    // The frame that feeds the tube (position 0 of its group) runs one tract sample and refills the queue.
    float u = 0.0f;
    bool tick = false;
    if (!lite) {
      if (p == 0) {
        u = down_[0].push_even(x);
        tick = true;
      } else {
        down_[0].push_odd(x);
      }
    } else if (p == 0) {
      u = down_[1].push_even(down_[0].push_even(x));
      tick = true;
    } else if (p == 2) {
      down_[1].push_odd(down_[0].push_even(x));
    } else {
      down_[0].push_odd(x);
    }
    if (tick) {
      const float y = tube_.tick(u);
      ed_ += (u * u - ed_) * agc_a_;
      ew_ += (y * y - ew_) * agc_a_;
      if (hiss_amt_ > 0.0f) {
        env_ += (std::fabs(u) - env_) * kEnvA;
        rng_ = rng_ * 1664525u + 1013904223u;
        const float noise = static_cast<float>(static_cast<std::int32_t>(rng_)) * (1.0f / 2147483648.0f);
        tube_.inject(hiss_j_, noise * hiss_amt_ * env_);
      }
      if (!lite) {
        up_[0].push(y, queue_);
      } else {
        float m[2];
        up_[0].push(y, m);
        up_[1].push(m[0], queue_);
        up_[1].push(m[1], queue_ + 2);
      }
    }

    if (dip_state_ != 0) {
      if (dip_state_ == 1) {
        dip_ -= dip_step_;
        if (dip_ <= 0.0f) {
          dip_ = 0.0f;
          tube_.set_size(n_pending_);
          shape_dirty_ = true;
          ctl_ = 0;   // the next frame recomputes the reflections of the new size before it ticks
          dip_state_ = 2;
        }
      } else {
        dip_ += dip_step_;
        if (dip_ >= 1.0f) {
          dip_ = 1.0f;
          dip_state_ = 0;
        }
      }
    }

    ramp(mix_, mix_t_, mix_step_);
    float o = x;
    if (mix_ > 0.0f) {
      float w = queue_[p] * (dip_state_ != 0 ? gain_ * dip_ : gain_);
      w = w > kWetMax ? kWetMax : (w < -kWetMax ? -kWetMax : w);   // a blown-up tube never reaches the output
      o = mix_ >= 1.0f ? w : x + (w - x) * mix_;
    }
    out[i] = o;
    p = (p + 1u) & gmask;
  }
  phase_ = p;
}

}  // namespace o3sdk::extras::tract
