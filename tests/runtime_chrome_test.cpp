// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screen::chrome() and Screen::theme(), Context::cpu_meter() and cpu_percent() (kit/app screen.h, context.h, runtime.cpp draw(); docs/FRAMEWORK.md, "Runtime"): a screen with the
// defaults is drawn as it always was, a screen with chrome None gets the whole 800 x 480 and no header or soft-key row, the theme a screen names is the one the frame is drawn
// with, and the system Config screen keeps the full chrome and the context's theme over either. That the defaults change no frame of the kit's own apps is the digest tests' job
// (tests/digests/).
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/ui/theme.h>

#include <cstdint>

using namespace o3sdk;
using app::Button;
using app::Chrome;

namespace {

// A theme that is not one of the built-in three: the Amiga one with its own pane colour, so the frame's clear colour tells which theme drew it.
constexpr ui::Rgb kOwnPane{12, 200, 34};
const ui::Theme& own_theme() {
  static const ui::Theme t = [] {
    ui::Theme c = ui::theme(ui::ThemeId::Amiga);
    c.colour[static_cast<unsigned>(ui::Color::Pane)] = kOwnPane;
    return c;
  }();
  return t;
}

// What the screen saw, and what it asks for.
class Probe final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  Chrome chrome(const app::Context&) const noexcept override { return chrome_; }
  const ui::Theme* theme(const app::Context&) const noexcept override { return theme_; }
  void draw(app::Context& ctx, ui::Renderer& r, const ui::Rect& area) noexcept override {
    ++draws;
    seen = area;
    strip_seen = ctx.strip_area();
    seen_theme = &r.theme();
  }
  void soft_keys(app::Context&, app::SoftKeys&) noexcept override { ++key_calls; }

  Chrome chrome_ = Chrome::Full;
  const ui::Theme* theme_ = nullptr;
  unsigned draws = 0;
  unsigned key_calls = 0;
  ui::Rect seen{-1, -1, -1, -1};
  ui::Rect strip_seen{-1, -1, -1, -1};  // Context::strip_area() as the last draw() saw it
  const ui::Theme* seen_theme = nullptr;
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"CHROME", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen); }
  Probe screen;
};

struct Frame {
  explicit Frame(const sim::Harness& h) : px(h.raster().rgba()) {}
  std::uint32_t at(int x, int y) const {
    const std::uint8_t* p = px + (static_cast<std::size_t>(y) * 800u + static_cast<std::size_t>(x)) * 4u;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | std::uint32_t{p[2]};
  }
  const std::uint8_t* px;
};

constexpr std::uint32_t pack(ui::Rgb c) { return (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | std::uint32_t{c.b}; }

// Sample points: the middle of the header strip, of the soft-key row, and of the 28 px the system strip would take at the bottom.
constexpr int kHeaderY = 16;
constexpr int kKeysY = 480 - 11;

struct Rig {
  ProbeApp app;
  sim::Harness h{app};
  Rig() {
    REQUIRE(h.start());
    h.run_ms(80);
  }
  Probe& screen() { return app.screen; }
};

}  // namespace

TEST_CASE("chrome: the defaults are what a screen always had: Full, the context's theme, the content area, header and keys drawn") {
  Rig r;
  const app::Context& ctx = r.h.runtime().context();
  CHECK(r.screen().chrome(ctx) == Chrome::Full);
  CHECK(r.screen().theme(ctx) == nullptr);
  REQUIRE(r.screen().draws > 0u);
  CHECK(r.screen().seen.x == app::kContentArea.x);
  CHECK(r.screen().seen.y == app::kContentArea.y);
  CHECK(r.screen().seen.w == app::kContentArea.w);
  CHECK(r.screen().seen.h == app::kContentArea.h);
  CHECK(r.screen().seen_theme == &ui::theme(ctx.theme()));
  CHECK(r.screen().key_calls > 0u);
  const Frame f(r.h);
  const std::uint32_t pane = pack(ui::theme(ctx.theme()).rgb(ui::Color::Pane));
  CHECK(f.at(400, kHeaderY) != pane);  // the header strip is drawn
  CHECK(f.at(400, kKeysY) != pane);    // and the key-cap row
}

TEST_CASE("chrome: None gives the screen all 800 x 480, draws no header and no soft-key row and never asks for the keys") {
  Rig r;
  r.screen().chrome_ = Chrome::None;
  const unsigned keys_before = r.screen().key_calls;
  r.h.runtime().context().redraw();
  r.h.run_ms(80);
  CHECK(r.screen().seen.x == app::kScreenArea.x);
  CHECK(r.screen().seen.y == app::kScreenArea.y);
  CHECK(r.screen().seen.w == 800);
  CHECK(r.screen().seen.h == 480);
  CHECK(r.screen().key_calls == keys_before);
  const Frame f(r.h);
  const std::uint32_t pane = pack(ui::theme(r.h.runtime().context().theme()).rgb(ui::Color::Pane));
  CHECK(f.at(400, kHeaderY) == pane);  // the screen drew nothing there, and the kit nothing over it
  CHECK(f.at(400, kKeysY) == pane);
  CHECK(f.at(5, 5) == pane);
  CHECK(f.at(795, 475) == pane);
}

TEST_CASE("chrome: back to Full on the next frame, nothing is kept") {
  Rig r;
  r.screen().chrome_ = Chrome::None;
  r.h.run_ms(80);
  CHECK(r.screen().seen.h == 480);
  r.screen().chrome_ = Chrome::Full;
  r.h.run_ms(80);
  CHECK(r.screen().seen.h == app::kContentArea.h);
  CHECK(Frame(r.h).at(400, kHeaderY) != pack(ui::theme(r.h.runtime().context().theme()).rgb(ui::Color::Pane)));
}

TEST_CASE("theme: the one a screen names draws the whole frame, with chrome Full or None; null is the context's") {
  Rig r;
  r.screen().theme_ = &own_theme();
  r.h.run_ms(80);
  CHECK(r.screen().seen_theme == &own_theme());
  CHECK(Frame(r.h).at(5, 300) == pack(kOwnPane));  // the clear colour: the frame's, not the context's

  r.screen().chrome_ = Chrome::None;
  r.h.run_ms(80);
  CHECK(r.screen().seen_theme == &own_theme());
  CHECK(Frame(r.h).at(400, kHeaderY) == pack(kOwnPane));
  CHECK(Frame(r.h).at(400, kKeysY) == pack(kOwnPane));

  r.screen().theme_ = nullptr;
  r.h.run_ms(80);
  CHECK(r.screen().seen_theme == &ui::theme(r.h.runtime().context().theme()));
}

TEST_CASE("chrome and theme: the system Config screen has the full chrome and the context's theme over a screen that asks for neither") {
  Rig r;
  r.screen().chrome_ = Chrome::None;
  r.screen().theme_ = &own_theme();
  r.h.run_ms(80);
  const std::uint32_t own = pack(kOwnPane);
  const std::uint32_t ctx_pane = pack(ui::theme(r.h.runtime().context().theme()).rgb(ui::Color::Pane));
  REQUIRE(own != ctx_pane);
  CHECK(Frame(r.h).at(400, kHeaderY) == own);

  r.h.press(Button::Config);
  r.h.run_ms(2);
  r.h.release(Button::Config);
  r.h.run_ms(80);
  REQUIRE(r.h.runtime().system_open());
  const Frame f(r.h);
  CHECK(f.at(5, 300) == ctx_pane);        // the context's theme clears the frame
  CHECK(f.at(400, kHeaderY) != ctx_pane);  // and the header is drawn

  const unsigned draws = r.screen().draws;
  r.h.run_ms(80);
  CHECK(r.screen().draws == draws);  // the app's screen is not drawn while Config is open

  r.h.press(Button::Config);  // closing it
  r.h.run_ms(2);
  r.h.release(Button::Config);
  r.h.run_ms(80);
  CHECK_FALSE(r.h.runtime().system_open());
  CHECK(r.screen().seen_theme == &own_theme());
  CHECK(r.screen().seen.h == 480);
}

namespace {

bool is_empty(const ui::Rect& r) { return r.w == 0 && r.h == 0; }
bool is_rect(const ui::Rect& r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }

}  // namespace

TEST_CASE("strip_area: empty with no strip, the exact rect of the strip while one is up under either chrome, empty again after it expires") {
  Rig r;
  REQUIRE(r.screen().draws > 0u);
  CHECK_FALSE(r.h.runtime().system().strip_active());
  CHECK(is_empty(r.screen().strip_seen));

  // Full chrome: the strip sits just above the 22 px soft-key row. A learn's notice is this call with kNoticeMs (runtime.cpp, learn_done).
  r.h.runtime().system_strip("LEARNED CC 74 CHANNEL 3", -1, app::SystemConfig::kNoticeMs);
  r.h.run_ms(80);
  REQUIRE(r.h.runtime().system().strip_active());
  CHECK(is_rect(r.screen().strip_seen, 0, 480 - app::kKeysH - 28, 800, 28));
  CHECK(r.screen().strip_seen.y == 430);

  // None: the strip is the bottom 28 px of the whole display.
  r.screen().chrome_ = Chrome::None;
  r.h.run_ms(80);
  CHECK(is_rect(r.screen().strip_seen, 0, 452, 800, 28));

  // The rect is the one the strip is drawn in: its first pixel row is the strip's top edge, and the row above is not the strip's.
  const Frame f(r.h);
  const std::uint32_t pane = pack(ui::theme(r.h.runtime().context().theme()).rgb(ui::Color::Pane));
  CHECK(f.at(795, 452 + 14) != pane);  // opaque strip over the (empty) screen
  CHECK(f.at(795, 451) == pane);

  r.h.run_ms(3000);  // kNoticeMs
  CHECK_FALSE(r.h.runtime().system().strip_active());
  CHECK(is_empty(r.screen().strip_seen));
  CHECK(Frame(r.h).at(795, 452 + 14) == pane);
}

TEST_CASE("strip_area: the screen is redrawn when the strip expires, at once, not on the frame clock") {
  Rig r;
  // The frame clock is 33 ms and a requested redraw is allowed 17 ms after the last frame: whatever the phase of the clock when the strip goes, a frame follows within 17 ms.
  for (std::uint32_t phase = 0; phase < 33; phase += 4) {
    r.h.run_ms(phase);
    r.h.runtime().system_strip("NOTICE", -1, 100);
    r.h.run_ms(60);
    REQUIRE(r.h.runtime().system().strip_active());
    CHECK_FALSE(is_empty(r.screen().strip_seen));
    while (r.h.runtime().system().strip_active()) {
      r.h.run_ms(1);
    }
    r.h.run_ms(18);
    CHECK(is_empty(r.screen().strip_seen));  // the last frame drawn is one without the strip
    r.h.run_ms(80);
  }
}

namespace {

std::uint32_t fake_cycles() noexcept {
  static std::uint32_t t = 0;
  t += hal::audio::kCyclesPerBlock / 4u;
  return t;
}

}  // namespace

TEST_CASE("cpu: Context::cpu_meter() is the platform's flag from construction, cpu_percent() the render's load of the last block, drawn or not") {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  ProbeApp app;
  engine.set_cycle_counter(&fake_cycles);

  app::Platform without{display, input, nullptr, clock, false};
  app::Runtime rt_without{app, engine, without};
  CHECK_FALSE(rt_without.context().cpu_meter());
  CHECK(rt_without.context().cpu_percent() == 0u);

  app::Platform with{display, input, nullptr, clock, true};
  app::Runtime rt{app, engine, with};
  CHECK(rt.context().cpu_meter());
  CHECK(rt.context().cpu_percent() == 0u);  // nothing rendered, nothing drawn
  REQUIRE(rt.start());

  float l[hal::audio::kBlock] = {};
  float rr[hal::audio::kBlock] = {};
  engine.render(nullptr, nullptr, l, rr, hal::audio::kBlock);
  app.screen.chrome_ = Chrome::None;  // the percentage reaches a screen that draws its own header: published by draw(), whatever the chrome
  CHECK(rt.draw());
  CHECK(rt.context().cpu_percent() == 25u);
  app.screen.chrome_ = Chrome::Full;
  CHECK(rt.draw());
  CHECK(rt.context().cpu_percent() == 25u);

  // Without the meter the percentage stays 0 even with a counter on the engine.
  app::Platform off{display, input, nullptr, clock, false};
  app::Runtime rt_off{app, engine, off};
  CHECK(rt_off.draw());
  CHECK(rt_off.context().cpu_percent() == 0u);
}
