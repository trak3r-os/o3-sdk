// SPDX-License-Identifier: GPL-3.0-or-later
// Refused setup calls are recorded (Engine::setup_errors) and the simulator's Harness fails on one. The calls themselves behave as
// they always did: false (attach: kNoUnit) and no change of the routing.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/sim/harness.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3sdk;

namespace {

constexpr core::ParamDesc kOneParam[] = {
    {"LEVEL", "", nullptr, nullptr, 0.0f, 1.0f, 0.5f, 100, 10, 0, core::ParamUnit::NormPct, core::ParamCurve::Linear, 0, 0},
};

// 65 parameters: one more than a unit may declare. Only the count matters to attach(); the table is never read past its first row
// because attach refuses the unit before it copies a default.
constexpr unsigned kTooMany = dsp::kMaxParamsPerUnit + 1u;

class Dummy final : public dsp::Unit {
 public:
  explicit Dummy(dsp::UnitKind kind = dsp::UnitKind::Instrument, std::uint16_t params = 1)
      : info_{"DUMMY", kind, kOneParam, params, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  // An instrument outputs 0.25; an effect doubles its input, so an insert or a send that took effect would show in the output.
  void process(const dsp::ProcessArgs& a) noexcept override {
    const bool effect = info_.kind == dsp::UnitKind::Effect;
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = effect ? a.in_l[i] * 2.0f : 0.25f;
      a.out_r[i] = effect ? a.in_r[i] * 2.0f : 0.25f;
    }
  }

 private:
  dsp::UnitInfo info_;
};

class NoHook final : public dsp::Clocked {
 public:
  void block(dsp::BlockContext&) noexcept override {}
};

// The refusal at position i is `call` for `reason`, with the arguments the call was given.
void expect_error(const dsp::Engine& e, unsigned i, dsp::SetupCall call, dsp::SetupReason reason, unsigned slot, unsigned index,
                  unsigned unit) {
  const dsp::SetupError* err = e.setup_error(i);
  REQUIRE(err != nullptr);
  CHECK(err->call == call);
  CHECK(err->reason == reason);
  CHECK(err->slot == slot);
  CHECK(err->index == index);
  CHECK(err->unit == unit);
}

}  // namespace

TEST_CASE("setup errors: a setup that goes through leaves no record") {
  dsp::Engine e;
  Dummy inst;
  Dummy fx{dsp::UnitKind::Effect};
  NoHook hook;
  const dsp::UnitId i = e.attach(inst);
  const dsp::UnitId f = e.attach(fx);
  CHECK(e.set_source(0, i));
  CHECK(e.set_insert(0, 1, f));
  CHECK(e.set_send_effect(1, f));
  CHECK(e.set_master_insert(1, f));
  CHECK(e.add_hook(hook));
  e.start();
  CHECK(e.setup_errors() == 0u);
  CHECK(e.setup_error(0) == nullptr);
}

TEST_CASE("setup errors: each refusal returns as before and is recorded with its call, reason and arguments") {
  Dummy inst;
  Dummy fx{dsp::UnitKind::Effect};
  {
    dsp::Engine e;
    const dsp::UnitId i = e.attach(inst);  // 0
    const dsp::UnitId f = e.attach(fx);    // 1
    REQUIRE(e.setup_errors() == 0u);

    unsigned n = 0;
    // set_source: slot out of range, an id never attached, an effect given as a source.
    CHECK_FALSE(e.set_source(dsp::kMaxSlots, i));
    expect_error(e, n++, dsp::SetupCall::SetSource, dsp::SetupReason::OutOfRange, dsp::kMaxSlots, 0, i);
    CHECK_FALSE(e.set_source(0, 7));
    expect_error(e, n++, dsp::SetupCall::SetSource, dsp::SetupReason::UnknownUnit, 0, 0, 7);
    CHECK_FALSE(e.set_source(2, f));
    expect_error(e, n++, dsp::SetupCall::SetSource, dsp::SetupReason::WrongKind, 2, 0, f);
    // set_insert: an index past the inserts, an instrument as an insert, the unit id of a refused attach (kNoUnit).
    CHECK_FALSE(e.set_insert(1, dsp::kMaxInserts, f));
    expect_error(e, n++, dsp::SetupCall::SetInsert, dsp::SetupReason::OutOfRange, 1, dsp::kMaxInserts, f);
    CHECK_FALSE(e.set_insert(1, 0, i));
    expect_error(e, n++, dsp::SetupCall::SetInsert, dsp::SetupReason::WrongKind, 1, 0, i);
    CHECK_FALSE(e.set_insert(3, 1, dsp::kNoUnit));
    expect_error(e, n++, dsp::SetupCall::SetInsert, dsp::SetupReason::UnknownUnit, 3, 1, dsp::kNoUnit);
    CHECK(e.setup_errors() == n);
    CHECK(e.setup_error(n) == nullptr);  // nothing past the last one

    // The refusals changed nothing: the good calls after them route as usual.
    CHECK(e.set_source(0, i));
    CHECK(e.set_insert(0, 0, f));
    CHECK(e.setup_errors() == n);
  }
  {
    dsp::Engine e;
    const dsp::UnitId i = e.attach(inst);
    const dsp::UnitId f = e.attach(fx);
    unsigned n = 0;
    CHECK_FALSE(e.set_send_effect(dsp::kSendCount, f));
    expect_error(e, n++, dsp::SetupCall::SetSendEffect, dsp::SetupReason::OutOfRange, dsp::kSendCount, 0, f);
    CHECK_FALSE(e.set_send_effect(0, i));
    expect_error(e, n++, dsp::SetupCall::SetSendEffect, dsp::SetupReason::WrongKind, 0, 0, i);
    CHECK_FALSE(e.set_master_insert(dsp::kMaxInserts, f));
    expect_error(e, n++, dsp::SetupCall::SetMasterInsert, dsp::SetupReason::OutOfRange, 0, dsp::kMaxInserts, f);
    CHECK_FALSE(e.set_master_insert(0, 9));
    expect_error(e, n++, dsp::SetupCall::SetMasterInsert, dsp::SetupReason::UnknownUnit, 0, 0, 9);
    // A huge argument is recorded as 255, not wrapped.
    CHECK_FALSE(e.set_source(0x1234u, i));
    expect_error(e, n++, dsp::SetupCall::SetSource, dsp::SetupReason::OutOfRange, 255, 0, i);
    CHECK(e.setup_errors() == n);
  }
}

TEST_CASE("setup errors: attach and add_hook say when the engine is full or too small for the unit") {
  dsp::Engine e;
  Dummy units[dsp::kMaxUnits];
  for (Dummy& u : units) {
    CHECK(e.attach(u) != dsp::kNoUnit);
  }
  Dummy one_more;
  CHECK(e.attach(one_more) == dsp::kNoUnit);
  expect_error(e, 0, dsp::SetupCall::Attach, dsp::SetupReason::Full, 0, 0, dsp::kNoUnit);

  dsp::Engine small;
  Dummy wide{dsp::UnitKind::Instrument, static_cast<std::uint16_t>(kTooMany)};
  CHECK(small.attach(wide) == dsp::kNoUnit);
  expect_error(small, 0, dsp::SetupCall::Attach, dsp::SetupReason::TooManyParams, 0, 0, dsp::kNoUnit);
  CHECK(small.unit_count() == 0u);  // nothing was half-attached

  NoHook hooks[dsp::kMaxHooks + 1u];
  for (unsigned h = 0; h < dsp::kMaxHooks; ++h) {
    CHECK(small.add_hook(hooks[h]));
  }
  CHECK_FALSE(small.add_hook(hooks[dsp::kMaxHooks]));
  expect_error(small, 1, dsp::SetupCall::AddHook, dsp::SetupReason::Full, 0, 0, dsp::kNoUnit);
  CHECK(small.setup_errors() == 2u);
}

TEST_CASE("setup errors: after start() every setup call is refused and recorded, and nothing is rerouted") {
  dsp::Engine e;
  Dummy inst;
  Dummy fx{dsp::UnitKind::Effect};
  NoHook hook;
  const dsp::UnitId i = e.attach(inst);
  const dsp::UnitId f = e.attach(fx);
  REQUIRE(e.set_source(0, i));
  e.start();
  REQUIRE(e.setup_errors() == 0u);  // start() itself is not a refusal

  Dummy late;
  CHECK(e.attach(late) == dsp::kNoUnit);
  CHECK_FALSE(e.set_source(1, i));
  CHECK_FALSE(e.set_insert(0, 0, f));
  CHECK_FALSE(e.set_send_effect(0, f));
  CHECK_FALSE(e.set_master_insert(0, f));
  CHECK_FALSE(e.add_hook(hook));
  CHECK(e.setup_errors() == 6u);
  expect_error(e, 0, dsp::SetupCall::Attach, dsp::SetupReason::AfterStart, 0, 0, dsp::kNoUnit);
  expect_error(e, 1, dsp::SetupCall::SetSource, dsp::SetupReason::AfterStart, 1, 0, i);
  expect_error(e, 2, dsp::SetupCall::SetInsert, dsp::SetupReason::AfterStart, 0, 0, f);
  expect_error(e, 3, dsp::SetupCall::SetSendEffect, dsp::SetupReason::AfterStart, 0, 0, f);
  expect_error(e, 4, dsp::SetupCall::SetMasterInsert, dsp::SetupReason::AfterStart, 0, 0, f);
  expect_error(e, 5, dsp::SetupCall::AddHook, dsp::SetupReason::AfterStart, 0, 0, dsp::kNoUnit);
  CHECK(e.unit_count() == 2u);

  // Slot 0 still plays the instrument and nothing else (the refused insert and send did not take): 0.25 at unity, centred.
  float l[64];
  float r[64];
  for (int b = 0; b < 4; ++b) {
    e.render(nullptr, nullptr, l, r, 64);
  }
  CHECK(l[63] == doctest::Approx(0.25f).epsilon(0.001));
  CHECK(r[63] == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("setup errors: the count goes on past the records kept, and every enumerator has a name") {
  dsp::Engine e;
  Dummy inst;
  const dsp::UnitId i = e.attach(inst);
  const unsigned total = dsp::kMaxSetupErrors + 5u;
  for (unsigned k = 0; k < total; ++k) {
    CHECK_FALSE(e.set_source(dsp::kMaxSlots + k, i));
  }
  CHECK(e.setup_errors() == total);
  for (unsigned k = 0; k < dsp::kMaxSetupErrors; ++k) {
    const dsp::SetupError* err = e.setup_error(k);
    REQUIRE(err != nullptr);
    CHECK(err->slot == dsp::kMaxSlots + k);  // the first ones, in order
  }
  CHECK(e.setup_error(dsp::kMaxSetupErrors) == nullptr);

  for (unsigned c = 0; c <= static_cast<unsigned>(dsp::SetupCall::AddHook); ++c) {
    const char* name = dsp::Engine::setup_call_name(static_cast<dsp::SetupCall>(c));
    CHECK((name != nullptr && std::strcmp(name, "?") != 0 && name[0] != '\0'));
  }
  for (unsigned r = 0; r <= static_cast<unsigned>(dsp::SetupReason::WrongKind); ++r) {
    const char* name = dsp::Engine::setup_reason_name(static_cast<dsp::SetupReason>(r));
    CHECK((name != nullptr && std::strcmp(name, "?") != 0 && name[0] != '\0'));
  }
}

namespace {

// An app whose setup ignores the return value of a refused route, as `(void)e.set_source(...)` does.
class SloppyApp final : public app::App {
 public:
  explicit SloppyApp(unsigned slot) : slot_(slot) {}
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"SLOPPY", "0.0.1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    dsp::Engine& e = ctx.engine();
    const dsp::UnitId id = e.attach(unit_);
    (void)e.set_source(slot_, id);
  }

 private:
  unsigned slot_;
  Dummy unit_;
};

}  // namespace

TEST_CASE("sim harness: start() fails on a refused setup call and says which; a clean setup passes") {
  SloppyApp good(0);
  {
    auto h = std::make_unique<sim::Harness>(good);
    CHECK(h->start());
    CHECK(h->engine().setup_errors() == 0u);
    CHECK(h->setup_report().empty());
  }
  SloppyApp bad(9);  // the engine has eight slots
  auto h = std::make_unique<sim::Harness>(bad);
  CHECK_FALSE(h->start());  // the refusal is also written to stderr
  CHECK(h->engine().setup_errors() == 1u);
  const std::string report = h->setup_report();
  CHECK(report == "setup refused: set_source(slot 9, unit 0): slot, bus or index out of range\n");
}
