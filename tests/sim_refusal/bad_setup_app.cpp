// SPDX-License-Identifier: GPL-3.0-or-later
// An app whose setup() ignores a refused call, as `(void)e.set_source(...)` does: built by tests/sim_refusal/CMakeLists.txt into
// `bad_setup_sim`, which must exit 1 and say why (expect_refusal.cmake).
#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/dsp/engine.h>

#include <cstdint>

namespace {

class Silence final : public o3sdk::dsp::Unit {
 public:
  const o3sdk::dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  void process(const o3sdk::dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = 0.0f;
      a.out_r[i] = 0.0f;
    }
  }

 private:
  o3sdk::dsp::UnitInfo info_{"SILENCE", o3sdk::dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0};
};

class BadSetupApp final : public o3sdk::app::App {
 public:
  const o3sdk::app::AppInfo& info() const noexcept override {
    static constexpr o3sdk::app::AppInfo kInfo{"BADSETUP", "0.0.1"};
    return kInfo;
  }
  void setup(o3sdk::app::Context& ctx) noexcept override {
    o3sdk::dsp::Engine& e = ctx.engine();
    const o3sdk::dsp::UnitId id = e.attach(unit_);
    (void)e.set_source(o3sdk::dsp::kMaxSlots + 1u, id);  // slot 9 of 8: refused
  }

 private:
  Silence unit_;
};

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept {
  static BadSetupApp app;
  return app;
}
