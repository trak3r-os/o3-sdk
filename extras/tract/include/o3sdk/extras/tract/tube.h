// SPDX-License-Identifier: GPL-3.0-or-later
// The tube of the vocal tract: a Kelly-Lochbaum lattice (J. L. Kelly and C. C. Lochbaum, 1962; the lossless-tube form of J. D. Markel and
// A. H. Gray, "Linear Prediction of Speech", 1976), written here from that textbook material. Sections 0..N-1, 0 at the glottis and N-1 at
// the lips, area A[i] = d[i]^2. The pressure wave reflects at the junction i | i+1 with k[i] = (A[i] - A[i+1]) / (A[i] + A[i+1]).
//
// One array of state: b[i], the left-going wave of section i, one sample old, updated in place. A tract sample (tick):
//   f = x + rG * b[0];                      the glottis: the source plus the returning wave
//   for each junction i | i+1:  w = k[i] * (f - b[i+1]);  b[i] = b[i+1] + w;  f = f + w;
//   b[N-1] = -rL * f;                       the lips, an open end
//   y = (1 - rL) * lip_gain * f;            what leaves the mouth
// The forward path has no delay and every section delays the backward wave one sample, so the round trip is N samples and a uniform tube
// resonates at (2m - 1) * fs_t / (2 N): N = 22 at fs_t = 22,050 Hz is 17.5 cm (501, 1503, 2506 Hz). |k| < 1 and rG, rL < 1 make it
// passive while k stands still; the losses are at the two ends only. Under motion of k it is not passive: this form (pressure waves, not
// waves normalised by the square root of the area) gains energy when the areas change, and fast random changes of every k make it grow
// without bound (tube_test measures both). With the motion the processor makes, a one-pole of 5 ms at the fastest, it stays bounded;
// sanitize() and the processor's clamp of the wet signal are the safety net.
//
// lip_gain (not in the textbook lattice): the output is the flow at the lips, and the pressure of the last section is not: a lip opening
// whose area is under kLipRef (a diameter of 0.3) lets less flow out, down to 1 % when it is shut, where the pressure there would
// double. Without it a closure of the lips would not silence the output. It is 1 for every opening wider than that.
//
// No heap, no double: all state is members (under 400 bytes), the tube is copyable. The hot loop is tick(); its cost is counted in
// processor.h.
#pragma once

#include <cstdint>

namespace o3sdk::extras::tract {

class Tube {
 public:
  static constexpr unsigned kMaxSections = 28;
  static constexpr float kLipRef = 0.09f;  // a lip opening of this area (diameter 0.3) and wider passes the pressure as it is

  Tube() noexcept;

  void reset() noexcept;                              // clears the state
  void set_size(unsigned n) noexcept;                 // 2..kMaxSections; keeps b[], zeroes the new tail
  unsigned size() const noexcept { return n_; }
  void set_ends(float r_glottis, float r_lips) noexcept;                // both in [0, 0.99]
  void set_diameters(const float* d) noexcept;        // size() values, each > 0: the reflections k[i] and the lip gain
  void set_reflection(unsigned i, float k) noexcept;  // junction i | i+1, |k| < 1 (tests); lip_gain unchanged
  const float* reflections() const noexcept { return k_; }

  // One tract sample. Defined here so that the processor inlines it into its sample loop.
  float tick(float x) noexcept {
    float* const b = b_;
    const float* const k = k_;
    float f = x + rg_ * b[0];
    const unsigned last = n_ - 1;
    for (unsigned i = 0; i < last; ++i) {
      const float w = k[i] * (f - b[i + 1]);
      b[i] = b[i + 1] + w;
      f += w;
    }
    b[last] = -rl_ * f;
    f_ = f;
    return out_gain_ * f;
  }

  float forward() const noexcept { return f_; }       // the forward wave of the last tick at the lips
  void inject(unsigned j, float v) noexcept { b_[j] += v; }   // adds to the left-going wave of section j (the hiss)

  // Once a control period: zeroes states under 1e-15 (no denormal tail) and clears the whole tube when one is not finite or beyond 1e6.
  // Returns true when it cleared.
  bool sanitize() noexcept;

 private:
  alignas(8) float b_[kMaxSections] = {};
  alignas(8) float k_[kMaxSections] = {};
  float rg_ = 0.8f;
  float rl_ = 0.8f;
  float lip_gain_ = 1.0f;
  float out_gain_ = 0.2f;   // (1 - rl) * lip_gain
  float f_ = 0.0f;
  unsigned n_ = 22;
};

static_assert(alignof(Tube) >= 8, "the state arrays are 8-byte aligned");

}  // namespace o3sdk::extras::tract
