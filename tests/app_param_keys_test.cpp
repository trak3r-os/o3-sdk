// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/param_keys.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/fakes.h>

#include <cstdint>
#include <string>

using namespace o3sdk;
using app::Button;

namespace {

constexpr core::ParamDesc kParams[] = {
    {"A", "", nullptr, nullptr, 0.0f, 100.0f, 50.0f, 100, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0},
    {"B", "", nullptr, nullptr, 0.0f, 100.0f, 50.0f, 100, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0},
    {"C", "", nullptr, nullptr, 0.0f, 100.0f, 50.0f, 100, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0},
};

class Silent final : public dsp::Unit {
 public:
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override {}
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      a.out_l[i] = 0.0f;
      a.out_r[i] = 0.0f;
    }
  }
  dsp::UnitInfo info_{"SILENT", dsp::UnitKind::Instrument, kParams, 3, nullptr, 0};
};

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "T"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class TestApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo i{"TEST", "0"};
    return i;
  }
  void setup(app::Context& ctx) noexcept override {
    unit_id = ctx.engine().attach(unit);
    ctx.show(screen);
  }
  Silent unit;
  Blank screen;
  dsp::UnitId unit_id = dsp::kNoUnit;
};

struct Rig {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  TestApp app;
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{app, engine, platform};
  app::ParamKeys keys;
  app::ParamPage pages[2] = {};

  Rig() {
    runtime.start();
    pages[0] = app::ParamPage{"P1", app.unit_id, {0, 1, 2, 0, 0, 0, 0, 0}, 3};
    pages[1] = app::ParamPage{"P2", app.unit_id, {2, 0, 0, 0, 0, 0, 0, 0}, 1};
    keys.set_pages(pages, 2);
  }
  app::Input button(Button b, bool down, bool shift = false) const {
    return app::Input{down ? app::Input::Type::Press : app::Input::Type::Release, core::InputKind::Button,
                      static_cast<std::uint8_t>(b), static_cast<std::int16_t>(down ? 1 : 0), shift, 0};
  }
  app::Input turn(int d, bool shift = false) const {
    return app::Input{app::Input::Type::Turn, core::InputKind::Encoder, 0, static_cast<std::int16_t>(d), shift, 0};
  }
  bool feed(const app::Input& in) { return keys.input(runtime.context(), in); }
  float value(std::uint16_t id) const { return engine.param(app.unit_id, id); }
};

}  // namespace

TEST_CASE("param keys: the encoder drives F1's parameter until another key is tapped") {
  Rig r;
  CHECK(r.keys.focus() == 0u);
  CHECK(r.feed(r.turn(5)));
  CHECK(r.value(0) == doctest::Approx(55.0f));
  CHECK(r.feed(r.button(Button::F2, true)));
  CHECK(r.feed(r.button(Button::F2, false)));
  CHECK(r.keys.focus() == 1u);
  CHECK(r.feed(r.turn(-3)));
  CHECK(r.value(1) == doctest::Approx(47.0f));
  CHECK(r.value(0) == doctest::Approx(55.0f));
}

TEST_CASE("param keys: hold and turn is momentary, the latched key comes back at the release") {
  Rig r;
  r.feed(r.button(Button::F1, true));
  r.feed(r.button(Button::F1, false));
  r.feed(r.button(Button::F3, true));
  CHECK(r.keys.focus() == 2u);
  r.feed(r.turn(10));
  CHECK(r.value(2) == doctest::Approx(60.0f));
  r.feed(r.button(Button::F3, false));
  CHECK(r.keys.focus() == 0u);
  r.feed(r.turn(1));
  CHECK(r.value(0) == doctest::Approx(51.0f));
}

TEST_CASE("param keys: two held keys move together; Shift turns in coarse steps") {
  Rig r;
  r.feed(r.button(Button::F1, true));
  r.feed(r.button(Button::F2, true));
  CHECK(r.keys.held() == 3u);
  r.feed(r.turn(2, true));  // coarse: 2 x 10
  CHECK(r.value(0) == doctest::Approx(70.0f));
  CHECK(r.value(1) == doctest::Approx(70.0f));
  r.feed(r.button(Button::F2, false));
  r.feed(r.button(Button::F1, false));
  CHECK(r.keys.held() == 0u);
}

TEST_CASE("param keys: a blank key is taken and does nothing; pages switch and keep a focus they have") {
  Rig r;
  CHECK(r.feed(r.button(Button::F6, true)));  // page 1 has three keys
  CHECK(r.keys.focus() == 0u);
  r.feed(r.button(Button::F6, false));
  r.feed(r.button(Button::F3, true));
  r.feed(r.button(Button::F3, false));
  CHECK(r.keys.focus() == 2u);
  r.keys.next_page();  // page 2 has one key: the focus falls back to F1
  CHECK(r.keys.page_index() == 1u);
  CHECK(r.keys.focus() == 0u);
  r.feed(r.turn(1));
  CHECK(r.value(2) == doctest::Approx(51.0f));
  r.keys.next_page();
  CHECK(r.keys.page_index() == 0u);
  CHECK_FALSE(r.feed(r.button(Button::Play, true)));  // not an F key or the encoder: not taken
}

TEST_CASE("panel: held state, Shift and names") {
  app::Panel p;
  p.apply(core::InputEvent{0, core::InputKind::Button, static_cast<std::uint8_t>(Button::Shift), 1});
  p.apply(core::InputEvent{0, core::InputKind::Pad, 47, 127});
  p.apply(core::InputEvent{0, core::InputKind::Button, static_cast<std::uint8_t>(Button::F8), 1});
  CHECK(p.shift());
  CHECK(p.pad_held(47));
  CHECK(p.pads_held() == 1u);
  CHECK(p.fkeys_held() == 0x80u);
  p.apply(core::InputEvent{0, core::InputKind::Pad, 47, 0});
  CHECK_FALSE(p.pad_held(47));
  Button b{};
  CHECK(app::find_button("SampleLoad", b));
  CHECK(b == Button::SampleLoad);
  CHECK(std::string(app::button_name(Button::Right)) == "right");
  CHECK_FALSE(app::find_button("nope", b));
  const app::Input in = app::make_input(core::InputEvent{5, core::InputKind::Button, static_cast<std::uint8_t>(Button::Play), 1}, true);
  CHECK(in.press(Button::Play));
  CHECK(in.shift);
  const app::Input s = app::make_input(core::InputEvent{5, core::InputKind::Button, static_cast<std::uint8_t>(Button::Shift), 1}, true);
  CHECK_FALSE(s.shift);  // Shift itself is not "with Shift"
}
