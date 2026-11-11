// SPDX-License-Identifier: GPL-3.0-or-later
// The tract processor (processor.h), both qualities: bypass is bit exact, the output does not depend on the block size, vowel moves and
// closures do not click, a closure silences and its release pops, a size change is bounded, the level stays near the dry one, and
// silence leaves exact zeros.
#include <doctest/doctest.h>

#include <o3sdk/extras/tract/processor.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3sdk::extras::tract;
using Quality = Processor::Quality;

namespace {

constexpr float kFs = 44100.0f;

struct Saw {   // a band-unlimited saw: a voice box's buzz is rich in harmonics, which is what the tube needs
  float ph = 0.0f, inc;
  explicit Saw(float f0) : inc(f0 / kFs) {}
  float next() {
    ph += inc;
    if (ph >= 1.0f) ph -= 1.0f;
    return 0.5f * (2.0f * ph - 1.0f);
  }
};

struct Rng {
  std::uint32_t s = 777u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
  }
};

// Runs n frames of the saw through p in blocks of `block`; appends to out.
void run(Processor& p, Saw& saw, std::size_t n, std::vector<float>& out, std::size_t block = 64) {
  std::vector<float> in(block), o(block);
  std::size_t done = 0;
  while (done < n) {
    const std::size_t m = std::min(block, n - done);
    for (std::size_t i = 0; i < m; ++i) in[i] = saw.next();
    p.process(in.data(), o.data(), static_cast<std::uint32_t>(m));
    out.insert(out.end(), o.begin(), o.begin() + static_cast<std::ptrdiff_t>(m));
    done += m;
  }
}

constexpr std::size_t ms(float t) { return static_cast<std::size_t>(t * 0.001f * kFs); }

float peak_of(const std::vector<float>& x, std::size_t a, std::size_t b) {
  float m = 0.0f;
  for (std::size_t i = a; i < b && i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i]));
  return m;
}

float step_of(const std::vector<float>& x, std::size_t a, std::size_t b) {   // the largest sample to sample change
  float m = 0.0f;
  for (std::size_t i = (a ? a : 1); i < b && i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

double rms_of(const std::vector<float>& x, std::size_t a, std::size_t b) {
  double s = 0.0;
  for (std::size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  return std::sqrt(s / static_cast<double>(b - a));
}

const Quality kQualities[2] = {Quality::Full, Quality::Lite};

}  // namespace

TEST_CASE("tract processor: sections and the size steps") {
  Processor f(Quality::Full), l(Quality::Lite);
  CHECK(f.sections() == 22);
  CHECK(l.sections() == 11);
  const unsigned wf[5] = {26, 24, 22, 20, 18}, wl[5] = {13, 12, 11, 10, 9};
  for (int s = -2; s <= 2; ++s) {
    f.set_size(s);
    l.set_size(s);
    Saw a(130.0f), b(130.0f);
    std::vector<float> o;
    run(f, a, ms(20.0f), o);
    run(l, b, ms(20.0f), o);
    CHECK(f.sections() == wf[s + 2]);
    CHECK(l.sections() == wl[s + 2]);
  }
}

TEST_CASE("tract processor: Mix 0 passes the signal bit for bit, with in == out too") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(0.5f);
    Saw saw(130.0f);
    std::vector<float> wet;
    run(p, saw, ms(50.0f), wet);   // runs wet, then Mix goes to 0
    p.set_mix(0.0f);
    std::vector<float> x(4096), y(4096);
    Rng r;
    for (float& v : x) v = r.next();
    std::vector<float> z = x;
    p.process(z.data(), z.data(), 4096);          // the first 5 ms ramp; later samples are the input
    for (std::size_t i = ms(8.0f); i < 4096; ++i) CHECK(z[i] == x[i]);
    CHECK(p.bypassed());
    p.process(x.data(), y.data(), 4096);
    CHECK(std::memcmp(x.data(), y.data(), sizeof(float) * 4096) == 0);
  }
}

TEST_CASE("tract processor: the output does not depend on the block size") {
  for (Quality q : kQualities) {
    std::vector<float> ref;
    for (std::size_t block : {64u, 1u, 7u, 16u, 33u}) {
      Processor p(q);
      p.set_vowel(1.0f);
      p.set_bend(0.2f, -0.1f, 0.3f);
      p.set_hiss(0.5f);
      p.set_mix(0.7f);
      Saw saw(110.0f);
      std::vector<float> out;
      run(p, saw, 20000, out, block);
      if (block == 64) ref = out;
      else {
        REQUIRE(out.size() == ref.size());
        CHECK(std::memcmp(out.data(), ref.data(), sizeof(float) * out.size()) == 0);
      }
    }
  }
}

TEST_CASE("tract processor: the wet path is the dry one band-limited and late: a vowel is audible") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(0.0f);
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(300.0f), out);
    CHECK(rms_of(out, ms(200.0f), ms(300.0f)) > 0.05);
    CHECK(peak_of(out, 0, out.size()) < 2.0f);
  }
}

TEST_CASE("tract processor: a vowel jump from 0 to 1 at GLIDE 5 ms does not click") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_glide_ms(5.0f);
    p.set_vowel(0.0f);
    p.reset();
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(300.0f), out);
    const std::size_t at = out.size();
    const float p0 = peak_of(out, at - ms(100.0f), at), s0 = step_of(out, at - ms(100.0f), at);
    p.set_vowel(1.0f);
    run(p, saw, ms(300.0f), out);
    CAPTURE(p0);
    CHECK(peak_of(out, at, out.size()) < 4.0f * p0);   // the level trim follows in about 60 ms: the new vowel may overshoot for that long
    CHECK(step_of(out, at, out.size()) < 3.0f * s0);
    CHECK(std::isfinite(peak_of(out, at, out.size())));
  }
}

TEST_CASE("tract processor: the closures of the lips, tip and back move without a click") {
  for (Quality q : kQualities) {
    for (unsigned which = 0; which < 3; ++which) {
      CAPTURE(which);
      Processor p(q);
      p.set_vowel(0.0f);
      p.reset();
      Saw saw(130.0f);
      std::vector<float> out;
      run(p, saw, ms(300.0f), out);
      const std::size_t a = out.size();
      const float p0 = peak_of(out, a - ms(100.0f), a), s0 = step_of(out, a - ms(100.0f), a);
      p.set_closure(which, true);
      run(p, saw, ms(200.0f), out);
      const std::size_t b = out.size();
      p.set_closure(which, false);
      run(p, saw, ms(300.0f), out);
      CHECK(step_of(out, a, b) < 3.0f * s0);
      CHECK(step_of(out, b, out.size()) < 6.0f * s0);   // the release is a burst: allowed twice as much
      CHECK(peak_of(out, b, out.size()) < 6.0f * p0);
    }
  }
}

TEST_CASE("tract processor: a closure of the lips silences the output within 30 ms by 40 dB, and its release pops") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(0.0f);
    p.reset();
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(400.0f), out);
    const std::size_t a = out.size();
    const double steady = rms_of(out, a - ms(100.0f), a);
    const float steady_peak = peak_of(out, a - ms(100.0f), a);
    p.set_closure(0, true);
    run(p, saw, ms(60.0f), out);
    const double closed = rms_of(out, a + ms(30.0f), a + ms(55.0f));
    CAPTURE(steady);
    CAPTURE(closed);
    CHECK(20.0 * std::log10(closed / steady) < -40.0);
    const std::size_t b = out.size();
    p.set_closure(0, false);
    run(p, saw, ms(100.0f), out);
    // the pop: the release lets the pressure that built up behind the lips out, more than the steady peak
    CHECK(peak_of(out, b, out.size()) > 1.1f * steady_peak);
  }
}

TEST_CASE("tract processor: a size change from -2 to +2 stays bounded and finite") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_size(-2);
    p.reset();
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(300.0f), out);
    const std::size_t a = out.size();
    const float p0 = peak_of(out, a - ms(100.0f), a), s0 = step_of(out, a - ms(100.0f), a);
    p.set_size(2);
    run(p, saw, ms(300.0f), out);
    CHECK(std::isfinite(peak_of(out, a, out.size())));
    CHECK(peak_of(out, a, out.size()) < 3.0f * p0);
    CHECK(step_of(out, a, out.size()) < 3.0f * s0);
  }
}

TEST_CASE("tract processor: the wet level stays within 4 dB of the dry one over the five vowels and RES 0, 0.5, 1") {
  for (Quality q : kQualities) {
    for (float res : {0.0f, 0.5f, 1.0f}) {
      for (unsigned v = 0; v < 5; ++v) {
        Processor p(q);
        p.set_vowel(0.25f * static_cast<float>(v));
        p.set_res(res);
        p.reset();
        Saw saw(65.4f);   // C2
        std::vector<float> out, dry;
        run(p, saw, 2 * 44100, out);
        Saw ref(65.4f);
        for (std::size_t i = 0; i < out.size(); ++i) dry.push_back(ref.next());
        const double r = rms_of(out, 44100, out.size()) / rms_of(dry, 44100, dry.size());
        CAPTURE(res);
        CAPTURE(v);
        CHECK(std::fabs(20.0 * std::log10(r)) < 4.0);
      }
    }
  }
}

TEST_CASE("tract processor: sixty seconds of silence leave exact zeros, no denormals") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(0.5f);
    p.set_res(1.0f);
    p.set_hiss(1.0f);
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(500.0f), out);
    std::vector<float> zeros(4096, 0.0f), o(4096);
    std::size_t sub = 0, nonzero_tail = 0;
    const std::size_t blocks = (60u * 44100u) / 4096u;
    for (std::size_t b = 0; b < blocks; ++b) {
      p.process(zeros.data(), o.data(), 4096);
      for (float v : o) {
        if (std::fpclassify(v) == FP_SUBNORMAL) ++sub;
        if (b > blocks / 2 && v != 0.0f) ++nonzero_tail;
      }
    }
    CHECK(sub == 0);
    CHECK(nonzero_tail == 0);
  }
}

TEST_CASE("tract processor: NaN parameters take their defaults and the processor keeps running") {
  const float nan = std::nanf("");
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(nan);
    p.set_bend(nan, nan, nan);
    p.set_glide_ms(nan);
    p.set_res(nan);
    p.set_hiss(nan);
    p.set_mix(nan);
    p.set_closure(7, true);   // out of range: ignored
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(100.0f), out);
    for (float v : out) REQUIRE(std::isfinite(v));
    CHECK(peak_of(out, 0, out.size()) < 8.5f);
  }
}

TEST_CASE("tract processor: a tube that blew up is cleared and never reaches the output as more than the clamp") {
  for (Quality q : kQualities) {
    Processor p(q);
    p.set_vowel(0.3f);
    Saw saw(130.0f);
    std::vector<float> out;
    run(p, saw, ms(100.0f), out);
    std::vector<float> loud(2048);
    Rng r;
    for (float& v : loud) v = 1.0e3f * r.next();   // a source far beyond range
    std::vector<float> o(2048);
    p.process(loud.data(), o.data(), 2048);
    // the dry part is the input itself (Mix 1 is wet only): the output is the wet clamp
    for (float v : o) REQUIRE(std::fabs(v) <= 8.0f);
    run(p, saw, ms(300.0f), out);
    CHECK(rms_of(out, out.size() - ms(100.0f), out.size()) > 0.0);
    for (float v : out) REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("tract processor: prepare at 48 kHz and 22.05 kHz gives sane ramps") {
  for (float fs : {48000.0f, 22050.0f}) {
    Processor p(Quality::Full);
    p.prepare(fs);
    p.set_vowel(1.0f);
    std::vector<float> in(4096), out(4096);
    Rng r;
    for (float& v : in) v = 0.3f * r.next();
    p.process(in.data(), out.data(), 4096);
    for (float v : out) REQUIRE(std::isfinite(v));
  }
}
