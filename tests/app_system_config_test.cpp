// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system Config screen (kit/app system_config.h, docs/M2B2-PLAN.md section 4) in the simulator's harness, on a probe app: the runtime takes
// the Config button, the screen is modal, FIRMWARE walks the flow to INSTALL on the fake sink, DISPLAY changes the theme, the strip is the
// platform's line, and the `state` pairs say where the screen is. The flow's own rules are app_firmware_flow_test.cpp.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/platform/host/fake_firmware.h>
#include <o3sdk/sim/harness.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3sdk;
using app::Button;
using platform::host::FakeFirmwareSink;

namespace {

class ProbeScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  void input(app::Context&, const app::Input& in) noexcept override {
    if (in.type == app::Input::Type::Press) {
      ++presses;
    } else if (in.type == app::Input::Type::Release) {
      ++releases;
    } else {
      ++turns;
    }
    if (in.is(Button::Config)) {
      ++config;
    }
  }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
  void leds(app::Context&, app::PadLeds& leds) noexcept override { leds.set(0, 255); }

  unsigned presses = 0;
  unsigned releases = 0;
  unsigned turns = 0;
  unsigned config = 0;
};

class ProbeApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"PROBE", "1.2.3"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen); }
  bool input(app::Context&, const app::Input& in) noexcept override {
    if (in.is(Button::Config)) {
      ++config;
    }
    ++seen;
    return false;
  }
  const char* update_blocker(const app::Context&) const noexcept override { return blocker; }

  ProbeScreen screen;
  unsigned config = 0;  // Config reached App::input
  unsigned seen = 0;    // every input that reached App::input
  const char* blocker = nullptr;
};

// A bare app: the defaults of the new hooks.
class BareApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"BARE", "0"};
    return kInfo;
  }
  void setup(app::Context&) noexcept override {}
};

struct Rig {
  ProbeApp app;
  FakeFirmwareSink fake;
  std::unique_ptr<sim::Harness> h;

  explicit Rig(bool with_firmware = true) {
    fake.add("O3SDK_HELLO.PTF", 612384);
    fake.add("O3SDK_303.PTF", 668920);
    fake.add("TRACKER.PTF", 1048000);
    h = std::make_unique<sim::Harness>(app, with_firmware ? &fake : nullptr);
    REQUIRE(h->start());
    h->run_ms(50);
  }
  void tap(Button b) {
    h->press(b);
    h->run_ms(1);
    h->release(b);
    h->run_ms(1);
  }
  std::string state() {
    char buf[400];
    const std::size_t n = h->runtime().state_line(buf, sizeof buf);
    return std::string(buf, n);
  }
  bool has(const char* text) { return state().find(text) != std::string::npos; }
  const app::FirmwareView& view() { return h->runtime().system().firmware(); }
  // The way to question 2 from the list: F1 (question 1), F1 (the check), time for its three slices.
  void to_question_two() {
    tap(Button::F1);
    tap(Button::F1);
    h->run_ms(20);
  }
  void open() {
    tap(Button::Config);
    REQUIRE(h->runtime().system_open());
  }
};

}  // namespace

TEST_CASE("system config: the hooks have defaults, and the state pairs start closed") {
  BareApp bare;
  sim::Harness h(bare);
  REQUIRE(h.start());
  CHECK(bare.update_blocker(h.runtime().context()) == nullptr);
  CHECK(bare.config_pages() == 0u);

  Rig with;
  CHECK(with.has(" sys=0 section=firmware fw=closed"));
  Rig without(false);
  CHECK(without.has(" sys=0 section=display fw=closed"));
  CHECK_FALSE(without.h->runtime().system_open());
}

TEST_CASE("system config: Config opens and closes the screen and the app never sees the button") {
  Rig r;
  r.open();
  CHECK(r.has(" sys=1 section=firmware fw=list"));
  CHECK(r.view().n == 3);
  r.tap(Button::Config);
  CHECK_FALSE(r.h->runtime().system_open());
  CHECK(r.has(" sys=0 section=firmware fw=closed"));
  CHECK(r.app.config == 0u);
  CHECK(r.app.screen.config == 0u);
  CHECK(r.app.seen == 0u);
  CHECK(r.app.screen.presses == 0u);
  CHECK(r.app.screen.releases == 0u);  // the release of Config is the runtime's too
  r.tap(Button::F2);
  CHECK(r.app.screen.presses == 1u);  // any other button still reaches the app, and the screen after it
  CHECK(r.app.seen == 2u);
}

TEST_CASE("system config: while open the app gets no new input; a release of what it saw pressed still reaches it") {
  Rig r;
  r.h->pad_down(5);  // pressed before the screen opened
  r.h->run_ms(2);
  CHECK(r.app.screen.presses == 1u);
  r.open();

  r.h->pad_down(6);  // the system's: the app sees neither the press nor, later, the release
  r.h->run_ms(2);
  r.h->turn(1);
  r.h->run_ms(2);
  r.tap(Button::F5);
  CHECK(r.app.screen.presses == 1u);
  CHECK(r.app.screen.turns == 0u);

  r.h->pad_up(5);  // the app saw it pressed: it must see it released, or its note would sound for ever
  r.h->run_ms(2);
  CHECK(r.app.screen.releases == 1u);

  r.tap(Button::Config);
  r.h->pad_up(6);  // released after the screen closed: still the system's
  r.h->run_ms(2);
  CHECK(r.app.screen.releases == 1u);
  r.h->pad_down(6);
  r.h->run_ms(2);
  r.h->pad_up(6);
  r.h->run_ms(2);
  CHECK(r.app.screen.presses == 2u);
  CHECK(r.app.screen.releases == 2u);

  // The pads are dark while the screen is up, the app's own again after.
  r.open();
  r.h->run_ms(100);
  r.h->runtime().draw();
  CHECK(r.h->leds().level(0) == 0);
  r.tap(Button::Config);
  r.h->run_ms(100);
  r.h->runtime().draw();
  CHECK(r.h->leds().level(0) == 255);
}

TEST_CASE("system config: with no update path the sections are DISPLAY and INFO, and the theme follows the encoder, UP, DOWN and F1") {
  Rig r(false);
  r.open();
  CHECK(r.has("sys=1 section=display fw=closed"));
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Amiga);
  r.h->turn(1);
  r.h->run_ms(2);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);
  r.tap(Button::F1);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::White);
  r.tap(Button::Down);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Amiga);
  r.tap(Button::Up);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::White);
  r.h->turn(-1);
  r.h->run_ms(2);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);

  r.tap(Button::Right);
  CHECK(r.has("section=info"));
  r.tap(Button::Right);  // wraps over the hidden FIRMWARE
  CHECK(r.has("section=display"));
  r.tap(Button::Left);
  CHECK(r.has("section=info"));
  r.h->turn(1);  // INFO has no setting: the encoder does nothing
  r.h->run_ms(2);
  CHECK(r.h->runtime().context().theme() == ui::ThemeId::Black);
  r.tap(Button::F8);  // BACK on the last level closes the screen
  CHECK_FALSE(r.h->runtime().system_open());
  r.open();
  CHECK(r.has("section=info"));  // it comes back where it was left
}

TEST_CASE("system config: the keys walk FIRMWARE to INSTALL, and past it nothing acts") {
  Rig r;
  r.open();
  REQUIRE(r.has("fw=list"));
  r.h->turn(1);
  r.h->run_ms(2);
  CHECK(r.view().cursor == 1);
  r.tap(Button::Down);
  CHECK(r.view().cursor == 2);
  r.tap(Button::Down);
  CHECK(r.view().cursor == 2);  // clamped
  r.tap(Button::Up);
  CHECK(r.view().cursor == 1);

  r.tap(Button::F1);
  CHECK(r.has("fw=ask_update"));
  CHECK(r.fake.count("begin_check O3SDK_303.PTF") == 0u);
  r.tap(Button::F1);
  CHECK(r.has("fw=checking"));
  CHECK(r.fake.last_name == "O3SDK_303.PTF");
  r.h->run_ms(20);
  CHECK(r.has("fw=ask_install"));
  CHECK(r.fake.installs == 0u);  // nothing is written before INSTALL
  r.tap(Button::F1);
  CHECK(r.has("fw=installing"));
  CHECK(r.fake.installs == 1u);
  CHECK(r.h->runtime().system().locked());

  // No key acts: not Config, not BACK, not the sections, not a pad.
  r.tap(Button::Config);
  r.tap(Button::F8);
  r.tap(Button::Right);
  r.tap(Button::F1);
  r.h->pad_down(3);
  r.h->run_ms(2);
  r.h->pad_up(3);
  r.h->run_ms(50);
  CHECK(r.has("sys=1 section=firmware fw=installing"));
  CHECK(r.fake.installs == 1u);
  CHECK(r.app.seen == 0u);
}

TEST_CASE("system config: a failed check comes back to the list; a question is taken back by another control, and any exit cancels") {
  Rig r;
  FakeFirmwareSink::Script bad;
  bad.verdict = hal::firmware::CheckStatus::State::Failed;
  bad.offset = 77;
  r.fake.script("TRACKER.PTF", bad);
  r.open();
  r.h->turn(2);
  r.h->run_ms(2);
  r.to_question_two();
  CHECK(r.has("fw=list"));
  CHECK(r.view().bad);
  CHECK(r.view().msg == "NOT A VALID IMAGE: BAD VECTOR TABLE AT 77");

  // Question 1, taken back by the encoder: the list again, the cursor moved by the same turn.
  r.tap(Button::F1);
  CHECK(r.has("fw=ask_update"));
  r.h->turn(-1);
  r.h->run_ms(2);
  CHECK(r.has("fw=list"));
  CHECK(r.view().cursor == 1);

  // Left and right do not leave a check or a question...
  FakeFirmwareSink::Script slow;
  slow.slices = 400;
  r.fake.script("O3SDK_303.PTF", slow);
  r.tap(Button::F1);
  r.tap(Button::F1);
  CHECK(r.has("fw=checking"));
  r.tap(Button::Right);
  CHECK(r.has("section=firmware fw=checking"));
  r.tap(Button::F8);  // CANCEL
  CHECK(r.has("fw=list"));
  // ...and a section change from the list closes it and cancels; coming back reads it again.
  const unsigned reads = r.fake.count("list_begin");
  r.tap(Button::Right);
  CHECK(r.has("section=display fw=closed"));
  r.tap(Button::Left);
  CHECK(r.has("section=firmware fw=list"));
  CHECK(r.fake.count("list_begin") == reads + 1u);

  // Closing the screen on question 2 cancels the good check.
  r.to_question_two();
  CHECK(r.has("fw=ask_install"));
  const unsigned cancels = r.fake.count("cancel");
  r.tap(Button::Config);
  CHECK(r.has("sys=0 section=firmware fw=closed"));
  CHECK(r.fake.count("cancel") > cancels);
  CHECK(r.fake.poll_check().state == hal::firmware::CheckStatus::State::Idle);
  CHECK(r.fake.installs == 0u);
}

TEST_CASE("system config: F8 steps back one level and the last level closes") {
  Rig r;
  r.open();
  r.tap(Button::F1);
  CHECK(r.has("fw=ask_update"));
  r.tap(Button::F8);
  CHECK(r.has("sys=1 section=firmware fw=list"));
  r.tap(Button::F8);  // BACK on the list closes the list, the page stays
  CHECK(r.has("sys=1 section=firmware fw=closed"));
  r.tap(Button::F1);  // LIST reads it again
  CHECK(r.has("fw=list"));
  r.tap(Button::F8);
  r.tap(Button::F8);
  CHECK(r.has("sys=0"));
}

TEST_CASE("system config: a card taken out or put in closes a list that is not past INSTALL; the hand-over is not touched") {
  Rig r;
  r.open();
  r.to_question_two();
  REQUIRE(r.has("fw=ask_install"));
  const unsigned cancels = r.fake.count("cancel");
  r.h->runtime().card_changed();
  r.h->run_ms(2);
  CHECK(r.has("sys=1 section=firmware fw=closed"));
  CHECK(r.fake.count("cancel") > cancels);
  CHECK(r.fake.installs == 0u);

  r.tap(Button::F1);  // LIST again, then all the way to INSTALL
  r.to_question_two();
  r.tap(Button::F1);
  REQUIRE(r.has("fw=installing"));
  const unsigned after = r.fake.count("cancel");
  r.h->runtime().card_changed();
  r.h->run_ms(20);
  CHECK(r.has("fw=installing"));
  CHECK(r.fake.count("cancel") == after);
}

TEST_CASE("system config: the app's update_blocker refuses the list and INSTALL, with its text on the strip") {
  Rig r;
  r.app.blocker = "A RECORDING IS RUNNING";
  r.open();
  CHECK(r.has("sys=1 section=firmware fw=closed"));  // refused: nothing was read
  CHECK(r.fake.count("list_begin") == 0u);
  r.tap(Button::F1);
  CHECK(r.has("fw=closed"));
  CHECK(r.h->runtime().system().strip_active());

  r.app.blocker = nullptr;
  r.tap(Button::F1);
  CHECK(r.has("fw=list"));
  r.to_question_two();
  REQUIRE(r.has("fw=ask_install"));
  r.app.blocker = "SAVE THE PROJECT FIRST";  // it came while the question waited
  r.tap(Button::F1);
  CHECK(r.has("fw=list"));
  CHECK(r.view().msg == "SAVE THE PROJECT FIRST");
  CHECK(r.fake.installs == 0u);
}

TEST_CASE("system config: locked input reaches neither the app nor the screen, and Config does nothing; unlocked, all works") {
  Rig r;
  r.h->runtime().lock_input(true);
  CHECK(r.h->runtime().input_locked());
  r.tap(Button::Config);
  r.tap(Button::F3);
  r.h->pad_down(9);
  r.h->run_ms(2);
  r.h->pad_up(9);
  r.h->run_ms(2);
  CHECK_FALSE(r.h->runtime().system_open());
  CHECK(r.app.seen == 0u);
  r.h->runtime().lock_input(false);
  r.tap(Button::F3);
  CHECK(r.app.seen == 2u);
  r.open();
  r.h->runtime().lock_input(true);  // locked with the screen open: no key moves it
  r.tap(Button::F8);
  CHECK(r.h->runtime().system_open());
  r.h->runtime().lock_input(false);
  r.tap(Button::F8);  // the list steps back first, then the screen closes
  r.tap(Button::F8);
  CHECK_FALSE(r.h->runtime().system_open());
}

namespace {

// Does the strip's band of the screen differ between two frames?
bool strip_band_differs(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int width) {
  const int y0 = ui::kScreenH - app::kKeysH - 28;
  for (int y = y0; y < y0 + 28; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
      if (std::memcmp(&a[i], &b[i], 4) != 0) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

TEST_CASE("system config: the strip is one line over the screen, kept up by its caller and gone after its hold") {
  Rig r(false);
  int w = 0;
  int hgt = 0;
  r.h->runtime().draw();
  const std::vector<std::uint8_t> plain = r.h->panel_image(w, hgt);

  r.h->runtime().system_strip("POWERING OFF");
  r.h->runtime().draw();
  const std::vector<std::uint8_t> with = r.h->panel_image(w, hgt);
  CHECK(strip_band_differs(plain, with, w));
  CHECK(r.h->runtime().system().strip_active());

  r.h->run_ms(300);  // the default hold is 400 ms: still up
  CHECK(r.h->runtime().system().strip_active());
  r.h->runtime().system_strip("POWERING OFF", 700);  // fed again, with a bar
  r.h->run_ms(300);
  CHECK(r.h->runtime().system().strip_active());
  r.h->run_ms(300);  // not fed for 600 ms
  CHECK_FALSE(r.h->runtime().system().strip_active());
  r.h->runtime().draw();
  CHECK_FALSE(strip_band_differs(plain, r.h->panel_image(w, hgt), w));

  r.h->runtime().system_strip("A MESSAGE", -1, 1000);
  CHECK(r.h->runtime().system().strip_active());
  r.h->runtime().system_strip(nullptr);  // taken down
  CHECK_FALSE(r.h->runtime().system().strip_active());
}
