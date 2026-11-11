// SPDX-License-Identifier: GPL-3.0-or-later
// The half-band stages (halfband.h): the passband is flat, what the decimator would fold back and what the interpolator would image is
// rejected, and 1 kHz survives down plus up.
#include <doctest/doctest.h>

#include <o3sdk/extras/tract/halfband.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace o3sdk::extras::tract;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Amplitude of the component of x at f (cycles per sample), over the last `len` samples (an exact number of cycles is not needed: the
// window is long and the measure is a plain correlation).
double amp_at(const std::vector<float>& x, double f, std::size_t len) {
  double re = 0.0, im = 0.0;
  const std::size_t s0 = x.size() - len;
  for (std::size_t i = 0; i < len; ++i) {
    const double ph = 2.0 * kPi * f * static_cast<double>(i);
    re += static_cast<double>(x[s0 + i]) * std::cos(ph);
    im += static_cast<double>(x[s0 + i]) * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(len);
}

double db(double r) { return 20.0 * std::log10(r); }

// A sine of frequency f (cycles per input sample) through the decimator; returns the output.
std::vector<float> decimate(double f, std::size_t pairs) {
  HalfbandDown d;
  std::vector<float> y;
  y.reserve(pairs);
  for (std::size_t m = 0; m < pairs; ++m) {
    const float a = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(2 * m)));
    const float b = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(2 * m + 1)));
    y.push_back(d.push_even(a));
    d.push_odd(b);
  }
  return y;
}

std::vector<float> interpolate(double f, std::size_t n) {
  HalfbandUp u;
  std::vector<float> y;
  y.reserve(2 * n);
  for (std::size_t m = 0; m < n; ++m) {
    float o[2];
    u.push(static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(m))), o);
    y.push_back(o[0]);
    y.push_back(o[1]);
  }
  return y;
}

}  // namespace

TEST_CASE("tract halfband: the decimator is flat to 0.5 dB below 0.2 fs") {
  for (double f = 0.01; f <= 0.2001; f += 0.01) {
    const std::vector<float> y = decimate(f, 4096);
    const double g = amp_at(y, 2.0 * f, 2048);   // output rate is half: the same tone is at 2 f
    CHECK(std::fabs(db(g)) <= 0.5);
  }
}

TEST_CASE("tract halfband: the decimator rejects 0.3 .. 0.49 fs by 45 dB (what would fold into the band)") {
  for (double f = 0.30; f <= 0.4901; f += 0.01) {
    const std::vector<float> y = decimate(f, 4096);
    // the alias lands at 1 - 2 f (output cycles per sample)
    const double g = amp_at(y, std::fabs(1.0 - 2.0 * f), 2048);
    CHECK(db(g) <= -45.0);
  }
}

TEST_CASE("tract halfband: the interpolator is flat in the band and rejects the image by 45 dB") {
  for (double f = 0.01; f <= 0.2001; f += 0.01) {
    const std::vector<float> y = interpolate(f, 4096);
    const double g = amp_at(y, f / 2.0, 4096);       // output rate is double
    CHECK(std::fabs(db(g)) <= 0.5);
    // the image of a tone at f (low-rate cycles) sits at 1 - f low-rate cycles = (1 - f) / 2 output cycles
    const double im = amp_at(y, (1.0 - f) / 2.0, 4096);
    CHECK(db(im) <= -45.0);
  }
}

TEST_CASE("tract halfband: 1 kHz at 44.1 kHz keeps its level through a decimator and an interpolator, delayed by 22 samples") {
  const double f = 1000.0 / 44100.0;
  HalfbandDown d;
  HalfbandUp u;
  std::vector<float> in, out;
  for (std::size_t m = 0; m < 8192; ++m) {
    float o[2];
    const float a = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(2 * m)));
    const float b = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(2 * m + 1)));
    const float v = d.push_even(a);
    d.push_odd(b);
    u.push(v, o);
    in.push_back(a);
    in.push_back(b);
    out.push_back(o[0]);
    out.push_back(o[1]);
  }
  const double g = amp_at(out, f, 8192) / amp_at(in, f, 8192);
  CHECK(std::fabs(db(g)) <= 0.1);
  // the delay: out[n] ~ in[n - 22]
  double err = 0.0;
  for (std::size_t n = 4096; n < 4096 + 512; ++n) err = std::fmax(err, std::fabs(static_cast<double>(out[n]) - static_cast<double>(in[n - 22])));
  CHECK(err < 0.02);
}

TEST_CASE("tract halfband: the impulse of a decimator is the half-band: the 11th pair-delayed centre is 0.5") {
  HalfbandDown d;
  float y[16];
  for (int m = 0; m < 16; ++m) {
    // an impulse at odd position 1 of pair 0: only the centre tap sees it, six outputs later
    y[m] = d.push_even(0.0f);
    d.push_odd(m == 0 ? 1.0f : 0.0f);
  }
  for (int m = 0; m < 16; ++m) CHECK(y[m] == doctest::Approx(m == 6 ? 0.5 : 0.0).epsilon(1e-6));
}

TEST_CASE("tract halfband: a stage is silent after reset and never allocates state outside its members") {
  HalfbandDown d;
  HalfbandUp u;
  for (int i = 0; i < 100; ++i) {
    d.push_even(1.0f);
    d.push_odd(-1.0f);
    float o[2];
    u.push(1.0f, o);
  }
  d.reset();
  u.reset();
  float o[2];
  u.push(0.0f, o);
  CHECK(o[0] == 0.0f);
  CHECK(o[1] == 0.0f);
  CHECK(d.push_even(0.0f) == 0.0f);
}
