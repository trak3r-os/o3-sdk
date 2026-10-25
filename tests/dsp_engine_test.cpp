// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/dsp/engine.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace o3sdk;

namespace {

constexpr core::ParamDesc kProbeParams[] = {
    {"LEVEL", "", nullptr, nullptr, 0.0f, 1.0f, 0.5f, 100, 10, 0, core::ParamUnit::NormPct, core::ParamCurve::Linear, 0, 0},
    {"FREQ", "Hz", nullptr, nullptr, 20.0f, 20000.0f, 1000.0f, 100, 10, 0, core::ParamUnit::Hz, core::ParamCurve::Log, 0, 0},
};

// An instrument that outputs its LEVEL as DC and records every event it gets.
class Probe final : public dsp::Unit {
 public:
  explicit Probe(dsp::UnitKind kind = dsp::UnitKind::Instrument) : info_{"PROBE", kind, kProbeParams, 2, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float rate) noexcept override {
    rate_ = rate;
    level_ = kProbeParams[0].def;
    prepared_ = true;
  }
  void process(const dsp::ProcessArgs& a) noexcept override {
    ++calls_;
    for (std::uint32_t i = 0; i < a.event_count; ++i) {
      events_.push_back(a.events[i]);
      if (a.events[i].kind == dsp::EventKind::Param && a.events[i].param == 0) {
        level_ = a.events[i].value;
      }
    }
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      if (info_.kind == dsp::UnitKind::Effect) {
        a.out_l[i] = a.in_l[i] * 2.0f;  // an effect that doubles
        a.out_r[i] = a.in_r[i] * 2.0f;
      } else {
        a.out_l[i] = level_;
        a.out_r[i] = level_;
      }
    }
  }
  dsp::UnitInfo info_;
  float rate_ = 0.0f;
  float level_ = 0.0f;
  bool prepared_ = false;
  unsigned calls_ = 0;
  std::vector<dsp::Event> events_;
};

// A hook that places one note at a frame of every block while playing, and remembers the transport flags it saw.
class Placer final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext& ctx) noexcept override {
    started_ += ctx.transport().started ? 1 : 0;
    stopped_ += ctx.transport().stopped ? 1 : 0;
    if (ctx.transport().playing) {
      (void)ctx.emit(unit_, dsp::note_on(60, 100, 0, frame_));
    }
  }
  dsp::UnitId unit_ = 0;
  std::uint16_t frame_ = 10;
  int started_ = 0;
  int stopped_ = 0;
};

struct Block {
  float l[64];
  float r[64];
};

Block render(dsp::Engine& e) {
  Block b{};
  e.render(nullptr, nullptr, b.l, b.r, 64);
  return b;
}

}  // namespace

TEST_CASE("engine: attach prepares the unit; start delivers every stored value once") {
  dsp::Engine e;
  Probe p;
  const dsp::UnitId id = e.attach(p);
  REQUIRE(id == 0);
  CHECK(p.prepared_);
  CHECK(p.rate_ > 44000.0f);
  CHECK(e.set_param(id, 0, 0.25f));  // before start: stored, delivered by start()
  e.start();
  REQUIRE(p.events_.size() == 2u);
  CHECK(p.events_[0].kind == dsp::EventKind::Param);
  CHECK(p.events_[0].value == doctest::Approx(0.25f));
  CHECK(p.events_[1].value == doctest::Approx(1000.0f));
  CHECK(e.attach(p) == dsp::kNoUnit);  // setup is over
}

TEST_CASE("engine: a routed instrument reaches the output at unity, centred; an unrouted one is not run") {
  dsp::Engine e;
  Probe a;
  Probe b;
  const dsp::UnitId ia = e.attach(a);
  (void)e.attach(b);
  REQUIRE(e.set_source(0, ia));
  e.start();
  (void)render(e);  // the first block ramps the gain up from 0
  const Block out = render(e);
  CHECK(out.l[63] == doctest::Approx(0.5f).epsilon(0.001));
  CHECK(out.r[63] == doctest::Approx(0.5f).epsilon(0.001));
  CHECK(a.calls_ == 3u);  // start + two blocks
  CHECK(b.calls_ == 1u);  // start only
}

TEST_CASE("engine: level, pan and mute act through the ring, ramped over one block") {
  dsp::Engine e;
  Probe a;
  const dsp::UnitId ia = e.attach(a);
  REQUIRE(e.set_source(0, ia));
  e.start();
  (void)render(e);
  (void)render(e);
  CHECK(e.set_mix(0, dsp::MixField::Pan, -1.0f));
  Block out = render(e);  // ramping
  out = render(e);
  CHECK(out.r[63] == doctest::Approx(0.0f).epsilon(0.001));
  CHECK(out.l[63] == doctest::Approx(0.5f * 1.41421356f).epsilon(0.001));
  CHECK(e.set_mix(0, dsp::MixField::Mute, 1.0f));
  (void)render(e);
  out = render(e);
  CHECK(out.l[63] == doctest::Approx(0.0f));
  CHECK(e.mix(0, dsp::MixField::Mute) == 1.0f);
}

TEST_CASE("engine: inserts and sends run where they are routed") {
  dsp::Engine e;
  Probe src;
  Probe insert(dsp::UnitKind::Effect);
  Probe send(dsp::UnitKind::Effect);
  const dsp::UnitId is = e.attach(src);
  const dsp::UnitId ii = e.attach(insert);
  const dsp::UnitId isend = e.attach(send);
  REQUIRE(e.set_source(0, is));
  REQUIRE(e.set_insert(0, 0, ii));
  REQUIRE(e.set_send_effect(0, isend));
  CHECK_FALSE(e.set_source(1, ii));  // an effect is not a source
  CHECK_FALSE(e.set_insert(0, 1, is));  // nor is an instrument an insert
  REQUIRE(e.set_mix(0, dsp::MixField::Level, 0.5f));
  REQUIRE(e.set_mix(0, dsp::MixField::SendA, 0.25f));
  e.set_master_level(1.0f);
  e.start();
  (void)render(e);
  const Block out = render(e);
  // source 0.5, doubled by the insert = 1.0; dry at level 0.5 = 0.5; send 1.0 x 0.25 doubled by the send effect = 0.5; total 1.0,
  // which the master's soft clip bends: above 0.8 and below 1.
  CHECK(out.l[63] > 0.8f);
  CHECK(out.l[63] < 1.0f);
  const dsp::EngineStats s = e.stats();
  CHECK(s.peak_l == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(s.clipped >= 1u);
}

TEST_CASE("engine: main-loop events land at frame 0, hook events at their frame, sorted") {
  dsp::Engine e;
  Probe p;
  Placer hook;
  const dsp::UnitId id = e.attach(p);
  REQUIRE(e.set_source(0, id));
  hook.unit_ = id;
  REQUIRE(e.add_hook(hook));
  e.start();
  p.events_.clear();
  CHECK(e.play());
  CHECK(e.note_on(id, 64, 90));
  (void)render(e);
  REQUIRE(p.events_.size() == 2u);
  CHECK(p.events_[0].frame == 0);
  CHECK(p.events_[0].note == 64);
  CHECK(p.events_[1].frame == 10);
  CHECK(p.events_[1].note == 60);
  CHECK(hook.started_ == 1);
  CHECK(e.stop());
  (void)render(e);
  (void)render(e);
  CHECK(hook.stopped_ == 1);  // seen once
  CHECK_FALSE(e.stats().playing);
}

TEST_CASE("engine: a unit's event list overflows by dropping and counting") {
  dsp::Engine e;
  Probe p;
  const dsp::UnitId id = e.attach(p);
  REQUIRE(e.set_source(0, id));
  e.start();
  for (unsigned i = 0; i < dsp::kMaxEventsPerUnit + 5u; ++i) {
    (void)e.note_on(id, 60, 100);
  }
  (void)render(e);
  CHECK(e.stats().dropped_events == 5u);
}

TEST_CASE("engine: parameters are clamped, stepped through the curve and formatted") {
  dsp::Engine e;
  Probe p;
  const dsp::UnitId id = e.attach(p);
  e.start();
  CHECK(e.set_param(id, 0, 7.0f));
  CHECK(e.param(id, 0) == 1.0f);
  CHECK_FALSE(e.set_param(id, 9, 0.0f));
  CHECK_FALSE(e.set_param(5, 0, 0.0f));
  (void)e.set_param(id, 1, 1000.0f);
  const float up = e.step_param(id, 1, 10);
  CHECK(up > 1000.0f);
  CHECK(e.step_param(id, 1, -10) == doctest::Approx(1000.0f).epsilon(0.01));
  const float coarse = e.step_param(id, 1, 1, true);
  CHECK(coarse == doctest::Approx(up).epsilon(0.01));  // one coarse step = ten fine ones
  char text[16];
  (void)e.set_param(id, 1, 1200.0f);
  e.format_param(id, 1, text, sizeof text);
  CHECK(std::string(text) == "1.20k");
  (void)e.set_param(id, 1, 440.0f);
  e.format_param(id, 1, text, sizeof text);
  CHECK(std::string(text) == "440");
}

TEST_CASE("engine: tempo commands reach the transport") {
  dsp::Engine e;
  e.start();
  CHECK(e.set_tempo(140'000));
  CHECK(e.tempo() == 140'000u);
  CHECK(e.play());
  Block b{};
  e.render(nullptr, nullptr, b.l, b.r, 64);
  const dsp::EngineStats s = e.stats();
  CHECK(s.tempo_mbpm == 140'000u);
  CHECK(s.pos == dsp::ticks_per_sample_q32(140'000) * 64u);
  CHECK(e.set_tempo(5'000));
  CHECK(e.tempo() == dsp::kMinTempoMilliBpm);
}

TEST_CASE("engine: int16 conversion round-trips and clips") {
  std::int16_t in[8] = {0, 32767, -32768, 16384, 1, -1, 100, -100};
  float l[4];
  float r[4];
  dsp::s16_to_planes(in, l, r, 4);
  CHECK(l[0] == 0.0f);
  CHECK(r[0] == doctest::Approx(32767.0f / 32768.0f));
  std::int16_t out[8];
  dsp::planes_to_s16(l, r, out, 4);
  CHECK(out[0] == 0);
  CHECK(out[2] == -32767);  // clipped at -1.0 x 32767
  float big[1] = {3.0f};
  float neg[1] = {-3.0f};
  std::int16_t o2[2];
  dsp::planes_to_s16(big, neg, o2, 1);
  CHECK(o2[0] == 32767);
  CHECK(o2[1] == -32767);
}
