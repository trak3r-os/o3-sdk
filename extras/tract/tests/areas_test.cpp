// SPDX-License-Identifier: GPL-3.0-or-later
// The area entry of the tract processor (LT1): set_areas drives the tube from a caller's areas, use_shape goes back. The old path is untouched: a
// digest of a fixed run through every setter, taken from the sources before the entry existed (a49dc6a), still comes out; the areas are
// normalised, clamped, stretched over any N and followed with the vowel's glide; garbage is ignored; closures, size changes, the bypass and the
// block size behave; the way back lands on the shape.
#include <doctest/doctest.h>

#include <o3sdk/extras/tract/processor.h>
#include <o3sdk/extras/tract/shape.h>

#include "tract_probe.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace o3sdk::extras::tract;
using Quality = Processor::Quality;

namespace {

// A fixed run through the shape-driven path: every setter, both qualities, odd block sizes. Returns an FNV-1a digest of the output bits.
struct LegacyRun {
  std::uint64_t h = 1469598103934665603ull;
  float ph = 0.0f;
  std::uint32_t s = 4242u;
  float saw() {
    ph += 130.0f / 44100.0f;
    if (ph >= 1.0f) ph -= 1.0f;
    return 0.5f * (2.0f * ph - 1.0f);
  }
  float noise() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
  }
  void feed(o3sdk::extras::tract::Processor& p, unsigned n, unsigned block, bool use_noise) {
    float in[96], out[96];
    while (n) {
      const unsigned m = n < block ? n : block;
      for (unsigned i = 0; i < m; ++i) in[i] = use_noise ? 0.25f * noise() : saw();
      p.process(in, out, m);
      for (unsigned i = 0; i < m; ++i) {
        std::uint32_t b;
        std::memcpy(&b, &out[i], 4);
        for (int k = 0; k < 4; ++k) {
          h ^= (b >> (8 * k)) & 0xFFu;
          h *= 1099511628211ull;
        }
      }
      n -= m;
    }
  }
};

inline std::uint64_t legacy_digest() {
  using namespace o3sdk::extras::tract;
  LegacyRun r;
  for (int q = 0; q < 2; ++q) {
    Processor p(q == 0 ? Processor::Quality::Full : Processor::Quality::Lite);
    p.prepare(44100.0f);
    p.set_mix(1.0f);
    p.set_res(0.5f);
    p.set_hiss(0.3f);
    r.feed(p, 3000, 64, false);
    for (int v = 0; v <= 8; ++v) {                     // a vowel sweep, glide 30 ms
      p.set_glide_ms(30.0f);
      p.set_vowel(0.125f * static_cast<float>(v));
      r.feed(p, 1500, 37, false);
    }
    p.set_bend(0.5f, -0.4f, 0.7f);                     // bends
    r.feed(p, 2000, 64, false);
    p.set_bend(-1.0f, 1.0f, -1.0f);
    r.feed(p, 2000, 17, false);
    p.set_closure(0, true);                            // a closure and its release
    r.feed(p, 1500, 64, false);
    p.set_closure(0, false);
    p.set_closure(2, true);
    r.feed(p, 1500, 64, true);
    p.set_closure(2, false);
    r.feed(p, 1500, 64, false);
    for (int z = -2; z <= 2; ++z) {                    // every size step
      p.set_size(z);
      r.feed(p, 1800, 64, false);
    }
    p.set_size(0);
    p.set_res(0.95f);                                  // res, hiss on noise
    p.set_hiss(1.0f);
    p.set_closure(1, true);
    r.feed(p, 1500, 64, true);
    p.set_closure(1, false);
    p.set_glide_ms(500.0f);
    p.set_vowel(1.0f);
    r.feed(p, 4000, 64, false);
    p.set_glide_ms(5.0f);
    p.set_vowel(0.2f);
    r.feed(p, 2000, 64, false);
    p.set_mix(0.4f);                                   // mix, then a bypass and the wake-up
    r.feed(p, 1500, 64, false);
    p.set_mix(0.0f);
    r.feed(p, 1500, 64, false);
    p.set_mix(1.0f);
    r.feed(p, 3000, 64, false);
    p.set_res(0.0f);
    r.feed(p, 2000, 96, true);
  }
  return r.h;
}

// ---- helpers -------------------------------------------------------------------------------------------------------------------------------

constexpr float kFs = 44100.0f;

void silence(Processor& p, unsigned frames) {
  std::vector<float> z(64, 0.0f), o(64);
  for (unsigned d = 0; d < frames; d += 64) p.process(z.data(), o.data(), 64);
}

// The areas of a vowel's shape for n sections (diameters squared): what a caller who measured that tract would hand over.
std::vector<float> vowel_areas(float v, unsigned n) {
  float d[Tube::kMaxSections];
  const float closed[3] = {0.0f, 0.0f, 0.0f};
  diameters(vowel_shape(v), closed, n, d);
  std::vector<float> a(n);
  for (unsigned i = 0; i < n; ++i) a[i] = d[i] * d[i];
  return a;
}

// The formants of what the processor is now: the impulse response's spectrum peaks (host only, doubles).
std::vector<double> formants(Processor& p, unsigned len = 8192) {
  std::vector<float> in(len, 0.0f), out(len);
  in[0] = 1.0f;
  p.process(in.data(), out.data(), len);
  std::vector<double> h(out.begin(), out.end());
  return probe::peaks(probe::magnitude(h, 2048), 0.5 * static_cast<double>(kFs), 0.02);
}

Processor::Quality kQ[2] = {Quality::Full, Quality::Lite};

void prep(Processor& p) {
  p.set_mix(1.0f);
  p.set_res(0.5f);
  p.set_hiss(0.0f);
  p.set_glide_ms(30.0f);
}

float max_step(const std::vector<float>& x) {
  float m = 0.0f;
  for (std::size_t i = 1; i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

float peak(const std::vector<float>& x) {
  float m = 0.0f;
  for (float v : x) m = std::fmax(m, std::fabs(v));
  return m;
}

// A buzz and the processor over frames, in blocks of 64.
struct Buzz {
  float ph = 0.0f;
  float next() {
    ph += 130.0f / kFs;
    if (ph >= 1.0f) ph -= 1.0f;
    return 0.5f * (2.0f * ph - 1.0f);
  }
  void run(Processor& p, unsigned frames, std::vector<float>& out, unsigned block = 64) {
    std::vector<float> in(block), o(block);
    for (unsigned d = 0; d < frames; d += block) {
      for (float& v : in) v = next();
      p.process(in.data(), o.data(), block);
      out.insert(out.end(), o.begin(), o.end());
    }
  }
};

}  // namespace

// ---- the old path ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("areas: the shape-driven path is bit-exact with the sources before set_areas existed (digest of a fixed run, both qualities)") {
  // Taken by building extras/tract at a49dc6a (the tree before LT1) with this scenario and -O2 -ffp-contract=off, before any edit.
  const std::uint64_t d = legacy_digest();
  std::printf("tract areas: legacy digest 0x%016llX\n", static_cast<unsigned long long>(d));
  CHECK(d == 0x5D75041412D76E5Aull);
}

TEST_CASE("areas: use_shape without areas, and a processor that never saw them, run the same bits") {
  Processor a(Quality::Full), b(Quality::Full);
  prep(a);
  prep(b);
  a.set_vowel(0.5f);
  b.set_vowel(0.5f);
  Buzz x, y;
  std::vector<float> oa, ob;
  x.run(a, 3000, oa);
  y.run(b, 3000, ob);
  a.use_shape();
  CHECK_FALSE(a.areas_active());
  x.run(a, 3000, oa);
  y.run(b, 3000, ob);
  REQUIRE(oa.size() == ob.size());
  CHECK(std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) == 0);
}

// ---- the areas -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("areas: a vowel's own areas, in any scale, give the formants of the vowel path (n = N, the identity stretch)") {
  for (Quality q : kQ) {
    const unsigned n = q == Quality::Full ? 22u : 11u;
    for (float v : {0.0f, 0.5f, 1.0f}) {
      Processor ref(q), p(q);
      prep(ref);
      prep(p);
      ref.set_vowel(v);
      std::vector<float> a = vowel_areas(v, n);
      for (float& e : a) e *= 37.0f;   // the scale is the caller's: the tube takes the ratios
      p.set_areas(a.data(), n);
      CHECK(p.areas_active());
      silence(ref, 20000);
      silence(p, 20000);
      const auto fr = formants(ref), fp = formants(p);
      REQUIRE(fr.size() >= 2);
      REQUIRE(fp.size() >= 2);
      for (int i = 0; i < 2; ++i) {
        const double err = std::fabs(fp[static_cast<std::size_t>(i)] - fr[static_cast<std::size_t>(i)]) / fr[static_cast<std::size_t>(i)];
        std::printf("tract areas: v %.1f F%d ref %.0f areas %.0f Hz (%.2f %%)\n", static_cast<double>(v), i + 1,
                    fr[static_cast<std::size_t>(i)], fp[static_cast<std::size_t>(i)], 100.0 * err);
        CHECK(err < 0.01);
      }
    }
  }
}

TEST_CASE("areas: n points are stretched over the tube's N, a size change needs no new call, a smaller N raises the formants") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> a = vowel_areas(0.0f, 13);   // 13 points over 22 sections
  p.set_areas(a.data(), 13);
  silence(p, 20000);
  const auto f0 = formants(p);
  p.set_size(2);   // 18 sections
  silence(p, 20000);
  CHECK(p.sections() == 18);
  CHECK(p.areas_active());
  const auto f2 = formants(p);
  REQUIRE(f0.size() >= 2);
  REQUIRE(f2.size() >= 2);
  std::printf("tract areas: 13 points, N 22: F1 %.0f F2 %.0f; N 18: F1 %.0f F2 %.0f\n", f0[0], f0[1], f2[0], f2[1]);
  CHECK(f2[1] / f0[1] > 1.12);
  CHECK(f2[1] / f0[1] < 1.35);
  p.set_size(-2);   // 26
  silence(p, 20000);
  CHECK(p.sections() == 26);
  const auto fm = formants(p);
  REQUIRE(fm.size() >= 2);
  CHECK(fm[1] / f0[1] < 0.9);
}

TEST_CASE("areas: the widest section is the open tract and the narrowest is clamped (an area ratio of 1e-12 stays a tube that speaks)") {
  Processor p(Quality::Full);
  prep(p);
  std::vector<float> a(12, 1.0f);
  a[5] = 1.0e-12f;
  a[0] = 1.0e20f;   // a scale no one uses
  p.set_areas(a.data(), 12);
  Buzz b;
  std::vector<float> out;
  b.run(p, 20000, out);
  for (float v : out) REQUIRE(std::isfinite(v));
  CHECK(peak(out) < 8.0f + 1e-3f);   // the wet clamp
  CHECK(peak(std::vector<float>(out.end() - 4000, out.end())) > 0.0f);
}

TEST_CASE("areas: garbage is ignored, the last areas stay, bit for bit (null, n < 2, NaN, infinity, zero, negative, too large)") {
  Processor a(Quality::Full), b(Quality::Full);
  prep(a);
  prep(b);
  const std::vector<float> good = vowel_areas(0.25f, 13);
  a.set_areas(good.data(), 13);
  b.set_areas(good.data(), 13);
  Buzz x, y;
  std::vector<float> oa, ob;
  x.run(a, 2000, oa);
  y.run(b, 2000, ob);
  const float nan = std::nanf(""), inf = INFINITY;
  std::vector<float> bad;
  const float cases[6] = {nan, inf, 0.0f, -1.0f, 1.0e31f, -inf};
  for (float c : cases) {
    bad = good;
    bad[6] = c;
    a.set_areas(bad.data(), 13);
  }
  a.set_areas(nullptr, 13);
  a.set_areas(good.data(), 1);
  a.set_areas(good.data(), 0);
  x.run(a, 4000, oa);
  y.run(b, 4000, ob);
  REQUIRE(oa.size() == ob.size());
  CHECK(std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) == 0);
  // And a first call that is garbage leaves the vowel path where it was.
  Processor c(Quality::Full);
  prep(c);
  bad = good;
  bad[0] = nan;
  c.set_areas(bad.data(), 13);
  CHECK_FALSE(c.areas_active());
}

TEST_CASE("areas: more than kMaxAreas are cut to kMaxAreas, 2 is the least") {
  Processor p(Quality::Full);
  prep(p);
  std::vector<float> a(64);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = 1.0f + 0.5f * std::sin(static_cast<float>(i));
  p.set_areas(a.data(), 64);
  CHECK(p.areas_active());
  const float two[2] = {1.0f, 0.2f};
  p.set_areas(two, 2);
  Buzz b;
  std::vector<float> out;
  b.run(p, 8000, out);
  for (float v : out) REQUIRE(std::isfinite(v));
}

// ---- the glide -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("areas: a jump to other areas glides with GLIDE: not there after a sixth of it, there after five times it, no click") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> a = vowel_areas(0.0f, 22), u = vowel_areas(1.0f, 22);
  p.set_areas(a.data(), 22);
  silence(p, 20000);
  const auto fa = formants(p);
  Buzz b;
  std::vector<float> pre, post;
  b.run(p, 4096, pre);
  p.set_areas(u.data(), 22);
  b.run(p, 8 * 441, post);   // 80 ms: the jump itself
  const float natural = max_step(pre);   // the buzz's own reset step through the tube
  const float step = max_step(post);
  std::printf("tract areas: jump A to U: the buzz's own step %.3f, step across the jump %.3f\n", static_cast<double>(natural),
              static_cast<double>(step));
  CHECK(step < 1.25f * natural);
  silence(p, 20000);
  const auto fu = formants(p);
  Processor ref(Quality::Full);
  prep(ref);
  ref.set_vowel(1.0f);
  silence(ref, 20000);
  const auto fr = formants(ref);
  REQUIRE(fa.size() >= 2);
  REQUIRE(fu.size() >= 2);
  REQUIRE(fr.size() >= 2);
  CHECK(std::fabs(fu[1] - fr[1]) / fr[1] < 0.01);
  CHECK(std::fabs(fu[1] - fa[1]) / fa[1] > 0.2);
  // Slow glide: 46 ms into a 500 ms glide the tube is still near A, not at U.
  Processor r(Quality::Full);
  prep(r);
  r.set_glide_ms(500.0f);
  r.set_areas(a.data(), 22);
  silence(r, 40000);
  r.set_areas(u.data(), 22);
  silence(r, 2048);
  const auto fs = formants(r, 4096);
  REQUIRE(fs.size() >= 2);
  CHECK(std::fabs(fs[1] - fa[1]) < std::fabs(fs[1] - fr[1]));
}

TEST_CASE("areas: the same bits whatever the block size (areas set at multiples of 128 frames)") {
  const std::vector<float> a = vowel_areas(0.0f, 13), u = vowel_areas(0.75f, 13);
  std::vector<float> res[3];
  const unsigned blocks[3] = {64, 32, 16};
  for (int k = 0; k < 3; ++k) {
    Processor p(Quality::Full);
    prep(p);
    Buzz b;
    for (unsigned chunk = 0; chunk < 40; ++chunk) {
      p.set_areas(chunk & 1u ? a.data() : u.data(), 13);
      b.run(p, 128, res[k], blocks[k]);
    }
  }
  REQUIRE(res[0].size() == res[1].size());
  CHECK(std::memcmp(res[0].data(), res[1].data(), res[0].size() * sizeof(float)) == 0);
  CHECK(std::memcmp(res[0].data(), res[2].data(), res[0].size() * sizeof(float)) == 0);
}

// ---- the other controls in area mode ---------------------------------------------------------------------------------------------------------

TEST_CASE("areas: a closure still silences the output and its release brings the voice back; the hiss follows the narrowest section") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> a = vowel_areas(0.0f, 22);
  p.set_areas(a.data(), 22);
  Buzz b;
  std::vector<float> on, shut, back;
  b.run(p, 12000, on);
  p.set_closure(0, true);
  b.run(p, 6000, shut);
  p.set_closure(0, false);
  b.run(p, 12000, back);
  const std::vector<float> endon(on.end() - 2048, on.end()), endshut(shut.end() - 1024, shut.end()), endback(back.end() - 2048, back.end());
  CHECK(peak(endshut) < 0.01f * peak(endon));
  CHECK(peak(endback) > 0.5f * peak(endon));
  CHECK(p.areas_active());
  // A tube narrowed to the clamp at one section makes turbulence from a silent input.
  Processor h(Quality::Full);
  prep(h);
  h.set_hiss(1.0f);
  std::vector<float> w(22, 1.0f);
  w[10] = 1.0e-6f;
  h.set_areas(w.data(), 22);
  std::vector<float> z(64, 0.05f), o(64);   // a DC input keeps the source alive
  float e = 0.0f;
  for (int i = 0; i < 400; ++i) {
    h.process(z.data(), o.data(), 64);
    for (float v : o) e += v * v;
  }
  CHECK(e > 0.0f);
}

TEST_CASE("areas: Mix 0 is the bypass in area mode too, and Mix back up lands on the areas") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> a = vowel_areas(1.0f, 13);
  p.set_areas(a.data(), 13);
  silence(p, 8000);
  p.set_mix(0.0f);
  std::vector<float> x(4096), y(4096);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * std::sin(0.01f * static_cast<float>(i));
  std::vector<float> z = x;
  p.process(z.data(), z.data(), 4096);
  p.process(x.data(), y.data(), 4096);
  CHECK(p.bypassed());
  CHECK(std::memcmp(x.data(), y.data(), sizeof(float) * 4096) == 0);
  p.set_mix(1.0f);
  silence(p, 20000);
  Processor ref(Quality::Full);
  prep(ref);
  ref.set_areas(a.data(), 13);
  silence(ref, 20000);
  const auto fp = formants(p), fr = formants(ref);
  REQUIRE(fp.size() >= 2);
  REQUIRE(fr.size() >= 2);
  CHECK(std::fabs(fp[1] - fr[1]) / fr[1] < 0.01);
}

// ---- the way back ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("areas: use_shape glides to the vowel (no click), then the vowel path runs and areas_active is false") {
  for (Quality q : kQ) {
    const unsigned n = q == Quality::Full ? 22u : 11u;
    Processor p(q);
    prep(p);
    p.set_vowel(0.0f);
    const std::vector<float> u = vowel_areas(1.0f, n);
    Buzz b;
    std::vector<float> out;
    b.run(p, 4096, out);   // the vowel path first
    p.set_areas(u.data(), n);
    b.run(p, 16000, out);
    CHECK(p.areas_active());
    const std::size_t mark = out.size();
    p.use_shape();
    CHECK(p.areas_active());   // gliding
    b.run(p, 16000, out);
    CHECK_FALSE(p.areas_active());
    const std::vector<float> win(out.begin() + static_cast<std::ptrdiff_t>(mark), out.begin() + static_cast<std::ptrdiff_t>(mark) + 3000);
    const std::vector<float> before(out.begin() + static_cast<std::ptrdiff_t>(mark) - 3000, out.begin() + static_cast<std::ptrdiff_t>(mark));
    std::printf("tract areas: way back (%s): step %.3f, steady peak %.3f\n", q == Quality::Full ? "Full" : "Lite", static_cast<double>(max_step(win)),
                static_cast<double>(peak(before)));
    CHECK(max_step(win) < 0.5f * peak(before));
    silence(p, 20000);
    Processor ref(q);
    prep(ref);
    ref.set_vowel(0.0f);
    silence(ref, 20000);
    const auto fp = formants(p), fr = formants(ref);
    REQUIRE(fp.size() >= 2);
    REQUIRE(fr.size() >= 2);
    CHECK(std::fabs(fp[0] - fr[0]) / fr[0] < 0.01);
    CHECK(std::fabs(fp[1] - fr[1]) / fr[1] < 0.01);
  }
}

TEST_CASE("areas: a reset in area mode lands on the areas, a reset during the way back lands on the shape; set_areas during the way back resumes") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> u = vowel_areas(1.0f, 13);
  p.set_vowel(0.0f);
  p.set_areas(u.data(), 13);
  silence(p, 10000);
  p.reset();
  CHECK(p.areas_active());
  silence(p, 10000);
  Processor ref(Quality::Full);
  prep(ref);
  ref.set_areas(u.data(), 13);
  silence(ref, 10000);
  const auto fp = formants(p), fr = formants(ref);
  REQUIRE(fp.size() >= 2);
  REQUIRE(fr.size() >= 2);
  CHECK(std::fabs(fp[1] - fr[1]) / fr[1] < 0.01);
  p.use_shape();
  p.reset();
  CHECK_FALSE(p.areas_active());
  p.set_areas(u.data(), 13);
  p.use_shape();
  silence(p, 640);
  p.set_areas(u.data(), 13);   // resumes
  CHECK(p.areas_active());
  silence(p, 30000);
  CHECK(p.areas_active());
}

TEST_CASE("areas: a size change during the way back and NaN in the signal leave the output finite") {
  Processor p(Quality::Full);
  prep(p);
  const std::vector<float> u = vowel_areas(1.0f, 13);
  p.set_areas(u.data(), 13);
  silence(p, 6000);
  p.use_shape();
  silence(p, 320);
  p.set_size(1);
  Buzz b;
  std::vector<float> out;
  b.run(p, 20000, out);
  for (float v : out) REQUIRE(std::isfinite(v));
  CHECK_FALSE(p.areas_active());
  p.set_areas(u.data(), 13);
  std::vector<float> in(64, 0.1f), o(64);
  in[3] = std::nanf("");
  for (int i = 0; i < 100; ++i) p.process(in.data(), o.data(), 64);
  in[3] = 0.1f;
  for (int i = 0; i < 100; ++i) p.process(in.data(), o.data(), 64);
  for (float v : o) CHECK(std::isfinite(v));
}
