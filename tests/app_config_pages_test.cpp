// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pages an app adds to the system Config screen (kit/app config_page.h, system_config.h, docs/M2C-PLAN.md section 4, slice CP) and the rest of the slice that is not MIDI learn: a page's
// items as the kit reads them, the keys (UP / DOWN, the encoder, F1; an action asks twice), the values kept in the settings and given back at boot, DISPLAY's BRIGHTNESS and PAD LIGHTS
// (never dark), App::header(), the `state` pair setup_errors.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/brightness.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/platform/host/fake_midi.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace o3sdk;
using app::Button;

namespace {

constexpr core::ParamDesc kLevel = {"LEVEL", "%", nullptr, nullptr, 0.0f, 100.0f, 80.0f, 100, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
constexpr const char* kModes[] = {"OFF", "SLOW", "FAST"};
constexpr core::ParamDesc kMode = {"MODE", "", kModes, nullptr, 0.0f, 2.0f, 1.0f, 2, 1, 3, core::ParamUnit::Enum, core::ParamCurve::Linear, 0, 0};
constexpr core::ParamDesc kBad = {"BAD", "", nullptr, nullptr, 5.0f, 5.0f, 5.0f, 1, 1, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};  // min == max: not a value

constexpr app::ConfigItem kPageA[] = {
    {1, "LEVEL", &kLevel},
    {2, "MODE", &kMode},
    {3, "CLEAR", nullptr},      // an action
    {0, "NO KEY", &kLevel},     // left out: key 0
    {1, "TWICE", &kLevel},      // left out: the key is the first item's
    {4, "BROKEN", &kBad},       // left out: the descriptor is not valid
    {5, nullptr, &kLevel},      // left out: no caption
};
constexpr app::ConfigItem kPageB[] = {{10, "OTHER", &kLevel}, {2, "STOLEN", &kLevel}};  // key 2 is page A's: left out
constexpr app::ConfigItem kPageDead[] = {{0, "NOTHING", nullptr}};                     // no valid item: the page is not shown

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PAGES"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
  void leds(app::Context&, app::PadLeds& leds) noexcept override {
    leds.set(0, 255);
    leds.set(1, 40);
    leds.set(2, 5);
  }
};

class PagesApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"PAGES", "1"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    ctx.show(screen);
    if (break_setup) {
      (void)ctx.engine().set_source(99, 0);  // a call the engine refuses: counted
    }
  }
  unsigned config_pages() const noexcept override { return pages; }
  bool config_page(unsigned index, app::ConfigPage& out) const noexcept override {
    switch (index) {
      case 0: out = {"ONE", kPageA, sizeof kPageA / sizeof kPageA[0]}; return true;
      case 1: out = {"TWO", kPageB, sizeof kPageB / sizeof kPageB[0]}; return true;
      case 2: out = {"DEAD", kPageDead, 1}; return true;
      default: return false;
    }
  }
  float config_get(const app::Context&, std::uint16_t key) const noexcept override { return key == 1 ? level : (key == 2 ? mode : (key == 10 ? other : 0.0f)); }
  void config_set(app::Context&, std::uint16_t key, float v) noexcept override {
    ++sets;
    (key == 1 ? level : (key == 2 ? mode : other)) = v;
  }
  void config_run(app::Context&, std::uint16_t key) noexcept override {
    ++runs;
    last_run = key;
  }
  app::HeaderStyle header() const noexcept override { return style; }

  Blank screen;
  unsigned pages = 3;
  bool break_setup = false;
  app::HeaderStyle style;
  float level = 80.0f;
  float mode = 1.0f;
  float other = 0.0f;
  unsigned sets = 0;
  unsigned runs = 0;
  unsigned last_run = 0;
};

class BareApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"BARE", "0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen); }
  Blank screen;
};

struct Rig {
  PagesApp app;
  std::unique_ptr<sim::Harness> h;
  explicit Rig(bool backlight = false, platform::host::HostCard* card = nullptr) {
    h = std::make_unique<sim::Harness>(app, nullptr, nullptr, card);
    h->set_backlight(backlight);
    REQUIRE(h->start());
    h->run_ms(50);
  }
  void tap(Button b) {
    h->press(b);
    h->run_ms(1);
    h->release(b);
    h->run_ms(1);
  }
  void turn(int d, bool shift = false) {
    if (shift) {
      h->press(Button::Shift);
      h->run_ms(1);
    }
    h->turn(d);
    h->run_ms(2);
    if (shift) {
      h->release(Button::Shift);
      h->run_ms(1);
    }
  }
  std::string state() {
    char buf[400];
    const std::size_t n = h->runtime().state_line(buf, sizeof buf);
    return std::string(buf, n);
  }
  bool has(const char* text) { return state().find(text) != std::string::npos; }
  // CONFIG, then RIGHT until the section is `want` ("display", "app"; for "app" also the page): the sections present depend on the platform (a card adds CARD).
  void go(const char* want, unsigned page = 0) {
    if (!h->runtime().system_open()) {
      tap(Button::Config);
    }
    for (unsigned i = 0; i < 12; ++i) {
      if (has((std::string("section=") + want).c_str()) && (std::string(want) != "app" || h->runtime().system().app_page() == page)) {
        return;
      }
      tap(Button::Right);
    }
    FAIL("section not reached");
  }
  void to_page(unsigned page = 0) { go("app", page); }
};

}  // namespace

TEST_CASE("config pages: the kit reads the valid items of the valid pages and leaves the rest out") {
  PagesApp a;
  app::ConfigView v;
  v.build(a);
  CHECK(v.pages == 2u);  // ONE and TWO; DEAD has no valid item
  CHECK(v.page[0].valid() == 3u);
  CHECK(v.page[1].valid() == 1u);
  CHECK(v.values == 3u);  // LEVEL, MODE and OTHER (CLEAR is an action)
  NEED(v.page[0].nth(0) != nullptr);
  CHECK(v.page[0].nth(0)->key == 1u);
  CHECK(v.page[0].nth(2)->key == 3u);
  CHECK(v.page[0].nth(3) == nullptr);
  CHECK(v.page[1].nth(0)->key == 10u);
  CHECK(v.find_value(1) != nullptr);
  CHECK(v.find_value(3) == nullptr);  // an action has no value
  CHECK(v.find_value(4) == nullptr);
  CHECK(v.find_value(99) == nullptr);
  a.pages = 0;
  app::ConfigView none;
  none.build(a);
  CHECK(none.pages == 0u);
  a.pages = 9;  // more than the kit shows: only what config_page() gives, up to kMaxConfigPages
  app::ConfigView many;
  many.build(a);
  CHECK(many.pages == 2u);
  BareApp bare;
  app::ConfigView def;
  def.build(bare);  // the defaults: no page
  CHECK(def.pages == 0u);
}

TEST_CASE("config pages: the app's pages are tabs after INFO, LEFT and RIGHT go round through them, an app with none has the kit's sections alone") {
  Rig r;
  r.tap(Button::Config);
  CHECK(r.has("sys=1 section=display"));
  r.tap(Button::Right);
  CHECK(r.has("section=info"));
  r.tap(Button::Right);
  CHECK(r.has("section=app"));
  CHECK(r.h->runtime().system().app_page() == 0u);
  r.tap(Button::Right);
  CHECK(r.has("section=app"));
  CHECK(r.h->runtime().system().app_page() == 1u);
  r.tap(Button::Right);  // past the last: round to the first section
  CHECK(r.has("section=display"));
  r.tap(Button::Left);  // and back
  CHECK(r.has("section=app"));
  CHECK(r.h->runtime().system().app_page() == 1u);
  r.tap(Button::Config);
  CHECK(r.has("sys=0"));

  BareApp bare;
  sim::Harness h(bare);
  REQUIRE(h.start());
  h.press(Button::Config);
  h.run_ms(2);
  h.release(Button::Config);
  h.run_ms(2);
  h.press(Button::Right);
  h.run_ms(2);
  h.release(Button::Right);
  h.run_ms(2);
  char buf[300];
  (void)h.runtime().state_line(buf, sizeof buf);
  CHECK(std::string(buf).find("section=info") != std::string::npos);
  h.press(Button::Right);
  h.run_ms(2);
  h.release(Button::Right);
  h.run_ms(2);
  (void)h.runtime().state_line(buf, sizeof buf);
  CHECK(std::string(buf).find("section=display") != std::string::npos);  // INFO is the last for an app with no page
}

TEST_CASE("config pages: UP and DOWN choose a row, the encoder and F1 change a value (clamped; F1 goes round; SHIFT is coarse), the app sees every change") {
  Rig r;
  r.to_page();
  CHECK(r.app.level == doctest::Approx(80.0f));
  r.turn(5);
  CHECK(r.app.level == doctest::Approx(85.0f));  // 100 detents over 0..100
  r.turn(-100);
  CHECK(r.app.level == doctest::Approx(0.0f));   // clamped at the bottom
  r.turn(3, true);
  CHECK(r.app.level == doctest::Approx(30.0f));  // coarse: 10 fine steps a detent
  r.turn(500);
  CHECK(r.app.level == doctest::Approx(100.0f));
  const unsigned sets = r.app.sets;
  r.turn(1);  // at the top: nothing to set
  CHECK(r.app.sets == sets);
  r.tap(Button::F1);  // F1 at the top goes round to the bottom
  CHECK(r.app.level == doctest::Approx(0.0f));
  r.tap(Button::Down);  // MODE: an enum of three names
  r.turn(1);
  CHECK(r.app.mode == doctest::Approx(2.0f));
  r.tap(Button::F1);
  CHECK(r.app.mode == doctest::Approx(0.0f));
  r.tap(Button::Down);  // CLEAR
  r.tap(Button::Down);  // (the last row: stays)
  r.tap(Button::Up);
  r.tap(Button::Up);
  r.turn(1);
  CHECK(r.app.level == doctest::Approx(1.0f));  // back on LEVEL
}

TEST_CASE("config pages: an action asks with F1 and runs with the next F1; anything else, or time, takes the question back") {
  Rig r;
  r.to_page();
  r.tap(Button::Down);
  r.tap(Button::Down);  // CLEAR
  r.tap(Button::F1);
  CHECK(r.app.runs == 0u);
  r.tap(Button::F1);
  CHECK(r.app.runs == 1u);
  CHECK(r.app.last_run == 3u);
  r.tap(Button::F1);  // asks again
  r.tap(Button::Up);  // another control: taken back
  r.tap(Button::Down);
  r.tap(Button::F1);  // the first F1 again, not the second
  CHECK(r.app.runs == 1u);
  r.turn(1);  // the encoder takes it back too
  r.tap(Button::F1);
  CHECK(r.app.runs == 1u);
  r.h->run_ms(app::SystemConfig::kAskMs + 100);  // nobody confirmed
  r.tap(Button::F1);
  CHECK(r.app.runs == 1u);
  r.tap(Button::F1);
  CHECK(r.app.runs == 2u);
  // Shift + F1 is no F1: it neither asks nor confirms.
  r.h->press(Button::Shift);
  r.h->run_ms(1);
  r.tap(Button::F1);
  r.tap(Button::F1);
  r.h->release(Button::Shift);
  r.h->run_ms(1);
  CHECK(r.app.runs == 2u);
}

TEST_CASE("config pages: the values are kept in the settings and given back at boot over what setup() set, clamped, and an item the app no longer has is dropped") {
  platform::host::ScratchDir dir("pages-keep");
  NEED(dir.ok());
  platform::host::DirCard card(dir.path());
  {
    Rig r(false, &card);
    r.to_page();
    r.turn(-30);
    r.tap(Button::Down);
    r.turn(1);  // MODE FAST
    r.tap(Button::Right);  // the second page
    r.turn(40);
    CHECK(r.app.level == doctest::Approx(50.0f));
    CHECK(r.app.mode == doctest::Approx(2.0f));
    CHECK(r.app.other == doctest::Approx(40.0f));
    r.h->run_ms(2500);
    NEED(r.h->reboot());  // the app object keeps what it holds in memory: put the defaults back first, as a fresh unit has them
    r.app.level = 80.0f;
    r.app.mode = 1.0f;
    r.app.other = 0.0f;
    r.h->run_ms(0);
  }
  PagesApp second;  // a fresh app on the same card: the defaults of setup(), then the user's values
  sim::Harness h(second, nullptr, nullptr, &card);
  REQUIRE(h.start());
  h.run_ms(50);
  CHECK(second.level == doctest::Approx(50.0f));
  CHECK(second.mode == doctest::Approx(2.0f));
  CHECK(second.other == doctest::Approx(40.0f));
  // An app of another build that has only page one: the value of an item it does not have is dropped, the others come back.
  PagesApp third;
  third.pages = 1;
  sim::Harness g(third, nullptr, nullptr, &card);
  REQUIRE(g.start());
  g.run_ms(50);
  CHECK(third.level == doctest::Approx(50.0f));
  CHECK(third.other == doctest::Approx(0.0f));
}

TEST_CASE("config pages: DISPLAY has BRIGHTNESS and PAD LIGHTS with a backlight: the duty is 20, 50 or 128 (never dark), high costs no call, the pads' levels follow in thirds") {
  Rig r(true);
  r.go("display");
  CHECK(r.h->backlight_duty() == 0u);  // never told: high is the hardware's own
  r.tap(Button::Config);
  r.h->run_ms(40);
  CHECK(r.h->leds().level(0) == 255);
  r.tap(Button::Config);
  r.tap(Button::Down);  // BRIGHTNESS
  r.turn(-1);
  CHECK(r.h->backlight_duty() == 50u);
  r.turn(-5);
  CHECK(r.h->backlight_duty() == 20u);
  CHECK(r.h->runtime().context().brightness() == 0u);
  r.turn(1);
  CHECK(r.h->backlight_duty() == 50u);
  r.tap(Button::F1);
  CHECK(r.h->backlight_duty() == 128u);
  r.tap(Button::F1);  // round to LOW
  CHECK(r.h->backlight_duty() == 20u);
  r.tap(Button::Down);  // PAD LIGHTS
  r.turn(-2);
  CHECK(r.h->runtime().context().pad_lights() == 0u);
  r.tap(Button::Config);  // (the pads are the app's again, and the levels reach the LEDs)
  r.h->run_ms(40);
  CHECK(r.h->leds().level(0) == 85);                       // a third
  CHECK(r.h->leds().level(1) == app::kPadLevelFloor);      // a lit pad never goes dark: the driver's first visible step
  CHECK(r.h->leds().level(2) == 5);                        // a level below the floor is not raised
  r.tap(Button::Config);
  r.turn(1);
  r.tap(Button::Config);
  r.h->run_ms(40);
  CHECK(r.h->leds().level(0) == 170);
  r.tap(Button::Config);
  r.turn(1);
  r.tap(Button::Config);
  r.h->run_ms(40);
  CHECK(r.h->leds().level(0) == 255);
  // The theme is row 0 still.
  r.tap(Button::Config);
  r.tap(Button::Up);
  r.tap(Button::Up);
  r.turn(1);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);
  // The settings keep both (a card is not needed to check the value the runtime keeps).
  CHECK(app::backlight_duty(0) == 20u);
  CHECK(app::backlight_duty(1) == 50u);
  CHECK(app::backlight_duty(2) == 128u);
  CHECK(app::backlight_duty(99) == 128u);
}

TEST_CASE("config pages: brightness and pad lights are kept across a reboot, and the backlight is told what was kept") {
  platform::host::ScratchDir dir("pages-bright");
  NEED(dir.ok());
  platform::host::DirCard card(dir.path());
  Rig r(true, &card);
  r.go("display");
  r.tap(Button::Down);
  r.turn(-2);
  r.tap(Button::Down);
  r.turn(-1);
  CHECK(r.h->backlight_duty() == 20u);
  r.h->run_ms(2500);
  NEED(r.h->reboot());
  r.h->run_ms(20);
  CHECK(r.h->runtime().context().brightness() == 0u);
  CHECK(r.h->runtime().context().pad_lights() == 1u);
  CHECK(r.h->backlight_duty() == 20u);
}

TEST_CASE("config pages: without a backlight DISPLAY is the theme alone and UP / DOWN / F1 change it, as they always did") {
  Rig r(false);
  r.tap(Button::Config);
  r.tap(Button::Down);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);
  r.tap(Button::F1);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::White);
  r.tap(Button::Up);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);
  CHECK(r.h->backlight_duty() == 0u);
}

TEST_CASE("config pages: INFO has a LAST CRASH line when the platform gives one, and none when it does not") {
  Rig plain;
  plain.go("info");
  plain.h->runtime().draw();
  const std::uint32_t without = plain.h->runtime().last_frame_words();
  PagesApp a;
  sim::Harness h(a);
  h.set_crash_line("HARDFAULT AT 12 S, PC 0x0800ABCD LR 0x0800ABCF");
  REQUIRE(h.start());
  h.run_ms(20);
  for (unsigned i = 0; i < 3; ++i) {  // CONFIG, then RIGHT to INFO (DISPLAY is where it opens)
    h.press(i == 0 ? Button::Config : Button::Right);
    h.run_ms(1);
    h.release(i == 0 ? Button::Config : Button::Right);
    h.run_ms(1);
    if (i == 1) {
      break;
    }
  }
  h.runtime().draw();
  CHECK(h.runtime().last_frame_words() > without);  // the line is on the screen
}

TEST_CASE("config pages: App::header() drops the tempo and the transport badge from the strip, and the default keeps them") {
  Rig r;
  r.h->runtime().draw();
  const std::uint32_t full = r.h->runtime().last_frame_words();
  r.app.style.tempo = false;
  r.h->runtime().draw();
  const std::uint32_t no_tempo = r.h->runtime().last_frame_words();
  CHECK(no_tempo < full);
  r.app.style.transport = false;
  r.h->runtime().draw();
  CHECK(r.h->runtime().last_frame_words() < no_tempo);
  BareApp bare;
  CHECK(bare.header().tempo);
  CHECK(bare.header().transport);
}

TEST_CASE("config pages: the state line has setup_errors, 0 for a setup that went through and the count of the calls the engine refused") {
  {
    Rig r;
    CHECK(r.has(" setup_errors=0"));
  }
  PagesApp bad;
  bad.break_setup = true;
  sim::Harness h(bad);
  CHECK_FALSE(h.start());  // the harness fails a setup the engine refused a call of
  char buf[300];
  (void)h.runtime().state_line(buf, sizeof buf);
  CHECK(std::string(buf).find(" setup_errors=1") != std::string::npos);
}

#ifdef O3SDK_HELLO_SCENARIOS
// HELLO's scenarios of this slice run as they are, set up as the simulator sets them up: a card, a MIDI device, a codec input (config_pages.txt), the backlight.
TEST_CASE("config pages: scenarios/config_pages.txt and picker.txt run as they are on HELLO") {
  for (const char* name : {"config_pages.txt", "picker.txt"}) {
    platform::host::ScratchDir card_dir("pages-scn-card");
    platform::host::ScratchDir out_dir("pages-scn-out");
    NEED(card_dir.ok() && out_dir.ok());
    platform::host::DirCard card(card_dir.path());
    platform::host::FakeMidi midi;
    platform::host::FakeCapture capture;
    sim::Harness h(o3sdk_app(), nullptr, &midi, &card, nullptr, &capture);
    h.set_backlight(true);
    NEED(h.start());
    const std::filesystem::path file = std::filesystem::path(O3SDK_HELLO_SCENARIOS) / name;
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    NEED(!text.str().empty());
    const sim::ScenarioResult r = sim::run_scenario(h, text.str(), out_dir.path());
    CHECK_MESSAGE(r.ok, file.string() << ":" << r.line << ": " << r.error);
    CHECK(card.violations() == 0u);
  }
}
#endif

TEST_CASE("config pages: pad scaling follows the PAD LIGHTS row's condition (no backlight, no scaling), and load_settings before start() does nothing") {
  Rig r(false);  // no backlight: no PAD LIGHTS row
  r.h->runtime().context().set_pad_lights(0);
  r.h->run_ms(80);
  CHECK(r.h->leds().level(0) == 255);  // the setting is not applied where the row that sets it is not shown

  PagesApp a;
  sim::Harness h(a);
  CHECK(h.runtime().load_settings() == app::Files::SettingsLoad::NotRead);
  REQUIRE(h.start());
}
