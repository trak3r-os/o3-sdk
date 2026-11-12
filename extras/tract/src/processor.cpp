// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/extras/tract/processor.h>

#include <o3sdk/extras/tract/shape.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace o3sdk::extras::tract {

namespace {

constexpr float kSettle = 2e-4f;       // a one-pole lands on its target when this close
constexpr float kAreaHuge = 1.0e30f;   // set_areas takes areas under this (its sqrt and a ratio stay finite)
constexpr float kHissOpen = 0.3f;      // a section narrower than this makes turbulence
constexpr float kWetMax = 8.0f;        // the wet signal is clamped here
constexpr std::uint32_t kWetMaxBits = 0x41000000u;   // the bits of kWetMax (8.0f)
static_assert(kWetMax == 8.0f, "kWetMaxBits is the bit pattern of 8.0f");
constexpr float kTrimFloor = 1e-7f;    // the level trim holds while the source's power is under this (-70 dB)
constexpr float kTrimMin = 1.0f / 32.0f;   // -30 dB
constexpr float kTrimMax = 16.0f;          // +24 dB
constexpr float kEnvA = 0.02f;         // the follower of the source, per tract sample

inline float max3(float a, float b, float c) noexcept {   // no NaN here (the closures are 0..1): the plain compares, not fmaxf's calls
  const float m = a > b ? a : b;
  return m > c ? m : c;
}

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

// The wet sample of a frame: the up-sampled tract sample times the wet gain, clamped. The one test on the bits (|w| over 8, or not a number)
// replaces two float compares; the clamp itself is the plain expression, so a NaN still passes through as it did.
inline float wet_of(float q, float gain) noexcept {
  float w = q * gain;
  std::uint32_t bits;
  std::memcpy(&bits, &w, sizeof bits);
  if (__builtin_expect((bits & 0x7fffffffu) > kWetMaxBits, 0)) w = w > kWetMax ? kWetMax : (w < -kWetMax ? -kWetMax : w);
  return w;
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
  live_n_ = 0;   // area mode: the next control step lands on the areas (the state is cleared, a glide has nothing to start from)
  if (area_mode_ == 2) area_mode_ = 0;   // and a way back to the shape is taken at once: update_shape lands on it
  vowel_ = vowel_t_;
  for (unsigned i = 0; i < 3; ++i) {
    bend_[i] = bend_t_[i];
    clos_[i] = clos_t_[i];
  }
  lvl_ = lvl_t_;
  apply_ends();
  lvl_ = lvl_t_;
  seal_ = 1.0f - max3(clos_[0], clos_[1], clos_[2]);
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

void Processor::shape_diameters(float* d, unsigned n) const noexcept {
  Shape s = vowel_shape(vowel_);
  s.tongue += bend_[0];
  s.height -= bend_[1];
  s.lips += bend_[2];
  diameters(s, clos_, n, d);
}

void Processor::commit_diameters(unsigned n) noexcept {
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
}

void Processor::update_shape() noexcept {
  const unsigned n = tube_.size();
  shape_diameters(diam_, n);
  commit_diameters(n);
  shape_dirty_ = false;
}

// The areas of the caller as diameters: the widest section is the open tract (the shape model's 1.5), the others follow as sqrt of the ratio,
// clamped to kMinDiameter..1.5. Anything that is not a finite positive area makes the call a no-op.
void Processor::set_areas(const float* area, unsigned n) noexcept {
  if (area == nullptr || n < 2u) return;
  if (n > kMaxAreas) n = kMaxAreas;
  float top = 0.0f;
  for (unsigned i = 0; i < n; ++i) {
    const float a = area[i];
    if (!(a > 0.0f) || !(a < kAreaHuge)) return;   // NaN, <= 0 and infinity all fail one of the two
    top = a > top ? a : top;
  }
  const float scale = kAreaModel.open_d / std::sqrt(top);
  for (unsigned i = 0; i < n; ++i) src_d_[i] = clampf(std::sqrt(area[i]) * scale, kMinDiameter, kAreaModel.open_d);
  src_n_ = n;
  area_new_ = true;
  if (area_mode_ == 0) live_n_ = shape_dirty_ ? 0u : tube_.size();   // diam_ holds the shape: glide from it, unless it is stale (snap)
  area_mode_ = 1;
}

void Processor::use_shape() noexcept {
  if (area_mode_ == 1) area_mode_ = 2;
}

// src_d_ (src_n_ points, section i the centre of the i-th of src_n equal parts of the tube) stretched over n sections, linear in diameter.
void Processor::resample_areas(unsigned n) noexcept {
  const float ratio = static_cast<float>(src_n_) / static_cast<float>(n);
  const float last = static_cast<float>(src_n_ - 1u);
  for (unsigned j = 0; j < n; ++j) {
    float x = (static_cast<float>(j) + 0.5f) * ratio - 0.5f;
    x = clampf(x, 0.0f, last);
    unsigned i = static_cast<unsigned>(x);
    if (i >= src_n_ - 1u) i = src_n_ - 2u;
    const float f = x - static_cast<float>(i);
    dt_[j] = src_d_[i] + (src_d_[i + 1u] - src_d_[i]) * f;
  }
}

void Processor::area_step() noexcept {
  const unsigned n = tube_.size();
  bool refresh = shape_dirty_;
  if (area_mode_ == 2) {
    if (live_n_ != n) {   // the size changed under the way back (the dip hides the jump): the vowel path takes over
      area_mode_ = 0;
      update_shape();
      return;
    }
    shape_diameters(dt_, n);
  } else if (live_n_ != n || area_new_) {
    resample_areas(n);
    area_new_ = false;
  }
  if (live_n_ != n) {   // nothing to glide from (a wake, a size change, a stale shape): land on the target
    live_n_ = n;
    for (unsigned i = 0; i < n; ++i) diam_[i] = dt_[i];
    refresh = true;
  } else {
    bool moved = false;
    for (unsigned i = 0; i < n; ++i) moved |= approach(diam_[i], dt_[i], glide_a_);
    if (!moved && area_mode_ == 2) {   // landed on the shape: the vowel path again
      area_mode_ = 0;
      update_shape();
      return;
    }
    refresh |= moved;
  }
  if (refresh) {
    commit_diameters(n);
    shape_dirty_ = false;
  }
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
  seal_ = 1.0f - max3(clos_[0], clos_[1], clos_[2]);
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
  if (area_mode_ != 0) area_step();
  else if (moved) update_shape();
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

// The frames between two control steps (a chunk) go through three passes: the half-band down samples the whole chunk into the tract samples,
// the tube runs them (two at a time, tick2), and the half-band up and the wet path write the frames. The samples are the ones the plain
// frame by frame machine makes, bit for bit: a frame with a size dip running (rare, 5 ms) is run by that machine itself (slow_frame).
template <bool Lite>
void Processor::run(const float* in, float* out, std::uint32_t n) noexcept {
  constexpr unsigned kGroup = Lite ? 4u : 2u;
  constexpr unsigned kMask = kGroup - 1u;
  constexpr unsigned kMaxTicks = kPeriod / kGroup;   // tract samples in at most kPeriod frames
  unsigned p = phase_;   // in a register for the block

  // The frame by frame machine, for the frames of a size dip.
  auto slow_frame = [&](float x) -> float {
    --ctl_;
    float u = 0.0f;
    bool tick = false;
    if constexpr (!Lite) {
      if (p == 0) {
        u = down_[0].push_even(x);
        tick = true;
      } else {
        down_[0].push_odd(x);
      }
    } else {
      if (p == 0) {
        u = down_[1].push_even(down_[0].push_even(x));
        tick = true;
      } else if (p == 2) {
        down_[1].push_odd(down_[0].push_even(x));
      } else {
        down_[0].push_odd(x);
      }
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
      if constexpr (!Lite) {
        up_[0].push(y, queue_);
      } else {
        float m[2];
        up_[0].push(y, m);
        up_[1].push(m[0], queue_);
        up_[1].push(m[1], queue_ + 2);
      }
    }
    if (dip_state_ == 1) {
      dip_ -= dip_step_;
      if (dip_ <= 0.0f) {
        dip_ = 0.0f;
        tube_.set_size(n_pending_);
        shape_dirty_ = true;
        ctl_ = 0;   // the next frame recomputes the reflections of the new size before it ticks
        dip_state_ = 2;
      }
    } else if (dip_state_ != 0) {
      dip_ += dip_step_;
      if (dip_ >= 1.0f) {
        dip_ = 1.0f;
        dip_state_ = 0;
      }
    }
    ramp(mix_, mix_t_, mix_step_);
    float o = x;
    if (mix_ > 0.0f) {
      float w = queue_[p] * (dip_state_ != 0 ? gain_ * dip_ : gain_);
      w = w > kWetMax ? kWetMax : (w < -kWetMax ? -kWetMax : w);   // a blown-up tube never reaches the output
      o = mix_ >= 1.0f ? w : x + (w - x) * mix_;
    }
    p = (p + 1u) & kMask;
    return o;
  };

  std::uint32_t i = 0;
  while (i < n) {
    if (ctl_ == 0) {
      control();
      ctl_ = kPeriod;
    }
    if (dip_state_ != 0) {
      out[i] = slow_frame(in[i]);
      ++i;
      continue;
    }
    const unsigned m = static_cast<unsigned>(n - i < ctl_ ? n - i : ctl_);
    ctl_ -= m;
    const float* const x = in + i;
    float* const o = out + i;

    // Pass 1: the half-band down; one tract sample u[t] per frame at position 0 of its group.
    float u[kMaxTicks];
    float y[kMaxTicks];
    unsigned ticks = 0;
    {
      unsigned q = p;
      for (unsigned f = 0; f < m; ++f) {
        const float xv = x[f];
        if constexpr (!Lite) {
          if (q == 0) u[ticks++] = down_[0].push_even(xv);
          else down_[0].push_odd(xv);
        } else {
          if (q == 0) u[ticks++] = down_[1].push_even(down_[0].push_even(xv));
          else if (q == 2) down_[1].push_odd(down_[0].push_even(xv));
          else down_[0].push_odd(xv);
        }
        q = (q + 1u) & kMask;
      }
    }

    // Pass 2: the tube, and the followers of its input and output.
    if (hiss_amt_ == 0.0f) {
      unsigned t = 0;
      for (; t + 3 < ticks; t += 4) tube_.tick4(u + t, y + t);
      for (; t + 1 < ticks; t += 2) tube_.tick2(u[t], u[t + 1], y[t], y[t + 1]);
      if (t < ticks) y[t] = tube_.tick(u[t]);
      float ed = ed_, ew = ew_;
      const float a = agc_a_;
      for (t = 0; t < ticks; ++t) {
        ed += (u[t] * u[t] - ed) * a;
        ew += (y[t] * y[t] - ew) * a;
      }
      ed_ = ed;
      ew_ = ew;
    } else {
      for (unsigned t = 0; t < ticks; ++t) {
        y[t] = tube_.tick(u[t]);
        ed_ += (u[t] * u[t] - ed_) * agc_a_;
        ew_ += (y[t] * y[t] - ew_) * agc_a_;
        env_ += (std::fabs(u[t]) - env_) * kEnvA;
        rng_ = rng_ * 1664525u + 1013904223u;
        const float noise = static_cast<float>(static_cast<std::int32_t>(rng_)) * (1.0f / 2147483648.0f);
        tube_.inject(hiss_j_, noise * hiss_amt_ * env_);
      }
    }

    // Pass 3: the half-band up (the queue of the group) and the wet path. The groups: the head is what is left of the group begun in an
    // earlier chunk, then each tract sample starts a group (the last may be cut by the end of the chunk).
    const float gain = gain_;
    auto pass3 = [&](auto mode) {   // mode: 0 wet only, 1 mixed at a settled mix, 2 mix still ramping
      constexpr int kMode = decltype(mode)::value;
      float mx = mix_;
      const float mt = mix_t_, ms = mix_step_;
      auto frame = [&](unsigned f, float q) {
        const float xv = x[f];
        if constexpr (kMode == 0) {
          o[f] = wet_of(q, gain);
        } else if constexpr (kMode == 1) {
          const float w = wet_of(q, gain);
          o[f] = xv + (w - xv) * mx;
        } else {
          ramp(mx, mt, ms);
          float r = xv;
          if (mx > 0.0f) {
            const float w = wet_of(q, gain);
            r = mx >= 1.0f ? w : xv + (w - xv) * mx;
          }
          o[f] = r;
        }
      };
      unsigned f = 0;
      if (p != 0) {   // head
        const unsigned c = kGroup - p < m ? kGroup - p : m;
        float qv[kGroup];
        for (unsigned j = 0; j < kGroup; ++j) qv[j] = queue_[j];
        for (unsigned j = 0; j < c; ++j) frame(f + j, qv[p + j]);
        f = c;
      }
      for (unsigned t = 0; f < m; ++t) {
        float qv[kGroup];
        if constexpr (!Lite) {
          up_[0].push(y[t], qv);
        } else {
          float mm[2];
          up_[0].push(y[t], mm);
          up_[1].push(mm[0], qv);
          up_[1].push(mm[1], qv + 2);
        }
        for (unsigned j = 0; j < kGroup; ++j) queue_[j] = qv[j];
        const unsigned c = m - f < kGroup ? m - f : kGroup;
        for (unsigned j = 0; j < c; ++j) frame(f + j, qv[j]);
        f += c;
      }
      mix_ = mx;
    };
    if (mix_ == mix_t_ && mix_ > 0.0f) {
      if (mix_ >= 1.0f) pass3(std::integral_constant<int, 0>{});
      else pass3(std::integral_constant<int, 1>{});
    } else {
      pass3(std::integral_constant<int, 2>{});
    }
    p = (p + m) & kMask;
    i += m;
  }
  phase_ = p;
}

void Processor::process(const float* in, float* out, std::uint32_t n) noexcept {
  if (n == 0) return;
  if (bypassed()) {
    if (out != in) std::memcpy(out, in, static_cast<std::size_t>(n) * sizeof(float));
    dormant_ = true;
    return;
  }
  if (dormant_) wake();
  if (quality_ == Quality::Lite) run<true>(in, out, n);
  else run<false>(in, out, n);
}

}  // namespace o3sdk::extras::tract
