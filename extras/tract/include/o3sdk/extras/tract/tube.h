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

  // Two consecutive tract samples, xa then xb, in one pass over the sections: ya, yb are bit for bit what tick(xa), tick(xb) return, and the
  // state after is the same. The second sample runs one section behind the first, so the left-going wave a section takes from the first
  // sample goes straight into the second one's junction from a register: per section one load of b, one of k and one store serve both
  // samples (tick() has 2 loads and 1 store each). Nothing may touch the tube between the two samples (the processor's hiss injects between
  // ticks and so uses tick()).
  void tick2(float xa, float xb, float& ya, float& yb) noexcept {
    float* const b = b_;
    const float* const k = k_;
    const unsigned last = n_ - 1;   // junctions 0..last-1, section last is the lips
    // sample A, junction 0; ba is its new b[0], which sample B takes as its returning wave
    float fa = xa + rg_ * b[0];
    float ka = k[0];
    const float wa0 = ka * (fa - b[1]);
    float ba = b[1] + wa0;
    fa += wa0;
    float fb = xb + rg_ * ba;
    float kb = ka;   // B's junction 0 uses k[0]
    // step s: A's junction s (new b[s] into ba) and B's junction s - 1 (reads A's new b[s], stores B's b[s - 1]); kp is k[s - 1]
    auto step = [&](unsigned s, float ks, float kp) {
      const float bo = b[s + 1];
      const float wa = ks * (fa - bo);
      const float bna = bo + wa;
      fa += wa;
      const float wb = kp * (fb - bna);
      b[s - 1] = bna + wb;
      fb += wb;
    };
    unsigned s = 1;
    for (; s + 3 < last; s += 4) {   // four steps a pass: the k of the step before is the name of the one loaded, no moves, one test
      const float k0 = k[s], k1 = k[s + 1], k2 = k[s + 2], k3 = k[s + 3];
      step(s, k0, kb);
      step(s + 1, k1, k0);
      step(s + 2, k2, k1);
      step(s + 3, k3, k2);
      kb = k3;
    }
    for (; s < last; ++s) {
      ka = k[s];
      step(s, ka, kb);
      kb = ka;
    }
    // step last: A's lips, B's junction last - 1
    const float bal = -rl_ * fa;
    const float wbl = kb * (fb - bal);
    b[last - 1] = bal + wbl;
    fb += wbl;
    b[last] = -rl_ * fb;
    f_ = fb;
    ya = out_gain_ * fa;
    yb = out_gain_ * fb;
  }

  // Four consecutive tract samples, x[0..3], in one pass: y[j] and the state after are bit for bit what four tick() calls give. Lane j runs j
  // sections behind lane 0, so the left-going wave a section takes from one lane goes into the next lane's junction from a register: per
  // section one load of b, one of k and one store serve the four samples. Needs five sections or more (fewer: two tick2). As tick2, nothing
  // may touch the tube between the samples.
  void tick4(const float* x, float* y) noexcept {
    const unsigned last = n_ - 1;
    if (last < 5) {
      tick2(x[0], x[1], y[0], y[1]);
      tick2(x[2], x[3], y[2], y[3]);
      return;
    }
    float* const b = b_;
    const float* const k = k_;
    const float rg = rg_;
    // one junction of a lane: the lane's forward wave f meets bin (the left-going wave of the section after it), returns the new b
    auto junc = [](float& f, float kk, float bin) noexcept {
      const float w = kk * (f - bin);
      f += w;
      return bin + w;
    };
    // the ramp up: step s runs junction s - j of lane j (lane j starts at step j, after the lane before it has made its b[0])
    float fa = x[0] + rg * b[0];
    float ba = junc(fa, k[0], b[1]);
    float fb = x[1] + rg * ba;
    ba = junc(fa, k[1], b[2]);
    float bb = junc(fb, k[0], ba);
    float fc = x[2] + rg * bb;
    ba = junc(fa, k[2], b[3]);
    bb = junc(fb, k[1], ba);
    float bc = junc(fc, k[0], bb);
    float fd = x[3] + rg * bc;
    ba = junc(fa, k[3], b[4]);
    bb = junc(fb, k[2], ba);
    bc = junc(fc, k[1], bb);
    b[0] = junc(fd, k[0], bc);
    // all four lanes; p1..p3 are the k of the three steps before. Four steps a pass, no moves inside, the loads before the stores
    float p1 = k[3], p2 = k[2], p3 = k[1];
    unsigned s = 4;
    for (; s + 3 < last; s += 4) {
      const float k0 = k[s], k1 = k[s + 1], k2 = k[s + 2], k3 = k[s + 3];
      const float o0 = b[s + 1], o1 = b[s + 2], o2 = b[s + 3], o3 = b[s + 4];
      float r0, r1, r2, r3;
      ba = junc(fa, k0, o0);
      bb = junc(fb, p1, ba);
      bc = junc(fc, p2, bb);
      r0 = junc(fd, p3, bc);
      ba = junc(fa, k1, o1);
      bb = junc(fb, k0, ba);
      bc = junc(fc, p1, bb);
      r1 = junc(fd, p2, bc);
      ba = junc(fa, k2, o2);
      bb = junc(fb, k1, ba);
      bc = junc(fc, k0, bb);
      r2 = junc(fd, p1, bc);
      ba = junc(fa, k3, o3);
      bb = junc(fb, k2, ba);
      bc = junc(fc, k1, bb);
      r3 = junc(fd, k0, bc);
      b[s - 3] = r0;
      b[s - 2] = r1;
      b[s - 1] = r2;
      b[s] = r3;
      p1 = k3;
      p2 = k2;
      p3 = k1;
    }
    for (; s < last; ++s) {
      const float ks = k[s];
      ba = junc(fa, ks, b[s + 1]);
      bb = junc(fb, p1, ba);
      bc = junc(fc, p2, bb);
      b[s - 3] = junc(fd, p3, bc);
      p3 = p2;
      p2 = p1;
      p1 = ks;
    }
    // the ramp down: the lips of lane 0 at step last, of lane 1 at last + 1, of lane 2 at last + 2, of lane 3 the tube's own
    const float rl = rl_;
    ba = -rl * fa;
    bb = junc(fb, k[last - 1], ba);
    bc = junc(fc, k[last - 2], bb);
    b[last - 3] = junc(fd, k[last - 3], bc);
    bb = -rl * fb;
    bc = junc(fc, k[last - 1], bb);
    b[last - 2] = junc(fd, k[last - 2], bc);
    bc = -rl * fc;
    b[last - 1] = junc(fd, k[last - 1], bc);
    b[last] = -rl * fd;
    f_ = fd;
    const float g = out_gain_;
    y[0] = g * fa;
    y[1] = g * fb;
    y[2] = g * fc;
    y[3] = g * fd;
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
