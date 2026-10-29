// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's stem staging (docs/M2C-PLAN.md section 7, slice SE): what stem_block() hands the platform's StemTap, to the sample, and that
// a stem that is not selected costs no render work.
#include <doctest/doctest.h>

#include <o3sdk/dsp/engine.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace o3sdk;

namespace {

constexpr unsigned kN = dsp::kMaxBlockFrames;

// The unit of the tests: a constant plus a sine on the left and a cosine on the right, so the two channels and every sample differ.
class Gen final : public dsp::Unit {
 public:
  Gen(float dc, float sine, dsp::UnitKind kind = dsp::UnitKind::Instrument) : info_{"GEN", kind, nullptr, 0, nullptr, 0}, dc_(dc), sine_(sine) {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override { n_ = 0; }
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i, ++n_) {
      if (info_.kind == dsp::UnitKind::Effect) {
        a.out_l[i] = a.in_l[i] * 2.0f;  // an effect that doubles
        a.out_r[i] = a.in_r[i] * 2.0f;
      } else {
        a.out_l[i] = dc_ + sine_ * std::sin(0.07f * static_cast<float>(n_));
        a.out_r[i] = dc_ + sine_ * std::cos(0.05f * static_cast<float>(n_));
      }
    }
  }
  dsp::UnitInfo info_;
  float dc_;
  float sine_;
  std::uint32_t n_ = 0;
};

// What a slot's stem is, written out: the same operations in the same order as the engine (the mono of the two channels, the gain, planes_to_s16).
std::int16_t ref_s16(float v) {
  const float a = std::fmax(-1.0f, std::fmin(1.0f, v)) * 32767.0f;
  return static_cast<std::int16_t>(a < 0.0f ? a - 0.5f : a + 0.5f);
}

struct Rendered {
  float l[kN];
  float r[kN];
  std::int16_t stem[dsp::kStemSources][kN];
  bool has[dsp::kStemSources];
  bool any;
};

Rendered render(dsp::Engine& e, std::uint32_t frames = kN) {
  Rendered o{};
  e.render(nullptr, nullptr, o.l, o.r, frames);
  const std::int16_t* const* t = e.stem_block();
  o.any = t != nullptr;
  for (unsigned i = 0; i < dsp::kStemSources; ++i) {
    o.has[i] = t != nullptr && t[i] != nullptr;
    if (o.has[i]) {
      for (unsigned k = 0; k < kN; ++k) {
        o.stem[i][k] = t[i][k];
      }
    }
  }
  return o;
}

// Three sources (slots 0, 1 and 3), the middle one panned hard right with both sends up, a send effect on bus A. Generators are known so the test can
// recompute every stem; `n` is the number of samples they have produced, kept in step by render().
struct Rig {
  Rig() {
    const dsp::UnitId a = e.attach(g0);
    const dsp::UnitId b = e.attach(g1);
    const dsp::UnitId c = e.attach(g3);
    const dsp::UnitId f = e.attach(fx);
    ok = e.set_source(0, a) && e.set_source(1, b) && e.set_source(3, c) && e.set_send_effect(0, f);
    e.start();
    (void)e.set_mix(1, dsp::MixField::Pan, 1.0f);
    (void)e.set_mix(1, dsp::MixField::SendA, 0.5f);
    (void)e.set_mix(1, dsp::MixField::SendB, 0.5f);
    for (unsigned i = 0; i < 3; ++i) {
      (void)render(e);  // the mixer's gains settle (they ramp up from 0 in the first block)
    }
  }
  Gen g0{0.0f, 0.5f};
  Gen g1{0.1f, 0.3f};
  Gen g3{-0.2f, 0.25f};
  Gen fx{0.0f, 0.0f, dsp::UnitKind::Effect};
  dsp::Engine e;
  bool ok = false;
};

// The block a generator made as the engine saw it (the generator keeps its own sample counter, so the test replays it).
void gen_block(const Gen& g, std::uint32_t end_n, std::uint32_t frames, float* l, float* r) {
  for (std::uint32_t i = 0; i < frames; ++i) {
    const std::uint32_t n = end_n - frames + i;
    l[i] = g.dc_ + g.sine_ * std::sin(0.07f * static_cast<float>(n));
    r[i] = g.dc_ + g.sine_ * std::cos(0.05f * static_cast<float>(n));
  }
}

}  // namespace

TEST_CASE("stems: nothing is selected, nothing is staged and nothing is counted") {
  Rig rig;
  REQUIRE(rig.ok);
  CHECK(rig.e.stem_slots() == 0u);
  CHECK(rig.e.stem_block() == nullptr);
  for (unsigned i = 0; i < 50; ++i) {
    const Rendered o = render(rig.e);
    CHECK_FALSE(o.any);
  }
  const dsp::FeatureWork w = rig.e.feature_work();
  CHECK(w.stem_frames == 0u);
  CHECK(w.gate_frames == 0u);
  CHECK(w.monitor_frames == 0u);
  CHECK(w.peak_frames == 0u);
}

TEST_CASE("stems: the selection is the platform's source order, and bits that name no source are dropped") {
  static_assert(dsp::kStemSources == 16, "the USB stem sources");
  static_assert(dsp::kStemMasterL == 12 && dsp::kStemMasterR == 13 && dsp::kStemSendA == 14 && dsp::kStemSendB == 15, "stem_src order");
  dsp::Engine e;
  e.set_stem_slots(0xFFFFFFFFu);
  CHECK(e.stem_slots() == 0xF0FFu);  // T1..T8, ML, MR, SA, SB: T9..T12 are not slots of this engine
  e.set_stem_slots(0x0300u);
  CHECK(e.stem_slots() == 0u);
}

TEST_CASE("stems: a slot's stem is its post-fader mono block to the sample, before the pan and the sends") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_stem_slots(0x0Bu);  // slots 0, 1 and 3
  rig.e.set_mix(1, dsp::MixField::Level, 0.5f);
  (void)render(rig.e);  // the fader ramps to 0.5 in this block
  Rendered o = render(rig.e);
  const std::uint32_t end_n = 5u * kN;  // five blocks rendered so far
  REQUIRE(o.any);
  CHECK(o.has[0]);
  CHECK(o.has[1]);
  CHECK_FALSE(o.has[2]);  // no source in slot 2
  CHECK(o.has[3]);
  for (unsigned i = 4; i < 12; ++i) {
    CHECK_FALSE(o.has[i]);
  }
  const Gen* gens[4] = {&rig.g0, &rig.g1, nullptr, &rig.g3};
  const float level[4] = {1.0f, 0.5f, 1.0f, 1.0f};
  for (unsigned s = 0; s < 4; ++s) {
    if (gens[s] == nullptr) {
      continue;
    }
    float l[kN];
    float r[kN];
    gen_block(*gens[s], end_n, kN, l, r);
    unsigned wrong = 0;
    for (unsigned k = 0; k < kN; ++k) {
      wrong += o.stem[s][k] != ref_s16((l[k] + r[k]) * 0.5f * level[s]) ? 1u : 0u;
    }
    CHECK_MESSAGE(wrong == 0u, "slot ", s);
  }
}

TEST_CASE("stems: a fader move ramps across the block like the mixer, and a slot that starts staging begins at its level") {
  Rig rig;
  REQUIRE(rig.ok);
  (void)rig.e.set_mix(0, dsp::MixField::Level, 0.5f);
  (void)render(rig.e);  // slot 0 is at 0.5 and was never staged
  rig.e.set_stem_slots(0x01u);
  const Rendered first = render(rig.e);  // its first staged block: 0.5 from the first sample, not a ramp from 0
  float l[kN];
  float r[kN];
  gen_block(rig.g0, 5u * kN, kN, l, r);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(first.stem[0][k] == ref_s16((l[k] + r[k]) * 0.5f * 0.5f));
  }
  // Then a move to 1.0: the gain is 0.5 + 0.5 * (k + 1) / 64 at sample k, within one step of the rounding.
  (void)rig.e.set_mix(0, dsp::MixField::Level, 1.0f);
  const Rendered up = render(rig.e);
  gen_block(rig.g0, 6u * kN, kN, l, r);
  for (unsigned k = 0; k < kN; ++k) {
    const float g = 0.5f + 0.5f * static_cast<float>(k + 1u) / static_cast<float>(kN);
    CHECK(std::abs(static_cast<int>(up.stem[0][k]) - static_cast<int>(ref_s16((l[k] + r[k]) * 0.5f * g))) <= 1);
  }
  // Mute: down to 0 across the block, silence after, and the stem still arrives (null would be a gap in the stream).
  (void)rig.e.set_mix(0, dsp::MixField::Mute, 1.0f);
  const Rendered down = render(rig.e);
  CHECK(down.has[0]);
  CHECK(down.stem[0][kN - 1u] == 0);
  const Rendered shut = render(rig.e);
  CHECK(shut.has[0]);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(shut.stem[0][k] == 0);
  }
}

TEST_CASE("stems: Master L and R are the output of render(), what planes_to_s16 sends to the codec") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_stem_slots((1u << dsp::kStemMasterL) | (1u << dsp::kStemMasterR));
  (void)rig.e.set_master_level(0.9f);
  (void)render(rig.e);
  const Rendered o = render(rig.e);
  REQUIRE(o.any);
  CHECK(o.has[dsp::kStemMasterL]);
  CHECK(o.has[dsp::kStemMasterR]);
  CHECK_FALSE(o.has[0]);  // only what is selected
  std::int16_t lr[2 * kN];
  dsp::planes_to_s16(o.l, o.r, lr, kN);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(o.stem[dsp::kStemMasterL][k] == lr[2u * k]);
    CHECK(o.stem[dsp::kStemMasterR][k] == lr[2u * k + 1u]);
  }
}

TEST_CASE("stems: a send's stem is the mono of its effect's return; a bus with no effect has none") {
  Rig rig;
  REQUIRE(rig.ok);
  // Slot 1 at level 0 (silent in the master) with send A at 0.5: the bus holds the source times 0.5, the effect doubles it.
  (void)rig.e.set_mix(1, dsp::MixField::Level, 0.0f);
  rig.e.set_stem_slots((1u << dsp::kStemSendA) | (1u << dsp::kStemSendB));
  (void)render(rig.e);
  const Rendered o = render(rig.e);
  REQUIRE(o.any);
  CHECK(o.has[dsp::kStemSendA]);
  CHECK_FALSE(o.has[dsp::kStemSendB]);  // no effect on bus B
  float l[kN];
  float r[kN];
  gen_block(rig.g1, 5u * kN, kN, l, r);
  // The send of slot 1 is the only one set; the effect doubles the bus: the stem is the mono of 2 * 0.5 * the source.
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(std::abs(static_cast<int>(o.stem[dsp::kStemSendA][k]) - static_cast<int>(ref_s16((l[k] + r[k]) * 0.5f))) <= 1);
  }
}

TEST_CASE("stems: the table is aligned, a source with no unit is null, and clearing the selection ends the stream of blocks") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_stem_slots(0xF0FFu);
  Rendered o = render(rig.e);
  REQUIRE(o.any);
  const std::int16_t* const* t = rig.e.stem_block();
  REQUIRE(t != nullptr);
  for (unsigned i = 0; i < dsp::kStemSources; ++i) {
    if (t[i] != nullptr) {
      CHECK(reinterpret_cast<std::uintptr_t>(t[i]) % 4u == 0u);
    }
  }
  CHECK(t[2] == nullptr);   // slot 2: no source
  CHECK(t[4] == nullptr);   // slot 4..7: no source
  CHECK(t[8] == nullptr);   // T9..T12 are not slots
  CHECK(t[11] == nullptr);
  CHECK(t[dsp::kStemMasterL] != nullptr);
  CHECK(t[dsp::kStemSendA] != nullptr);
  CHECK(t[dsp::kStemSendB] == nullptr);
  rig.e.set_stem_slots(0u);
  o = render(rig.e);
  CHECK_FALSE(o.any);
  CHECK(rig.e.stem_block() == nullptr);
}

TEST_CASE("stems: a short render stages its frames and zeroes the rest of the block; a long one is cut to a block") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_stem_slots(0x01u | (1u << dsp::kStemMasterL));
  const Rendered full = render(rig.e);
  REQUIRE(full.any);
  const Rendered shorter = render(rig.e, 40);
  REQUIRE(shorter.any);
  CHECK(shorter.stem[0][39] != 0);
  for (unsigned k = 40; k < kN; ++k) {
    CHECK(shorter.stem[0][k] == 0);
    CHECK(shorter.stem[dsp::kStemMasterL][k] == 0);
  }
  float ol[kN];
  float orr[kN];
  rig.e.render(nullptr, nullptr, ol, orr, 200);  // cut to kMaxBlockFrames, as render() always did
  CHECK(rig.e.stem_block() != nullptr);
}

TEST_CASE("stems: full scale saturates, a NaN is silence") {
  dsp::Engine e;
  Gen loud{1.5f, 0.0f};
  Gen bad{std::numeric_limits<float>::quiet_NaN(), 0.0f};
  Gen neg{-1.5f, 0.0f};
  const dsp::UnitId a = e.attach(loud);
  const dsp::UnitId b = e.attach(bad);
  const dsp::UnitId c = e.attach(neg);
  REQUIRE(e.set_source(0, a));
  REQUIRE(e.set_source(1, b));
  REQUIRE(e.set_source(2, c));
  e.start();
  e.set_stem_slots(0x07u);
  (void)render(e);
  const Rendered o = render(e);
  REQUIRE(o.any);
  for (unsigned k = 0; k < kN; ++k) {
    CHECK(o.stem[0][k] == 32767);
    CHECK(o.stem[1][k] == 0);
    CHECK(o.stem[2][k] == -32767);
  }
}

TEST_CASE("stems: the work is what is selected, and only while it is") {
  Rig rig;
  REQUIRE(rig.ok);
  rig.e.set_stem_slots(0x03u);  // two slots
  for (unsigned i = 0; i < 10; ++i) {
    (void)render(rig.e);
  }
  CHECK(rig.e.feature_work().stem_frames == 10u * 2u * kN);
  rig.e.set_stem_slots(0x03u | (1u << dsp::kStemMasterL) | (1u << dsp::kStemMasterR) | (1u << dsp::kStemSendA));
  (void)render(rig.e);
  CHECK(rig.e.feature_work().stem_frames == 10u * 2u * kN + 5u * kN);
  rig.e.set_stem_slots(0u);
  for (unsigned i = 0; i < 10; ++i) {
    (void)render(rig.e);
  }
  CHECK(rig.e.feature_work().stem_frames == 10u * 2u * kN + 5u * kN);  // unchanged: off again costs nothing
  CHECK(rig.e.feature_work().gate_frames == 0u);
  CHECK(rig.e.feature_work().monitor_frames == 0u);
  CHECK(rig.e.feature_work().peak_frames == 0u);
}
