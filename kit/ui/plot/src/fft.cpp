// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/plot/fft.h>

#include <cmath>

namespace o3sdk::ui::plot {

namespace {
constexpr float kTwoPi = 6.28318530717959f;
}

bool RealFft::init(unsigned n) noexcept {
  n_ = 0;
  if (n < kMinN || n > kMaxN || (n & (n - 1u)) != 0) {
    return false;
  }
  unsigned log2_half = 0;
  while ((1u << log2_half) < n / 2u) {
    ++log2_half;
  }
  log2_half_ = log2_half;
  for (unsigned k = 0; k < n / 2u; ++k) {
    const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(n);
    tw_[2u * k] = std::cos(a);
    tw_[2u * k + 1u] = -std::sin(a);
  }
  for (unsigned i = 0; i < n / 2u; ++i) {  // bit reversal over log2_half bits
    unsigned r = 0;
    for (unsigned b = 0; b < log2_half; ++b) {
      r |= ((i >> b) & 1u) << (log2_half - 1u - b);
    }
    rev_[i] = static_cast<std::uint16_t>(r);
  }
  n_ = n;
  return true;
}

void RealFft::forward(const float* in, float* out) noexcept {
  if (n_ == 0) {
    return;
  }
  const unsigned half = n_ / 2u;  // complex points
  // z[m] = in[2m] + i in[2m + 1], in bit-reversed order
  for (unsigned m = 0; m < half; ++m) {
    const unsigned r = rev_[m];
    z_[2u * r] = in[2u * m];
    z_[2u * r + 1u] = in[2u * m + 1u];
  }
  // radix-2 decimation in time over `half` points: stage with butterflies of span `len`
  for (unsigned len = 2; len <= half; len <<= 1) {
    const unsigned step = n_ / len;  // twiddle e^(-2 pi i j / len) = tw[j * n / len]
    for (unsigned base = 0; base < half; base += len) {
      for (unsigned j = 0; j < len / 2u; ++j) {
        const float wr = tw_[2u * j * step], wi = tw_[2u * j * step + 1u];
        const unsigned a = 2u * (base + j), b = 2u * (base + j + len / 2u);
        const float tr = z_[b] * wr - z_[b + 1u] * wi;
        const float ti = z_[b] * wi + z_[b + 1u] * wr;
        z_[b] = z_[a] - tr;
        z_[b + 1u] = z_[a + 1u] - ti;
        z_[a] += tr;
        z_[a + 1u] += ti;
      }
    }
  }
  // split the complex transform Z of the even / odd samples into the real signal's X:
  //   X[k] = (Z[k] + conj Z[N-k]) / 2  +  e^(-2 pi i k / n) (Z[k] - conj Z[N-k]) / (2i),  N = n / 2
  out[0] = z_[0] + z_[1];
  out[1] = z_[0] - z_[1];
  for (unsigned k = 1; k < half; ++k) {
    const float zr = z_[2u * k], zi = z_[2u * k + 1u];
    const float cr = z_[2u * (half - k)], ci = -z_[2u * (half - k) + 1u];  // conj Z[N-k]
    const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);              // the even part
    const float dr = 0.5f * (zr - cr), di = 0.5f * (zi - ci);
    const float orr = di, oi = -dr;                                        // (Z - conj) / (2i): divide by i
    const float wr = tw_[2u * k], wi = tw_[2u * k + 1u];
    out[2u * k] = er + (orr * wr - oi * wi);
    out[2u * k + 1u] = ei + (orr * wi + oi * wr);
  }
}

}  // namespace o3sdk::ui::plot
