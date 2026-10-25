// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/dsp/engine.h>

#include <cmath>
#include <cstdint>
#include <limits>
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

// A hook that writes mixer fields: the test queues the writes for the next block, the hook performs them in order and records each
// call's result and what mix() reads back right after it, plus the slot 0 level it saw before writing anything.
class MixWriter final : public dsp::Clocked {
 public:
  struct Write {
    unsigned slot;
    dsp::MixField field;
    float value;
  };
  void block(dsp::BlockContext& ctx) noexcept override {
    ++calls_;
    before_level_ = ctx.mix(0, dsp::MixField::Level);
    for (const Write& w : writes_) {
      results_.push_back(ctx.set_mix(w.slot, w.field, w.value));
      readback_.push_back(ctx.mix(w.slot, w.field));
    }
    writes_.clear();
  }
  void queue(unsigned slot, dsp::MixField field, float value) { writes_.push_back({slot, field, value}); }
  void forget() {
    results_.clear();
    readback_.clear();
  }
  std::vector<Write> writes_;
  std::vector<bool> results_;
  std::vector<float> readback_;
  float before_level_ = -1.0f;
  int calls_ = 0;
};

// Slot 0: a DC source (0.5) at the centre, send A into an effect that doubles it, two hooks. The first hook added runs first.
struct MixRig {
  MixRig() {
    const dsp::UnitId is = e.attach(src);
    const dsp::UnitId ifx = e.attach(fx);
    ok = e.set_source(0, is) && e.set_send_effect(0, ifx) && e.add_hook(first) && e.add_hook(second);
    e.start();
  }
  // Renders `n` blocks (the first ones of a rig ramp the gains up from 0, so settle() first) and appends them to `l` and `r`.
  void run(unsigned n, std::vector<float>& l, std::vector<float>& r) {
    for (unsigned i = 0; i < n; ++i) {
      const Block b = render(e);
      l.insert(l.end(), b.l, b.l + 64);
      r.insert(r.end(), b.r, b.r + 64);
    }
  }
  void settle() {
    std::vector<float> l;
    std::vector<float> r;
    run(3, l, r);
    last_l = l.back();
    last_r = r.back();
  }
  dsp::Engine e;
  Probe src;
  Probe fx{dsp::UnitKind::Effect};
  MixWriter first;
  MixWriter second;
  bool ok = false;
  float last_l = 0.0f;
  float last_r = 0.0f;
};

// The largest difference between neighbouring samples of `v`, whose first element is the sample before the block under test.
float max_step(const std::vector<float>& v) {
  float m = 0.0f;
  for (std::size_t i = 1; i < v.size(); ++i) {
    m = std::fmax(m, std::fabs(v[i] - v[i - 1]));
  }
  return m;
}

// `bound` is the ramp's step (the target's distance over the 64 frames of the block, in the output's scale), plus float slack.
bool within(float step, float bound) { return step <= bound * 1.001f + 1e-6f; }

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

// ---- Mixer writes from a hook (BlockContext::set_mix) -----------------------------------------------------------------------

TEST_CASE("engine hook mixer: a Level write is ramped across the block, so a full jump is click-free") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  CHECK(rig.last_l == doctest::Approx(0.5f).epsilon(1e-4));  // source 0.5 at unity, centred
  std::vector<float> l = {rig.last_l};
  std::vector<float> r = {rig.last_r};

  rig.first.queue(0, dsp::MixField::Level, 0.0f);  // the whole distance at once: -1.0 of gain, -0.5 of output
  rig.run(1, l, r);
  REQUIRE(rig.first.results_.size() == 1u);
  CHECK(rig.first.results_[0]);
  CHECK(l.back() == doctest::Approx(0.0f).epsilon(1e-4));        // reached at the end of the block
  CHECK(l[1] == doctest::Approx(0.5f - 0.5f / 64.0f).epsilon(1e-4));  // started from where the last block left it
  CHECK(within(max_step(l), 0.5f / 64.0f));                        // an unramped jump would step 0.5
  CHECK(within(max_step(r), 0.5f / 64.0f));

  // And up past unity, 1.5 of gain (0.75 out, under the clipper's knee), again in one block.
  l = {l.back()};
  r = {r.back()};
  rig.first.queue(0, dsp::MixField::Level, 1.5f);
  rig.run(1, l, r);
  CHECK(l.back() == doctest::Approx(0.75f).epsilon(1e-4));
  CHECK(within(max_step(l), 0.75f / 64.0f));
}

TEST_CASE("engine hook mixer: a send A write is ramped the same way") {
  MixRig rig;
  REQUIRE(rig.ok);
  REQUIRE(rig.e.set_mix(0, dsp::MixField::Level, 0.0f));  // the dry path off, so only the send reaches the output
  rig.settle();
  CHECK(rig.last_l == doctest::Approx(0.0f).epsilon(1e-5));
  std::vector<float> l = {rig.last_l};
  std::vector<float> r = {rig.last_r};

  rig.first.queue(0, dsp::MixField::SendA, 0.4f);  // 0.5 source x 0.4 send x 2 (the send effect doubles) = 0.4 out
  rig.run(1, l, r);
  CHECK(l.back() == doctest::Approx(0.4f).epsilon(1e-4));
  CHECK(within(max_step(l), 0.4f / 64.0f));
  CHECK(l[1] == doctest::Approx(0.4f / 64.0f).epsilon(1e-4));

  l = {l.back()};
  r = {r.back()};
  rig.first.queue(0, dsp::MixField::SendA, 0.0f);
  rig.run(1, l, r);
  CHECK(l.back() == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(within(max_step(l), 0.4f / 64.0f));
}

TEST_CASE("engine hook mixer: Pan and Mute are ramped too") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  std::vector<float> l = {rig.last_l};
  std::vector<float> r = {rig.last_r};
  rig.first.queue(0, dsp::MixField::Pan, -1.0f);  // right gain 1 -> 0, left gain 1 -> sqrt(2)
  rig.run(1, l, r);
  CHECK(r.back() == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(l.back() == doctest::Approx(0.5f * 1.41421356f).epsilon(1e-4));
  CHECK(within(max_step(r), 0.5f / 64.0f));
  CHECK(within(max_step(l), 0.5f * 0.41421356f / 64.0f));

  l = {l.back()};
  r = {r.back()};
  rig.first.queue(0, dsp::MixField::Mute, 1.0f);
  rig.run(1, l, r);
  CHECK(l.back() == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(within(max_step(l), 0.5f * 1.41421356f / 64.0f));
  CHECK(rig.first.readback_.back() == 1.0f);
}

TEST_CASE("engine hook mixer: the last write wins; the value stays; the main loop's next write replaces it") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  std::vector<float> l;
  std::vector<float> r;

  // A main-loop write and a hook write to the same field in the same block: the main loop's is applied first (the hook already sees
  // it), the hook's replaces it; Engine::mix() is the main loop's own value and does not follow the hook.
  REQUIRE(rig.e.set_mix(0, dsp::MixField::Level, 0.2f));
  rig.first.queue(0, dsp::MixField::Level, 0.8f);
  rig.run(1, l, r);
  CHECK(rig.first.before_level_ == doctest::Approx(0.2f));
  CHECK(l.back() == doctest::Approx(0.4f).epsilon(1e-4));  // the hook's 0.8 x 0.5, not the main loop's 0.1
  CHECK(rig.e.mix(0, dsp::MixField::Level) == doctest::Approx(0.2f));

  // No write in the next blocks: the hook's value is still the target (nothing restores the main loop's).
  rig.run(2, l, r);
  CHECK(rig.first.before_level_ == doctest::Approx(0.8f));
  CHECK(l.back() == doctest::Approx(0.4f).epsilon(1e-4));
  CHECK(l[64] == doctest::Approx(0.4f).epsilon(1e-4));  // flat from the second block on

  // The main loop's next write, applied at the start of the next block, replaces it; the hook that does not write leaves it alone.
  REQUIRE(rig.e.set_mix(0, dsp::MixField::Level, 0.3f));
  rig.run(1, l, r);
  CHECK(rig.first.before_level_ == doctest::Approx(0.3f));
  CHECK(l.back() == doctest::Approx(0.15f).epsilon(1e-4));

  // A hook that writes every block (a lane) keeps the field against the main loop: the hook runs after the queue is drained.
  for (int i = 0; i < 3; ++i) {
    REQUIRE(rig.e.set_mix(0, dsp::MixField::Level, 1.0f));
    rig.first.queue(0, dsp::MixField::Level, 0.6f);
    rig.run(1, l, r);
  }
  CHECK(l.back() == doctest::Approx(0.3f).epsilon(1e-4));
}

TEST_CASE("engine hook mixer: several writes in one block collapse to the last; the hooks run in the order added") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  std::vector<float> l = {rig.last_l};
  std::vector<float> r = {rig.last_r};

  // A slew's four writes (every 16 frames) in one block: the last is the target, and the ramp still spans the whole block.
  rig.first.queue(0, dsp::MixField::Level, 0.1f);
  rig.first.queue(0, dsp::MixField::Level, 0.9f);
  rig.first.queue(0, dsp::MixField::Level, 0.3f);
  rig.first.queue(0, dsp::MixField::Level, 0.6f);
  rig.run(1, l, r);
  CHECK(l.back() == doctest::Approx(0.3f).epsilon(1e-4));
  CHECK(l[1] == doctest::Approx(0.5f + (0.3f - 0.5f) / 64.0f).epsilon(1e-4));
  CHECK(within(max_step(l), 0.2f / 64.0f));
  REQUIRE(rig.first.readback_.size() == 4u);
  CHECK(rig.first.readback_[1] == doctest::Approx(0.9f));  // mix() follows each call

  // Two hooks on the same field: the second one added runs later and wins.
  l = {l.back()};
  r = {r.back()};
  rig.first.queue(0, dsp::MixField::Level, 1.0f);
  rig.second.queue(0, dsp::MixField::Level, 0.2f);
  rig.run(1, l, r);
  CHECK(rig.first.before_level_ == doctest::Approx(0.6f));   // the first hook starts from the previous block's last write
  CHECK(rig.second.before_level_ == doctest::Approx(1.0f));  // the second one starts after the first has written
  CHECK(l.back() == doctest::Approx(0.1f).epsilon(1e-4));
}

TEST_CASE("engine hook mixer: a slot or field out of range is refused and writes nothing; values are clamped") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  std::vector<float> l = {rig.last_l};
  std::vector<float> r = {rig.last_r};
  const auto bad_field = static_cast<dsp::MixField>(dsp::kMixFieldCount);

  rig.first.queue(dsp::kMaxSlots, dsp::MixField::Level, 0.0f);
  rig.first.queue(0xFFFFFFFFu, dsp::MixField::Level, 0.0f);
  rig.first.queue(0, bad_field, 0.0f);
  rig.first.queue(0, static_cast<dsp::MixField>(255), 0.0f);
  rig.first.queue(dsp::kMaxSlots, bad_field, 0.0f);
  rig.run(1, l, r);
  REQUIRE(rig.first.results_.size() == 5u);
  for (bool ok : rig.first.results_) {
    CHECK_FALSE(ok);
  }
  for (float v : rig.first.readback_) {
    CHECK(v == 0.0f);  // as Engine::mix()
  }
  CHECK(l.back() == doctest::Approx(0.5f).epsilon(1e-4));  // nothing moved
  CHECK(max_step(l) < 1e-5f);
  CHECK(rig.first.before_level_ == 1.0f);
  CHECK(rig.e.mix(dsp::kMaxSlots, dsp::MixField::Level) == 0.0f);  // the main loop's reader refuses the same indexes

  // The last slot and the last field are in range; a slot with no source takes the value (it is only a target) and sounds nothing.
  rig.first.forget();
  rig.first.queue(dsp::kMaxSlots - 1u, dsp::MixField::Mute, 1.0f);
  rig.run(1, l, r);
  REQUIRE(rig.first.results_.size() == 1u);
  CHECK(rig.first.results_[0]);
  CHECK(rig.first.readback_[0] == 1.0f);
  CHECK(l.back() == doctest::Approx(0.5f).epsilon(1e-4));

  // Values clamp as Engine::set_mix clamps them; NaN gives the field's default.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  rig.first.forget();
  rig.first.queue(0, dsp::MixField::Level, 9.0f);
  rig.first.queue(0, dsp::MixField::Level, -1.0f);
  rig.first.queue(0, dsp::MixField::Level, nan);
  rig.first.queue(0, dsp::MixField::SendA, nan);
  rig.first.queue(0, dsp::MixField::SendB, 9.0f);
  rig.first.queue(0, dsp::MixField::Pan, 5.0f);
  rig.first.queue(0, dsp::MixField::Pan, -5.0f);
  rig.first.queue(0, dsp::MixField::Mute, 0.49f);
  rig.first.queue(0, dsp::MixField::Mute, 0.5f);
  rig.run(1, l, r);
  const float expect[] = {1.5f, 0.0f, 1.0f, 0.0f, 1.5f, 1.0f, -1.0f, 0.0f, 1.0f};
  REQUIRE(rig.first.readback_.size() == 9u);
  for (std::size_t i = 0; i < 9u; ++i) {
    CHECK(rig.first.results_[i]);
    CHECK(rig.first.readback_[i] == expect[i]);
  }
}

TEST_CASE("engine hook mixer: a hook's write does not disturb a main-loop mix that no hook touches") {
  MixRig rig;
  REQUIRE(rig.ok);
  rig.settle();
  std::vector<float> l;
  std::vector<float> r;
  REQUIRE(rig.e.set_mix(0, dsp::MixField::Pan, -1.0f));
  rig.first.queue(0, dsp::MixField::Level, 0.5f);  // another field
  rig.run(2, l, r);
  CHECK(r.back() == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(l.back() == doctest::Approx(0.5f * 0.5f * 1.41421356f).epsilon(1e-4));
  CHECK(rig.e.mix(0, dsp::MixField::Pan) == -1.0f);
}
