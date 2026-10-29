// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's audio input (docs/M2C-PLAN.md section 5, slice SE): the gate into the units, the monitor into the master bus, the input
// peaks, and the proof that a feature that is off costs no render work and changes no sample.
#include <doctest/doctest.h>

#include <o3sdk/dsp/engine.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace o3sdk;

namespace {

constexpr unsigned kN = dsp::kMaxBlockFrames;

// An instrument: a sine at a fixed step plus its input passed through (in_l to out_l, in_r to out_r), so the codec input is audible
// through the units, as a through-effect or an instrument that listens would make it. `own` = 0 gives the input alone.
class Through final : public dsp::Unit {
 public:
  explicit Through(float own, dsp::UnitKind kind = dsp::UnitKind::Instrument) : info_{"THRU", kind, nullptr, 0, nullptr, 0}, own_(own) {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override { phase_ = 0.0f; }
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      const float s = own_ * std::sin(phase_);
      phase_ += 0.05f;
      if (phase_ > 6.2831853f) {
        phase_ -= 6.2831853f;
      }
      a.out_l[i] = s + a.in_l[i];
      a.out_r[i] = s * 0.5f + a.in_r[i];
    }
  }
  dsp::UnitInfo info_;
  float own_;
  float phase_ = 0.0f;
};

// An effect with state (a one-pole low-pass), so a golden render exercises inserts, sends and the master chain.
class Pole final : public dsp::Unit {
 public:
  Pole() : info_{"POLE", dsp::UnitKind::Effect, nullptr, 0, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override { zl_ = zr_ = 0.0f; }
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      zl_ += 0.3f * (a.in_l[i] - zl_);
      zr_ += 0.3f * (a.in_r[i] - zr_);
      a.out_l[i] = zl_;
      a.out_r[i] = zr_;
    }
  }
  dsp::UnitInfo info_;
  float zl_ = 0.0f;
  float zr_ = 0.0f;
};

std::uint64_t fnv(std::uint64_t h, float v) {
  std::uint32_t bits;
  std::memcpy(&bits, &v, sizeof bits);
  for (unsigned k = 0; k < 4; ++k) {
    h ^= (bits >> (8u * k)) & 0xFFu;
    h *= 0x100000001B3ull;
  }
  return h;
}

// The scene the golden hash is of: three sources, an insert, both sends, a master insert, mixer moves and a codec input, 300 blocks.
// Everything the engine did before this slice; none of the new calls is made.
struct Scene {
  Scene() {
    const dsp::UnitId a = e.attach(s0);
    const dsp::UnitId b = e.attach(s1);
    const dsp::UnitId c = e.attach(s2);
    const dsp::UnitId f0 = e.attach(fx0);
    const dsp::UnitId f1 = e.attach(fx1);
    const dsp::UnitId f2 = e.attach(fx2);
    const dsp::UnitId f3 = e.attach(fx3);
    ok = e.set_source(0, a) && e.set_source(1, b) && e.set_source(2, c) && e.set_insert(1, 0, f0) && e.set_send_effect(0, f1) &&
         e.set_send_effect(1, f2) && e.set_master_insert(0, f3);
    e.start();
    (void)e.set_mix(0, dsp::MixField::SendA, 0.4f);
    (void)e.set_mix(1, dsp::MixField::Pan, -0.6f);
    (void)e.set_mix(1, dsp::MixField::SendB, 0.3f);
    (void)e.set_mix(2, dsp::MixField::Level, 0.7f);
  }
  Through s0{0.4f};
  Through s1{0.3f};
  Through s2{0.2f};
  Pole fx0;
  Pole fx1;
  Pole fx2;
  Pole fx3;
  dsp::Engine e;
  bool ok = false;
};

// 300 blocks of a golden scene with an input tone, mixer moves every 50 blocks, folded into one FNV-1a hash of the output bits.
std::uint64_t golden_hash(Scene& sc, bool with_input) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  float il[kN];
  float ir[kN];
  float ol[kN];
  float orr[kN];
  unsigned n = 0;
  for (unsigned blk = 0; blk < 300; ++blk) {
    if (blk % 50 == 25) {
      (void)sc.e.set_mix(0, dsp::MixField::Level, blk % 100 == 25 ? 0.2f : 1.0f);
      (void)sc.e.set_mix(2, dsp::MixField::Pan, blk % 100 == 25 ? 0.8f : -0.4f);
      (void)sc.e.set_master_level(blk % 100 == 25 ? 0.5f : 0.9f);
    }
    for (unsigned i = 0; i < kN; ++i, ++n) {
      il[i] = 0.5f * std::sin(0.11f * static_cast<float>(n));
      ir[i] = 0.25f * std::sin(0.07f * static_cast<float>(n));
    }
    sc.e.render(with_input ? il : nullptr, with_input ? ir : nullptr, ol, orr, kN);
    for (unsigned i = 0; i < kN; ++i) {
      h = fnv(h, ol[i]);
      h = fnv(h, orr[i]);
    }
  }
  return h;
}

// The hash the engine gave before the input gate, the monitor, the peaks and the stems existed (HEAD 774df16, this same scene). With every new
// feature at its default the engine must still give it: the API only grows and an app that never calls it renders bit-identically.
constexpr std::uint64_t kGoldenWithInput = 1302090179385901880ull;
constexpr std::uint64_t kGoldenNoInput = 12750551464963158972ull;

}  // namespace

TEST_CASE("golden: with every new feature off the render is bit-identical to the engine before it") {
  Scene a;
  REQUIRE(a.ok);
  const std::uint64_t ha = golden_hash(a, true);
  Scene b;
  const std::uint64_t hb = golden_hash(b, false);
  CHECK(ha == kGoldenWithInput);
  CHECK(hb == kGoldenNoInput);
}

namespace {

// A hook that records the largest |input| it saw in a block (BlockContext::input_l / input_r).
class Listener final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override {
    seen_ = 0.0f;
    for (unsigned i = 0; i < kN; ++i) {
      seen_ = std::fmax(seen_, std::fmax(std::fabs(ctx.input_l()[i]), std::fabs(ctx.input_r()[i])));
    }
  }
  float seen_ = 0.0f;
};

// The codec input of the tests: a sine with a known slope, or a constant.
struct Input {
  float amp = 0.4f;
  float step = 0.05f;  // radians a sample; 0 = a constant `amp`
  std::uint32_t n = 0;
  void block(float* l, float* r) {
    for (unsigned i = 0; i < kN; ++i, ++n) {
      l[i] = step == 0.0f ? amp : amp * std::sin(step * static_cast<float>(n));
      r[i] = step == 0.0f ? amp : amp * std::cos(step * static_cast<float>(n));
    }
  }
};

// One source that passes the input through (own = 0), centred at unity gain, the master at unity, the listener hook; the mixer's ramps settled.
struct Rig {
  Rig() {
    const dsp::UnitId a = e.attach(thru);
    ok = e.set_source(0, a) && e.add_hook(hook);
    e.start();
    for (unsigned i = 0; i < 3; ++i) {
      (void)run();
    }
  }
  // Renders one block of the input; returns the output and leaves the block in l/r.
  void run() {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
  }
  Through thru{0.0f};
  Listener hook;
  dsp::Engine e;
  Input in;
  float il[kN];
  float ir[kN];
  float ol[kN];
  float orr[kN];
  bool ok = false;
};

// The largest step between neighbouring samples of `x` (across blocks: `prev` is the sample before x[0]).
float max_step(const float* x, float prev) {
  float m = 0.0f;
  for (unsigned i = 0; i < kN; ++i) {
    m = std::fmax(m, std::fabs(x[i] - prev));
    prev = x[i];
  }
  return m;
}

}  // namespace

TEST_CASE("gate: open at construction, the input reaches the units and the hooks untouched and the gate does no work") {
  Rig rig;
  REQUIRE(rig.ok);
  CHECK(rig.e.input_gate());
  for (unsigned i = 0; i < 20; ++i) {
    rig.run();
  }
  CHECK(rig.hook.seen_ > 0.3f);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(rig.ol[k] == doctest::Approx(rig.il[k]).epsilon(1e-4));
  }
  CHECK(rig.e.feature_work().gate_frames == 0u);
}

TEST_CASE("gate: closing and opening fade across one block with no click, and a shut gate does no work") {
  for (const float step : {0.05f, 0.0f}) {  // a sine and a constant: the constant is the worst case for a gate that switches
    Rig rig;
    REQUIRE(rig.ok);
    rig.in.step = step;
    rig.in.amp = 0.4f;
    for (unsigned i = 0; i < 5; ++i) {
      rig.run();
    }
    const float natural = rig.in.amp * (step == 0.0f ? 0.0f : step) * 1.2f;  // the signal's own steepest step (the right channel, a cosine, is as steep)
    const float ramp = rig.in.amp / static_cast<float>(kN);                  // what a one-block fade adds at most
    const float bound = natural + ramp * 1.1f + 1e-4f;
    float prev = rig.ol[kN - 1u];

    rig.e.set_input_gate(false);
    rig.run();  // the fade out
    const float close_step = max_step(rig.ol, prev);
    CHECK_MESSAGE(close_step <= bound, "closing, step ", close_step, " bound ", bound);
    CHECK(std::fabs(rig.ol[kN - 1u]) < 1e-6f);  // ends at exactly silence
    CHECK(rig.hook.seen_ > 0.0f);                // the hooks saw the fading block
    CHECK(rig.e.feature_work().gate_frames == kN);
    prev = rig.ol[kN - 1u];

    for (unsigned i = 0; i < 10; ++i) {
      rig.run();
      CHECK(rig.hook.seen_ == 0.0f);  // shut: the hooks hear nothing
      for (unsigned k = 0; k < kN; ++k) {
        CHECK(rig.ol[k] == 0.0f);
        CHECK(rig.orr[k] == 0.0f);
      }
    }
    CHECK(rig.e.feature_work().gate_frames == kN);  // a shut gate costs nothing more

    rig.e.set_input_gate(true);
    rig.run();  // the fade in
    const float open_step = max_step(rig.ol, 0.0f);
    CHECK_MESSAGE(open_step <= bound, "opening, step ", open_step, " bound ", bound);
    CHECK(rig.e.feature_work().gate_frames == 2u * kN);
    rig.run();
    for (unsigned k = 0; k < kN; ++k) {
      CHECK(rig.ol[k] == doctest::Approx(rig.il[k]).epsilon(1e-4));  // back to the input
    }
    CHECK(rig.e.feature_work().gate_frames == 2u * kN);
  }
}

TEST_CASE("gate: a toggle every block never steps more than a block's fade") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.in.step = 0.0f;
  rig.run();
  float prev = rig.ol[kN - 1u];
  float worst = 0.0f;
  for (unsigned b = 0; b < 40; ++b) {
    rig.e.set_input_gate(b % 2 == 0);
    rig.run();
    worst = std::fmax(worst, max_step(rig.ol, prev));
    prev = rig.ol[kN - 1u];
  }
  CHECK(worst <= rig.in.amp / static_cast<float>(kN) * 1.1f + 1e-4f);
}

TEST_CASE("gate: a null input is silence and the gate leaves it so") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_input_gate(false);
  rig.e.render(nullptr, nullptr, rig.ol, rig.orr, kN);
  rig.e.set_input_gate(true);
  rig.e.render(nullptr, nullptr, rig.ol, rig.orr, kN);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(rig.ol[k] == 0.0f);
  }
}

TEST_CASE("monitor: off at construction and free; on, it joins the master bus before the master inserts") {
  dsp::Engine e;
  struct Double final : dsp::Unit {
    Double() : info_{"X2", dsp::UnitKind::Effect, nullptr, 0, nullptr, 0} {}
    const dsp::UnitInfo& info() const noexcept override { return info_; }
    void prepare(float) noexcept override {}
    void process(const dsp::ProcessArgs& a) noexcept override {
      for (std::uint32_t i = 0; i < a.frames; ++i) {
        a.out_l[i] = a.in_l[i] * 2.0f;
        a.out_r[i] = a.in_r[i] * 2.0f;
      }
    }
    dsp::UnitInfo info_;
  } x2;
  const dsp::UnitId id = e.attach(x2);
  REQUIRE(e.set_master_insert(0, id));
  e.start();
  CHECK(e.input_monitor() == 0.0f);
  float il[kN];
  float ir[kN];
  float ol[kN];
  float orr[kN];
  Input in;
  in.amp = 0.2f;
  for (unsigned b = 0; b < 5; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
    for (unsigned k = 0; k < kN; ++k) {
      CHECK(ol[k] == 0.0f);  // nothing is routed: the input is not heard
    }
  }
  CHECK(e.feature_work().monitor_frames == 0u);

  e.set_input_monitor(0.5f);
  in.block(il, ir);
  e.render(il, ir, ol, orr, kN);  // ramps 0 -> 0.5
  CHECK(e.feature_work().monitor_frames == kN);
  CHECK(std::fabs(ol[0]) < std::fabs(ol[kN - 1u]));
  for (unsigned b = 0; b < 3; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
    for (unsigned k = 0; k < kN; ++k) {
      CHECK(ol[k] == doctest::Approx(il[k] * 0.5f * 2.0f).epsilon(1e-5));  // the master insert (x2) hears it: before the inserts
      CHECK(orr[k] == doctest::Approx(ir[k] * 0.5f * 2.0f).epsilon(1e-5));
    }
  }
  // The master level is after the insert: it scales the monitor too.
  (void)e.set_master_level(0.5f);
  for (unsigned b = 0; b < 2; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
  }
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(ol[k] == doctest::Approx(il[k] * 0.5f).epsilon(1e-5));
  }
  // Off again: it ramps down in one block, then no work.
  e.set_input_monitor(0.0f);
  in.block(il, ir);
  e.render(il, ir, ol, orr, kN);
  const std::uint32_t after = e.feature_work().monitor_frames;
  for (unsigned b = 0; b < 5; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
    CHECK(ol[0] == 0.0f);
  }
  CHECK(e.feature_work().monitor_frames == after);
}

TEST_CASE("monitor: the gate shuts it and the value is clamped") {
  dsp::Engine e;
  e.start();
  e.set_input_monitor(9.0f);
  CHECK(e.input_monitor() == 1.5f);
  e.set_input_monitor(-1.0f);
  CHECK(e.input_monitor() == 0.0f);
  e.set_input_monitor(std::numeric_limits<float>::quiet_NaN());
  CHECK(e.input_monitor() == 0.0f);
  e.set_input_monitor(0.7f);
  float il[kN];
  float ir[kN];
  float ol[kN];
  float orr[kN];
  Input in;
  in.amp = 0.3f;
  for (unsigned b = 0; b < 3; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
  }
  CHECK(ol[kN - 1u] == doctest::Approx(il[kN - 1u] * 0.7f).epsilon(1e-5));
  e.set_input_gate(false);
  for (unsigned b = 0; b < 2; ++b) {
    in.block(il, ir);
    e.render(il, ir, ol, orr, kN);
  }
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(ol[k] == 0.0f);  // the monitor listens to the gated input: a source that settles is not heard
  }
}

TEST_CASE("peaks: off they cost nothing; on, each call returns the peak since the last and clears it") {
  Rig rig;
  REQUIRE(rig.ok);
  CHECK_FALSE(rig.e.input_peaks_on());
  for (unsigned i = 0; i < 10; ++i) {
    rig.run();
  }
  CHECK(rig.e.feature_work().peak_frames == 0u);
  dsp::InputPeaks p = rig.e.input_peaks();
  CHECK(p.l == 0.0f);
  CHECK(p.r == 0.0f);

  rig.e.set_input_peaks(true);
  rig.in.step = 0.0f;
  rig.in.amp = 0.25f;
  rig.run();
  p = rig.e.input_peaks();
  CHECK(p.l == doctest::Approx(0.25f));
  CHECK(p.r == doctest::Approx(0.25f));
  CHECK(rig.e.feature_work().peak_frames == kN);
  p = rig.e.input_peaks();  // read and cleared
  CHECK(p.l == 0.0f);
  CHECK(p.r == 0.0f);

  // A peak hold across blocks: the loud block is in the middle of three.
  rig.in.amp = 0.1f;
  rig.run();
  rig.in.amp = 0.7f;
  rig.run();
  rig.in.amp = 0.2f;
  rig.run();
  p = rig.e.input_peaks();
  CHECK(p.l == doctest::Approx(0.7f));
  CHECK(p.r == doctest::Approx(0.7f));

  // Negative samples count by magnitude; the channels are separate.
  float il[kN];
  float ir[kN];
  for (unsigned k = 0; k < kN; ++k) {
    il[k] = k == 17 ? -0.9f : 0.0f;
    ir[k] = k == 3 ? 0.4f : 0.0f;
  }
  rig.e.render(il, ir, rig.ol, rig.orr, kN);
  p = rig.e.input_peaks();
  CHECK(p.l == doctest::Approx(0.9f));
  CHECK(p.r == doctest::Approx(0.4f));

  // Off again: no more work, and nothing new is reported.
  rig.e.set_input_peaks(false);
  const std::uint32_t before = rig.e.feature_work().peak_frames;
  rig.in.amp = 0.8f;
  rig.run();
  CHECK(rig.e.feature_work().peak_frames == before);
  p = rig.e.input_peaks();
  CHECK(p.l == 0.0f);
}

TEST_CASE("peaks: measure the gated input, ignore a NaN") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_input_peaks(true);
  rig.in.step = 0.0f;
  rig.e.set_input_gate(false);
  for (unsigned i = 0; i < 3; ++i) {
    rig.run();
  }
  (void)rig.e.input_peaks();  // what the fade of the first block measured
  rig.run();
  const dsp::InputPeaks p = rig.e.input_peaks();
  CHECK(p.l == 0.0f);
  CHECK(p.r == 0.0f);

  float il[kN] = {};
  float ir[kN] = {};
  il[5] = std::numeric_limits<float>::quiet_NaN();
  ir[5] = std::numeric_limits<float>::infinity();
  rig.e.set_input_gate(true);
  rig.run();
  rig.run();
  (void)rig.e.input_peaks();
  rig.e.render(il, ir, rig.ol, rig.orr, kN);
  const dsp::InputPeaks q = rig.e.input_peaks();
  CHECK(q.l == q.l);  // a number
  CHECK(std::isfinite(q.r));
}

TEST_CASE("peaks: the main loop's read never loses a peak the render published") {
  dsp::Engine e;
  e.start();
  e.set_input_peaks(true);
  constexpr unsigned kBlocks = 3000;
  std::vector<float> reads;
  std::thread render([&] {
    float il[kN];
    float ir[kN];
    float ol[kN];
    float orr[kN];
    for (unsigned b = 1; b <= kBlocks; ++b) {
      // A rising peak: the last block's is the largest, so a read that dropped a published value would show as a maximum below it.
      const float v = 0.0001f * static_cast<float>(b);
      for (unsigned k = 0; k < kN; ++k) {
        il[k] = k == 7 ? v : 0.0f;
        ir[k] = k == 9 ? v * 0.5f : 0.0f;
      }
      e.render(il, ir, ol, orr, kN);
    }
  });
  while (e.stats().blocks < kBlocks) {
    reads.push_back(e.input_peaks().l);
  }
  render.join();
  reads.push_back(e.input_peaks().l);
  float top = 0.0f;
  for (const float v : reads) {
    top = std::fmax(top, v);
  }
  CHECK(top == doctest::Approx(0.0001f * static_cast<float>(kBlocks)));
}

TEST_CASE("off: with every feature off nothing is counted and the render is the old one") {
  Scene a;
  REQUIRE(a.ok);
  (void)golden_hash(a, true);
  const dsp::FeatureWork w = a.e.feature_work();
  CHECK(w.gate_frames == 0u);
  CHECK(w.monitor_frames == 0u);
  CHECK(w.peak_frames == 0u);
  CHECK(w.stem_frames == 0u);
  CHECK(a.e.stem_block() == nullptr);
  CHECK(a.e.input_gate());
  CHECK(a.e.input_monitor() == 0.0f);
  CHECK_FALSE(a.e.input_peaks_on());
  CHECK(a.e.stem_slots() == 0u);
}

TEST_CASE("on: opening a gate that was never closed, a monitor at 0 and a stem mask of 0 are the same as off") {
  Scene a;
  REQUIRE(a.ok);
  a.e.set_input_gate(true);
  a.e.set_input_monitor(0.0f);
  a.e.set_stem_slots(0u);
  CHECK(golden_hash(a, true) == kGoldenWithInput);
  CHECK(a.e.feature_work().gate_frames == 0u);
  CHECK(a.e.feature_work().monitor_frames == 0u);
}
