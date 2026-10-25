// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A real FFT of 16 to 512 points, single precision (ADR 0010 amendment 2: the spectrum's transform). It has the interface
// and the output layout of CMSIS-DSP's arm_rfft_fast_f32 (forward), so the MK66 build can swap that routine in behind
// RealFft::forward() without touching a caller; it is written here, as the radix-2 transform of n/2 complex points plus the
// real-signal split, because the host tests must run the very code that runs on the device (and the CMSIS tables for 256
// and 512 points would cost about 6 KB of flash that this does not: the twiddles are generated at init, into the object).
// Cost on a Cortex-M4F: n/4 log2(n/2) butterflies of a handful of single-cycle FPU operations each, about 20 000 cycles
// for 512 points (0.11 ms at 180 MHz, an estimate: the figure for the device is a hw-session bench).
//
// Output layout (CMSIS): out[0] = Re X[0], out[1] = Re X[n/2], then out[2k] = Re X[k] and out[2k + 1] = Im X[k] for k = 1 ..
// n/2 - 1, where X[k] = sum_m in[m] e^(-2 pi i k m / n), unscaled. The input is not modified.
//
// No heap; the tables are in the object (n floats of twiddles and n of work, n <= 512: 4 KB). Main loop only.

#include <cstdint>

namespace o3sdk::ui::plot {

class RealFft {
 public:
  static constexpr unsigned kMaxN = 512;
  static constexpr unsigned kMinN = 16;

  // n a power of two in [kMinN, kMaxN]; false (and size() == 0) otherwise. Generates the twiddles with cosf / sinf.
  bool init(unsigned n) noexcept;
  unsigned size() const noexcept { return n_; }

  // Transforms in[0 .. n) into out[0 .. n). `in` and `out` must not overlap.
  void forward(const float* in, float* out) noexcept;

 private:
  unsigned n_ = 0;
  unsigned log2_half_ = 0;                // log2(n / 2)
  float tw_[kMaxN] = {};                  // e^(-2 pi i k / n), k < n / 2: cos at 2k, -sin at 2k + 1
  std::uint16_t rev_[kMaxN / 2] = {};     // bit reversal of the n / 2 complex points
  float z_[kMaxN] = {};                   // the complex FFT's working array: n / 2 (re, im) pairs
};

}  // namespace o3sdk::ui::plot
